#pragma once

/*
    Shared console-test harness for reverb::ReverbEngine / reverb::FDNTank suites.

    Extracted from ReverbEngineTests.cpp in step 1a (PLAN-R2 step 6) and FROZEN from this point on:
    every DSP-level test .cpp that follows -- DecayTests.cpp, EarlyReflectionsTests.cpp,
    DensityTests.cpp, HouseTests.cpp, WetPathTests.cpp, ShimmerTests.cpp, DriveTests.cpp,
    VoicingTests.cpp -- includes this header and links into the ONE WarehouseTests target, so a later
    assignment must not edit it; escalate instead. Links only juce_dsp / juce_audio_basics, same as
    every DSP-level test target.

    Link trap: because more than one translation unit in the same target includes this file, a
    plain (non-inline) definition of a function or a non-inline variable here would be a duplicate
    symbol at link time the moment a second suite is added. Every function below is C++17 `inline`
    and `failureCount` is a C++17 `inline` variable for exactly that reason.

    failureCount is intentionally ONE variable shared by every suite linked into the target, not
    one per translation unit: the suites run strictly sequentially from a single main() (see
    EngineTestMain.cpp), never concurrently or re-entrantly, so each runXxxTests() resetting it to
    0 on entry and reading it back at the end (see runEngineTests() below) is enough to keep the
    counts separate per suite. Follow that pattern in any new suite that uses check().
*/

#include "dsp/ReverbEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

//==================================================================================================

inline int failureCount = 0;

inline void check (const char* name, bool passed, const std::string& measured)
{
    if (! passed)
        ++failureCount;

    std::printf ("[%s] %-34s  %s\n", passed ? "PASS" : "FAIL", name, measured.c_str());
    std::fflush (stdout);
}

inline std::string fmt (const char* pattern, double a, double b = 0.0, double c = 0.0)
{
    char buffer[256];
    std::snprintf (buffer, sizeof (buffer), pattern, a, b, c);
    return std::string (buffer);
}

//==================================================================================================
/** Wraps an engine plus scratch buffers so each test reads as a short script. */
struct Harness
{
    Harness (double sampleRateIn, int blockSizeIn, int numChannelsIn)
        : sampleRate (sampleRateIn), blockSize (blockSizeIn), numChannels (numChannelsIn)
    {
        left.resize (static_cast<size_t> (blockSize), 0.0f);
        right.resize (static_cast<size_t> (blockSize), 0.0f);
        engine.prepare (sampleRate, blockSize, numChannels);
    }

    void processBlock()
    {
        float* channels[2] = { left.data(), right.data() };
        engine.process (channels, numChannels, blockSize);

        // The engine's own non-finite guard zeroes the wet signal before it reaches the output, so
        // a divergence is invisible in the samples. Count the trips explicitly or the stability
        // sweep silently certifies a blown-up FDN.
        if (engine.didRecoverFromNonFinite())
            ++guardTrips;
    }

    void fillSilence()
    {
        std::fill (left.begin(), left.end(), 0.0f);
        std::fill (right.begin(), right.end(), 0.0f);
    }

    void fillNoise (std::mt19937& rng, float amplitude)
    {
        std::uniform_real_distribution<float> dist (-amplitude, amplitude);

        for (int n = 0; n < blockSize; ++n)
        {
            left[static_cast<size_t> (n)]  = dist (rng);
            right[static_cast<size_t> (n)] = dist (rng);
        }
    }

    double blockRms() const
    {
        double sum = 0.0;
        int    count = 0;

        for (int n = 0; n < blockSize; ++n)
        {
            const auto sn = static_cast<size_t> (n);
            sum += static_cast<double> (left[sn]) * left[sn];
            ++count;

            if (numChannels > 1)
            {
                sum += static_cast<double> (right[sn]) * right[sn];
                ++count;
            }
        }

        return count > 0 ? std::sqrt (sum / static_cast<double> (count)) : 0.0;
    }

