/*
    Tank density rework -- PLAN-R2 7.3, checks D1..D6 plus D6b. Step 3A.

    What is under test: the sixteen-line tank, the warped Size map and its 0.125 SECOND total-delay
    floor, the per-line energy compensation and its freeze guard, and the nested-allpass Density
    stage. The decay law itself belongs to DecayTests (T1..T9); this suite only asserts that Density
    does not move it (D6b).

    THE D3 THRESHOLD IS MEASURED, NOT CHOSEN. It was taken on the eight-line engine before a line of
    this step was written, and the numbers are recorded in PLAN-R2 7.3 as well as at D3 below. The
    measurement contradicted the plan's expectation in one respect that matters for reading these
    checks: normalised echo density is a TIME-DOMAIN measure, so the under-dense bottom of the Size
    knob -- 0.038 s of total delay, ~1800 modes -- mixes FASTER than a large Size, not slower,
    because its (too few) echoes are packed into a short period. Echo density therefore cannot see
    the low-Size defect at all; the total-delay floor (D1) is what sees that. Where echo density does
    discriminate is at Size 100% and above, and that is where D3's bar has teeth.
*/

#include "TestHarness.h"

#include "dsp/FDNTank.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace
{

using namespace reverb;

constexpr double pi = 3.14159265358979323846;

//==================================================================================================
// Measurement plumbing.
//==================================================================================================

/** Impulse response of the whole engine, left channel. The engine and not the bare tank, because
    the diffuser is part of what a listener hears mix, and at small Size it is most of it.
*/
std::vector<double> engineImpulseResponse (const ReverbEngine::Params& p, double fs, double seconds)
{
    constexpr int blockSize = 512;
    Harness h (fs, blockSize, 2);
    h.engine.setParameters (p);

    const int blocks = h.blocksFor (seconds);

    std::vector<double> ir;
    ir.reserve (static_cast<size_t> (blocks * blockSize));

    for (int b = 0; b < blocks; ++b)
    {
        h.fillSilence();

        if (b == 0)
            h.left[0] = h.right[0] = 1.0f;

        h.engine.setParameters (p);
        h.processBlock();

        for (int n = 0; n < blockSize; ++n)
            ir.push_back (static_cast<double> (h.left[static_cast<size_t> (n)]));
    }

    return ir;
}

//==================================================================================================
// Normalised echo density (Abel & Huang 2006), and mixing time.
//
// In a Hann window of length W: sigma = sqrt(sum w[k] h[k]^2), and the density is the weighted
// fraction of samples exceeding sigma divided by erfc(1/sqrt 2) = 0.31731 -- the fraction of a
// Gaussian lying more than one standard deviation from the mean. Gaussian noise normalises to 1.0;
// a sparse train of discrete echoes reads well below it, because a few large samples sit among many
// near-zero ones.
//
// Mixing time is the first window centre at which the density reaches 0.9 AND STAYS THERE for the
// next 10 ms. The sustain requirement is not cosmetic: the curve is not monotonic -- a single
// window can cross 0.9 early when two echoes happen to land inside it -- and a first-crossing
// definition then reports a mixing time well before the field is actually diffuse.
//==================================================================================================
constexpr double gaussianFraction = 0.317310507863;

struct DensityCurve
{
    std::vector<double> timeMs, eta;
    double mixingTimeMs = -1.0;   // -1 if the sustained 0.9 crossing never happens
};

DensityCurve echoDensity (const std::vector<double>& ir, double fs, double untilMs)
{
    constexpr double windowMs  = 20.0;
    constexpr double hopMs     = 1.0;
    constexpr int    sustain   = 10;     // windows, i.e. 10 ms at a 1 ms hop
    constexpr double threshold = 0.9;

    const auto width = static_cast<int> (std::round (windowMs * 0.001 * fs));
    const auto hop   = std::max (1, static_cast<int> (std::round (hopMs * 0.001 * fs)));

    std::vector<double> w (static_cast<size_t> (width));
    double sum = 0.0;

    for (int k = 0; k < width; ++k)
    {
        w[static_cast<size_t> (k)] = 0.5 - 0.5 * std::cos (2.0 * pi * (static_cast<double> (k) + 0.5)
                                                           / static_cast<double> (width));
        sum += w[static_cast<size_t> (k)];
    }

    for (auto& v : w)
        v /= sum;

    DensityCurve curve;

    const auto limit = static_cast<int> (std::min (static_cast<double> (ir.size()) - width,
                                                  untilMs * 0.001 * fs));

    for (int start = 0; start < limit; start += hop)
    {
        double variance = 0.0;

        for (int k = 0; k < width; ++k)
        {
            const double x = ir[static_cast<size_t> (start + k)];
            variance += w[static_cast<size_t> (k)] * x * x;
        }

        const double sigma = std::sqrt (variance);
        double exceeding = 0.0;

        for (int k = 0; k < width; ++k)
            if (std::abs (ir[static_cast<size_t> (start + k)]) > sigma)
                exceeding += w[static_cast<size_t> (k)];

        curve.timeMs.push_back ((static_cast<double> (start) + 0.5 * width) / fs * 1000.0);
        curve.eta.push_back (sigma > 0.0 ? exceeding / gaussianFraction : 0.0);
    }

    for (size_t i = 0; i + static_cast<size_t> (sustain) < curve.eta.size(); ++i)
    {
        bool held = true;

        for (int k = 0; k <= sustain && held; ++k)
            held = curve.eta[i + static_cast<size_t> (k)] >= threshold;

        if (held)
        {
            curve.mixingTimeMs = curve.timeMs[i];
            break;
        }
    }

    return curve;
}

/** Broadband RT60 by Schroeder backward integration over the standard -5 .. -35 dB window. Same
    method as ReverbEngineTests check 3; repeated here because D6b needs it once per density value
    and that check measures one configuration.
*/
double schroederRt60 (const std::vector<double>& x, double fs)
{
    const size_t length = x.size();

    if (length < 64)
        return 0.0;

    std::vector<double> edc (length, 0.0);
    double running = 0.0;

    for (size_t i = length; i-- > 0;)
    {
        running += x[i] * x[i];
        edc[i] = running;
    }

    if (edc[0] <= 0.0)
        return 0.0;

    const double reference = edc[0];
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

        const double t = static_cast<double> (i) / fs;
        sumX += t; sumY += db; sumXX += t * t; sumXY += t * db;
        ++count;
    }

    if (count < 16)
        return 0.0;

    const double n     = static_cast<double> (count);
    const double denom = n * sumXX - sumX * sumX;

    if (std::abs (denom) < 1.0e-18)
        return 0.0;

    const double slope = (n * sumXY - sumX * sumY) / denom;
    return slope < -1.0e-9 ? -60.0 / slope : 0.0;
}

