/*
    Early-reflections suite -- PLAN-R2 7.2 E1..E4, step 2B.

    The one thing worth reading before the tests: the ER impulse response is NOT a row of ten
    peaks. D4 puts a two-stage Schroeder allpass smear after the tap bank, and with g = 0.35 the
    smear's own first echo (1 - g^2 = 0.878) is LARGER than its direct path (g^2 = 0.123), so
    "locate the 10 largest peaks" applied to the raw response finds smear echoes, not taps. E2/E3
    therefore deconvolve first, which is exact rather than approximate: an allpass cascade has
    |S(w)| = 1, so its autocorrelation is a delta, and correlating the response with the same
    cascade's impulse response collapses it back to the tap train. See recoverTapTrain().

    That is why EarlyReflections publishes its smear constants -- the test rebuilds the identical
    cascade out of reverb::SchroederAllpass rather than trusting a hard-coded kernel.
*/

#include "TestHarness.h"

#include "dsp/EarlyReflections.h"
#include "dsp/SchroederAllpass.h"

#include <cstring>

namespace
{

using ER     = reverb::EarlyReflections;
using Params = reverb::ReverbEngine::Params;

constexpr double testSampleRate = 48000.0;
constexpr int    testBlockSize  = 512;

//==================================================================================================
/** measurementParams with the ER stage as the only thing feeding the wet path: fully wet so the
    dry signal cannot mask anything, no pre-delay unless a test asks for it. */
Params erParams (float erMix, float erSize, float erSpread)
{
    auto p = measurementParams (2.5f);
    p.mix           = 100.0f;
    p.erMix         = erMix;
    p.erSizePercent = erSize;
    p.erSpread      = erSpread;
    return p;
}

struct Signal
{
    std::vector<float> l, r;

