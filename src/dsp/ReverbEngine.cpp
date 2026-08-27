#include "ReverbEngine.h"

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

    float gainFromDecibels (float db) noexcept
    {
        // pow (10, 0) is exactly 1, which is what the bit-transparent bypass relies on.
        return std::pow (10.0f, db * 0.05f);
    }

    template <typename SmoothedType>
    void assignSmoothed (SmoothedType& s, float value, bool snap) noexcept
    {
        if (snap)
            s.setCurrentAndTargetValue (value);
        else
            s.setTargetValue (value);
    }
}

//==================================================================================================

void ReverbEngine::prepare (double newSampleRate, int maxBlockSize, int numChannels)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
    juce::ignoreUnused (numChannels);
    firstUpdate = true;

    const int preDelayMax = static_cast<int> (std::ceil (maxPreDelayMs * 0.001 * sampleRate)) + 8;

    for (int c = 0; c < 2; ++c)
    {
        const auto idx = static_cast<size_t> (c);
        preDelay[idx].prepare (preDelayMax);
        bandwidth[idx].reset();
    }

    diffuser[0].prepare (sampleRate, Diffuser::Channel::left);
    diffuser[1].prepare (sampleRate, Diffuser::Channel::right);

    tank.prepare (sampleRate);
    ducker.prepare (sampleRate);
    earlyReflections.prepare (sampleRate);
    wetChain.prepare (sampleRate);

    const int steps = std::max (1, static_cast<int> (smoothingSeconds * sampleRate));

    for (auto* s : { &mixSmoothed, &outputGainSmoothed,
                     &preDelaySamples, &bandwidthCoeff, &diffusionSmoothed, &erMixSmoothed })
        s->reset (steps);

    // Its own, shorter window (D6): 30 ms of crossfade, not 50 ms of glide.
    preDelayCrossfade.reset (std::max (1, static_cast<int> (preDelayCrossfadeSeconds * sampleRate)));
    preDelayCrossfade.setCurrentAndTargetValue (1.0f);
    preDelayFading       = false;
    lastPreDelayDivision = -1;   // so the first setParameters() cannot look like a division change

    const Params defaults;

    mixSmoothed.setCurrentAndTargetValue (defaults.mix * 0.01f);
    outputGainSmoothed.setCurrentAndTargetValue (gainFromDecibels (defaults.outputGainDb));
    preDelaySamples.setCurrentAndTargetValue (defaults.preDelayMs * 0.001f * static_cast<float> (sampleRate));
    bandwidthCoeff.setCurrentAndTargetValue (OnePoleLP::coefficientForCutoff (defaults.bandwidthHz, sampleRate));
    diffusionSmoothed.setCurrentAndTargetValue (defaults.diffusion * 0.01f);
    erMixSmoothed.setCurrentAndTargetValue (defaults.erMix * 0.01f);

    const auto capacity = static_cast<size_t> (std::max (maxBlockSize, 32));
    wetL.assign (capacity, 0.0f);
    wetR.assign (capacity, 0.0f);
    erL.assign (capacity, 0.0f);
    erR.assign (capacity, 0.0f);
    duckGain.assign (capacity, 1.0f);

    reset();
}

void ReverbEngine::reset() noexcept
{
    for (int c = 0; c < 2; ++c)
    {
        const auto idx = static_cast<size_t> (c);
        preDelay[idx].clear();
        bandwidth[idx].reset();
        diffuser[idx].reset();
    }

    tank.reset();
    ducker.reset();
    earlyReflections.reset();
    wetChain.reset();

    std::fill (wetL.begin(), wetL.end(), 0.0f);
    std::fill (wetR.begin(), wetR.end(), 0.0f);
    std::fill (erL.begin(), erL.end(), 0.0f);
    std::fill (erR.begin(), erR.end(), 0.0f);
    std::fill (duckGain.begin(), duckGain.end(), 1.0f);

    // An in-flight pre-delay crossfade is fading between two reads of a line this call just
    // zeroed, so there is nothing left to fade; the parameter-tracking members (lastPreDelaySync /
    // lastPreDelayDivision) are deliberately NOT touched, since they describe the last parameter
    // update rather than audio state.
    preDelayCrossfade.setCurrentAndTargetValue (1.0f);
    preDelayFading = false;

    recovered = false;
}