FDNTank::Params tankParams (float sizePercent, float decaySeconds, float density)
{
    FDNTank::Params tp;
    tp.size          = sizePercent * 0.01f;
    tp.decaySeconds  = decaySeconds;
    tp.decayLowMult  = 1.0f;
    tp.decayHighMult = 1.0f;
    tp.lowCutHz      = 250.0f;
    tp.dampingHz     = 3500.0f;
    tp.modDepth      = 0.0f;
    tp.density       = density;
    return tp;
}

/** Per-line per-pass broadband gain, measured by driving the tank's REAL feedback chain with a
    1 kHz sine -- not recomputed from the decay law. 1 kHz because the filter is flat there (both
    multipliers 1.0) and it is clear of the 5 Hz DC blocker.
*/
double measuredPassGain (FDNTank& tank, int line, double fs)
{
    const double w = 2.0 * pi * 1000.0 / fs;
    double sumIn = 0.0, sumOut = 0.0;

    for (int n = 0; n < 8000; ++n)
    {
        const auto  x = static_cast<float> (std::sin (w * static_cast<double> (n)));
        const float y = tank.processFeedbackPath (line, x);

        if (n >= 2000)   // let the filter state settle
        {
            sumIn  += static_cast<double> (x) * x;
            sumOut += static_cast<double> (y) * y;
        }
    }

    return sumIn > 0.0 ? std::sqrt (sumOut / sumIn) : 0.0;
}

