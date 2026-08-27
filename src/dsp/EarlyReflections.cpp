#include "EarlyReflections.h"

#include "Voicing.h"

#include <algorithm>
#include <cmath>

namespace reverb
{

namespace
{
    float clampf (float v, float lo, float hi) noexcept
    {
        return std::min (std::max (v, lo), hi);
    }
}

//==================================================================================================

void EarlyReflections::prepare (double newSampleRate)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;

    // Worst case across every voicing and every sample rate: the longest tap any voicing may
    // specify, at ER Size 200 %. +4 for the fractional read's interpolation pair and
    // DelayLineFrac's own [1, size - 2] clamp.
    const int tapMax = static_cast<int> (std::ceil (static_cast<double> (maxTapMs) * 0.001
                                                    * static_cast<double> (maxSizeScale) * sampleRate)) + 4;

    float longestSmearMs = 0.0f;

    for (int s = 0; s < numSmearStages; ++s)
        longestSmearMs = std::max (longestSmearMs, std::max (smearLeftMs[s], smearRightMs[s]));

    const int smearMax = static_cast<int> (std::ceil (static_cast<double> (longestSmearMs) * 0.001 * sampleRate)) + 4;

    for (int c = 0; c < 2; ++c)
    {
        const auto ci = static_cast<size_t> (c);
        tapLine[ci].prepare (tapMax);

        for (int s = 0; s < numSmearStages; ++s)
            smear[ci][static_cast<size_t> (s)].prepare (smearMax);
    }

    const int steps = std::max (1, static_cast<int> (smoothingSeconds * sampleRate));

    for (auto* s : { &sizeSmoothed, &spreadSmoothed, &inputGain })
        s->reset (steps);

    const Params defaults;
    sizeSmoothed.setCurrentAndTargetValue (defaults.sizePercent * 0.01f);
    spreadSmoothed.setCurrentAndTargetValue (defaults.spreadPercent * 0.01f);
    inputGain.setCurrentAndTargetValue (1.0f);   // Params::freeze defaults to false

    // The tap table is held in milliseconds, so a sample-rate change has to re-derive the
    // sample-domain copies process() reads. Also the first thing that populates them at all: the
    // engine calls prepare() before its first setVoicing().
    loadTaps (voicingTable, voicingCount);

    reset();
}

void EarlyReflections::reset() noexcept
{
    for (int c = 0; c < 2; ++c)
    {
        const auto ci = static_cast<size_t> (c);
        tapLine[ci].clear();

        for (int s = 0; s < numSmearStages; ++s)
            smear[ci][static_cast<size_t> (s)].clear();
    }
}

void EarlyReflections::setVoicing (const Voicing& voicing) noexcept
{
    // The engine calls this every block (it has no cheaper way to notice the algorithm changed), so
    // skip the sqrt/divide work unless the table itself moved.
    if (voicing.erTapsMs == voicingTable && voicing.numErTaps == voicingCount)
        return;

    voicingTable = voicing.erTapsMs;
    voicingCount = voicing.numErTaps;

    loadTaps (voicingTable, voicingCount);
}

void EarlyReflections::setParameters (const Params& p, bool snap) noexcept
{
    const float sizeScale = clampf (p.sizePercent, 25.0f, 200.0f) * 0.01f;
    const float spread    = clampf (p.spreadPercent, 0.0f, 100.0f) * 0.01f;

    // Freeze holds the tail and takes the input away; the early cluster is a 142 ms FIR, so with
    // its input gated it drains rather than sustaining. That is the same trade the tank makes -- see
    // FDNTank::setParameters' frozen branch, which sets its own inputGain to 0.
    const float injection = p.freeze ? 0.0f : 1.0f;

    if (snap)
    {
        sizeSmoothed.setCurrentAndTargetValue (sizeScale);
        spreadSmoothed.setCurrentAndTargetValue (spread);
        inputGain.setCurrentAndTargetValue (injection);
    }
    else
    {
        sizeSmoothed.setTargetValue (sizeScale);
        spreadSmoothed.setTargetValue (spread);
        inputGain.setTargetValue (injection);
    }
}