    float peakAbs() const
    {
        float peak = 0.0f;

        for (int n = 0; n < blockSize; ++n)
        {
            const auto sn = static_cast<size_t> (n);
            peak = std::max (peak, std::abs (left[sn]));

            if (numChannels > 1)
                peak = std::max (peak, std::abs (right[sn]));
        }

        return peak;
    }

    bool allFinite() const
    {
        for (int n = 0; n < blockSize; ++n)
        {
            const auto sn = static_cast<size_t> (n);

            if (! std::isfinite (left[sn]))
                return false;

            if (numChannels > 1 && ! std::isfinite (right[sn]))
                return false;
        }

        return true;
    }

    int blocksFor (double seconds) const
    {
        return static_cast<int> (std::ceil (seconds * sampleRate / static_cast<double> (blockSize)));
    }

    double sampleRate;
    int blockSize;
    int numChannels;
    reverb::ReverbEngine engine;
    std::vector<float> left, right;
    int guardTrips = 0;
};

//==================================================================================================
/** Parameters that make the decay law measurable: filters wide open, no modulation, no pre-delay,
    and every P1-P3 field pinned to its neutral/inert value (see PLAN-R2 3.3's neutral-default
    table) so a suite measuring the decay law is not perturbed by a feature this step only wires
    the Params plumbing for -- no DSP reads any of them yet. decayLowMult/decayHighMult are pinned
    FLAT (1.0) rather than to their musical defaults specifically because T4 (PLAN-R2 7.1) requires
    that exact value to reproduce today's plain scalar bit-for-bit once 2A lands.
*/
inline reverb::ReverbEngine::Params measurementParams (float decaySeconds)
{
    reverb::ReverbEngine::Params p;
    p.mix          = 100.0f;
    p.preDelayMs   = 0.0f;
    p.size         = 100.0f;
    p.decaySeconds = decaySeconds;
    // D1a, THE FOLD (edited in step 2A under an explicit exception to this header's freeze -- the
    // only two lines changed): these are CROSSOVER frequencies now, ranges 1000..12000 and
    // 60..800, so 20000/20 are out of range and would simply be clamped. They no longer need to be
    // "wide open" either: after the fold the attenuation filter is flat because the two
    // multipliers below are 1.0, not because of where the crossovers sit -- a flat filter has
    // nothing for a crossover to place. These are the shipping product defaults (PLAN-R2 3.1).
    p.dampingHz    = 3500.0f;    // D1a: HIGH crossover
    p.lowCutHz     = 250.0f;     // D1a: LOW crossover
    p.bandwidthHz  = 20000.0f;
    p.diffusion    = 70.0f;
    p.modDepth     = 0.0f;
    p.modRateHz    = 0.4f;
    p.width        = 100.0f;
    p.outputGainDb = 0.0f;
    p.freeze       = false;

    // ---- Phase 1: flat/bypassed, per T4 above ----
    p.decayLowMult   = 1.0f;
    p.decayHighMult  = 1.0f;
    p.erMix          = 0.0f;     // late-only, so the tank's decay law stays what is measured
    p.erSizePercent  = 100.0f;
    p.erSpread       = 60.0f;
    p.density        = 0.0f;     // bypass, D10

    // ---- Phase 2: every gate neutral (PLAN-R2 3.3's gated-neutral list) ----
    p.duckAmount       = 0.0f;
    p.duckThresholdDb  = -24.0f;
    p.duckReleaseMs    = 250.0f;
    p.duckCharacter    = 0;      // Gentle
    p.preDelaySync     = false;
    p.preDelayDivision = 5;      // 1/8
    p.bassMonoHz       = 0.0f;   // off, exact bypass, D11
    p.wetLowCutHz      = 20.0f;  // off
    p.wetHighCutHz     = 20000.0f; // off
    p.wetTiltDb        = 0.0f;

    // ---- Phase 3: genuinely neutral defaults (PLAN-R2 3.3) ----
    p.shimmerMode   = 0;         // Off
    p.shimmerAmount = 0.0f;
    p.drive         = 0.0f;      // bit-exact identity, D7
    p.algorithm     = 2;         // Hall, the shipping default; unused until 8A

    return p;
}