//==================================================================================================
// D1. Total late-tank delay never falls below 0.125 SECONDS.
//
// The threshold is a TIME, and the assertion has to be in seconds or it is worthless: 0.125 s is
// 6000 samples at 48 kHz and 24000 at 192 kHz, so a sample-count assertion of ">= 6000" passes
// trivially at the high rates while the tank is a quarter as colorless as it should be. Four sample
// rates and the whole Size range, because the floor is a property of the map, not of one rate.
//
// FREEZE IS SWEPT TOO, because "no reachable setting" has to mean no reachable setting. Freeze holds
// each line at round(its current length), so it can shave up to half a sample per line -- 8 samples,
// 0.17 ms at 48 kHz, against 25 ms of margin -- and the point of checking is that the rounding is
// downward-only and therefore has to be bounded rather than assumed. Size and Freeze are the only
// two parameters that reach the line lengths at all; `algorithm` will be the third, at 8A.
//==================================================================================================
void testTotalDelayFloor()
{
    double worst = 1.0e30, worstSize = 0.0, worstRate = 0.0;
    int    breaches = 0;

    for (const double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        FDNTank tank;
        tank.prepare (fs);

        for (int sizePercent = 10; sizePercent <= 200; sizePercent += 10)
        {
            auto tp = tankParams (static_cast<float> (sizePercent), 2.5f, 0.0f);
            tank.setParameters (tp, true);

            // Free, then frozen AT that Size -- freeze holds whatever the lines are running, so the
            // order matters and setting freeze first would only ever test Size 100%.
            for (const bool freeze : { false, true })
            {
                tp.freeze = freeze;
                tank.setParameters (tp, true);

                const double total = tank.getTotalDelaySeconds();

                if (total < 0.125)
                    ++breaches;

                if (total < worst)
                {
                    worst     = total;
                    worstSize = static_cast<double> (sizePercent);
                    worstRate = fs;
                }
            }
        }
    }

    check ("D1. total delay >= 0.125 s",
           breaches == 0 && worst >= 0.125,
           fmt ("minimum %.4f s @ Size %.0f%% / %.0f Hz", worst, worstSize, worstRate)
           + fmt ("  breaches=%.0f  (bound 0.1250 s = %.0f samples at that rate)",
                  static_cast<double> (breaches), 0.125 * worstRate));
}

//==================================================================================================
// D2. The Size knob has no dead zone.
//
// D1 alone is satisfied by the wrong implementation -- max(scale, floor) clears the floor and makes
// every Size below it sound identical, which is a worse defect than the under-density it fixes. The
// two checks together force the warped map: strictly increasing everywhere AND above the floor
// everywhere. The 1% step bound is the plan's; the map delivers 5.3% at its tightest, which is at
// the top of the range where the branch is linear.
//==================================================================================================
void testNoDeadZone()
{
    constexpr double fs = 48000.0;

    FDNTank tank;
    tank.prepare (fs);

    double previous = 0.0, smallestStep = 1.0e30, smallestAt = 0.0;
    int    nonIncreasing = 0;

    for (int sizePercent = 10; sizePercent <= 200; sizePercent += 10)
    {
        tank.setParameters (tankParams (static_cast<float> (sizePercent), 2.5f, 0.0f), true);

        const double total = tank.getTotalDelaySeconds();

        if (sizePercent > 10)
        {
            const double ratio = previous > 0.0 ? total / previous : 0.0;

            if (! (ratio >= 1.01))
                ++nonIncreasing;

            if (ratio < smallestStep)
            {
                smallestStep = ratio;
                smallestAt   = static_cast<double> (sizePercent);
            }
        }

        previous = total;
    }

    check ("D2. Size map has no dead zone",
           nonIncreasing == 0,
           fmt ("smallest step %.2f%% @ Size %.0f%% (bound 1.00%%)",
                (smallestStep - 1.0) * 100.0, smallestAt)
           + fmt ("  steps below bound=%.0f", static_cast<double> (nonIncreasing)));
}