void ReverbEngine::setParameters (const Params& p) noexcept
{
    const bool snap = firstUpdate;
    firstUpdate = false;

    assignSmoothed (mixSmoothed,        clampf (p.mix, 0.0f, 100.0f) * 0.01f, snap);
    assignSmoothed (outputGainSmoothed, gainFromDecibels (clampf (p.outputGainDb, -60.0f, 24.0f)), snap);

    // ---- pre-delay: glide a continuous change, crossfade a discrete one (D6) -------------------
    //
    // p.preDelayMs already carries the RESOLVED length -- PluginProcessor substitutes the synced
    // value when the sync toggle is on, so the tempo and the playhead never enter the DSP (see
    // TempoSync.h). What the engine still needs is which KIND of change this is, and the two
    // fields below say so exactly: a change of division, or of the toggle itself, is a jump between
    // two musical grid positions, while everything else (the user dragging the knob, the host
    // ramping its tempo) is continuous and must keep gliding as it always has.
    const float preDelayTarget = clampf (p.preDelayMs, 0.0f, maxPreDelayMs)
                                 * 0.001f * static_cast<float> (sampleRate);

    const bool discretePreDelayChange = ! snap
                                        && (p.preDelaySync != lastPreDelaySync
                                            || p.preDelayDivision != lastPreDelayDivision);

    lastPreDelaySync     = p.preDelaySync;
    lastPreDelayDivision = p.preDelayDivision;

    if (discretePreDelayChange)
    {
        // Freeze the position being left, put the new one in place immediately, and ramp the blend.
        // A change arriving while a fade is still running fades from that fade's DESTINATION: with
        // only two reads available the interrupted position cannot also be kept, so two division
        // changes inside 30 ms can leave a small discontinuity. That is the documented bound of
        // D6's "two reads from one line", not an oversight.
        preDelayFadeFromSamples = preDelaySamples.getCurrentValue();
        preDelaySamples.setCurrentAndTargetValue (preDelayTarget);

        preDelayCrossfade.setCurrentAndTargetValue (0.0f);
        preDelayCrossfade.setTargetValue (1.0f);
        preDelayFading = true;
    }
    else
    {
        assignSmoothed (preDelaySamples, preDelayTarget, snap);
    }

    assignSmoothed (bandwidthCoeff,     OnePoleLP::coefficientForCutoff (p.bandwidthHz, sampleRate), snap);
    assignSmoothed (diffusionSmoothed,  clampf (p.diffusion, 0.0f, 100.0f) * 0.01f, snap);
    // Freeze steers the D4 blend to LATE-ONLY regardless of erMix, and it is the same smoother --
    // so it ramps over the same 50 ms on engage and back on release, with no click and no extra
    // state. Why overriding a user parameter is right here rather than a liberty: early reflections
    // are discrete echoes OF THE INPUT, not a tail, so once Freeze gates the input there is nothing
    // for them to hold; the thing Freeze holds is the late field. Without this, Freeze at
    // erMix = 100 % decays to SILENCE about 200 ms after the user asks for an infinite pad
    // (measured: the wet reaches exactly 0 by 1.2 s). erMix has no meaning while frozen because one
    // of the two fields it balances no longer exists. EarlyReflections' own input gate
    // (Params::freeze) is kept as well -- this makes the held tail audible, the gate stops new
    // material entering the tap line, and the two are checked apart (E5 for the gate, E7 for this).
    assignSmoothed (erMixSmoothed,      p.freeze ? 0.0f
                                                : clampf (p.erMix, 0.0f, 100.0f) * 0.01f, snap);

    FDNTank::Params tp;
    tp.size          = clampf (p.size, 10.0f, 200.0f) * 0.01f;
    tp.decaySeconds  = clampf (p.decaySeconds, 0.05f, 60.0f);
    tp.dampingHz     = p.dampingHz;
    tp.lowCutHz      = p.lowCutHz;
    tp.modDepth      = clampf (p.modDepth, 0.0f, 100.0f) * 0.01f;
    tp.modRateHz     = p.modRateHz;
    tp.freeze        = p.freeze;
    tp.decayLowMult  = p.decayLowMult;
    tp.decayHighMult = p.decayHighMult;
    tp.density       = p.density;
    tp.drive         = p.drive;
    tp.shimmerMode   = p.shimmerMode;
    tp.shimmerAmount = p.shimmerAmount;
    tp.algorithm     = p.algorithm;

    tank.setParameters (tp, snap);

    Ducker::Params dp;
    dp.amountPercent = clampf (p.duckAmount, 0.0f, 100.0f);
    dp.thresholdDb   = p.duckThresholdDb;
    dp.releaseMs     = p.duckReleaseMs;
    dp.character     = p.duckCharacter == 1 ? Ducker::Character::pump : Ducker::Character::gentle;
    ducker.setParameters (dp);

    // D8: algorithm selects the Voicing that (for now) every mode shares -- see Voicing.h's own
    // 1b no-op comment. Clamped defensively since p.algorithm is a plain, unvalidated int.
    const int algoCount = static_cast<int> (Algorithm::count);
    const int algoIndex = std::min (std::max (p.algorithm, 0), algoCount - 1);
    earlyReflections.setVoicing (voicingFor (static_cast<Algorithm> (algoIndex)));

    EarlyReflections::Params erp;
    erp.sizePercent   = clampf (p.erSizePercent, 25.0f, 200.0f);
    erp.spreadPercent = clampf (p.erSpread, 0.0f, 100.0f);
    erp.freeze        = p.freeze;   // gates the early cluster's input, as the tank gates its own
    earlyReflections.setParameters (erp, snap);

    WetChain::Params wcp;
    wcp.widthPercent  = clampf (p.width, 0.0f, 200.0f);
    wcp.wetLowCutHz   = p.wetLowCutHz;
    wcp.wetHighCutHz  = p.wetHighCutHz;
    wcp.tiltDb        = p.wetTiltDb;
    wcp.bassMonoHz    = p.bassMonoHz;
    wetChain.setParameters (wcp, snap);
}

