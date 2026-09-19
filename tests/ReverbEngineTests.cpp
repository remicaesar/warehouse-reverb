/*
    Console test harness for reverb::ReverbEngine.

    Links only juce_dsp / juce_audio_basics: the DSP must never reach for a GUI module. Returns a
    non-zero exit code if any check fails.

    check() / fmt() / Harness / measurementParams() moved to tests/TestHarness.h in step 1a (PLAN-R2
    step 6); that header is FROZEN from this point on -- see its own doc comment.
*/

#include "TestHarness.h"

#include "dsp/DelayLineFrac.h"
#include "dsp/TempoSync.h"   // Division::count, for the crossfade sweep below
#include "dsp/FDNTank.h"
#include "dsp/ReverbEngine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace
{

//==================================================================================================
/** DIVERGENCE ceiling, +12 dBFS. Not a level specification -- read this before changing it.

    One name for it because three checks assert it: the stability sweep (4), the worst-case corner
    (4b) and freeze-rejects-input (5b). A ceiling that lives in three literals gets raised in two.

    WHAT IT IS FOR, and why it is not simply deleted. Divergence is caught first by
    Harness::guardTrips and by allFinite(), and those assertions are the real stability check --
    but ReverbEngine's own guard fires only on a NON-FINITE sample, so a loop gain a hair above 1
    stays perfectly finite for minutes while it climbs. This ceiling is what sees that. It is
    therefore a runaway detector, and it wants to be as low as a legitimate signal allows.

    WHY IT IS NOT 2.0 ANY MORE. 2.0 was a round-1 "hasn't blown up" proxy, and 4b's honest corner
    legs showed it rejecting correct behaviour rather than divergence: an independent 1,152-config
    factorial put 1,115 configs above 2.0, worst 7.455, with every one finite and zero guard trips.
    A reverb sums many independent echoes, so its output is more Gaussian than its input and MUST
    have a higher crest factor. That is not a defect; it is what a reverb is.

    WHY 4.0, from the measurement rather than from taste. At the worst reachable corner (4b's
    `true corner` leg, 48 kHz) the charged wet output is Gaussian with sigma = 0.578 (-4.8 dBFS), and
    the largest sample SEEN grows as sqrt(2 ln N): 2.944 = 5.09 sigma over 30 s, 3.180 = 5.50 sigma
    over 600 s, against 5.45 sigma predicted. It never converges -- so no absolute peak bound is a
    property of this DSP, only a statement about how long you looked. 4.0 is 6.92 sigma there:
    2.0 dB above the longest run measured, and 0.075 expected exceedances in a 24-hour two-channel
    soak (4.5e-12 per sample). Divergence, being exponential, blows through it in seconds. Anything
    tighter measures observation length; anything looser is slower to see a runaway.
*/
constexpr float divergenceCeilingLinear = 4.0f;

//==================================================================================================
// 0. DelayLineFrac never reads outside its buffer - read() AND readLagrange().
//
// Regression test. read() used to compute the read position as a float and wrap it by adding size,
// which for a delay a hair above writeIndex rounds up to exactly size and indexes one past the end
// of the vector. The returned heap garbage then circulates in the FDN forever.
//
// The accept window is NOT [0, 1], and the whole buffer is written before the sweep starts. A read
// one past the end of a std::vector<float> most commonly returns 0.0f - freshly mapped heap pages
// are zero - so a window that includes 0.0f, or a buffer with any slot still holding its initial
// 0.0f, lets exactly the bug this test exists to catch read as a legitimate sample.
//
// readLagrange() needs its OWN, WIDER window, and deriving it is the point of the arithmetic below.
// A Lagrange kernel is not a convex combination of its taps: its coefficients sum to 1 but some of
// them are negative, so the result can overshoot the range of the samples it reads. With taps in
// [lo, hi] the reachable range is
//
//     [ sum_{c<0} c*hi + sum_{c>0} c*lo ,  sum_{c>0} c*hi + sum_{c<0} c*lo ]
//
// which for the 5-point kernel is worst at frac = 0.5, where sum |c| = 1.390625. For lo = 0.25,
// hi = 0.7448 that gives [0.15335, 0.84149]. The window below is that range, rounded outwards -
// still emphatically excluding 0.0f, which is the property that makes an out-of-bounds read a
// failure rather than a plausible sample. It is also why check 0b exists: a window this wide can no
// longer see a misindexed OUTER tap on its own.
//==================================================================================================

// Distinct, strictly in-range values so garbage cannot masquerade as a real sample. Every value
// lands in [0.25, 0.7448], and a linear interpolation between any two stays there.
float boundsSampleValue (int i) { return 0.25f + 0.5f * static_cast<float> (i % 97) / 97.0f; }

void testDelayLineBounds()
{
    constexpr int lineSize = 6914;   // the size that first tripped this

    constexpr float lowest  = 0.25f - 1.0e-6f;
    constexpr float highest = 0.75f + 1.0e-6f;

    // See the derivation in the comment above: 0.15335 .. 0.84149, rounded outwards.
    constexpr float lagLowest  = 0.153f;
    constexpr float lagHighest = 0.842f;

    reverb::DelayLineFrac line;
    line.prepare (lineSize);

    // Leave no slot at 0.0f, so 0.0f is a violation rather than a plausible reading.
    for (int i = 0; i < lineSize; ++i)
        line.push (boundsSampleValue (i));

    double worst = 0.0, worstLag = 0.0;
    int    violations = 0, lagViolations = 0;

    for (int written = 0; written < lineSize + 8; ++written)
    {
        line.push (boundsSampleValue (written));

        // Sweep whole and fractional delays, including the boundaries and just past them. The extra
        // small entries and the lineSize - 5 .. lineSize - 3 entries are the boundaries readLagrange()
        // adds on top of read()'s: its own minimum is 3 rather than 1, and its own maximum is
        // size - 3 rather than size - 2, so read()'s upper bound is now an OVER-long delay that must
        // come back clamped rather than wrapped.
        // THE LIST AS IT ORIGINALLY STOOD WAS NOT ENOUGH, and the reason is worth stating because
        // it already looked exhaustive. The float wrap fires only when (writeIndex - delay) is a
        // TINY NEGATIVE that rounds up to exactly size when size is added, i.e. when the delay sits
        // a hair ABOVE writeIndex. The `written + 0.0001f` entry below was written for that case and
        // misses it by exactly one: after the priming loop writeIndex is (written + 1), so that
        // entry probes a hair above written and leaves (writeIndex - delay) at ~0.9999, safely
        // positive. Measured on this line size: the float-wrap formulation produces the
        // out-of-bounds index in 98408 places in the (writeIndex, delay) plane, and the original
        // list reached exactly 0 of them - so reinstating the bug left this check GREEN while it
        // blew the tank up to 279 dBFS elsewhere.
        //
        // So the list keeps the boundaries it was built for and gains the `written + 1.0001f`
        // entry, which is the one that lands in the failing region: at writeIndex == written + 1 it
        // leaves (writeIndex - delay) at -0.0001, exactly the tiny negative that the float wrap
        // rounds up to size. Reinstating the bug turns this check red on that entry alone.
        for (const float delay : { 0.0f, -1.0f, 0.5f, 1.0f, 1.5f, 2.0f, 2.5f, 3.0f, 3.5f, 4.5f,
                                   static_cast<float> (written) + 0.0001f,
                                   static_cast<float> (written) + 1.0001f,
                                   static_cast<float> (lineSize - 5) + 0.5f,
                                   static_cast<float> (lineSize - 4),
                                   static_cast<float> (lineSize - 4) + 0.5f,
                                   static_cast<float> (lineSize - 3) + 0.75f,
                                   static_cast<float> (lineSize - 2),
                                   static_cast<float> (lineSize - 1),
                                   static_cast<float> (lineSize),
                                   static_cast<float> (lineSize) * 4.0f,
                                   std::numeric_limits<float>::quiet_NaN(),
                                   std::numeric_limits<float>::infinity() })
        {
            const float value = line.read (delay);

            if (! std::isfinite (value) || value < lowest || value > highest)
            {
                ++violations;
                worst = std::max (worst, std::abs (static_cast<double> (value)));
            }

            const float lag = line.readLagrange (delay);

            if (! std::isfinite (lag) || lag < lagLowest || lag > lagHighest)
            {
                ++lagViolations;
                worstLag = std::max (worstLag, std::abs (static_cast<double> (lag)));
            }
        }
    }

    check ("0. delay line stays in bounds",
           violations == 0 && lagViolations == 0,
           fmt ("read() outside [0.25,0.75]=%.0f worst=%.4g", static_cast<double> (violations), worst)
           + fmt ("  readLagrange() outside [0.153,0.842]=%.0f worst=%.4g",
                  static_cast<double> (lagViolations), worstLag));
}

//==================================================================================================
// 0b. readLagrange() reads the samples it claims to read, at the delay it claims to honour.
//
// The range check in check 0 is necessary but not sufficient, and the gap matters. The kernel's
// outermost coefficient peaks at 1/24, so a tap that lands on the wrong slot - or one past the end
// and comes back 0.0f - moves the result by about 2% of the sample range, far inside the
// [0.153, 0.842] window check 0 has to allow. A window alone therefore catches a misindexed INNER
// tap and misses a misindexed OUTER one, which is precisely the half of the window that got wider.
// Mutation-checked: widening the fast path by one slot past the end of the buffer leaves check 0
// green and turns this one red.
//
// Two legs, because two different things can be wrong:
//
//   1. WHICH SAMPLES. For delays strictly inside the honoured range, compare against the Lagrange
//      basis evaluated in double precision over the pushed sequence, which is known independently
//      of the delay line (the sample j steps ago is the j-th value back in the push history). No
//      clamping is involved, so this leg says nothing about the clamp.
//
//   2. WHICH DELAY. For delays outside the honoured range - too short, too long, NaN, infinite -
//      the result must equal the kernel at the delay DelayLineFrac documents it will fall back to:
//      clamped into [minLagrangeDelay, getMaxDelayLagrange()], with anything not greater than the
//      lower bound (NaN included) landing on that lower bound. This leg restates the PUBLIC
//      contract, not the private expression that implements it, so widening the internal clamp
//      without moving the accessor turns it red - which is the mutation leg 1 cannot see, because
//      leg 1 never probes past the accessor.
//==================================================================================================
void testDelayLineLagrangeReference()
{
    constexpr int lineSize = 6914;

    reverb::DelayLineFrac line;
    line.prepare (lineSize);

    // Every value ever pushed, in order, recorded outside the line. The sample j steps ago is
    // history[history.size() - j] - a definition that owes nothing to DelayLineFrac. Keeping the
    // whole history rather than recomputing the pattern modulo the buffer length is deliberate: the
    // modular version silently goes wrong once the sweep laps the buffer, which is exactly where the
    // interesting write positions are.
    std::vector<float> history;
    history.reserve (static_cast<size_t> (2 * lineSize + 16));

    auto pushAndRecord = [&line, &history] (float v)
    {
        line.push (v);
        history.push_back (v);
    };

    auto pushed = [&history] (long long j)   // j steps ago, j >= 1
    {
        return static_cast<double> (history[history.size() - static_cast<size_t> (j)]);
    };

    // `static` is what lets the lambdas below use this without capturing it. MSVC rejects an
    // implicit read of a non-static constexpr local inside a lambda (C3493); clang and GCC
    // accept it, and reject the obvious fixes in turn -- an explicit capture draws
    // -Wunused-lambda-capture and an init-capture draws -Wshadow-uncaptured-local. Static
    // storage duration sidesteps all three: nothing is captured on any compiler. This file
    // had never been compiled by MSVC, because the Windows build died at CMake configure
    // long before reaching a compiler.
    static constexpr int width = reverb::DelayLineFrac::kernelWidth;
    static constexpr int first = -reverb::DelayLineFrac::lagrangeAhead;

    // The Lagrange basis over the taps at (whole + first) .. (whole + first + width - 1) samples ago,
    // built from its definition rather than from any expanded form the implementation might use.
    auto reference = [&pushed] (int whole, double frac)
    {
        double sum = 0.0;

        for (int k = 0; k < width; ++k)
        {
            double basis = 1.0;

            for (int j = 0; j < width; ++j)
                if (j != k)
                    basis *= (frac - (first + j)) / static_cast<double> (k - j);

            sum += basis * pushed (whole + first + k);
        }

        return sum;
    };

    for (int i = 0; i < lineSize; ++i)
        pushAndRecord (boundsSampleValue (i));

    const int maxDelay = line.getMaxDelayLagrange();
    const int minDelay = reverb::DelayLineFrac::minLagrangeDelay;

    double worstInside = 0.0, worstClamped = 0.0;
    int    comparedInside = 0, comparedClamped = 0;

    // Every write position, not a sample of them: readLagrange() has a fast path for the case where
    // its taps do not straddle the buffer wrap and a separate one for the (kernelWidth - 1) positions
    // out of `size` where they do. A short sweep would exercise the fast path only.
    for (int step = 0; step < lineSize + 8; ++step)
    {
        pushAndRecord (boundsSampleValue (step));

        // ---- leg 1: which samples ----
        //
        // Spread the probes over the whole honoured range, both ends included, and over the whole
        // fractional range - the wrap of the sweep pattern (a 0.5 jump every 97 samples) guarantees
        // that some probes straddle a large step, where a one-slot error is unmissable.
        for (const int whole : { minDelay, minDelay + 1, 97, 193, 1531, 4096,
                                 maxDelay - 1, maxDelay })
        {
            for (const double frac : { 0.0, 0.125, 0.5, 0.5 + 1.0 / 97.0, 0.875 })
            {
                if (static_cast<double> (whole) + frac > static_cast<double> (maxDelay))
                    continue;

                const float d = static_cast<float> (static_cast<double> (whole) + frac);

                // The fraction the line ACTUALLY sees. A float cannot hold whole + frac exactly at
                // these magnitudes - at whole = 4096 the spacing is 4.9e-4 - so the reference has to
                // read the fraction back out of the float rather than use the double it started
                // from, or it is comparing two different interpolation points and the residual is
                // 1e-4 rather than 1e-7. That is about the float ARGUMENT, not about the clamp.
                const double f = static_cast<double> (d) - static_cast<double> (whole);

                worstInside = std::max (worstInside,
                                        std::abs (static_cast<double> (line.readLagrange (d))
                                                  - reference (whole, f)));
                ++comparedInside;
            }
        }

        // ---- leg 2: which delay ----
        //
        // Sample this leg rather than running it at every write position: it is the same arithmetic
        // 8 times over and the sweep still visits every position modulo 7.
        if (step % 7 != 0)
            continue;

        for (const float requested : { -1.0f, 0.0f, 1.0f, 2.0f, 2.5f,
                                       static_cast<float> (maxDelay) + 0.5f,
                                       static_cast<float> (maxDelay) + 1.0f,
                                       static_cast<float> (lineSize),
                                       static_cast<float> (lineSize) * 4.0f,
                                       std::numeric_limits<float>::quiet_NaN(),
                                       std::numeric_limits<float>::infinity() })
        {
            // DelayLineFrac's documented fallback, in words: clamp into [minLagrangeDelay,
            // getMaxDelayLagrange()], and anything NOT GREATER than the lower bound - which is how
            // NaN gets here - lands on the lower bound.
            const double clamped = ! (requested > static_cast<float> (minDelay))
                                     ? static_cast<double> (minDelay)
                                     : std::min (static_cast<double> (requested),
                                                 static_cast<double> (maxDelay));

            // Every probe above clamps to a whole number of samples, so frac is exactly 0 and the
            // reference collapses to the single stored sample at `clamped` steps ago.
            worstClamped = std::max (worstClamped,
                                     std::abs (static_cast<double> (line.readLagrange (requested))
                                               - reference (static_cast<int> (clamped), 0.0)));
            ++comparedClamped;
        }
    }

    // Float accumulation over five products on values around 0.5, and the implementation evaluates
    // the polynomial form rather than the basis form, so the two differ by rounding only. 1e-6 is
    // still two orders of magnitude below the smallest error a one-slot misindex can produce (1/24 of
    // the 0.00515 step between adjacent sweep values, i.e. 2.1e-4).
    check ("0b. lagrange read matches the kernel",
           worstInside < 1.0e-6 && worstClamped < 1.0e-6
               && comparedInside > 0 && comparedClamped > 0,
           fmt ("inside range: probes=%.0f worst=%.3g",
                static_cast<double> (comparedInside), worstInside)
           + fmt ("  clamped: probes=%.0f worst=%.3g",
                  static_cast<double> (comparedClamped), worstClamped));
}

//==================================================================================================
// 1. Silence in -> silence out, nothing non-finite.
//==================================================================================================
void testSilence()
{
    Harness h (48000.0, 512, 2);

    auto p = measurementParams (3.0f);
    h.engine.setParameters (p);

    float worstPeak = 0.0f;
    bool  finite = true;

    for (int b = 0; b < h.blocksFor (2.0); ++b)
    {
        h.fillSilence();
        h.engine.setParameters (p);
        h.processBlock();

        worstPeak = std::max (worstPeak, h.peakAbs());
        finite = finite && h.allFinite();
    }

    check ("1. silence in -> silence out",
           finite && worstPeak <= 0.0f,
           fmt ("peak=%.3e finite=%.0f", static_cast<double> (worstPeak), finite ? 1.0 : 0.0));
}

//==================================================================================================
// 2. mix = 0 must be bit-identical to the input.
//
// Two halves. The value check below (ordinary noise) is necessary but not sufficient: deleting the
// whole `mixIsZero && outIsUnity` early return from ReverbEngine::processChunk leaves it green,
// because at m == 0 exactly, `x * (1 - m) + wl * m` is `x + (+/-0.0)` and then `* 1.0f`, which is
// bit-exact for every value a uniform distribution produces.
//
// So the second half observes the branch itself rather than its value. -0.0f is the one input the
// two paths disagree on: the arithmetic turns `-0.0f + 0.0f` into `+0.0f`, while the bypass leaves
// the sign bit alone. The tank is charged first so the wet samples have mixed signs - `wl * 0.0f`
// carries wl's sign, and a wet sample that happens to be negative would preserve -0.0f even on the
// arithmetic path, so the verdict is taken over a whole block rather than one sample. Denormals go
// through the same check: they do not discriminate between the paths today (no FTZ in this
// harness), but they pin the claim that the dry path never scales the input.
//==================================================================================================
void testDryBitIdentical()
{
    Harness h (48000.0, 512, 2);

    auto p = measurementParams (2.5f);
    p.mix = 0.0f;
    p.outputGainDb = 0.0f;
    h.engine.setParameters (p);

    std::mt19937 rng (0x5EED1234u);

    std::vector<float> refL (static_cast<size_t> (h.blockSize));
    std::vector<float> refR (static_cast<size_t> (h.blockSize));

    int mismatches = 0;

    for (int b = 0; b < h.blocksFor (2.0); ++b)
    {
        h.fillNoise (rng, 0.9f);
        refL = h.left;
        refR = h.right;

        h.engine.setParameters (p);
        h.processBlock();

        for (int n = 0; n < h.blockSize; ++n)
        {
            const auto sn = static_cast<size_t> (n);

            // Exact comparison: the dry path must not have been multiplied or filtered at all.
            if (std::memcmp (&h.left[sn],  &refL[sn], sizeof (float)) != 0) ++mismatches;
            if (std::memcmp (&h.right[sn], &refR[sn], sizeof (float)) != 0) ++mismatches;
        }
    }

    // ---- branch observation: -0.0f and a denormal, fed after the tank is charged ----------------
    constexpr float negZero  = -0.0f;
    const     float denormal = std::numeric_limits<float>::denorm_min();

    int signFlips = 0;
    int denormalMismatches = 0;

    for (int b = 0; b < 8; ++b)
    {
        for (int n = 0; n < h.blockSize; ++n)
        {
            const auto sn = static_cast<size_t> (n);
            const float v = (n % 2 == 0) ? negZero : denormal;
            h.left[sn]  = v;
            h.right[sn] = v;
        }

        h.engine.setParameters (p);
        h.processBlock();

        for (int n = 0; n < h.blockSize; ++n)
        {
            const auto sn = static_cast<size_t> (n);
            const float expected = (n % 2 == 0) ? negZero : denormal;

            for (const float* got : { &h.left[sn], &h.right[sn] })
            {
                if (std::memcmp (got, &expected, sizeof (float)) == 0)
                    continue;

                if (n % 2 == 0)
                    ++signFlips;
                else
                    ++denormalMismatches;
            }
        }
    }

    check ("2. mix=0 bit-identical",
           mismatches == 0 && signFlips == 0 && denormalMismatches == 0,
           fmt ("mismatched noise samples=%.0f  -0.0f sign flips=%.0f  denormal changes=%.0f",
                static_cast<double> (mismatches),
                static_cast<double> (signFlips),
                static_cast<double> (denormalMismatches)));
}

//==================================================================================================
// 3. RT60 from the impulse response.
//
// Method: Schroeder backward energy integration of the impulse response, converted to dB relative
// to the total energy, then a least-squares line fitted over the -5 dB .. -35 dB span (the standard
// T30 window, which avoids both the direct-sound transient and the truncated tail). RT60 is the
// extrapolation of that slope to -60 dB.
//==================================================================================================
double measureRt60 (double sampleRate, float decaySeconds)
{
    const int blockSize = 512;
    Harness h (sampleRate, blockSize, 2);

    auto p = measurementParams (decaySeconds);
    h.engine.setParameters (p);

    const int totalBlocks = h.blocksFor (static_cast<double> (decaySeconds) * 2.5 + 0.5);

    std::vector<double> impulseResponse;
    impulseResponse.reserve (static_cast<size_t> (totalBlocks * blockSize));

    for (int b = 0; b < totalBlocks; ++b)
    {
        h.fillSilence();

        if (b == 0)
        {
            h.left[0]  = 1.0f;
            h.right[0] = 1.0f;
        }

        h.engine.setParameters (p);
        h.processBlock();

        for (int n = 0; n < blockSize; ++n)
            impulseResponse.push_back (static_cast<double> (h.left[static_cast<size_t> (n)]));
    }

    // Backward cumulative energy.
    const size_t length = impulseResponse.size();
    std::vector<double> edc (length, 0.0);
    double running = 0.0;

    for (size_t i = length; i-- > 0;)
    {
        running += impulseResponse[i] * impulseResponse[i];
        edc[i] = running;
    }

    if (edc[0] <= 0.0)
        return 0.0;

    const double reference = edc[0];

    // Least squares over the -5 .. -35 dB window.
    double sumX = 0.0, sumY = 0.0, sumXX = 0.0, sumXY = 0.0;
    int    count = 0;

    for (size_t i = 0; i < length; ++i)
    {
        if (edc[i] <= 0.0)
            break;

        const double db = 10.0 * std::log10 (edc[i] / reference);

        if (db > -5.0)
            continue;

        if (db < -35.0)
            break;

        const double t = static_cast<double> (i) / sampleRate;

        sumX  += t;
        sumY  += db;
        sumXX += t * t;
        sumXY += t * db;
        ++count;
    }

    if (count < 16)
        return 0.0;

    const double n     = static_cast<double> (count);
    const double denom = n * sumXX - sumX * sumX;

    if (std::abs (denom) < 1.0e-18)
        return 0.0;

    const double slope = (n * sumXY - sumX * sumY) / denom;   // dB per second

    if (slope >= -1.0e-9)
        return 0.0;

    return -60.0 / slope;
}

void testRt60()
{
    for (const float requested : { 1.0f, 4.0f })
    {
        const double measured = measureRt60 (48000.0, requested);
        const double error    = std::abs (measured - static_cast<double> (requested))
                                / static_cast<double> (requested);

        const std::string name = "3. RT60 @ decay=" + fmt ("%.1f s", static_cast<double> (requested));

        // 15%, tightened from 30% in step 2A. The fold removed the two loss terms the looser
        // tolerance had to absorb: with the per-line damping lowpass and low-cut highpass gone from
        // the feedback path, the only thing setting the broadband rate is the attenuation filter's
        // mid gain, and the measured error dropped from 5.9%/8.5% to 0.3%/0.1%. PLAN-R2 7.1 T2.
        check (name.c_str(),
               error <= 0.15,
               fmt ("measured=%.3f s  error=%.1f%%", measured, error * 100.0));
    }
}

//==================================================================================================
// 4. Stability sweep: full-range random parameters, every block, at four sample rates.
//==================================================================================================
void testStabilitySweep()
{
    // Full-scale noise. The FDN is linear for fixed parameters, so the input level cannot change a
    // stability verdict; it only sets the headroom the +6 dBFS ceiling is measured against.
    constexpr float noiseAmplitude = 1.0f;

    // A single seed proves very little here: a marginally unstable FDN diverges on some parameter
    // trajectories and not others. Set REVERB_SWEEP_SEEDS higher for a deeper stress run.
    int numSeeds = 4;

    if (const char* override = std::getenv ("REVERB_SWEEP_SEEDS"))
        numSeeds = std::max (1, std::atoi (override));

    for (const double sampleRate : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        float worstPeak = 0.0f;
        bool  finite = true;
        int   totalTrips = 0;
        int   worstSeed = -1;
        int   freezeBlocks = 0;

        for (int seed = 0; seed < numSeeds; ++seed)
        {
            Harness h (sampleRate, 512, 2);

            std::mt19937 rng (0xC0FFEEu ^ static_cast<unsigned> (sampleRate)
                              ^ (static_cast<unsigned> (seed) * 2654435761u));

            std::uniform_real_distribution<float> unit (0.0f, 1.0f);

            auto pick = [&] (float lo, float hi) { return lo + unit (rng) * (hi - lo); };

            reverb::ReverbEngine::Params p;
            h.engine.setParameters (p);

            float peak = 0.0f;

            const int totalBlocks = h.blocksFor (30.0);

            for (int b = 0; b < totalBlocks; ++b)
            {
                p.mix          = pick (0.0f, 100.0f);
                p.preDelayMs   = pick (0.0f, 500.0f);
                p.size         = pick (10.0f, 200.0f);
                p.decaySeconds = pick (0.1f, 30.0f);
                p.dampingHz    = pick (500.0f, 20000.0f);
                p.lowCutHz     = pick (20.0f, 1000.0f);
                p.bandwidthHz  = pick (1000.0f, 20000.0f);
                p.diffusion    = pick (0.0f, 100.0f);
                p.modDepth     = pick (0.0f, 100.0f);
                p.modRateHz    = pick (0.01f, 5.0f);
                p.width        = pick (0.0f, 200.0f);
                // Output trim is swept over its attenuating half only: +12 dB of deliberate makeup
                // gain would break any absolute ceiling by construction and says nothing about the
                // FDN.
                p.outputGainDb = pick (-24.0f, 0.0f);
                p.freeze       = unit (rng) < 0.15f;
                // erMix ADDED IN 3A, on the CTO's ruling. Its absence was a real blind spot rather
                // than an omission: the early cluster's peak gain is sum|g_k|, not sqrt(sum g_k^2),
                // so it reached +9.6 dBFS at erMix = 100 and no check in this file could see it,
                // because this sweep left erMix at its default and 4b pinned it there. The defect it
                // hid is fixed (EarlyReflections' erHeadroom); the blind spot is closed here.
                p.erMix        = pick (0.0f, 100.0f);

                // duckAmount and preDelayDivision ADDED BY THE CTO after step 5A, which found the
                // same shape of blind spot builder-2B found for erMix: this sweep left duckAmount at
                // 0 and never varied the division, so neither the ducker nor the pre-delay crossfade
                // was exercised anywhere in a randomised 30 s run at four sample rates.
                //
                // The ducker itself cannot raise a peak (its gain is always in (0, 1]), so the bound
                // is not what this buys. The CROSSFADE is: a division or sync change takes a SECOND
                // simultaneous read from the pre-delay line over 30 ms, and this project has already
                // shipped one out-of-bounds delay read. An unswept second read is exactly the kind of
                // thing that is fine until it is not.
                p.duckAmount   = pick (0.0f, 100.0f);
                p.duckThresholdDb = pick (-60.0f, 0.0f);
                p.duckReleaseMs   = pick (20.0f, 1000.0f);
                p.duckCharacter = unit (rng) < 0.5f ? 0 : 1;

                // Toggling sync and stepping the division at random is what puts the crossfade under
                // test -- a change every few blocks means fades interrupting fades.
                p.preDelaySync     = unit (rng) < 0.5f;
                p.preDelayDivision = static_cast<int> (unit (rng)
                    * static_cast<float> (reverb::temposync::Division::count));

                if (p.freeze)
                    ++freezeBlocks;

                h.fillNoise (rng, noiseAmplitude);
                h.engine.setParameters (p);
                h.processBlock();

                finite = finite && h.allFinite();
                peak   = std::max (peak, h.peakAbs());
            }

            totalTrips += h.guardTrips;

            if (peak > worstPeak)
            {
                worstPeak = peak;
                worstSeed = seed;
            }
        }

        const std::string name = "4. stability @ " + fmt ("%.0f Hz", sampleRate);

        check (name.c_str(),
               finite && worstPeak <= divergenceCeilingLinear && totalTrips == 0,
               fmt ("worst peak=%.3f (%.2f dBFS) @ seed %.0f",
                    static_cast<double> (worstPeak),
                    20.0 * std::log10 (std::max (static_cast<double> (worstPeak), 1.0e-12)),
                    static_cast<double> (worstSeed))
               + fmt ("  seeds=%.0f  frozen blocks=%.0f  guard trips=%.0f",
                      static_cast<double> (numSeeds),
                      static_cast<double> (freezeBlocks),
                      static_cast<double> (totalTrips)));
    }
}

//==================================================================================================
// 4b. Worst-case corner, held long enough for every smoother to settle.
//
// Randomising every block means the 50 ms smoothers never reach the ends of their ranges, so this
// pass pins the most energetic corner instead: longest decay, largest size, widest bandwidth, no
// damping, fully wet, unity trim.
//
// WHY THERE IS A LEG TABLE. Until now this check built its corner from a bare
// reverb::ReverbEngine::Params and set only the fields it meant to push, which silently left four
// peak-REMOVING defaults standing -- decayLowMult 1.4 / decayHighMult 0.7 (the high band is most of
// the spectrum, so shortening its decay is the largest of the four), density 45 (the nested
// allpasses spread the energy in time), bassMonoHz 130 (a monoed bass is that much side energy
// gone), erMix 25 (the early cluster displaces late level) -- and it PINNED modDepth to 100, which
// removes peak as well, because modulation spreads the modes. The figure this file has been quoting
// as the project's headroom margin was therefore a mid-range configuration, not a corner. Every
// bound-relevant field is now named in the table below, so a leg cannot inherit one by omission
// again.
//
// The two historical legs carry exactly those old default values, written out, so their numbers stay
// comparable across the changes that produced them -- 1.411 before THE FOLD, 1.774 after the
// Lagrange interpolation fix removed the accidental peak limiting linear interpolation had been
// providing, 1.688 with 3A's sixteen lines. erMix 100% stays one of them because that is where the
// early cluster's peak gain (sum|g_k|, not sqrt(sum g_k^2)) lives; it measured +9.61 dBFS before
// EarlyReflections' erHeadroom constant existed, i.e. it would have shipped a configuration this
// bound rejects.
//
// The two `true corner` legs are the corner the divergence ceiling is actually about. The 4.0/4.0
// leg exists because the band multipliers do not cancel out of the level: the injection
// normalisation in FDNTank::setParameters is derived from the band-energy-equivalent per-pass gain
// precisely so that a multiplier above 1.0 lengthens the band decay without also adding level, and
// this leg is what measures that. Reverting that normalisation to the mid-band gain alone takes it
// from 2.669 to 3.723 at 48 kHz and from 1.674 to 2.471 at 192 kHz -- it is the leg that moves.
//
// WHY THE CORNER LEGS RUN LONGER THAN THE HISTORICAL ONES. 30 s at a 30 s decay does not charge the
// tank, so a 30 s figure is a lower bound on the corner rather than the corner. Measured in 30 s
// windows out to 600 s, the window RMS settles by the SECOND window and is then flat to +/-0.06%:
// the 0..30 s window reads 0.34 dB low on the flat leg and 0.68 dB low at multipliers 4.0. 90 s is
// three times the decay and half again the measured charge time.
//
// What does NOT converge is the largest sample seen, and no run length fixes that: the charged
// output is Gaussian, so its maximum grows as sqrt(2 ln N) forever -- 5.09 sigma over 30 s, 5.50
// sigma over 600 s on the flat leg. That is the whole reason the assertion is a divergence ceiling
// with margin rather than a level bound; see divergenceCeilingLinear.
//
// THE LEVEL INVARIANT, and why the ceiling is not the regression test. divergenceCeilingLinear sits
// at 4.0 because that is where a runaway detector belongs (see its comment), and at 4.0 it is far
// too loose to notice the normalisation defect this leg set was written for. Measured with the fix
// reverted -- the injection derived from the mid-band gain alone -- the 4.0/4.0 leg peaks at 3.942
// at 48 kHz and 2.472 at 192 kHz: both wrong, both under 4.0, both green.
//
// So the defect is asserted directly instead, as the invariant it actually is: raising a decay
// multiplier must not raise the wet LEVEL. The 4.0/4.0 leg's charged RMS is bounded by the flat
// corner's, and that assertion reads +2.17 dB (48 kHz) / +2.94 dB (192 kHz) with the fix reverted
// against -1.21 dB / -0.45 dB with it in place. RMS rather than peak because RMS is stationary to
// better than 0.1% over the window, so this assertion has no extreme-value noise in it at all --
// which is what lets its bound sit at exactly 0.00 dB, the invariant itself, with no tolerance.
//
// The +12 dB tilt legs are INFO and deliberately unasserted: a +/-12 dB EQ breaks any absolute
// output ceiling by construction, which is exactly why `output` is swept only over -24..0 dB.
// Printing them makes that exclusion a documented choice rather than an absence.
//==================================================================================================
struct CornerLeg
{
    const char* label;
    float decayLowMult, decayHighMult;
    float density, bassMonoHz, erMix, modDepth, wetTiltDb;
    double seconds;
    bool  asserted;

    // The level invariant, asserted separately from the ceiling -- see the LEVEL INVARIANT note
    // above. Exactly one leg per sample rate sets isLevelReference; a leg with a non-zero
    // maxLevelVsReference asserts its charged RMS against that reference and must appear after it
    // in the table.
    bool  isLevelReference;
    float maxLevelVsReference;
};

void testWorstCaseCorner()
{
    static constexpr CornerLeg legs[] =
    {
        // 30 s on the historical legs is deliberate: it is the duration their published figures were
        // measured over, and changing it would break the comparison they exist for.
        //
        // label                        decay mults  density   bassmono   erMix    mod    tilt  secs  assert  ref   vs ref
        { "historical, erMix 25%",      1.4f, 0.7f,    45.0f,    130.0f,   25.0f, 100.0f,  0.0f, 30.0, true,   false, 0.0f },
        { "historical, erMix 100%",     1.4f, 0.7f,    45.0f,    130.0f,  100.0f, 100.0f,  0.0f, 30.0, true,   false, 0.0f },
        { "true corner",                1.0f, 1.0f,     0.0f,      0.0f,    0.0f,   0.0f,  0.0f, 90.0, true,   true,  0.0f },
        { "true corner, mults 4.0",     4.0f, 4.0f,     0.0f,      0.0f,    0.0f,   0.0f,  0.0f, 90.0, true,   false, 1.0f },
        { "true corner, wettilt +12",   1.0f, 1.0f,     0.0f,      0.0f,    0.0f,   0.0f, 12.0f, 90.0, false,  false, 0.0f },
        { "mults 4.0, wettilt +12",     4.0f, 4.0f,     0.0f,      0.0f,    0.0f,   0.0f, 12.0f, 90.0, false,  false, 0.0f },
    };

    for (const double sampleRate : { 48000.0, 192000.0 })
    {
        double referenceRms = 0.0;

        for (const auto& leg : legs)
        {
            Harness h (sampleRate, 512, 2);
            std::mt19937 rng (0xDEADu ^ static_cast<unsigned> (sampleRate));

            reverb::ReverbEngine::Params p;
            p.mix           = 100.0f;
            p.preDelayMs    = 0.0f;
            p.size          = 200.0f;
            p.decaySeconds  = 30.0f;
            p.dampingHz     = 20000.0f;
            p.lowCutHz      = 20.0f;
            p.bandwidthHz   = 20000.0f;
            p.diffusion     = 100.0f;
            p.modRateHz     = 5.0f;
            p.width         = 200.0f;
            p.outputGainDb  = 0.0f;
            p.freeze        = false;

            p.decayLowMult  = leg.decayLowMult;
            p.decayHighMult = leg.decayHighMult;
            p.density       = leg.density;
            p.bassMonoHz    = leg.bassMonoHz;
            p.erMix         = leg.erMix;
            p.modDepth      = leg.modDepth;
            p.wetTiltDb     = leg.wetTiltDb;

            float  peak = 0.0f;
            bool   finite = true;
            double sumOfSquares = 0.0;
            int    rmsBlocks = 0;

            // The last third of the run is the only part guaranteed past the charge transient, so
            // the RMS -- the level fact, as opposed to the extreme-value one -- is taken from there.
            const int totalBlocks = h.blocksFor (leg.seconds);
            const int rmsFrom     = (totalBlocks * 2) / 3;

            for (int b = 0; b < totalBlocks; ++b)
            {
                h.fillNoise (rng, 1.0f);
                h.engine.setParameters (p);
                h.processBlock();

                finite = finite && h.allFinite();
                peak   = std::max (peak, h.peakAbs());

                if (b >= rmsFrom)
                {
                    const double rms = h.blockRms();
                    sumOfSquares += rms * rms;
                    ++rmsBlocks;
                }
            }

            const double chargedRms = rmsBlocks > 0
                                    ? std::sqrt (sumOfSquares / static_cast<double> (rmsBlocks))
                                    : 0.0;

            const std::string name = std::string ("4b. ") + leg.label + " @ "
                                     + fmt ("%.0f Hz", sampleRate);

            // Peak AND charged RMS, plus the peak in sigma. The crest figure is what makes a moved
            // peak readable: a level regression moves the RMS, while extreme-value luck moves only
            // the sigma multiple, and one number cannot tell those apart.
            const std::string measured =
                fmt ("peak=%.3f (%.2f dBFS) = %.2f sigma", static_cast<double> (peak),
                     20.0 * std::log10 (std::max (static_cast<double> (peak), 1.0e-12)),
                     static_cast<double> (peak) / std::max (chargedRms, 1.0e-12))
                + fmt ("  charged rms=%.4f (%.2f dBFS) over %.0f s", chargedRms,
                       20.0 * std::log10 (std::max (chargedRms, 1.0e-12)), leg.seconds)
                + fmt ("  guard trips=%.0f", static_cast<double> (h.guardTrips));

            if (leg.isLevelReference)
                referenceRms = chargedRms;

            // The level invariant. A leg carrying it fails if raising a decay multiplier raised the
            // wet level, whatever the peak did -- and it reads the RMS, which is stationary to
            // better than 0.1% over 30 s, rather than the peak, which is an extreme-value draw.
            const bool levelHeld = leg.maxLevelVsReference <= 0.0f
                                   || (referenceRms > 0.0
                                       && chargedRms <= referenceRms
                                                        * static_cast<double> (leg.maxLevelVsReference));

            const std::string full = leg.maxLevelVsReference > 0.0f
                                   ? measured + fmt ("  level vs flat corner=%+.2f dB (bound %+.2f)",
                                                     20.0 * std::log10 (std::max (chargedRms, 1.0e-12)
                                                                        / std::max (referenceRms, 1.0e-12)),
                                                     20.0 * std::log10 (static_cast<double> (leg.maxLevelVsReference)))
                                   : measured;

            if (leg.asserted)
                check (name.c_str(),
                       finite && peak <= divergenceCeilingLinear && h.guardTrips == 0 && levelHeld,
                       full);
            else
                std::printf ("[INFO] %-34s  %s\n", name.c_str(), full.c_str());
        }
    }
}

//==================================================================================================
// 5. Freeze holds: the tail must neither decay away nor grow.
//
// Run twice, and what makes the second run worth running changed with THE FOLD (PLAN-R2 7.1,
// "vacuity introduced by the fold"). It used to enter from damping 1 kHz / low cut 500 Hz, where a
// feedback path that kept filtering would strip the tail past -3 dB in a second. After the fold
// those two arguments are CROSSOVER FREQUENCIES, and with both decay multipliers at 1.0 the
// attenuation filter is flat wherever its crossovers sit -- so "dark filters" became bit-for-bit
// the same test as "open filters" while still printing two PASSes. Confirmed before the fix: both
// variants printed rms 0.11741 -> 0.11670, drift -0.05 dB, identical to five figures.
//
// The non-neutrality therefore moves onto the multipliers, which is where the fold put it: at
// decaylow 3.0 / decayhigh 0.1 the per-line filter is a deep two-shelf cut-and-boost, and a
// feedback path still running it while frozen cannot hold a tail.
//==================================================================================================
void testFreezeHolds (const char* label, float decayLowMult, float decayHighMult)
{
    Harness h (48000.0, 512, 2);

    auto p = measurementParams (4.0f);
    p.modDepth      = 25.0f;
    p.decayLowMult  = decayLowMult;
    p.decayHighMult = decayHighMult;
    h.engine.setParameters (p);

    std::mt19937 rng (0xF00Du);

    // Charge the tank.
    for (int b = 0; b < h.blocksFor (3.0); ++b)
    {
        h.fillNoise (rng, 0.5f);
        h.engine.setParameters (p);
        h.processBlock();
    }

    p.freeze = true;

    auto runSilence = [&] (double seconds)
    {
        for (int b = 0; b < h.blocksFor (seconds); ++b)
        {
            h.fillSilence();
            h.engine.setParameters (p);
            h.processBlock();
        }
    };

    auto measureRms = [&] (double seconds)
    {
        double sum = 0.0;
        int    blocks = 0;

        for (int b = 0; b < h.blocksFor (seconds); ++b)
        {
            h.fillSilence();
            h.engine.setParameters (p);
            h.processBlock();

            const double rms = h.blockRms();
            sum += rms * rms;
            ++blocks;
        }

        return blocks > 0 ? std::sqrt (sum / static_cast<double> (blocks)) : 0.0;
    };

    // Let the input-gain ramp reach zero before taking the reference.
    runSilence (0.5);
    const double referenceRms = measureRms (0.25);

    runSilence (5.0);
    const double laterRms = measureRms (0.25);

    const double drift = (referenceRms > 0.0 && laterRms > 0.0)
                       ? 20.0 * std::log10 (laterRms / referenceRms)
                       : -999.0;

    const std::string name = std::string ("5. freeze holds, ") + label;

    check (name.c_str(),
           std::abs (drift) <= 3.0,
           fmt ("rms %.5f -> %.5f, drift=%.2f dB", referenceRms, laterRms, drift));
}

//==================================================================================================
// 5b. Freeze rejects new input.
//
// Test 5 only ever feeds silence while frozen, so deleting the
// `assignSmoothed (inputGain, 0.0f, snap)` in FDNTank::setParameters leaves it green. Here the tank
// is fed full-scale noise for 30 s *while frozen*: the feedback gain is exactly 1 and the mixing
// matrix is orthogonal, so any injection at all accumulates - energy grows linearly, RMS as
// sqrt(t) - and the held tail walks away instead of holding.
//==================================================================================================
void testFreezeRejectsInput()
{
    Harness h (48000.0, 512, 2);

    auto p = measurementParams (4.0f);
    h.engine.setParameters (p);

    std::mt19937 rng (0x1CEB00Du);

    // Charge the tank, then freeze and keep the noise coming.
    for (int b = 0; b < h.blocksFor (2.0); ++b)
    {
        h.fillNoise (rng, 1.0f);
        h.engine.setParameters (p);
        h.processBlock();
    }

    p.freeze = true;

    bool  finite = true;
    float peak   = 0.0f;

    auto runNoise = [&] (double seconds)
    {
        for (int b = 0; b < h.blocksFor (seconds); ++b)
        {
            h.fillNoise (rng, 1.0f);
            h.engine.setParameters (p);
            h.processBlock();

            finite = finite && h.allFinite();
            peak   = std::max (peak, h.peakAbs());
        }
    };

    auto measureRms = [&] (double seconds)
    {
        double sum = 0.0;
        int    blocks = 0;

        for (int b = 0; b < h.blocksFor (seconds); ++b)
        {
            h.fillNoise (rng, 1.0f);
            h.engine.setParameters (p);
            h.processBlock();

            finite = finite && h.allFinite();
            peak   = std::max (peak, h.peakAbs());

            const double rms = h.blockRms();
            sum += rms * rms;
            ++blocks;
        }

        return blocks > 0 ? std::sqrt (sum / static_cast<double> (blocks)) : 0.0;
    };

    // Let the input-gain ramp reach zero before taking the reference.
    runNoise (0.5);
    const double referenceRms = measureRms (0.25);

    runNoise (30.0);
    const double laterRms = measureRms (0.25);

    const double drift = (referenceRms > 0.0 && laterRms > 0.0)
                       ? 20.0 * std::log10 (laterRms / referenceRms)
                       : -999.0;

    // The peak is ASSERTED, not just printed. It used to appear only in the measured string, so a
    // regression that walked it up while keeping the drift inside 3 dB left this check green. Same
    // divergence ceiling as 4 and 4b, decided in one place; the drift and guard-trip assertions
    // either side of it are what test the thing this check is named for.
    check ("5b. freeze rejects new input",
           finite && h.guardTrips == 0 && std::abs (drift) <= 3.0
               && peak <= divergenceCeilingLinear,
           fmt ("rms %.5f -> %.5f, drift=%.2f dB", referenceRms, laterRms, drift)
           + fmt ("  peak=%.3f (bound %.1f) finite=%.0f", static_cast<double> (peak),
                  static_cast<double> (divergenceCeilingLinear), finite ? 1.0 : 0.0));

    // Same invariant one layer down, so a regression is localised rather than only visible as a
    // growing tail: once Freeze has engaged, the tank injects nothing at all.
    reverb::FDNTank tank;
    tank.prepare (48000.0);

    reverb::FDNTank::Params tp;
    tank.setParameters (tp, true);
    const float openGain = tank.getInputGain();

    tp.freeze = true;
    tank.setParameters (tp, true);
    const float frozenGain = tank.getInputGain();

    check ("5c. frozen tank input gain = 0",
           frozenGain == 0.0f && openGain > 0.0f,
           fmt ("open=%.6f frozen=%.6f",
                static_cast<double> (openGain), static_cast<double> (frozenGain)));
}

//==================================================================================================
// 6. reset() clears the tail completely.
//==================================================================================================
void testResetClearsTail()
{
    Harness h (48000.0, 512, 2);

    auto p = measurementParams (10.0f);
    h.engine.setParameters (p);

    std::mt19937 rng (0xBEEFu);

    for (int b = 0; b < h.blocksFor (2.0); ++b)
    {
        h.fillNoise (rng, 1.0f);
        h.engine.setParameters (p);
        h.processBlock();
    }

    const double loudRms = h.blockRms();

    h.engine.reset();

    h.fillSilence();
    h.engine.setParameters (p);
    h.processBlock();

    const float afterPeak = h.peakAbs();

    check ("6. reset() clears the tail",
           loudRms > 1.0e-4 && afterPeak <= 0.0f,
           fmt ("tail rms before=%.5f, peak after reset=%.3e", loudRms, static_cast<double> (afterPeak)));
}

//==================================================================================================
// 7. Mono path.
//
// Run at Mix = 0 and Mix = 100 rather than 50. At 50 % the dry signal alone contributes an RMS of
// ~0.144 for amplitude-0.5 uniform noise, so "output is not silent" says nothing about the wet
// term: dropping the wet contribution from the mono branch of ReverbEngine::processChunk entirely
// would still leave a 50 % test comfortably above any small threshold. Here the fully wet run must
// land in a plausible band on its own, and must differ substantially from the fully dry run.
//==================================================================================================
void testMono()
{
    // Fully wet, the mono fold is (wl + wr) * 0.5 of two decorrelated taps at roughly unity RMS
    // through-gain less 3 dB of headroom, so ~0.14 is expected for amplitude-0.5 noise. The band is
    // wide enough to survive a voicing change and narrow enough that a missing or runaway wet term
    // falls outside it.
    constexpr double wetRmsMin = 0.03;
    constexpr double wetRmsMax = 0.60;

    auto run = [] (float mix)
    {
        Harness h (48000.0, 512, 1);

        auto p = measurementParams (2.0f);
        p.mix = mix;
        h.engine.setParameters (p);

        std::mt19937 rng (0xA11Cu);

        bool   finite = true;
        double lastRms = 0.0;

        for (int b = 0; b < h.blocksFor (1.0); ++b)
        {
            h.fillNoise (rng, 0.5f);
            h.engine.setParameters (p);
            h.processBlock();

            finite = finite && h.allFinite();
            lastRms = h.blockRms();
        }

        return std::make_pair (finite, lastRms);
    };

    // Same seed both times, so the two runs see identical input.
    const auto dry = run (0.0f);
    const auto wet = run (100.0f);

    const double separation = dry.second > 0.0
                            ? std::abs (wet.second - dry.second) / dry.second
                            : 0.0;

    check ("7. mono (1 channel) path",
           dry.first && wet.first
           && wet.second > wetRmsMin && wet.second < wetRmsMax
           && separation > 0.20,
           fmt ("dry rms=%.5f wet rms=%.5f separation=%.1f%%",
                dry.second, wet.second, separation * 100.0));
}

//==================================================================================================
// 8. A non-finite input sample must not reach the output.
//
// The wet path sanitises its copy of the input at the top of processChunk, so the FDN never sees a
// NaN and the wet-path guard never trips. That left the dry path reading the raw buffer: one NaN in
// gave one NaN out, straight past every check in this file. The cleaned value is now written back,
// so this asserts both halves - nothing non-finite escapes, and the guard did not have to fire
// (a trip would mean the poison reached the feedback network after all).
//==================================================================================================
void testNonFiniteInput()
{
    Harness h (48000.0, 512, 2);

    auto p = measurementParams (2.0f);
    p.mix = 50.0f;
    h.engine.setParameters (p);

    std::mt19937 rng (0x0BADF00Du);

    const float nan  = std::numeric_limits<float>::quiet_NaN();
    const float inf  = std::numeric_limits<float>::infinity();

    int    escaped = 0;
    double lastRms = 0.0;

    for (int b = 0; b < h.blocksFor (1.0); ++b)
    {
        h.fillNoise (rng, 0.5f);

        h.left [10] = nan;
        h.right[11] = nan;
        h.left [20] = inf;
        h.right[21] = -inf;

        h.engine.setParameters (p);
        h.processBlock();

        for (int n = 0; n < h.blockSize; ++n)
        {
            const auto sn = static_cast<size_t> (n);

            if (! std::isfinite (h.left[sn]))  ++escaped;
            if (! std::isfinite (h.right[sn])) ++escaped;
        }

        lastRms = h.blockRms();
    }

    check ("8. non-finite input contained",
           escaped == 0 && h.guardTrips == 0 && lastRms > 1.0e-3,
           fmt ("non-finite outputs=%.0f guard trips=%.0f rms=%.5f",
                static_cast<double> (escaped), static_cast<double> (h.guardTrips), lastRms));
}

//==================================================================================================
// 8b. The guard-trip counter itself works.
//
// WHY THIS EXISTS, because it is not obvious and its absence was invisible. Roughly fifteen
// assertions in this suite - every stability sweep, every freeze-hold, the decay-EQ sweeps - lean on
// `h.guardTrips == 0` to distinguish "the tank is stable" from "the tank blew up and the non-finite
// guard zeroed the evidence before the output". Nothing asserted that the counter can ever be
// non-zero. Measured: deleting the single line that latches it (`recovered = true` in
// ReverbEngine::processChunk) left all 102 engine checks GREEN, which means a refactor that broke
// the latch would silently turn every one of those fifteen assertions vacuous and return the suite
// to the exact blindness the counter was added to remove.
//
// Check 8 above cannot cover this: it feeds non-finite INPUT, which the input sanitiser replaces
// with 0 before the FDN ever sees it, so the guard correctly never fires there. The poison has to
// originate INSIDE the feedback loop.
//
// How this drives it there without touching production code: a finite but astronomically large
// input is not sanitised - only non-finite values are - so feeding samples near the top of float's
// range at the tank's longest decay lets the 16-line sum and the Hadamard mix overflow to infinity
// within the loop. That is a genuine internal divergence, which is precisely what the guard is for.
//
// THREE LEGS, AND THEY ARE NOT EQUALLY STRONG. Which one can actually catch what is worth stating
// plainly, because two assertions on this branch already turned out to prove nothing.
//
//   1. THE GUARD FIRES (h.guardTrips > 0). What intent item 6 asks for, and sound: deleting the
//      `recovered = true` latch in ReverbEngine::processChunk turns it red.
//
//   2. THE SCOPED FINITENESS SCAN (escaped == 0) CANNOT FAIL. It is kept for its scope, not for
//      its strength, and it is not evidence that the guard contains anything. The branch that
//      latches the counter also zeroes the whole output block and returns, and one Harness block
//      is exactly one processChunk here (prepare() sets capacity to max(maxBlockSize, 32) = 512),
//      so "the guard fired in this block" already implies "this block is all zeros". The scoping
//      is still right: in a block where the guard does NOT fire the tank output is finite by
//      definition but may be astronomically large, and WetChain's (l - r) * 0.5f can overflow on
//      it, so an unconditional scan would depend on FMA contraction and vectorisation -- green
//      under clang, possibly red under MSVC.
//
//   3. THE RECOVERY LEG is the falsifiable one, and it is what actually tests containment. The
//      guard's documented promise is to "trade one silent block for a full state clear", so once
//      the drive stops the tank must be clean: silence in, finite out, and no further trips.
//      DELETING THE reset() CALL FROM THE GUARD BRANCH KILLS THIS LEG -- the tank stays poisoned
//      and trips on every subsequent silent block.
//
//      THE DRIVE STOPS ON AN OBSERVED TRIP, NOT AFTER A FIXED BLOCK COUNT, and that is what makes
//      this leg's precondition something the test controls. reset() clears every FDN line, and at
//      Size 200% the shortest line is longer than three blocks, so the tank cannot re-trip on the
//      very next block: trips come roughly every fourth block, and a fixed-length drive would end
//      on a trip block only by luck. End it anywhere else and the lines still hold 1e38 content
//      that the first silent blocks read back, which trips the guard again and reddens this leg
//      with the engine behaving exactly as designed -- a coin toss that FMA contraction and
//      vectorisation could flip on the MSVC job. Breaking on the first trip means the silent
//      phase always starts from a freshly reset tank. Never observing a trip is not a silent
//      skip: drivenTrips stays 0 and leg 1 fails.
//
// `recovered` is cleared at the top of every ReverbEngine::process call, so Harness::guardTrips
// advancing across a block is exactly "the guard fired in THIS block".
//==================================================================================================
void testNonFiniteGuardFires()
{
    Harness h (48000.0, 512, 2);

    auto p = measurementParams (30.0f);
    p.mix  = 100.0f;
    p.size = 200.0f;
    h.engine.setParameters (p);

    // Finite, so the input sanitiser passes it through untouched, and large enough that the
    // feedback sum reaches infinity in a handful of passes.
    //
    // ALTERNATING SIGN, NOT A CONSTANT, and the first attempt at this test got it wrong: a constant
    // +/-1e38 is pure DC, the tank's input DC blocker removes it, and the FDN sees essentially
    // nothing. The test then reported `guard trips=0` and looked exactly like a working guard that
    // simply had nothing to catch. Flipping sign every sample puts the whole of that amplitude at
    // Nyquist, which the DC blocker passes untouched.
    constexpr float huge = 1.0e38f;

    constexpr int recoveryBlocks = 8;

    int escaped = 0, guardedBlocks = 0;

    for (int b = 0; b < h.blocksFor (2.0); ++b)
    {
        for (int n = 0; n < h.blockSize; ++n)
        {
            const float s = (n % 2 == 0) ? huge : -huge;

            h.left [static_cast<size_t> (n)] =  s;
            h.right[static_cast<size_t> (n)] = -s;
        }

        h.engine.setParameters (p);

        const int tripsBefore = h.guardTrips;
        h.processBlock();

        if (h.guardTrips == tripsBefore)
            continue;

        ++guardedBlocks;

        for (int n = 0; n < h.blockSize; ++n)
        {
            const auto sn = static_cast<size_t> (n);

            if (! std::isfinite (h.left[sn]))  ++escaped;
            if (! std::isfinite (h.right[sn])) ++escaped;
        }

        break;
    }

    const int drivenTrips = h.guardTrips;

    int recoveryTrips = 0, recoveryEscaped = 0;

    for (int b = 0; b < recoveryBlocks; ++b)
    {
        h.fillSilence();
        h.engine.setParameters (p);

        const int tripsBefore = h.guardTrips;
        h.processBlock();

        if (h.guardTrips != tripsBefore)
            ++recoveryTrips;

        for (int n = 0; n < h.blockSize; ++n)
        {
            const auto sn = static_cast<size_t> (n);

            if (! std::isfinite (h.left[sn]))  ++recoveryEscaped;
            if (! std::isfinite (h.right[sn])) ++recoveryEscaped;
        }
    }

    check ("8b. the guard fires, and the state clear it promises really recovers the tank",
           drivenTrips > 0 && escaped == 0 && recoveryTrips == 0 && recoveryEscaped == 0,
           fmt ("guard trips=%.0f (want > 0)", static_cast<double> (drivenTrips))
           + fmt ("  non-finite outputs in those %.0f blocks=%.0f (want 0)",
                  static_cast<double> (guardedBlocks), static_cast<double> (escaped))
           + fmt ("  trips over %.0f silent blocks after the drive=%.0f (want 0)",
                  static_cast<double> (recoveryBlocks), static_cast<double> (recoveryTrips))
           + fmt ("  non-finite outputs there=%.0f (want 0)",
                  static_cast<double> (recoveryEscaped)));
}

//==================================================================================================
// 9. Blocks larger than the prepared size are chunked correctly.
//
// ReverbEngine::process splits a request bigger than the prepared maxBlockSize into chunks of that
// size. Every other test here calls process() with exactly the prepared block size, so the loop
// never ran more than once. A host is free to hand over more than it promised (and the processor
// does exactly that when the DAW changes buffer size without re-preparing), so: prepare at 64,
// drive 2048.
//
// The verdict is bit-equality against a second engine given the same input by hand in 64-sample
// pieces. Both are configured once, with the snap that follows prepare(), so every smoother sits on
// its target and the two runs are deterministic - anything but an exact match means the chunk
// boundaries are not transparent.
//==================================================================================================
void testChunkedProcessing()
{
    constexpr int prepared = 64;
    constexpr int large    = 2048;

    reverb::ReverbEngine chunked, reference;
    chunked.prepare (48000.0, prepared, 2);
    reference.prepare (48000.0, prepared, 2);

    auto p = measurementParams (2.5f);
    p.mix      = 60.0f;
    p.modDepth = 25.0f;

    chunked.setParameters (p);
    reference.setParameters (p);

    std::mt19937 rng (0xC0DEC0DEu);
    std::uniform_real_distribution<float> dist (-0.5f, 0.5f);

    std::vector<float> aL (large), aR (large), bL (large), bR (large);

    bool  finite = true;
    bool  audible = false;
    int   mismatches = 0;

    for (int b = 0; b < 8; ++b)
    {
        for (int n = 0; n < large; ++n)
        {
            const auto sn = static_cast<size_t> (n);
            aL[sn] = bL[sn] = dist (rng);
            aR[sn] = bR[sn] = dist (rng);
        }

        float* a[2] = { aL.data(), aR.data() };
        chunked.process (a, 2, large);

        for (int offset = 0; offset < large; offset += prepared)
        {
            float* bch[2] = { bL.data() + static_cast<size_t> (offset),
                              bR.data() + static_cast<size_t> (offset) };
            reference.process (bch, 2, prepared);
        }

        for (int n = 0; n < large; ++n)
        {
            const auto sn = static_cast<size_t> (n);

            finite  = finite && std::isfinite (aL[sn]) && std::isfinite (aR[sn]);
            audible = audible || std::abs (aL[sn]) > 1.0e-4f;

            if (std::memcmp (&aL[sn], &bL[sn], sizeof (float)) != 0) ++mismatches;
            if (std::memcmp (&aR[sn], &bR[sn], sizeof (float)) != 0) ++mismatches;
        }
    }

    check ("9. oversized block chunking",
           finite && audible && mismatches == 0 && ! chunked.didRecoverFromNonFinite(),
           fmt ("prepared=64 processed=2048  mismatches vs hand-chunked=%.0f  finite=%.0f",
                static_cast<double> (mismatches), finite ? 1.0 : 0.0));
}

//==================================================================================================
// Informational: CPU time per 512-sample block at 48 kHz.
//==================================================================================================
void reportCpu()
{
    Harness h (48000.0, 512, 2);

    auto p = measurementParams (2.5f);
    p.modDepth = 25.0f;
    h.engine.setParameters (p);

    std::mt19937 rng (1u);
    h.fillNoise (rng, 0.5f);

    constexpr int warmup = 200;
    constexpr int runs   = 4000;

    for (int b = 0; b < warmup; ++b)
    {
        h.engine.setParameters (p);
        h.processBlock();
    }

    const auto start = std::chrono::steady_clock::now();

    for (int b = 0; b < runs; ++b)
    {
        h.engine.setParameters (p);
        h.processBlock();
    }

    const auto elapsed = std::chrono::steady_clock::now() - start;
    const double microsPerBlock = std::chrono::duration<double, std::micro> (elapsed).count()
                                  / static_cast<double> (runs);
    const double blockDurationUs = 512.0 / 48000.0 * 1.0e6;

    std::printf ("[INFO] %-34s  %.2f us/block (%.2f%% of one core at 48 kHz / 512)\n",
                 "cpu per 512-sample block",
                 microsPerBlock,
                 microsPerBlock / blockDurationUs * 100.0);
    std::fflush (stdout);
}

} // namespace