//==================================================================================================
// D3. Normalised echo density reaches the measured bar.
//
// MEASURED ON THE EIGHT-LINE ENGINE FIRST (48 kHz, decay 2 s, erMix 0, density inert, and with the
// first-crossing definition the pre-change probe used rather than this file's sustained one, so the
// bars carry margin for that difference):
//
//     Size    total delay    mixing time (engine)    mixing time (tank alone)
//     10%       0.0380 s          39 ms                    27 ms
//     33%       0.1255 s          64 ms                   113 ms
//    100%       0.3802 s         166 ms                   449 ms
//    200%       0.7605 s         328 ms            never within 500 ms
//
// The bars: 60 ms at Size 10%, 150 ms at Size 100%, 280 ms at Size 200%. The Size 100% and 200% bars
// are DISCRIMINATORS -- the eight-line engine reads 166 ms and 328 ms and fails both. The Size 10%
// bar is a regression guard and is stated as such rather than dressed up: the eight-line engine was
// already fast there (39 ms) because the diffuser dominates a small tank, so no threshold at that
// Size can see the line count.
//
// That is a correction to the plan, which expected "revert to 8 lines" to fail at Size 10%. It does
// not, and cannot, for the reason in this file's header.
//==================================================================================================
void testEchoDensity()
{
    constexpr double fs = 48000.0;

    struct Probe { float sizePercent; double barMs; };

    for (const Probe& probe : { Probe { 10.0f, 60.0 }, Probe { 100.0f, 150.0 },
                                Probe { 200.0f, 280.0 } })
    {
        // Two density settings per Size: 0, so the figure is comparable with the pre-change
        // measurement and the bar tests the line count and the Size map alone; and 45, the shipping
        // default, so the nested allpasses cannot make mixing WORSE without this failing.
        double worst = 0.0, worstDensity = 0.0;
        bool   reached = true;

        for (const float density : { 0.0f, 45.0f })
        {
            auto p = measurementParams (2.0f);
            p.size    = probe.sizePercent;
            p.density = density;

            const auto curve = echoDensity (engineImpulseResponse (p, fs, 1.0), fs, 600.0);

            reached = reached && curve.mixingTimeMs >= 0.0;

            const double measured = curve.mixingTimeMs < 0.0 ? 1.0e6 : curve.mixingTimeMs;

            if (measured > worst)
            {
                worst        = measured;
                worstDensity = static_cast<double> (density);
            }
        }

        const std::string name = "D3. mixing time @ Size "
                                 + fmt ("%3.0f%%", static_cast<double> (probe.sizePercent));

        check (name.c_str(),
               reached && worst <= probe.barMs,
               fmt ("worst %.0f ms @ density %.0f%% (bar %.0f ms)", worst, worstDensity,
                    probe.barMs));
    }
}

//==================================================================================================
// D4. Per-line energy is balanced.
//
// The quantity: a line fed a unit injection accumulates energy in proportion to 1/(1 - g_i^2), so
// its modal energy is (injection weight)^2 / (1 - g_i^2). Both factors are read out of the running
// tank -- the weight through getEnergyCompensation(), and g_i by driving the REAL per-line feedback
// chain with a sine (measuredPassGain) rather than by recomputing the decay law.
//
// WHAT THIS DOES AND DOES NOT PROVE, because the difference is easy to overstate. It catches the
// mutation D2 names -- delete the compensation and the spread goes from 1.00 to 4.6 -- and it
// catches a compensation computed from a stale or differently scaled gain than the one the filter is
// running, which is a live risk now that the decay law is scaled by the line length PLUS the density
// allpass. It does NOT independently confirm the 1/(1 - g^2) model: the weight is defined as
// sqrt(1 - g^2), so the spread is 1.00 by construction whenever the two gains agree.
//
// One thing the model itself deserves on the record, because it is weaker than PLAN-R2 D2 implies:
// with an orthogonal mixing matrix the per-line POWER equalises on its own. The power pushed into
// line i is (1/N) sum_k g_k^2 P_k + inj_i^2, and the first term is common to every line, so the
// lines differ only by their injections and never by the length-ratio factor the plan quotes
// (5.26:1 for the shipped 16-line table -- the plan's own figure of 3.4:1 was the 8-line ratio and
// is stale; see decaycurve::shortestLineSeconds/longestLineSeconds, which T6b pins against a
// prepared tank rather than restating). What the
// compensation actually attenuates is the short lines' contribution to the DISCRETE echo pattern --
// their first echoes, whose comb spacing is wide enough to be heard as a pitch. That is a real
// effect and the compensation is a cheap answer to it, but it is not the stored-energy imbalance the
// plan describes. [analysis, not measured]
//==================================================================================================
void testPerLineEnergy()
{
    constexpr double fs        = 48000.0;
    constexpr double tolerance = 1.6;

    double worstRatio = 0.0, worstSize = 0.0, worstUncompensated = 0.0;

    for (const float sizePercent : { 10.0f, 100.0f, 200.0f })
    {
        FDNTank tank;
        tank.prepare (fs);
        tank.setParameters (tankParams (sizePercent, 4.0f, 0.0f), true);

        double lowest = 1.0e30, highest = 0.0, rawLowest = 1.0e30, rawHighest = 0.0;

        for (int line = 0; line < FDNTank::numLines; ++line)
        {
            const double g      = measuredPassGain (tank, line, fs);
            const double weight = static_cast<double> (tank.getEnergyCompensation (line));
            const double raw    = 1.0 / std::max (1.0 - g * g, 1.0e-12);
            const double energy = weight * weight * raw;

            lowest     = std::min (lowest, energy);
            highest    = std::max (highest, energy);
            rawLowest  = std::min (rawLowest, raw);
            rawHighest = std::max (rawHighest, raw);
        }

        const double ratio = lowest > 0.0 ? highest / lowest : 1.0e30;

        if (ratio > worstRatio)
        {
            worstRatio         = ratio;
            worstSize          = static_cast<double> (sizePercent);
            worstUncompensated = rawHighest / rawLowest;
        }
    }

    check ("D4. per-line energy balanced",
           worstRatio < tolerance,
           fmt ("worst spread %.3fx @ Size %.0f%% (bound %.2fx)", worstRatio, worstSize, tolerance)
           + fmt ("  uncompensated at that Size: %.3fx", worstUncompensated));
}