void EarlyReflections::processSample (float inL, float inR, float& outL, float& outR) noexcept
{
    const float sizeScale   = sizeSmoothed.getNextValue();
    const float spread      = spreadSmoothed.getNextValue();
    const float spreadScale = spread * sizeScale;

    const float inG = inputGain.getNextValue();

    tapLine[0].push (inL * inG);
    tapLine[1].push (inR * inG);

    float sumL = 0.0f;
    float sumR = 0.0f;

    for (int k = 0; k < numTaps; ++k)
    {
        const auto ki = static_cast<size_t> (k);

        // Spread interpolates each channel's tap time between the shared mean row and its own
        // divergent row (D4); ER Size then scales both. Fractional, so a Size sweep glides.
        const float mean = tapMeanSamples[ki] * sizeScale;
        const float dL   = mean + tapOffsetLeftSamples[ki]  * spreadScale;
        const float dR   = mean + tapOffsetRightSamples[ki] * spreadScale;

        sumL += tapGain[ki] * tapLine[0].read (dL);
        sumR += tapGain[ki] * tapLine[1].read (dR);
    }

    // Smear: two Schroeder allpasses per channel at a fixed gain, so a high ER Size is a diffuse
    // cluster rather than a row of discrete slapbacks. The lengths follow spread for the same
    // reason the tap times do -- at spread 0 the early field has to be genuinely mono, and two
    // different allpass cascades on the same signal would decorrelate it on their own.
    for (int s = 0; s < numSmearStages; ++s)
    {
        const auto si = static_cast<size_t> (s);
        const float mean = smearMeanSamples[si];

        sumL = smear[0][si].process (sumL, mean + smearOffsetLeftSamples[si]  * spread, smearGain);
        sumR = smear[1][si].process (sumR, mean + smearOffsetRightSamples[si] * spread, smearGain);
    }

    outL = sumL;
    outR = sumR;
}

float EarlyReflections::getMaxTapSeconds() const noexcept
{
    float longestMs = 0.0f;

    for (int k = 0; k < numTaps; ++k)
    {
        const auto ki = static_cast<size_t> (k);
        longestMs = std::max (longestMs, std::max (tapMeanMs[ki], std::max (tapLeftMs[ki], tapRightMs[ki])));
    }

    // At the ER Size currently being targeted, so a caller sizing a tail off this gets the length
    // the cluster actually occupies rather than the table's nominal length.
    return longestMs * 0.001f * sizeSmoothed.getTargetValue();
}

void EarlyReflections::loadTaps (const float* ms, int count) noexcept
{
    if (ms != nullptr && count > 0)
    {
        // D8: a voicing carries one row of tap times. The L/R rows are derived from it, because a
        // Voicing has nowhere to put explicit ones -- TODO(8A): widen Voicing if a mode wants its
        // own divergence rather than this fixed fraction.
        numTaps = std::min (count, maxTaps);

        for (int k = 0; k < numTaps; ++k)
        {
            const auto ki = static_cast<size_t> (k);
            const float t = clampf (ms[ki], 0.1f, maxTapMs);

            tapMeanMs[ki]  = t;
            tapLeftMs[ki]  = t * (1.0f - voicingSpreadFraction);
            tapRightMs[ki] = t * (1.0f + voicingSpreadFraction);
        }
    }
    else
    {
        numTaps = numDefaultTaps;

        for (int k = 0; k < numTaps; ++k)
        {
            const auto ki = static_cast<size_t> (k);
            tapMeanMs[ki]  = defaultTapsMeanMs[ki];
            tapLeftMs[ki]  = defaultTapsLeftMs[ki];
            tapRightMs[ki] = defaultTapsRightMs[ki];
        }
    }

    // D4's gain law: g_k = (-1)^k / sqrt (1 + t_k / t_0), normalised so sum (g_k^2) == 1. Taken
    // from the mean row, so the two channels share one set of gains and ER Size -- which scales
    // every t_k by the same factor -- leaves them untouched.
    float sumSquares = 0.0f;

    for (int k = 0; k < numTaps; ++k)
    {
        const auto ki = static_cast<size_t> (k);
        const float g = 1.0f / std::sqrt (1.0f + tapMeanMs[ki] / tapMeanMs[0]);

        tapGain[ki]  = (k % 2 == 0) ? g : -g;
        sumSquares  += g * g;
    }

    // erHeadroom rides along with the normalisation: see its declaration for the measurement that
    // fixes its value. sum (g_k^2) is then erHeadroom^2 rather than 1, which is the point.
    const float normalise = sumSquares > 0.0f ? erHeadroom / std::sqrt (sumSquares) : erHeadroom;

    for (int k = 0; k < numTaps; ++k)
        tapGain[static_cast<size_t> (k)] *= normalise;

    updateSampleTables();
}

void EarlyReflections::updateSampleTables() noexcept
{
    const auto fs = static_cast<float> (sampleRate);

    for (int k = 0; k < numTaps; ++k)
    {
        const auto ki = static_cast<size_t> (k);

        tapMeanSamples[ki]        = tapMeanMs[ki] * 0.001f * fs;
        tapOffsetLeftSamples[ki]  = (tapLeftMs[ki]  - tapMeanMs[ki]) * 0.001f * fs;
        tapOffsetRightSamples[ki] = (tapRightMs[ki] - tapMeanMs[ki]) * 0.001f * fs;
    }

    for (int s = 0; s < numSmearStages; ++s)
    {
        const auto si = static_cast<size_t> (s);

        const float left  = smearLeftMs[si]  * 0.001f * fs;
        const float right = smearRightMs[si] * 0.001f * fs;

        smearMeanSamples[si]        = (left + right) * 0.5f;
        smearOffsetLeftSamples[si]  = left  - smearMeanSamples[si];
        smearOffsetRightSamples[si] = right - smearMeanSamples[si];
    }
}

} // namespace reverb