void ReverbEngine::process (float* const* io, int numChannels, int numSamples) noexcept
{
    if (io == nullptr || numChannels <= 0 || numSamples <= 0 || wetL.empty())
        return;

    recovered = false;

    const int capacity = static_cast<int> (wetL.size());
    const int channels = std::min (numChannels, 2);

    for (int offset = 0; offset < numSamples;)
    {
        const int n = std::min (capacity, numSamples - offset);

        float* chunk[2] = { io[0] + offset,
                            channels > 1 ? io[1] + offset : nullptr };

        processChunk (chunk, channels, n);

        offset += n;
    }
}

void ReverbEngine::processChunk (float* const* ch, int numChannels, int numSamples) noexcept
{
    const bool stereo = numChannels > 1;

    // ---- wet path -------------------------------------------------------------------------------
    for (int n = 0; n < numSamples; ++n)
    {
        const auto sn = static_cast<size_t> (n);

        float xL = ch[0][sn];
        float xR = stereo ? ch[1][sn] : xL;

        // Keep a poisoned input sample out of the feedback network in the first place, and write
        // the cleaned value straight back: the dry path below reads the buffer again, so without
        // this one NaN in the input would walk past the wet-path guard and out to the host.
        // For every finite sample this stores the identical bit pattern, so Mix = 0 stays
        // bit-transparent - including for -0.0f and denormals.
        if (! std::isfinite (xL)) xL = 0.0f;
        if (! std::isfinite (xR)) xR = 0.0f;

        ch[0][sn] = xL;

        if (stereo)
            ch[1][sn] = xR;

        // D5: dry mono, sampled at the very top of processChunk -- before pre-delay, before the
        // early reflections, before the tank. It reads the SANITISED sample rather than the raw
        // one, which 1b's comment here had the other way round: the ducker's envelope is real
        // state that persists across blocks from step 5A on, so one NaN in the input would stick
        // in the follower forever and every later block would come out silent or non-finite. D5's
        // "before anything" is about the signal chain; the sanitiser is the input guard, not a
        // stage, and for every finite sample the value is bit-identical either way.
        duckGain[sn] = ducker.processSample ((xL + xR) * 0.5f);

        const float pd   = preDelaySamples.getNextValue();
        const float bwG  = bandwidthCoeff.getNextValue();
        const float diff = diffusionSmoothed.getNextValue();

        preDelay[0].push (xL);
        preDelay[1].push (xR);

        float dL, dR;

        if (preDelayFading)
        {
            // D6's crossfade: TWO reads of the SAME line, the position being left held still while
            // the blend ramps to the new one over 30 ms. Costs two extra reads per channel for
            // those 30 ms and nothing at all outside them, and at f = 1 the expression is
            // bit-identical to the single-read path below (a + 1*(b - a) is not, but the branch
            // clears the moment the ramp settles, on the sample it reaches exactly 1.0f).
            const float f    = preDelayCrossfade.getNextValue();
            const float from = preDelayFadeFromSamples;

            const float oldL = preDelay[0].read (from);
            const float oldR = preDelay[1].read (from);

            dL = oldL + f * (preDelay[0].read (pd) - oldL);
            dR = oldR + f * (preDelay[1].read (pd) - oldR);

            if (! preDelayCrossfade.isSmoothing())
                preDelayFading = false;
        }
        else
        {
            dL = preDelay[0].read (pd);
            dR = preDelay[1].read (pd);
        }

        // D4: ER taps are measured from after pre-delay, so pre-delay moves the whole early
        // cluster -- which is what puts a synced pre-delay's early cluster on the grid. Fed on
        // every sample regardless of erMix so the cluster has warm state when it is blended back
        // in; the blend itself is below.
        earlyReflections.processSample (dL, dR, erL[sn], erR[sn]);

        bandwidth[0].setCoefficient (bwG);
        bandwidth[1].setCoefficient (bwG);

        dL = bandwidth[0].process (dL);
        dR = bandwidth[1].process (dR);

        dL = diffuser[0].process (dL, diff);
        dR = diffuser[1].process (dR, diff);

        float wl = 0.0f, wr = 0.0f;
        tank.processSample (dL, dR, wl, wr);

        wetL[sn] = wl;
        wetR[sn] = wr;
    }

    // ---- non-finite guard -----------------------------------------------------------------------
    // A feedback network that swallows one NaN stays dead forever, so trade one silent block for
    // a full state clear.
    bool nonFinite = false;

    for (int n = 0; n < numSamples && ! nonFinite; ++n)
    {
        const auto sn = static_cast<size_t> (n);
        // duckGain is checked with the rest as of 5A. It is fed a sanitised sample and its detector
        // input is magnitude-clamped, so it should not be able to go non-finite -- but it is a
        // multiplier applied downstream of this guard, so if it ever did, a NaN would reach the
        // host with every wet sample here reading perfectly finite. Cheap insurance in a loop that
        // already early-exits, and it makes reset() (which clears the ducker) the recovery for
        // every buffer this loop covers.
        nonFinite = ! (std::isfinite (wetL[sn]) && std::isfinite (wetR[sn])
                       && std::isfinite (erL[sn]) && std::isfinite (erR[sn])
                       && std::isfinite (duckGain[sn]));
    }

    if (nonFinite)
    {
        reset();
        recovered = true;
    }

    // ---- dry/wet, width, output trim ------------------------------------------------------------
    const bool mixIsZero  = ! mixSmoothed.isSmoothing() && mixSmoothed.getTargetValue() <= 0.0f;
    const bool outIsUnity = ! outputGainSmoothed.isSmoothing()
                            && std::abs (outputGainSmoothed.getTargetValue() - 1.0f) < 1.0e-9f;

    if (mixIsZero && outIsUnity)
    {
        // Fully dry at unity: touch nothing, so the output is bit-identical to the input. Every
        // smoother still has to advance, here and on the non-finite path below, or a value changed
        // during bypass glides from a stale position once bypass ends -- including the ones that now
        // live inside WetChain, which is what WetChain::skip() is for.
        mixSmoothed.skip (numSamples);
        outputGainSmoothed.skip (numSamples);
        erMixSmoothed.skip (numSamples);
        wetChain.skip (numSamples);
        return;
    }

    if (nonFinite)
    {
        for (int c = 0; c < numChannels; ++c)
            std::fill (ch[c], ch[c] + numSamples, 0.0f);

        mixSmoothed.skip (numSamples);
        outputGainSmoothed.skip (numSamples);
        erMixSmoothed.skip (numSamples);
        wetChain.skip (numSamples);
        return;
    }

    for (int n = 0; n < numSamples; ++n)
    {
        const auto sn = static_cast<size_t> (n);

        const float m    = mixSmoothed.getNextValue();
        const float trim = outputGainSmoothed.getNextValue();

        // D4/D11: the early/late blend is the first thing after the tank, ahead of everything
        // WetChain does. The `> 0.0f` guard is the exact-identity guarantee D4 traded the series
        // topology for: at erMix = 0 the late samples are passed through with no arithmetic at
        // all, so the late path is bit-for-bit what it is without an ER stage -- not merely
        // arithmetically equal, which `late + 0 * (er - late)` is not for a late sample of -0.0f.
        //
        // THIS IS THE SINGLE BLEND POINT for early/late. WetChain's own erMix field and its
        // TODO(5B) for the same blend were deleted in this step so it cannot be applied twice.
        const float erAmount = erMixSmoothed.getNextValue();

        float lateL = wetL[sn];
        float lateR = wetR[sn];

        if (erAmount > 0.0f)
        {
            lateL += erAmount * (erL[sn] - lateL);
            lateR += erAmount * (erR[sn] - lateR);
        }

        // Everything after the blend, before this mix: wet EQ, M/S width, bass mono, duck gain
        // (PLAN-R2 D11). See WetChain's own doc comment for what is real as of 1b. erL/erR are
        // still passed through unchanged so WetChain keeps 1b's fixed signature.
        float wl = 0.0f, wr = 0.0f;
        wetChain.processSample (erL[sn], erR[sn], lateL, lateR, duckGain[sn], wl, wr);

        if (stereo)
        {
            ch[0][sn] = (ch[0][sn] * (1.0f - m) + wl * m) * trim;
            ch[1][sn] = (ch[1][sn] * (1.0f - m) + wr * m) * trim;
        }
        else
        {
            ch[0][sn] = (ch[0][sn] * (1.0f - m) + (wl + wr) * 0.5f * m) * trim;
        }
    }
}

} // namespace reverb