//==================================================================================================
// D5. Freeze holds with the compensation in place, and the compensation is exactly 1 while frozen.
//
// Two legs, and the second is the one that guards the guard.
//
// (a) The frozen tail must hold AND be audible, entered from Size 200% / decay 4 s -- the corner
//     where the per-line weights are furthest apart, so a weighting error is loudest here.
//     "Audible" is asserted because a silent tail holds perfectly.
//
// (b) Every weight must be exactly 1.0f while frozen. This is the plan's D5 mutation target, and the
//     honest reading of it has changed since the plan was written: D2 assumed Freeze sets the loop
//     gain to 1, which would make sqrt(1 - g^2) zero and the unit-mean normalisation 0/0. After THE
//     FOLD, Freeze holds the tail by crossfading the feedback path out (filterMix -> 0) and
//     getMidGain() stays sub-unity, so dropping the guard does NOT silence the frozen tail today --
//     verified by mutation: leg (a) stays green with the guard removed. Leg (b) therefore asserts
//     the guard directly rather than through a consequence it no longer has.
//==================================================================================================
void testFreezeWithCompensation()
{
    constexpr double fs = 48000.0;

    Harness h (fs, 512, 2);

    auto p = measurementParams (4.0f);
    p.size    = 200.0f;
    p.density = 45.0f;
    h.engine.setParameters (p);

    std::mt19937 rng (0xD5F0EEDu);

    for (int b = 0; b < h.blocksFor (4.0); ++b)
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

    runSilence (0.5);
    const double referenceRms = measureRms (0.5);

    runSilence (10.0);
    const double laterRms = measureRms (0.5);

    const double drift = (referenceRms > 0.0 && laterRms > 0.0)
                       ? 20.0 * std::log10 (laterRms / referenceRms)
                       : -999.0;

    // (b) the guard itself, one layer down.
    FDNTank tank;
    tank.prepare (fs);

    auto tp = tankParams (200.0f, 4.0f, 45.0f);
    tank.setParameters (tp, true);

    float openSpread = 0.0f;

    for (int line = 0; line < FDNTank::numLines; ++line)
        openSpread = std::max (openSpread, std::abs (tank.getEnergyCompensation (line) - 1.0f));

    tp.freeze = true;
    tank.setParameters (tp, true);

    int notExactlyOne = 0;

    for (int line = 0; line < FDNTank::numLines; ++line)
        if (tank.getEnergyCompensation (line) != 1.0f)
            ++notExactlyOne;

    check ("D5. freeze holds with compensation",
           std::abs (drift) <= 3.0 && referenceRms > 1.0e-3 && h.guardTrips == 0
               && notExactlyOne == 0 && openSpread > 0.05f,
           fmt ("rms %.5f -> %.5f, drift=%.2f dB", referenceRms, laterRms, drift)
           + fmt ("  frozen weights != 1: %.0f of %.0f  (un-frozen spread %.3f)",
                  static_cast<double> (notExactlyOne), static_cast<double> (FDNTank::numLines),
                  static_cast<double> (openSpread)));
}