//==================================================================================================

#include "TestSuites.h"

// Entry point for the suite runner (tests/TestSuites.h). Returns the failure COUNT, not 0/1,
// so the runner can sum across suites.
int runEngineTests()
{
    // TestHarness.h's failureCount is shared by every suite in this target (see its own doc
    // comment): reset it here so this suite's count does not carry over from -- or leak into --
    // whatever ran before or after it.
    failureCount = 0;

    std::printf ("ReverbEngine tests\n");
    std::printf ("------------------\n");

    testDelayLineBounds();
    testDelayLineLagrangeReference();
    testSilence();
    testDryBitIdentical();
    testRt60();
    testStabilitySweep();
    testWorstCaseCorner();
    testFreezeHolds ("flat decay EQ",     1.0f, 1.0f);
    testFreezeHolds ("non-flat decay EQ", 3.0f, 0.1f);
    testFreezeRejectsInput();
    testResetClearsTail();
    testMono();
    testNonFiniteInput();
    testNonFiniteGuardFires();
    testChunkedProcessing();
    reportCpu();

    std::printf ("------------------\n");
    std::printf ("%s (%d failure%s)\n",
                 failureCount == 0 ? "ALL CHECKS PASSED" : "FAILURES",
                 failureCount,
                 failureCount == 1 ? "" : "s");

    return failureCount;
}