    size_t length() const { return l.size(); }
};

/** Renders `seconds` of audio. `fill` writes the input block, `paramsFor` returns the parameters
    for that block, so a test can sweep. */
template <typename FillFn, typename ParamFn>
Signal renderEngine (double seconds, FillFn&& fill, ParamFn&& paramsFor)
{
    Harness h (testSampleRate, testBlockSize, 2);

    const int blocks = h.blocksFor (seconds);

    Signal out;
    out.l.reserve (static_cast<size_t> (blocks) * static_cast<size_t> (testBlockSize));
    out.r.reserve (static_cast<size_t> (blocks) * static_cast<size_t> (testBlockSize));

    for (int b = 0; b < blocks; ++b)
    {
        fill (h, b);

        const Params p = paramsFor (b);
        h.engine.setParameters (p);
        h.processBlock();

        out.l.insert (out.l.end(), h.left.begin(), h.left.end());
        out.r.insert (out.r.end(), h.right.begin(), h.right.end());
    }

    return out;
}

/** A single unit impulse in both channels at sample 0, then silence. */
Signal renderImpulse (double seconds, const Params& p)
{
    return renderEngine (seconds,
                         [] (Harness& h, int block)
                         {
                             h.fillSilence();

                             if (block == 0)
                             {
                                 h.left[0]  = 1.0f;
                                 h.right[0] = 1.0f;
                             }
                         },
                         [&p] (int) { return p; });
}

/** The same noise in both channels, from a fixed seed, so two renders see identical input. */
Signal renderNoise (double seconds, const Params& p, unsigned seed)
{
    std::mt19937 rng (seed);
    std::uniform_real_distribution<float> dist (-0.5f, 0.5f);

    return renderEngine (seconds,
                         [&rng, &dist] (Harness& h, int)
                         {
                             for (int n = 0; n < h.blockSize; ++n)
                             {
                                 const float v = dist (rng);
                                 h.left[static_cast<size_t> (n)]  = v;
                                 h.right[static_cast<size_t> (n)] = v;
                             }
                         },
                         [&p] (int) { return p; });
}

int countMismatches (const Signal& a, const Signal& b)
{
    int mismatches = 0;
    const size_t n = std::min (a.length(), b.length());

    for (size_t i = 0; i < n; ++i)
    {
        if (std::memcmp (&a.l[i], &b.l[i], sizeof (float)) != 0) ++mismatches;
        if (std::memcmp (&a.r[i], &b.r[i], sizeof (float)) != 0) ++mismatches;
    }

    return mismatches;
}

double rmsDifference (const Signal& a, const Signal& b)
{
    const size_t n = std::min (a.length(), b.length());
    double sum = 0.0;

    for (size_t i = 0; i < n; ++i)
    {
        const double dl = static_cast<double> (a.l[i]) - static_cast<double> (b.l[i]);
        const double dr = static_cast<double> (a.r[i]) - static_cast<double> (b.r[i]);
        sum += dl * dl + dr * dr;
    }

    return n > 0 ? std::sqrt (sum / static_cast<double> (2 * n)) : 0.0;
}

//==================================================================================================
// Deconvolution back to the tap train.
//==================================================================================================

/** Impulse response of one channel's smear cascade at a given ER Spread, rebuilt from
    EarlyReflections' own published constants and the same interpolation rule (mean row at spread 0,
    the channel's own row at spread 1). */
std::vector<float> smearKernel (int channel, float spread, int length)
{
    const auto fs = static_cast<float> (testSampleRate);

    std::array<reverb::SchroederAllpass, ER::numSmearStages> allpass {};
    std::array<float, ER::numSmearStages> delaySamples {};

    for (int s = 0; s < ER::numSmearStages; ++s)
    {
        const auto si = static_cast<size_t> (s);

        const float left  = ER::smearLeftMs[si]  * 0.001f * fs;
        const float right = ER::smearRightMs[si] * 0.001f * fs;
        const float mean  = (left + right) * 0.5f;
        const float own   = channel == 0 ? left : right;

        delaySamples[si] = mean + spread * (own - mean);

        allpass[si].prepare (static_cast<int> (std::ceil (std::max (left, right))) + 4);
    }

    std::vector<float> kernel (static_cast<size_t> (length), 0.0f);

    for (int n = 0; n < length; ++n)
    {
        float x = n == 0 ? 1.0f : 0.0f;

        for (int s = 0; s < ER::numSmearStages; ++s)
        {
            const auto si = static_cast<size_t> (s);
            x = allpass[si].process (x, delaySamples[si], ER::smearGain);
        }

        kernel[static_cast<size_t> (n)] = x;
    }

    return kernel;
}

/** Correlates a response with the smear kernel, which -- because the kernel is an allpass cascade,
    so its autocorrelation is a delta -- returns the tap train the smear was applied to. */
std::vector<double> recoverTapTrain (const std::vector<float>& response, const std::vector<float>& kernel)
{
    double energy = 0.0;

    for (const float k : kernel)
        energy += static_cast<double> (k) * static_cast<double> (k);

    const size_t n = response.size() > kernel.size() ? response.size() - kernel.size() : 0;
    std::vector<double> train (n, 0.0);

    for (size_t i = 0; i < n; ++i)
    {
        double sum = 0.0;

        for (size_t m = 0; m < kernel.size(); ++m)
            sum += static_cast<double> (response[i + m]) * static_cast<double> (kernel[m]);

        train[i] = energy > 0.0 ? sum / energy : 0.0;
    }

    return train;
}

/** The `count` largest |peaks| of a tap train, each suppressing a +/-guard window so one tap is not
    reported twice, returned in ascending sample order. */
std::vector<int> largestPeaks (std::vector<double> train, int count, int guard)
{
    std::vector<int> peaks;

    for (int p = 0; p < count; ++p)
    {
        size_t best = 0;
        double bestValue = -1.0;

        for (size_t i = 0; i < train.size(); ++i)
        {
            const double v = std::abs (train[i]);

            if (v > bestValue)
            {
                bestValue = v;
                best      = i;
            }
        }

        if (bestValue <= 0.0)
            break;

        peaks.push_back (static_cast<int> (best));

        const size_t lo = best > static_cast<size_t> (guard) ? best - static_cast<size_t> (guard) : 0;
        const size_t hi = std::min (train.size(), best + static_cast<size_t> (guard) + 1);

        for (size_t i = lo; i < hi; ++i)
            train[i] = 0.0;
    }

    std::sort (peaks.begin(), peaks.end());
    return peaks;
}

/** Where an impulse fed to the engine comes back out for tap k, in output samples.

    Both DelayLineFrac reads on the way (pre-delay, then the ER tap) return the sample pushed
    `delay - 1` steps ago, because push() precedes read() at both sites -- see DelayLineFrac's
    "after push (x), read (1.0f) returns x". Hence the -2. */
int expectedTapIndex (float preDelayMs, float tapMs, float sizeScale)
{
    const auto fs = static_cast<double> (testSampleRate);
    const double pd  = static_cast<double> (preDelayMs) * 0.001 * fs;
    const double tap = static_cast<double> (tapMs) * 0.001 * fs * static_cast<double> (sizeScale);

    return static_cast<int> (std::lround (pd + tap)) - 2;
}

double zeroLagCorrelation (const std::vector<float>& a, const std::vector<float>& b, size_t from)
{
    double sumAB = 0.0, sumAA = 0.0, sumBB = 0.0;
    const size_t n = std::min (a.size(), b.size());

    for (size_t i = from; i < n; ++i)
    {
        const double x = static_cast<double> (a[i]);
        const double y = static_cast<double> (b[i]);
        sumAB += x * y;
        sumAA += x * x;
        sumBB += y * y;
    }

    const double denom = std::sqrt (sumAA * sumBB);
    return denom > 0.0 ? sumAB / denom : 0.0;
}

/** Largest |y[n] - 2y[n-1] + y[n-2]| over a window. The SECOND difference, not the first: the
    first difference of the ER output is dominated by the input waveform itself, which is why the
    static-case baseline it is compared against would swamp a per-sample tap jump. Curvature is
    tiny for a smooth signal and huge at a step, which is exactly the discrimination E4 needs. */
double maxSecondDifference (const std::vector<float>& x, size_t from)
{
    double worst = 0.0;

    for (size_t i = std::max (from, static_cast<size_t> (2)); i < x.size(); ++i)
    {
        const double d = static_cast<double> (x[i]) - 2.0 * static_cast<double> (x[i - 1])
                         + static_cast<double> (x[i - 2]);
        worst = std::max (worst, std::abs (d));
    }

    return worst;
}

//==================================================================================================
// E1. erMix = 0 is bit-identical to the ER-free path.
//
// The reference the plan names -- "the same render with the ER object's processSample forced to
// write zeros" -- is not reachable from a test binary without a production hook, so this asserts
// the property that hook would have proven, from three directions at once:
//
//   (a) at erMix = 0, three wildly different ER configurations produce bit-identical output. Any
//       path from the ER stage into the late signal carries the ER settings with it, so if one
//       existed these would differ.
//   (b) the same three configurations at erMix = 100 differ from EACH OTHER, so (a) is not passing
//       because the configurations are secretly the same.
//   (c) erMix = 100 differs from erMix = 0 by a large margin, so (a) is not passing because the ER
//       stage is silent.
//
// ReverbEngine::processChunk gets the bit-exactness by SKIPPING the blend at a settled zero rather
// than relying on `late + 0 * (er - late)`, which is not an identity for a late sample of -0.0f.
// The plan's mutation (blend 0.001 * er + 0.999 * late) fails (a) and is the reason (a) compares
// three configurations rather than re-rendering one twice.
//==================================================================================================
void testErMixZeroIsIdentity()
{
    struct Config { float size, spread; int algorithm; };

    const Config configs[3] = { {  25.0f,   0.0f, 0 },
                                { 100.0f,  60.0f, 2 },
                                { 200.0f, 100.0f, 4 } };

    Signal silentBlend[3];
    Signal fullBlend[3];

    for (int c = 0; c < 3; ++c)
    {
        const auto ci = static_cast<size_t> (c);

        auto zero = erParams (0.0f, configs[ci].size, configs[ci].spread);
        zero.algorithm = configs[ci].algorithm;
        silentBlend[ci] = renderNoise (4.0, zero, 0xE12E20u);

        auto full = erParams (100.0f, configs[ci].size, configs[ci].spread);
        full.algorithm = configs[ci].algorithm;
        fullBlend[ci] = renderNoise (4.0, full, 0xE12E20u);
    }

    const int mismatches = countMismatches (silentBlend[0], silentBlend[1])
                           + countMismatches (silentBlend[1], silentBlend[2]);

    const double configSeparation = std::min (rmsDifference (fullBlend[0], fullBlend[1]),
                                              rmsDifference (fullBlend[1], fullBlend[2]));

    const double blendSeparation = rmsDifference (fullBlend[1], silentBlend[1]);

    check ("E1. erMix=0 bit-identical",
           mismatches == 0 && configSeparation > 1.0e-3 && blendSeparation > 1.0e-2,
           fmt ("mismatched samples=%.0f  config separation @ erMix=100 rms=%.5f  vs erMix=0 rms=%.5f",
                static_cast<double> (mismatches), configSeparation, blendSeparation));
}

//==================================================================================================
// E2. Tap times are where they are claimed to be.
//
// Impulse in, erMix = 100, erSpread = 0 (both channels on the mean row), deconvolved past the
// smear, then the ten largest peaks must land within +/-2 samples of preDelay + meanTap_k * erSize.
// Run at two ER Sizes because at 100 % the size scaling is the identity, so a single run cannot see
// the plan's first mutation (drop the erSize scaling). The 20 ms pre-delay is what makes the second
// mutation (measure taps from before pre-delay) a 958-sample error rather than a rounding one.
//==================================================================================================
void testTapTimes()
{
    constexpr float preDelayMs = 20.0f;
    constexpr int   kernelLength = 8192;

    const std::vector<float> kernel = smearKernel (0, 0.0f, kernelLength);

    int  worstError = 0;
    bool countsOk = true;

    for (const float erSize : { 100.0f, 200.0f })
    {
        auto p = erParams (100.0f, erSize, 0.0f);
        p.preDelayMs = preDelayMs;

        const Signal response = renderImpulse (1.0, p);
        const std::vector<double> train = recoverTapTrain (response.l, kernel);
        const std::vector<int> peaks = largestPeaks (train, ER::numDefaultTaps, 64);

        if (peaks.size() != static_cast<size_t> (ER::numDefaultTaps))
        {
            countsOk = false;
            continue;
        }

        for (int k = 0; k < ER::numDefaultTaps; ++k)
        {
            const int expected = expectedTapIndex (preDelayMs,
                                                   ER::defaultTapsMeanMs[static_cast<size_t> (k)],
                                                   erSize * 0.01f);

            worstError = std::max (worstError, std::abs (peaks[static_cast<size_t> (k)] - expected));
        }
    }

    check ("E2. tap times +/-2 samples",
           countsOk && worstError <= 2,
           fmt ("peaks found=%.0f  worst error=%.0f samples (erSize 100%% and 200%%)",
                countsOk ? static_cast<double> (ER::numDefaultTaps) : 0.0,
                static_cast<double> (worstError)));
}

//==================================================================================================
// E3. Spread actually decorrelates.
//
// Mono noise in, so any L/R difference is the ER stage's own doing. Two halves:
//   - the correlation itself: > 0.98 at erSpread = 0, < 0.5 at erSpread = 100;
//   - the recovered tap positions at erSpread = 100 must match each channel's OWN divergent row.
//
// The second half is what kills the plan's mutation (use the same tap table for both channels).
// The first half alone would not: the smear lengths also diverge with spread, and two different
// allpass cascades on the same signal decorrelate it on their own, so a shared tap table could
// still push the correlation under 0.5 and survive.
//==================================================================================================
void testSpreadDecorrelates()
{
    const auto skip = static_cast<size_t> (0.3 * testSampleRate);

    const Signal mono   = renderNoise (2.0, erParams (100.0f,   100.0f,   0.0f), 0xE3B0Du);
    const Signal spread = renderNoise (2.0, erParams (100.0f,   100.0f, 100.0f), 0xE3B0Du);

    const double monoCorrelation   = zeroLagCorrelation (mono.l,   mono.r,   skip);
    const double spreadCorrelation = zeroLagCorrelation (spread.l, spread.r, skip);

    // ---- tap positions per channel at full spread ----
    constexpr float preDelayMs = 20.0f;
    constexpr int   kernelLength = 8192;

    auto p = erParams (100.0f, 100.0f, 100.0f);
    p.preDelayMs = preDelayMs;

    const Signal response = renderImpulse (1.0, p);

    int  worstError = 0;
    bool countsOk = true;

    for (int channel = 0; channel < 2; ++channel)
    {
        const std::vector<float>& out = channel == 0 ? response.l : response.r;
        const std::vector<float> kernel = smearKernel (channel, 1.0f, kernelLength);

        const std::vector<double> train = recoverTapTrain (out, kernel);
        const std::vector<int> peaks = largestPeaks (train, ER::numDefaultTaps, 64);

        if (peaks.size() != static_cast<size_t> (ER::numDefaultTaps))
        {
            countsOk = false;
            continue;
        }

        for (int k = 0; k < ER::numDefaultTaps; ++k)
        {
            const auto ki = static_cast<size_t> (k);
            const float tapMs = channel == 0 ? ER::defaultTapsLeftMs[ki] : ER::defaultTapsRightMs[ki];
            const int expected = expectedTapIndex (preDelayMs, tapMs, 1.0f);

            worstError = std::max (worstError, std::abs (peaks[ki] - expected));
        }
    }

    check ("E3. spread decorrelates",
           monoCorrelation > 0.98 && spreadCorrelation < 0.5 && countsOk && worstError <= 2,
           fmt ("L/R correlation spread0=%.4f spread100=%.4f  per-channel tap error=%.0f samples",
                monoCorrelation, spreadCorrelation, static_cast<double> (worstError)));
}

//==================================================================================================
// E4. ER Size sweeps without a click.
//
// A 100 Hz sine rather than the plan's noise, and the second difference rather than the first: see
// maxSecondDifference(). With noise, or with the first difference, the input's own sample-to-sample
// step is far larger than a one-sample tap jump, and the plan's 4x bound passes with unsmoothed
// integer tap positions -- the mutation it exists to catch survives. Curvature separates them by
// more than an order of magnitude.
//==================================================================================================
void testSizeSweepIsSmooth()
{
    constexpr double seconds = 2.0;
    constexpr float  toneHz  = 100.0f;

    auto sine = [] (Harness& h, int block)
    {
        const double omega = 2.0 * juce::MathConstants<double>::pi
                             * static_cast<double> (toneHz) / testSampleRate;

        for (int n = 0; n < h.blockSize; ++n)
        {
            const auto index = static_cast<double> (block * h.blockSize + n);
            const auto v = static_cast<float> (0.5 * std::sin (omega * index));
            h.left[static_cast<size_t> (n)]  = v;
            h.right[static_cast<size_t> (n)] = v;
        }
    };

    const Params staticParams = erParams (100.0f, 100.0f, 60.0f);

    const Signal held = renderEngine (seconds, sine, [&staticParams] (int) { return staticParams; });

    const int totalBlocks = static_cast<int> (std::ceil (seconds * testSampleRate
                                                         / static_cast<double> (testBlockSize)));

    const Signal swept = renderEngine (seconds, sine,
                                       [totalBlocks] (int block)
                                       {
                                           const float t = static_cast<float> (block)
                                                           / static_cast<float> (std::max (1, totalBlocks - 1));
                                           return erParams (100.0f, 25.0f + t * 175.0f, 60.0f);
                                       });

    // Skip the first 0.25 s: the tank and the ER cluster are both still filling, and the swept
    // render's first setParameters snaps rather than ramps.
    const auto skip = static_cast<size_t> (0.25 * testSampleRate);

    const double heldWorst  = std::max (maxSecondDifference (held.l,  skip),
                                        maxSecondDifference (held.r,  skip));
    const double sweptWorst = std::max (maxSecondDifference (swept.l, skip),
                                        maxSecondDifference (swept.r, skip));

    const double ratio = heldWorst > 0.0 ? sweptWorst / heldWorst : 0.0;

    check ("E4. erSize sweep is click-free",
           heldWorst > 0.0 && ratio < 4.0,
           fmt ("max curvature static=%.3e swept=%.3e  ratio=%.2fx (bound 4x)",
                heldWorst, sweptWorst, ratio));
}

//==================================================================================================
// E5. Freeze gates the early cluster: no new input passes it.
//
// NOT one of the plan's E1-E4: added on the CTO's ruling after 2B reported that the parallel ER
// branch let new input reach the wet output while frozen.
//
// Tested on the EarlyReflections OBJECT, not through the engine, and the reason matters. The other
// half of the same ruling steers the engine's erMix blend to late-only while frozen, which makes
// the early branch INAUDIBLE at the engine's output -- so an engine-level assertion here would pass
// with the input gate deleted (the blend would be hiding it) and would be vacuous against its own
// mutation. The gate's property is "no new material enters the tap line", and this is where that is
// observable. E7 covers the blend override; the two mechanisms are checked apart.
//
// Full-scale input keeps arriving throughout. The unfrozen window is the non-vacuity half.
//==================================================================================================
void testFreezeGatesEarly()
{
    reverb::EarlyReflections er;
    er.prepare (testSampleRate);   // loads the D4 default tap table

    ER::Params p;
    p.sizePercent   = 100.0f;
    p.spreadPercent = 60.0f;
    p.freeze        = false;
    er.setParameters (p, true);

    std::mt19937 rng (0xF7EE2Eu);
    std::uniform_real_distribution<float> dist (-1.0f, 1.0f);

    auto runFor = [&] (double seconds, double measureLastSeconds)
    {
        const auto total   = static_cast<int> (seconds * testSampleRate);
        const auto measure = static_cast<int> (measureLastSeconds * testSampleRate);

        double sum = 0.0;
        int    count = 0;

        for (int n = 0; n < total; ++n)
        {
            const float in = dist (rng);
            float outL = 0.0f, outR = 0.0f;
            er.processSample (in, in, outL, outR);

            if (n >= total - measure)
            {
                sum += static_cast<double> (outL) * outL + static_cast<double> (outR) * outR;
                count += 2;
            }
        }

        return count > 0 ? std::sqrt (sum / static_cast<double> (count)) : 0.0;
    };

    const double liveRms = runFor (0.5, 0.1);

    p.freeze = true;
    er.setParameters (p, false);   // ramps, does not snap: engaging Freeze must not click

    // The window opens a full second after the gate closes because the cluster DRAINS rather than
    // stopping: 50 ms of injection ramp, then a 142 ms tap span, then the smear allpasses ringing
    // down at g = 0.35 per pass. Measured residue in 50 ms windows after the gate closes: 2.0e-1,
    // 1.1e-1, 2.2e-2, 6.1e-4, 8.2e-6, 1.2e-7, 1.8e-9, 2.8e-11, ... about -36 dB per 50 ms once the
    // taps are empty. A window opening at 250 ms measures 5.7e-8 -- the physics of the drain, not a
    // leak -- so it is the window that has to be right, not the bound.
    const double frozenRms = runFor (1.25, 0.25);

    check ("E5. freeze gates the early cluster",
           liveRms > 0.01 && frozenRms < 1.0e-9,
           fmt ("cluster output rms live=%.5f  frozen (input still full scale)=%.3e", liveRms, frozenRms));
}

//==================================================================================================
// E6. The fully-dry fast path advances the smoothers that live in sub-objects.
//
// Also not one of E1-E4: it covers WetChain::skip(), added on the CTO's ruling. Step 1b moved
// widthSmoothed out of ReverbEngine into WetChain and, having no skip() to call, dropped the two
// skip() calls on ReverbEngine's two early-return paths -- so a Width change made while bypassed
// glided from a stale position once bypass ended. Nothing in the suite observed that.
//
// Width is the observable because it is the one smoothed value that lives in WetChain today, and it
// only advances inside the block loop the fast path skips. Mono-identical input makes L-R a pure
// width artifact: the dry contributes nothing to it, and at width 0 the M/S form gives outL == outR
// bit-for-bit, so the assertion is an exact zero rather than a threshold. The reference render --
// same sequence, width left at 100 -- is the non-vacuity half.
//==================================================================================================
void testBypassAdvancesWetChain()
{
    auto measure = [] (float widthDuringBypass)
    {
        Harness h (testSampleRate, testBlockSize, 2);

        std::mt19937 rng (0xB7A55u);
        std::uniform_real_distribution<float> dist (-0.5f, 0.5f);

        auto fillMono = [&h, &rng, &dist]
        {
            for (int n = 0; n < h.blockSize; ++n)
            {
                const float v = dist (rng);
                h.left[static_cast<size_t> (n)]  = v;
                h.right[static_cast<size_t> (n)] = v;
            }
        };

        auto p = measurementParams (2.5f);
        p.mix   = 100.0f;
        p.width = 100.0f;
        h.engine.setParameters (p);     // snaps

        for (int b = 0; b < h.blocksFor (0.5); ++b)   // charge the tank
        {
            fillMono();
            h.engine.setParameters (p);
            h.processBlock();
        }

        // Bypass: mix 0 at unity trim is ReverbEngine's fully-dry early return. Let it ENGAGE
        // first -- the mix ramp itself takes 50 ms, and the block loop still runs while it is
        // smoothing, so a width retarget issued now would simply complete on that path and the
        // test would be vacuous (verified: it was, before this was split in two).
        p.mix = 0.0f;

        for (int b = 0; b < h.blocksFor (0.5); ++b)
        {
            fillMono();
            h.engine.setParameters (p);
            h.processBlock();
        }

        // Fully bypassed now. Retarget width with nothing being processed, and hold it for 10x the
        // 50 ms smoothing window: only skip() can advance it here.
        p.width = widthDuringBypass;

        for (int b = 0; b < h.blocksFor (0.5); ++b)
        {
            fillMono();
            h.engine.setParameters (p);
            h.processBlock();
        }

        // Un-bypass. Width is not retargeted again, so whatever it reads now is whatever the
        // bypassed blocks left it at.
        p.mix = 100.0f;
        fillMono();
        h.engine.setParameters (p);
        h.processBlock();

        double worst = 0.0;

        for (int n = 0; n < h.blockSize; ++n)
        {
            const auto sn = static_cast<size_t> (n);
            worst = std::max (worst, std::abs (static_cast<double> (h.left[sn])
                                               - static_cast<double> (h.right[sn])));
        }

        return worst;
    };

    const double narrowed = measure (0.0f);     // width retargeted to mono during bypass
    const double reference = measure (100.0f);  // width left alone, so the metric can be large

    check ("E6. bypass advances WetChain",
           narrowed <= 0.0 && reference > 1.0e-3,
           fmt ("max |L-R| in the first un-bypassed block: width->0 %.3e  width held at 100%% %.5f",
                narrowed, reference));
}

//==================================================================================================
// E7. Freeze holds the late tail at ANY erMix, and admits no new input.
//
// The other half of the CTO's Freeze ruling. At erMix = 100 % the wet path IS the early cluster, so
// gating the cluster alone made Freeze decay to silence ~200 ms after the user asked for an
// infinite pad; ReverbEngine::setParameters therefore steers the blend to late-only while frozen,
// and the held tank is heard at full level whatever erMix says.
//
// Two assertions on one pair of renders, because each half alone is satisfiable by something
// broken -- a silent engine "admits no new input" perfectly:
//
//   - NO NEW INPUT, exactly. Both renders charge on the same noise, freeze, then sit through 0.3 s
//     of silence so every 50 ms ramp settles. Only then does the input diverge: one gets a
//     full-scale 5 kHz tone, the other silence. From that point the two outputs must be
//     BIT-IDENTICAL -- a frozen engine's output cannot depend on its input at all. This replaced a
//     single-bin DFT at the probe frequency, which could not do the job: the held tail is
//     broadband, so its own energy in a 2 Hz bin (2.8e-3) sits only 35 dB below the tone and puts a
//     floor under the measurement, capping the achievable ratio at ~2e-2. Bit-identity has no floor.
//   - LEVEL HOLDS: rms 0.5 s after freeze vs 3.5 s after, within 1 dB, and audible. Without the
//     blend override this collapses toward silence and the drift figure stops meaning anything,
//     which is why the audibility floor is asserted too.
//
// The tone's amplitude in the un-frozen render is printed, so "the tone would otherwise be plainly
// there" is visible rather than assumed.
//==================================================================================================
void testFreezeHoldsLateAtFullErMix()
{
    constexpr double chargeSeconds = 0.5;
    constexpr double settleSeconds = 0.3;
    constexpr double totalSeconds  = 4.5;
    constexpr double probeHz       = 5000.0;

    constexpr int chargeBlocks  = static_cast<int> (chargeSeconds * testSampleRate
                                                    / static_cast<double> (testBlockSize));
    constexpr int divergeBlocks = static_cast<int> ((chargeSeconds + settleSeconds) * testSampleRate
                                                    / static_cast<double> (testBlockSize));

    // freezeAfterCharge: engage Freeze once charged. toneAfterSettle: what the input becomes once
    // every ramp has settled -- the tone, or nothing.
    auto render = [] (bool freezeAfterCharge, bool toneAfterSettle)
    {
        std::mt19937 rng (0xF0EE2Eu);
        std::uniform_real_distribution<float> dist (-1.0f, 1.0f);

        return renderEngine (totalSeconds,
                             [&rng, &dist, toneAfterSettle] (Harness& h, int block)
                             {
                                 const double omega = 2.0 * juce::MathConstants<double>::pi
                                                      * probeHz / testSampleRate;

                                 for (int n = 0; n < h.blockSize; ++n)
                                 {
                                     float v = 0.0f;

                                     if (block < chargeBlocks)
                                     {
                                         v = dist (rng);
                                     }
                                     else if (block >= divergeBlocks && toneAfterSettle)
                                     {
                                         const auto index = static_cast<double> (block * h.blockSize + n);
                                         v = static_cast<float> (std::sin (omega * index));
                                     }

                                     h.left[static_cast<size_t> (n)]  = v;
                                     h.right[static_cast<size_t> (n)] = v;
                                 }
                             },
                             [freezeAfterCharge] (int block)
                             {
                                 Params p = erParams (100.0f, 100.0f, 60.0f);
                                 p.decaySeconds = 10.0f;
                                 p.freeze = freezeAfterCharge && block >= chargeBlocks;
                                 return p;
                             });
    };

    auto windowRms = [] (const Signal& s, double fromSec, double toSec)
    {
        const auto from = static_cast<size_t> (fromSec * testSampleRate);
        const auto to   = std::min (s.length(), static_cast<size_t> (toSec * testSampleRate));

        double sum = 0.0;
        size_t count = 0;

        for (size_t i = from; i < to; ++i)
        {
            sum += static_cast<double> (s.l[i]) * static_cast<double> (s.l[i]);
            sum += static_cast<double> (s.r[i]) * static_cast<double> (s.r[i]);
            count += 2;
        }

        return count > 0 ? std::sqrt (sum / static_cast<double> (count)) : 0.0;
    };

    /** Amplitude of the probeHz component over a window. Informational only. */
    auto probeAmplitude = [] (const Signal& s, double fromSec, double toSec)
    {
        const auto from = static_cast<size_t> (fromSec * testSampleRate);
        const auto to   = std::min (s.length(), static_cast<size_t> (toSec * testSampleRate));

        const double omega = 2.0 * juce::MathConstants<double>::pi * probeHz / testSampleRate;

        double re = 0.0, im = 0.0;
        size_t count = 0;

        for (size_t i = from; i < to; ++i)
        {
            const auto t = static_cast<double> (i);
            re += static_cast<double> (s.l[i]) * std::cos (omega * t);
            im += static_cast<double> (s.l[i]) * std::sin (omega * t);
            ++count;
        }

        return count > 0 ? 2.0 * std::sqrt (re * re + im * im) / static_cast<double> (count) : 0.0;
    };

    const Signal frozenWithTone  = render (true, true);
    const Signal frozenSilent    = render (true, false);
    const Signal runningWithTone = render (false, true);

    // Compared only from the point the inputs diverge, so neither the identical charge phase nor
    // the settling window can mask a difference.
    int mismatches = 0;
    {
        const auto from = static_cast<size_t> (divergeBlocks) * static_cast<size_t> (testBlockSize);
        const size_t to = std::min (frozenWithTone.length(), frozenSilent.length());

        for (size_t i = from; i < to; ++i)
        {
            if (std::memcmp (&frozenWithTone.l[i], &frozenSilent.l[i], sizeof (float)) != 0) ++mismatches;
            if (std::memcmp (&frozenWithTone.r[i], &frozenSilent.r[i], sizeof (float)) != 0) ++mismatches;
        }
    }

    const double early = windowRms (frozenWithTone, 1.0, 1.5);   // 0.5 s after freeze
    const double late  = windowRms (frozenWithTone, 4.0, 4.5);   // 3.5 s after freeze
    const double driftDb = 20.0 * std::log10 (std::max (late, 1.0e-12) / std::max (early, 1.0e-12));

    const double toneIfRunning = probeAmplitude (runningWithTone, 4.0, 4.5);

    check ("E7. freeze holds late at erMix=100",
           mismatches == 0 && early > 0.01 && std::abs (driftDb) < 1.0,
           fmt ("held rms %.5f -> %.5f, drift=%.2f dB", early, late, driftDb)
           + fmt ("  samples changed by a full-scale tone=%.0f  (same tone un-frozen: %.4f)",
                  static_cast<double> (mismatches), toneIfRunning));
}

//==================================================================================================
// Informational, deliberately NOT a check: what the ER stage does to the wet peak at the same
// worst-case corner ReverbEngineTests check 4b pins (200 % size, 30 s decay, fully wet, unity trim,
// full-scale broadband in), swept over erMix.
//
// This is the measurement EarlyReflections::erHeadroom is derived from, kept live so the constant
// cannot silently stop being enough: D4's gain law normalises sum (g_k^2) = 1, so the cluster
// carries its input's ENERGY, but its worst-case peak gain is sum |g_k| = 3.0 (+9.5 dB) and the
// early path has no injection normalisation to spend that on. Still printed rather than asserted
// because the ceiling that owns it is ReverbEngineTests checks 4/4b (not 2B's file), which do not
// randomise erMix -- step 3A extends them.
//==================================================================================================
void reportPeakVersusErMix()
{
    for (const double fs : { testSampleRate, 192000.0 })
    for (const float erMix : { 25.0f, 50.0f, 100.0f })
    {
        Harness h (fs, testBlockSize, 2);
        std::mt19937 rng (0xDEADu ^ static_cast<unsigned> (fs));

        Params p;                       // 4b's corner verbatim, erMix swept
        p.mix           = 100.0f;
        p.preDelayMs    = 0.0f;
        p.size          = 200.0f;
        p.decaySeconds  = 30.0f;
        p.dampingHz     = 20000.0f;
        p.lowCutHz      = 20.0f;
        p.bandwidthHz   = 20000.0f;
        p.diffusion     = 100.0f;
        p.modDepth      = 100.0f;
        p.modRateHz     = 5.0f;
        p.width         = 200.0f;
        p.outputGainDb  = 0.0f;
        p.freeze        = false;
        p.erMix         = erMix;
        p.erSizePercent = 100.0f;
        p.erSpread      = 60.0f;

        float peak = 0.0f;

        for (int b = 0; b < h.blocksFor (30.0); ++b)
        {
            h.fillNoise (rng, 1.0f);
            h.engine.setParameters (p);
            h.processBlock();
            peak = std::max (peak, h.peakAbs());
        }

        std::printf ("[INFO] %-34s  peak=%.3f (%.2f dBFS) guard trips=%.0f\n",
                     (std::string ("4b corner @ ") + fmt ("%.0f Hz", fs)
                      + fmt (", erMix=%.0f%%", static_cast<double> (erMix))).c_str(),
                     static_cast<double> (peak),
                     20.0 * std::log10 (std::max (static_cast<double> (peak), 1.0e-12)),
                     static_cast<double> (h.guardTrips));
        std::fflush (stdout);
    }
}

} // namespace

//==================================================================================================

#include "TestSuites.h"

// Entry point for the suite runner (tests/TestSuites.h). Returns the failure COUNT, not 0/1.
int runEarlyReflectionsTests()
{
    // TestHarness.h's failureCount is shared by every suite in this target (see its own doc
    // comment): reset it here so this suite's count neither inherits nor leaks.
    failureCount = 0;

    std::printf ("EarlyReflections tests\n");
    std::printf ("----------------------\n");

    testErMixZeroIsIdentity();
    testTapTimes();
    testSpreadDecorrelates();
    testSizeSweepIsSmooth();
    testFreezeGatesEarly();
    testBypassAdvancesWetChain();
    testFreezeHoldsLateAtFullErMix();
    reportPeakVersusErMix();

    std::printf ("----------------------\n");
    std::printf ("%s (%d failure%s)\n",
                 failureCount == 0 ? "ALL CHECKS PASSED" : "FAILURES",
                 failureCount,
                 failureCount == 1 ? "" : "s");

    return failureCount;
}