//==================================================================================================
// D6. density = 0 is a bit-exact bypass.
//
// Two legs, because at density = 0 the allpass is PROVABLY unobservable and that limits what any
// test can reach. The proof: at crossfade weight 0 the value pushed into each line is independent of
// the allpass output, so the delay lines -- and therefore the tank's output, and therefore every
// later stage -- cannot depend on the allpass state at all. No input and no parameter perturbs the
// allpass while leaving the rest of the tank alone, so no output comparison can distinguish "the
// allpass is bypassed" from "the allpass is absent". What is left is to check the two things the
// bypass is made of, separately:
//
//   (a) the crossfade weight really is exactly 0 at density = 0 and exactly 1 above it, read out of
//       the tank rather than assumed. This is the leg that catches the plan's named mutation
//       (crossfade 0.0001) and any "just a little density at zero" drift.
//   (b) the crossfade EXPRESSION is a bit-exact identity at weight 0, over the values where a
//       "harmless" extra multiply or add shows up: +/-0.0f, denormals and the extremes. Same
//       expression and same order of operations as processSample's.
//
// Neither leg alone is a bypass proof; together they are one.
//==================================================================================================
void testDensityBypass()
{
    constexpr double fs = 48000.0;

    FDNTank tank;
    tank.prepare (fs);

    tank.setParameters (tankParams (100.0f, 2.5f, 0.0f), true);
    const float atZero = tank.getDensityMix();

    tank.setParameters (tankParams (100.0f, 2.5f, 45.0f), true);
    const float atDefault = tank.getDensityMix();

    tank.setParameters (tankParams (100.0f, 2.5f, 100.0f), true);
    const float atFull = tank.getDensityMix();

    // (b) the crossfade at weight 0, over the awkward values. `diffused` is deliberately of opposite
    // sign and much larger magnitude than `raw`, which is what an allpass output looks like relative
    // to its input.
    std::mt19937 rng (0xD6B17u);
    std::uniform_real_distribution<float> dist (-4.0f, 4.0f);

    int mismatches = 0, signedZeroNormalisations = 0;

    for (int i = 0; i < 20000; ++i)
    {
        const float raw = i == 0 ? 0.0f
                        : i == 1 ? -0.0f
                        : i == 2 ? std::numeric_limits<float>::denorm_min()
                        : i == 3 ? -std::numeric_limits<float>::denorm_min()
                        : i == 4 ? std::numeric_limits<float>::min()
                                 : dist (rng);

        const float diffused = -8.0f * raw + 0.5f;
        const float dense    = raw + 0.0f * (diffused - raw);

        if (std::memcmp (&dense, &raw, sizeof (float)) == 0)
            continue;

        // The one tolerated deviation, counted rather than ignored, and identical to the one §7.1 T5
        // records for the filterMix crossfade: for raw == -0.0f, 0.0f * (diffused - raw) is +0.0f
        // and -0.0f + 0.0f is +0.0f. No magnitude changes, so nothing is audible; it is the sign of
        // a zero. Any other difference is a real failure.
        if (dense == 0.0f && raw == 0.0f)
            ++signedZeroNormalisations;
        else
            ++mismatches;
    }

    check ("D6. density=0 is a bit-exact bypass",
           atZero == 0.0f && atDefault == 1.0f && atFull == 1.0f && mismatches == 0,
           fmt ("crossfade weight: density 0 -> %.6f, 45%% -> %.6f, 100%% -> %.6f",
                static_cast<double> (atZero), static_cast<double> (atDefault),
                static_cast<double> (atFull))
           + fmt ("  identity mismatches=%.0f (signed-zero normalisations=%.0f)",
                  static_cast<double> (mismatches),
                  static_cast<double> (signedZeroNormalisations)));
}

//==================================================================================================
// D6b. Density does not move the decay time. NOT in the plan; added because the way D10 has to be
// built makes this a live defect rather than a hypothetical one.
//
// The nested allpass sits in series inside the loop, so a pass takes the line's length PLUS the
// allpass's delay -- the mean group delay of a D-sample Schroeder allpass is exactly D samples for
// any gain. The decay law converts dB per SAMPLE into dB per pass, so it has to be scaled by the
// length of the pass the filter is actually in. Without that term the Density knob stretches T60 by
// the allpass's share of the loop: 10% at Size 100% and 50% at Size 10%, the latter far outside
// check 3's 15%, which would make Density a decay control. Mutation: drop the densityStep term from
// passLength in FDNTank::setParameters -> the Size 10% leg fails.
//
// Size 10% is where the term matters most, so it is the leg that carries the test; Size 100% is
// there to show the correction does not overshoot in the easy case.
//==================================================================================================
void testDecayIndependentOfDensity()
{
    constexpr double fs        = 48000.0;
    constexpr double requested = 2.0;
    constexpr double tolerance = 0.10;

    double worstError = 0.0, worstMeasured = 0.0, worstDensity = 0.0, worstSize = 0.0;

    for (const float sizePercent : { 10.0f, 100.0f })
    {
        for (const float density : { 0.0f, 45.0f, 100.0f })
        {
            auto p = measurementParams (static_cast<float> (requested));
            p.size    = sizePercent;
            p.density = density;

            const double measured = schroederRt60 (engineImpulseResponse (p, fs, 6.0), fs);
            const double error    = measured > 0.0 ? std::abs (measured - requested) / requested
                                                   : 1.0;

            if (error > worstError)
            {
                worstError    = error;
                worstMeasured = measured;
                worstDensity  = static_cast<double> (density);
                worstSize     = static_cast<double> (sizePercent);
            }
        }
    }

    check ("D6b. T60 independent of density",
           worstError <= tolerance,
           fmt ("worst %.1f%% @ density %.0f%% / Size %.0f%%", worstError * 100.0, worstDensity,
                worstSize)
           + fmt ("  (measured %.3f s vs %.3f s requested, tolerance %.0f%%)",
                  worstMeasured, requested, tolerance * 100.0));
}

//==================================================================================================
// Informational: the echo-density curve and the total delay behind the floor, so a change in the
// SHAPE of the mixing is visible rather than only a change in the crossing time.
//==================================================================================================
void reportDensityCurves()
{
    constexpr double fs = 48000.0;

    std::printf ("[INFO] echo density, density=0        Size  totalDelay   t_mix  "
                 "eta at 10/20/50/100/200 ms\n");

    for (const float sizePercent : { 10.0f, 33.0f, 100.0f, 200.0f })
    {
        FDNTank tank;
        tank.prepare (fs);
        tank.setParameters (tankParams (sizePercent, 2.0f, 0.0f), true);

        auto p = measurementParams (2.0f);
        p.size    = sizePercent;
        p.density = 0.0f;

        const auto curve = echoDensity (engineImpulseResponse (p, fs, 1.0), fs, 600.0);

        auto etaAt = [&curve] (double whenMs)
        {
            double best = 0.0, bestDelta = 1.0e30;

            for (size_t i = 0; i < curve.timeMs.size(); ++i)
            {
                const double delta = std::abs (curve.timeMs[i] - whenMs);

                if (delta < bestDelta)
                {
                    bestDelta = delta;
                    best      = curve.eta[i];
                }
            }

            return best;
        };

        std::printf ("[INFO] %-34s %4.0f%%  %8.4f s  %5.0f ms  %.2f/%.2f/%.2f/%.2f/%.2f\n",
                     "", static_cast<double> (sizePercent), tank.getTotalDelaySeconds(),
                     curve.mixingTimeMs, etaAt (10.0), etaAt (20.0), etaAt (50.0), etaAt (100.0),
                     etaAt (200.0));
        std::fflush (stdout);
    }
}

} // namespace

//==================================================================================================

#include "TestSuites.h"

// Entry point for the suite runner (tests/TestSuites.h). Returns the failure COUNT, not 0/1.
int runDensityTests()
{
    // TestHarness.h's failureCount is shared by every suite in this target: reset it on entry, as
    // runEngineTests() does, so this suite's count neither inherits nor leaks.
    failureCount = 0;

    std::printf ("Density tests\n");
    std::printf ("-------------\n");

    testTotalDelayFloor();
    testNoDeadZone();
    testEchoDensity();
    testPerLineEnergy();
    testFreezeWithCompensation();
    testDensityBypass();
    testDecayIndependentOfDensity();
    reportDensityCurves();

    std::printf ("-------------\n");
    std::printf ("%s (%d failure%s)\n",
                 failureCount == 0 ? "ALL CHECKS PASSED" : "FAILURES",
                 failureCount,
                 failureCount == 1 ? "" : "s");

    return failureCount;
}
