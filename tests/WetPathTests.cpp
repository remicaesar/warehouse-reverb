/*
    Wet chain -- PLAN-R2 7.4, checks W1..W4, plus W5..W7. Step 5B.

    What is under test: reverb::WetChain, i.e. everything between the tank's output and the dry/wet
    mix -- the wet-only EQ (low cut / high cut / tilt), the M/S width stage 1b relocated from
    ReverbEngine, and the D11 complementary bass mono. The ER/late blend is NOT in this class (step
    2B made ReverbEngine::processChunk the single blend point), so nothing here tests it.

    MOST OF THIS SUITE IS A UNIT TEST, deliberately. Three of the four checks the plan names assert
    BIT identity, and every stage between the tank and the mix that could perturb a bit is inside
    this one class -- so driving WetChain directly is both the tightest instrument available and the
    one that cannot pass for a reason living somewhere else in the engine. W7 is the exception: a
    peak sweep is only meaningful against the real feedback network, so it runs through the engine.

    HOW THE BIT-EXACT CHECKS AVOID BEING VACUOUS. WetChain's width stage is NOT bit-transparent for
    a stereo signal -- (L+R)/2 + (L-R)/2 does not recover L exactly -- so "output == input" is not
    available as a blanket reference. Two instruments are used instead, and which one is right
    depends on the property:
      - a MONO input makes the whole chain an exact identity when nothing else is engaged, because
        the side signal is exactly +0.0f. That is the instrument for W1 and for the mono leg of
        W3/W4, and it needs no reference implementation at all.
      - for a STEREO input, the reference is the width stage alone, reimplemented in this file. That
        copy cannot catch a width bug (E6 and the 1b relocation cover width), but it is the only way
        to assert that the wet EQ and bass mono add exactly nothing at their bypass values.
    Both instruments count signed-zero normalisations separately, as checks 2/5/D6 already do: -0.0f
    + 0.0f is +0.0f, so a mono -0.0f sample legitimately comes out positive.

    Denormals and -0.0f are planted in the input on purpose, for the same reason check 2 plants them.
*/

#include "TestHarness.h"

#include "dsp/WetChain.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
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
constexpr double testRate = 48000.0;

//==================================================================================================
// Measurement plumbing.
//==================================================================================================

struct Stereo
{
    std::vector<float> l, r;

    size_t size() const   { return l.size(); }
};

/** Neutral parameters: every stage at its bypass value, width at unity. The starting point for
    every check, so a leg only ever names the one thing it is testing. */
WetChain::Params neutralParams()
{
    WetChain::Params p;
    p.widthPercent = 100.0f;
    p.wetLowCutHz  = WetChain::lowCutMinHz;    // 20 Hz  = off
    p.wetHighCutHz = WetChain::highCutMaxHz;   // 20 kHz = off
    p.tiltDb       = 0.0f;                     // exactly unity
    p.bassMonoHz   = 0.0f;                     // off
    return p;
}

Stereo runChain (const WetChain::Params& p, const Stereo& in, double sampleRate = testRate,
                 float duckGain = 1.0f)
{
    WetChain chain;
    chain.prepare (sampleRate);
    chain.setParameters (p, true);   // snap: no ramp, so a measurement is of the target setting

    Stereo out;
    out.l.resize (in.size());
    out.r.resize (in.size());

    for (size_t n = 0; n < in.size(); ++n)
        chain.processSample (0.0f, 0.0f, in.l[n], in.r[n], duckGain, out.l[n], out.r[n]);

    return out;
}

/** The width stage on its own, in the same order and with the same arithmetic WetChain uses. The
    reference for every stereo bit-identity leg -- see the file header on what it can and cannot
    catch. */
Stereo widthOnly (const Stereo& in, float widthPercent)
{
    const float w = std::min (std::max (widthPercent, 0.0f), 200.0f) * 0.01f;

    Stereo out;
    out.l.resize (in.size());
    out.r.resize (in.size());

    for (size_t n = 0; n < in.size(); ++n)
    {
        const float mid  = (in.l[n] + in.r[n]) * 0.5f;
        const float side = (in.l[n] - in.r[n]) * 0.5f * w;

        out.l[n] = mid + side;
        out.r[n] = mid - side;
    }

    return out;
}

/** Counts samples whose bit patterns differ. A pair that is numerically equal but differs in the
    sign of zero is counted separately, not as a mismatch: -0.0f + 0.0f is +0.0f, so the width
    stage legitimately normalises a negative zero. */
int bitMismatches (const std::vector<float>& a, const std::vector<float>& b, int& signedZeros)
{
    int mismatches = 0;
    signedZeros = 0;

    const size_t n = std::min (a.size(), b.size());

    for (size_t i = 0; i < n; ++i)
    {
        if (std::memcmp (&a[i], &b[i], sizeof (float)) == 0)
            continue;

        if (a[i] == 0.0f && b[i] == 0.0f)
            ++signedZeros;
        else
            ++mismatches;
    }

    return mismatches;
}

/** Decorrelated (mono = false) or identical (mono = true) noise, with -0.0f, +0.0f, denormals and
    full scale planted so the bit-exact checks see the awkward values as well as the ordinary ones. */
Stereo makeNoise (unsigned seed, size_t numSamples, bool mono)
{
    std::mt19937 rng (seed);
    std::uniform_real_distribution<float> dist (-0.5f, 0.5f);

    Stereo s;
    s.l.resize (numSamples);
    s.r.resize (numSamples);

    for (size_t n = 0; n < numSamples; ++n)
    {
        s.l[n] = dist (rng);
        s.r[n] = mono ? s.l[n] : dist (rng);
    }

    const float denormal = std::numeric_limits<float>::denorm_min() * 3.0f;
    const float planted[] = { -0.0f, 0.0f, denormal, -denormal, 1.0f, -1.0f };

    for (size_t i = 0; i < sizeof (planted) / sizeof (float) && i < numSamples; ++i)
    {
        s.l[i] = planted[i];
        s.r[i] = mono ? planted[i] : planted[(i + 3) % (sizeof (planted) / sizeof (float))];
    }

    return s;
}

std::vector<double> sideOf (const Stereo& s)
{
    std::vector<double> side (s.size(), 0.0);

    for (size_t n = 0; n < s.size(); ++n)
        side[n] = 0.5 * (static_cast<double> (s.l[n]) - static_cast<double> (s.r[n]));

    return side;
}

/** |X(f)| by direct evaluation at one frequency, phasor stepped by a complex multiply rather than
    a trig call per sample (the band sums below evaluate hundreds of frequencies over tens of
    thousands of samples; |z| drifts by ~1e-12 over that, which is far below the ratios measured). */
double dftMag (const std::vector<double>& x, size_t start, size_t len, double freq, double sampleRate)
{
    const std::complex<double> step (std::cos (-2.0 * pi * freq / sampleRate),
                                     std::sin (-2.0 * pi * freq / sampleRate));
    std::complex<double> z (1.0, 0.0), acc (0.0, 0.0);

    for (size_t i = 0; i < len && start + i < x.size(); ++i)
    {
        acc += x[start + i] * z;
        z   *= step;
    }

    return std::abs (acc);
}

/** Summed |X(f)|^2 over a band, sampled on a fixed grid -- the "energy in the band" of W2. Both
    sides of every ratio below use the same grid and the same noise realisation, so the noise
    cancels and what remains is the filter. */
double bandEnergy (const std::vector<double>& x, size_t start, size_t len,
                   double from, double to, double step, double sampleRate)
{
    double total = 0.0;

    for (double f = from; f <= to + 1.0e-9; f += step)
    {
        const double mag = dftMag (x, start, len, f, sampleRate);
        total += mag * mag;
    }

    return total;
}

/** Steady-state gain in dB at one frequency, mono in (so width and bass mono are identities and
    what is measured is the wet EQ alone). freq must be a multiple of sampleRate / window. */
double gainDbAt (const WetChain::Params& p, double freq, double sampleRate = testRate)
{
    const size_t settle = 4800;                 // 0.1 s: >10 time constants of a 20 Hz one-pole
    const size_t window = 4800;                 // bin spacing 10 Hz at 48 kHz
    const size_t total  = settle + window;

    Stereo in;
    in.l.resize (total);
    in.r.resize (total);

    for (size_t n = 0; n < total; ++n)
    {
        const auto v = static_cast<float> (std::sin (2.0 * pi * freq * static_cast<double> (n) / sampleRate));
        in.l[n] = v;
        in.r[n] = v;
    }

    const Stereo out = runChain (p, in, sampleRate);

    std::vector<double> inD (total), outD (total);

    for (size_t n = 0; n < total; ++n)
    {
        inD[n]  = in.l[n];
        outD[n] = out.l[n];
    }

    const double num = dftMag (outD, settle, window, freq, sampleRate);
    const double den = dftMag (inD,  settle, window, freq, sampleRate);

    return den > 0.0 ? 20.0 * std::log10 (std::max (num, 1.0e-300) / den) : -999.0;
}

//==================================================================================================
// W1. Bass mono is an exact identity when the input is mono (PLAN-R2 7.4).
//
// This is the property D11 chose the complementary form FOR, and the reason the implementation takes
// the low band out of the SIDE signal rather than evaluating L - LOW(L) + (LOW(L)+LOW(R))/2 as
// written: the literal form is `L - a + a`, which is not the identity in floating point. Asserted at
// three widths, because a mono input makes the side signal exactly +0.0f whatever the width scale is.
//
// Mutation, RUN: the one PLAN-R2 7.4 names -- a separate highpass per channel, summed with the two
// channels' averaged low bands -> every leg fails, 4092 of 4096 samples per channel. Worth recording
// that W2, W3 and W4 all SURVIVE that mutation (its frequency response is nearly identical), so this
// check is the only thing standing between the plan's chosen topology and one that merely sounds
// like it.
//==================================================================================================
void testBassMonoMonoIdentity()
{
    const Stereo in = makeNoise (0xB055u, 4096, true);

    for (const float width : { 0.0f, 100.0f, 200.0f })
    {
        for (const float bassMonoHz : { 130.0f, 200.0f, 400.0f })
        {
            auto p = neutralParams();
            p.widthPercent = width;
            p.bassMonoHz   = bassMonoHz;

            const Stereo out = runChain (p, in);

            int zerosL = 0, zerosR = 0;
            const int mismatchL = bitMismatches (out.l, in.l, zerosL);
            const int mismatchR = bitMismatches (out.r, in.r, zerosR);

            const std::string name = "W1. mono in, bassmono " + fmt ("%.0f Hz", static_cast<double> (bassMonoHz))
                                     + fmt (", width %.0f%%", static_cast<double> (width));

            check (name.c_str(),
                   mismatchL == 0 && mismatchR == 0,
                   fmt ("bit mismatches L=%.0f R=%.0f", static_cast<double> (mismatchL),
                        static_cast<double> (mismatchR))
                   + fmt ("  signed-zero normalisations=%.0f/%.0f  samples=%.0f",
                          static_cast<double> (zerosL), static_cast<double> (zerosR),
                          static_cast<double> (in.size())));
        }
    }
}

//==================================================================================================
// W2. Bass mono actually monos the bass (PLAN-R2 7.4): side energy below 100 Hz drops >= 20 dB and
// above 500 Hz changes by < 0.5 dB, measured against the bypassed chain on the same noise.
//
// THREE CHECKS, BECAUSE THE PLAN'S TWO THRESHOLDS ARE A CLAIM ABOUT ONE CORNER AND THE FILTER IS A
// CLAIM ABOUT ALL OF THEM. The sweep below used to assert at 300 Hz and print the other four legs,
// which is the same as not checking them -- and the leg it printed rather than checked included the
// shipping default. So:
//
//   W2   asserts the plan's thresholds at 300 Hz, unchanged. PLAN-R2 names the two numbers and
//        leaves the corner to this file, and the corner is not a free choice: the side signal's
//        response is |HIGH| = r^2/sqrt(1 + r^4), so 20 dB of INTEGRATED rejection over 20..100 Hz
//        needs a corner of 250 Hz or more, while the above-500 Hz half starts to bind as the corner
//        approaches the 400 Hz top of the range. 300 Hz sits in that window with margin at both
//        ends.
//   W2b  asserts the same two thresholds at the SHIPPING DEFAULT, whatever it currently is: the
//        corner is read from ReverbEngine::Params rather than written out here, so moving the
//        default moves this check with it instead of leaving it behind on a corner nobody ships.
//        (This target deliberately does not link juce_audio_processors -- see the CMake comment --
//        so reverb::param's APVTS default is not reachable from here; ReverbEngine::Params is the
//        in-target source of truth for it, and PluginProcessor reads the APVTS into that struct.)
//   W2c  asserts every corner in the sweep against the second-order response the class comment
//        claims, so the whole range is bounded rather than the two corners that happen to clear a
//        product threshold. |HIGH| is exact for this filter, not an approximation: the TPT SVF's
//        highpass output is the bilinear image of s^2/(s^2 + s/Q + 1), which at Q = 1/sqrt(2) gives
//        |HIGH(f)| = x^2/sqrt(1 + x^4) with x the PREWARPED ratio tan(pi f/fs) / tan(pi fc/fs). The
//        prediction is weighted by the same bypass spectrum the measurement divides by, so the noise
//        realisation cancels out of both sides identically and what is compared is the filter.
//
// Measured with the plan's literal subtraction form instead of the band split that ships (see
// WetChain.h's deviation note): 2.8 dB at 130 Hz, 6.5 at 200, 8.4 at 250, 10.1 at 300, 12.6 at 400.
// No corner in range reaches 20 dB, which is why the deviation exists.
//
// Mutation, RUN: mono the whole signal (side = 0) -> the above-500 Hz leg fails at -3052 dB, i.e.
// the side signal is gone entirely. Second mutation, RUN: leave the side alone -> the below-100 Hz
// leg fails at +0.0 dB and W2c at 29.7 dB. Both also fail W6's bassmono leg, whose negative control
// notices that nothing depends on the coefficient any more.
//
// The mutation W2c is here for, RUN: land the corner 10 % low (svfGainForCutoff maps 0.9 * hz).
// Every product threshold survives it -- at 300 Hz the sub-100 Hz side energy is still -23.1 dB and
// the above-500 Hz change is still 0.02 dB, so W2 stays GREEN -- and W2c fails at 1.71 dB. A fourth,
// RUN: double the SVF's damping (r2 = 2/Q), which keeps the 12 dB/oct slope -> W2c fails at 4.82 dB
// and W2's above-500 Hz leg fails too, at -0.73 dB. These last two were run against a copy of
// WetChain in a scratch harness rather than by editing src/dsp/WetChain.*, which belongs to another
// assignment; unmutated, that copy reproduces every number this check prints.
//==================================================================================================

/** The side signal's own response at one frequency: bass mono is the SVF's highpass output at
    Q = 1/sqrt(2), so |HIGH| = x^2/sqrt(1 + x^4). x is the ratio of PREWARPED frequencies, which is
    what makes this the digital filter's exact magnitude rather than its analogue prototype's. */
double bassMonoSideMag (double freq, double cornerHz, double sampleRate)
{
    const double x  = std::tan (pi * freq / sampleRate) / std::tan (pi * cornerHz / sampleRate);
    const double x2 = x * x;

    return x2 / std::sqrt (1.0 + x2 * x2);
}

/** The integrated band rejection bassMonoSideMag() predicts, weighted by the bypassed side
    spectrum -- i.e. exactly the ratio the measurement forms, with the measured |HIGH| replaced by
    the analytic one. Same band, same grid, same noise, so the only difference between this and the
    measurement is the filter itself. */
double predictedBandDb (const std::vector<double>& bypassSide, size_t start, size_t len,
                        double from, double to, double step, double cornerHz, double sampleRate)
{
    double filtered = 0.0, bypassed = 0.0;

    for (double f = from; f <= to + 1.0e-9; f += step)
    {
        const double mag = dftMag (bypassSide, start, len, f, sampleRate);
        const double gain = bassMonoSideMag (f, cornerHz, sampleRate);

        filtered += mag * mag * gain * gain;
        bypassed += mag * mag;
    }

    return 10.0 * std::log10 (std::max (filtered, 1.0e-300) / bypassed);
}

void testBassMonoMonosTheBass()
{
    constexpr size_t settle  = 4800;
    constexpr size_t window  = 48000;
    const Stereo     in      = makeNoise (0x5EEDu, settle + window, false);

    auto p = neutralParams();

    const std::vector<double> bypassSide = sideOf (runChain (p, in));

    const double lowBypass  = bandEnergy (bypassSide, settle, window,  20.0,  100.0,  1.0, testRate);
    const double highBypass = bandEnergy (bypassSide, settle, window, 500.0, 5000.0, 10.0, testRate);

    // The shipping default, not a literal -- see W2b in the block comment.
    //
    // THIS READS THE STRUCT DEFAULT, NOT THE APVTS ONE, and that is a real indirection worth being
    // explicit about. This target links only juce_dsp / juce_audio_basics on purpose, so it cannot
    // include Parameters.h and cannot see createParameterLayout()'s default at all -- the value the
    // user actually gets. For a while that made this check's name a promise it could not keep:
    // changing ONLY the APVTS default from 250 to 130 Hz left this check green while shipping a
    // corner that measures about -18 dB against the -20 dB bar W2 asserts twenty lines below.
    //
    // What closes it is a2 in ProcessorTests.cpp, which runs in the target that CAN see both and
    // fails if ReverbEngine::Params' default and the APVTS default ever disagree. So this line is a
    // legitimate proxy for the shipping default because a2 holds them equal -- not because the two
    // happen to agree today. If a2 is ever deleted, this check silently goes back to measuring the
    // wrong constant.
    const float defaultHz = ReverbEngine::Params {}.bassMonoHz;

    std::vector<float> corners { 130.0f, 200.0f, 250.0f, 300.0f, 400.0f };

    if (std::find (corners.begin(), corners.end(), defaultHz) == corners.end())
    {
        corners.push_back (defaultHz);
        std::sort (corners.begin(), corners.end());
    }

    // Both product legs below are selected by POSITION rather than by comparing the loop variable
    // to a float, so neither the default nor the plan's 300 Hz can be missed by a value that is
    // equal to three decimal places and not to the bit.
    const size_t defaultLeg = static_cast<size_t> (std::find (corners.begin(), corners.end(), defaultHz)
                                                   - corners.begin());
    const size_t planLeg    = static_cast<size_t> (std::find (corners.begin(), corners.end(), 300.0f)
                                                   - corners.begin());

    double worstModelErrorDb = 0.0;
    float  worstModelCorner  = 0.0f;

    for (size_t leg = 0; leg < corners.size(); ++leg)
    {
        const float bassMonoHz = corners[leg];

        p.bassMonoHz = bassMonoHz;

        const std::vector<double> side = sideOf (runChain (p, in));

        const double lowDb  = 10.0 * std::log10 (std::max (bandEnergy (side, settle, window, 20.0, 100.0, 1.0, testRate),
                                                           1.0e-300) / lowBypass);
        const double highDb = 10.0 * std::log10 (std::max (bandEnergy (side, settle, window, 500.0, 5000.0, 10.0, testRate),
                                                           1.0e-300) / highBypass);

        const double lowModelDb  = predictedBandDb (bypassSide, settle, window,  20.0,  100.0,  1.0,
                                                    bassMonoHz, testRate);
        const double highModelDb = predictedBandDb (bypassSide, settle, window, 500.0, 5000.0, 10.0,
                                                    bassMonoHz, testRate);

        const double modelErrorDb = std::max (std::abs (lowDb - lowModelDb),
                                              std::abs (highDb - highModelDb));

        if (modelErrorDb > worstModelErrorDb)
        {
            worstModelErrorDb = modelErrorDb;
            worstModelCorner  = bassMonoHz;
        }

        std::printf ("[INFO] %-34s  20..100 Hz %+7.2f dB (model %+7.2f)   500..5000 Hz %+.3f dB"
                     " (model %+.3f)\n",
                     (fmt ("side energy, bassmono %.0f Hz", static_cast<double> (bassMonoHz))).c_str(),
                     lowDb, lowModelDb, highDb, highModelDb);
        std::fflush (stdout);

        // Every corner in the sweep is now covered by W2c below; these two additionally hold the
        // plan's product thresholds, at the corner the plan's reasoning picks and at the corner the
        // plugin ships. They are the same assertion on purpose -- if the default ever moves off a
        // value that clears the bar, W2b is what says so.
        if (leg == planLeg)
            check ("W2. bass mono monos the bass",
                   lowDb <= -20.0 && std::abs (highDb) < 0.5,
                   fmt ("side energy 20..100 Hz %+.1f dB (bound -20)", lowDb)
                   + fmt ("  500..5000 Hz %+.3f dB (bound 0.5)  corner %.0f Hz",
                          highDb, static_cast<double> (bassMonoHz)));

        if (leg == defaultLeg)
            check ("W2b. and it monos it at the shipping default",
                   lowDb <= -20.0 && std::abs (highDb) < 0.5,
                   fmt ("bassmono default %.0f Hz: side energy 20..100 Hz %+.1f dB (bound -20)",
                        static_cast<double> (defaultHz), lowDb)
                   + fmt ("  500..5000 Hz %+.3f dB (bound 0.5)", highDb));
    }

    // Bound 0.30 dB. The residual is 0.10..0.13 dB at EVERY corner and always in the same direction
    // (the measurement reads slightly more rejection than the model), so it is an offset in the
    // instrument rather than an error in the shape -- most likely the finite analysis window, which
    // cuts part of the filter's own settling out of the DFT. A shape error would grow or shrink with
    // the corner, and it does not.
    check ("W2c. the side response is the second-order model",
           worstModelErrorDb <= 0.30,
           fmt ("%.0f corners, 20..100 Hz and 500..5000 Hz vs x^2/sqrt(1+x^4):"
                " worst %.2f dB (bound 0.30)",
                static_cast<double> (corners.size()), worstModelErrorDb)
           + fmt (" @ corner %.0f Hz", static_cast<double> (worstModelCorner)));
}

//==================================================================================================
// W3. bassmono = 0 is a bit-exact bypass (PLAN-R2 7.4). Stereo, so the reference is the width stage
// alone; the mono case is covered by W1's width legs and by W4.
//
// Mutation, RUN: 1 Hz instead of a bypass -> 8191 of 8192 samples differ on each channel. It also
// fails W4's stereo leg and E6, which is the cross-guard the engine suite already had.
//==================================================================================================
void testBassMonoBypass()
{
    const Stereo in  = makeNoise (0xBEEFu, 8192, false);
    const auto   p   = neutralParams();
    const Stereo out = runChain (p, in);
    const Stereo ref = widthOnly (in, p.widthPercent);

    int zerosL = 0, zerosR = 0;
    const int mismatchL = bitMismatches (out.l, ref.l, zerosL);
    const int mismatchR = bitMismatches (out.r, ref.r, zerosR);

    check ("W3. bassmono=0 bit-exact bypass",
           mismatchL == 0 && mismatchR == 0,
           fmt ("bit mismatches L=%.0f R=%.0f  samples=%.0f",
                static_cast<double> (mismatchL), static_cast<double> (mismatchR),
                static_cast<double> (in.size()))
           + fmt ("  signed-zero normalisations=%.0f/%.0f",
                  static_cast<double> (zerosL), static_cast<double> (zerosR)));
}

//==================================================================================================
// W4. The wet EQ at its neutral values is a bit-exact bypass (PLAN-R2 7.4): wetlowcut 20 Hz,
// wethighcut 20 kHz, wettilt 0 dB -- which are also the three shipping defaults (PLAN-R2 3.2), so
// this is simultaneously the "neutral at its default" assertion.
//
// Two legs, because they fail differently. MONO: the whole chain is an exact identity, with no
// reference implementation involved at all. STEREO: against the width stage alone. A fourth leg
// engages one control at a time to show the bypass is per-control -- turning the tilt up must not
// engage the low cut, which is what "each control does exactly one thing" means at the bit level.
//
// Mutation, RUN: remove the neutral-value bypass, i.e. always run all three stages -> 17 checks
// fail, including the mono leg on 4094 of 4096 samples and the stereo leg on 8189 of 8192. Second
// mutation, RUN: gate all three on one combined flag -> five fail, the per-control leg among them,
// because a tilt of -6 dB then drags a 20 Hz highpass and a 20 kHz lowpass in with it (measured:
// 0.65 dB of high-cut loss at 50 Hz, and the tilt's own asymptotes pulled to +1.40 / -3.64 dB).
//==================================================================================================
void testWetEqBypass()
{
    const auto p = neutralParams();

    {
        const Stereo in  = makeNoise (0xE0E0u, 4096, true);
        const Stereo out = runChain (p, in);

        int zerosL = 0, zerosR = 0;
        const int mismatchL = bitMismatches (out.l, in.l, zerosL);
        const int mismatchR = bitMismatches (out.r, in.r, zerosR);

        check ("W4. wet EQ neutral, mono identity",
               mismatchL == 0 && mismatchR == 0,
               fmt ("bit mismatches L=%.0f R=%.0f  samples=%.0f",
                    static_cast<double> (mismatchL), static_cast<double> (mismatchR),
                    static_cast<double> (in.size()))
               + fmt ("  signed-zero normalisations=%.0f/%.0f",
                      static_cast<double> (zerosL), static_cast<double> (zerosR)));
    }

    {
        const Stereo in  = makeNoise (0xE1E1u, 8192, false);
        const Stereo out = runChain (p, in);
        const Stereo ref = widthOnly (in, p.widthPercent);

        int zerosL = 0, zerosR = 0;
        const int mismatchL = bitMismatches (out.l, ref.l, zerosL);
        const int mismatchR = bitMismatches (out.r, ref.r, zerosR);

        check ("W4. wet EQ neutral, stereo == width",
               mismatchL == 0 && mismatchR == 0,
               fmt ("bit mismatches L=%.0f R=%.0f  samples=%.0f",
                    static_cast<double> (mismatchL), static_cast<double> (mismatchR),
                    static_cast<double> (in.size()))
               + fmt ("  signed-zero normalisations=%.0f/%.0f",
                      static_cast<double> (zerosL), static_cast<double> (zerosR)));
    }

    {
        // Each control engaged alone must leave the other two bypassed. Measured where the other two
        // would show up if they were not: 30 Hz for the low cut, 15 kHz for the high cut.
        const double bypass30  = gainDbAt (p, 30.0);
        const double bypass15k = gainDbAt (p, 15000.0);

        auto tilted = p;
        tilted.tiltDb = -6.0f;

        auto lowCut = p;
        lowCut.wetLowCutHz = 200.0f;

        const double tilted30  = gainDbAt (tilted, 30.0);
        const double tilted15k = gainDbAt (tilted, 15000.0);
        const double lowCut15k = gainDbAt (lowCut, 15000.0);

        // A tilt is not flat, so what is asserted is that it matches its OWN two shelves' asymptotes
        // (-3 dB / +3 dB for -6 dB of tilt, sign inverted) rather than carrying a 20 Hz highpass or
        // a 20 kHz lowpass along with it.
        const bool ok = std::abs (bypass30) < 0.02 && std::abs (bypass15k) < 0.02
                        && std::abs (tilted30 - 3.0) < 0.35 && std::abs (tilted15k + 3.0) < 0.35
                        && std::abs (lowCut15k) < 0.02;

        check ("W4. bypasses are per-control",
               ok,
               fmt ("neutral: 30 Hz %+.4f dB  15 kHz %+.4f dB", bypass30, bypass15k)
               + fmt ("  tilt -6: %+.2f / %+.2f dB (want +3 / -3)", tilted30, tilted15k)
               + fmt ("  lowcut 200 @ 15 kHz %+.4f dB", lowCut15k));
    }
}

//==================================================================================================
// W5. Each wet-EQ control does exactly one job, asserted as a gain matrix at 50 Hz / 1 kHz / 10 kHz.
//
// This is the check that stops the three high-frequency controls in the product from blurring
// together. `bandwidth` and `decayhigh` live elsewhere (input lowpass, and in the feedback loop) and
// are covered by T1/T7; what is asserted here is that `wethighcut` moves the top and nothing else,
// `wetlowcut` moves the bottom and nothing else, and `wettilt` moves both ends symmetrically about
// its pivot and leaves the pivot alone.
//
// Mutation, RUN: swap the low-cut and high-cut coefficients -> both cut legs fail (the 200 Hz low
// cut becomes -35 dB at 1 kHz), plus W4's per-control leg and two W6 legs. Second mutation, RUN:
// make the tilt a plain broadband gain of tiltDb -> both tilt legs fail at the pivot, -6.00 dB and
// +6.00 dB where 0 is required.
//==================================================================================================
void testWetEqOneJobEach()
{
    const auto neutral = neutralParams();

    {
        auto p = neutral;
        p.wetLowCutHz = 200.0f;

        const double at50  = gainDbAt (p, 50.0);
        const double at1k  = gainDbAt (p, 1000.0);
        const double at10k = gainDbAt (p, 10000.0);

        check ("W5. wetlowcut cuts the bottom only",
               at50 <= -12.0 && std::abs (at1k) < 0.5 && std::abs (at10k) < 0.1,
               fmt ("200 Hz low cut: 50 Hz %+.2f dB (bound -12)", at50)
               + fmt ("  1 kHz %+.3f dB (bound 0.5)  10 kHz %+.4f dB (bound 0.1)", at1k, at10k));
    }

    {
        auto p = neutral;
        p.wetHighCutHz = 1000.0f;

        const double at50  = gainDbAt (p, 50.0);
        const double at1k  = gainDbAt (p, 1000.0);
        const double at10k = gainDbAt (p, 10000.0);

        check ("W5. wethighcut cuts the top only",
               at10k <= -18.0 && std::abs (at50) < 0.1 && at1k < -2.5 && at1k > -3.5,
               fmt ("1 kHz high cut: 10 kHz %+.2f dB (bound -18)", at10k)
               + fmt ("  50 Hz %+.4f dB (bound 0.1)  at the corner %+.2f dB (want -3)", at50, at1k));
    }

    for (const float tiltDb : { -6.0f, 6.0f })
    {
        auto p = neutral;
        p.tiltDb = tiltDb;

        const double at50    = gainDbAt (p, 50.0);
        const double atPivot = gainDbAt (p, static_cast<double> (WetChain::tiltPivotHz));
        const double at10k   = gainDbAt (p, 10000.0);

        const double expectedLow  = -0.5 * static_cast<double> (tiltDb);
        const double expectedHigh =  0.5 * static_cast<double> (tiltDb);

        const std::string name = fmt ("W5. wettilt %+.0f dB tilts about 1 kHz", static_cast<double> (tiltDb));

        check (name.c_str(),
               std::abs (at50 - expectedLow) < 0.35 && std::abs (at10k - expectedHigh) < 0.35
               && std::abs (atPivot) < 0.05,
               fmt ("50 Hz %+.2f dB (want %+.1f)", at50, expectedLow)
               + fmt ("  10 kHz %+.2f dB (want %+.1f)  pivot %+.4f dB (bound 0.05)",
                      at10k, expectedHigh, atPivot));
    }
}

//==================================================================================================
// W6. skip() advances EVERY smoothed value in the class, one leg per smoother.
//
// PLAN-R2 risk 16: step 1b moved width's smoother into this class and dropped the two
// ReverbEngine skip() calls that used to advance it, so a Width change made while the engine was on
// its fully-dry fast path glided from a stale position once the bypass ended. E6 guards that for
// width; 5B added nine more smoothed values and this is what guards those.
//
// Instrument: two chains reaching the same target two ways. A ramps (snap = false) and is then
// skipped past the end of its 50 ms window; B is snapped straight there. If skip() advanced that
// parameter's smoother, A and B are at the same coefficients with the same (zero) filter state, so
// the two must agree BIT for bit. The half-skip leg is the negative control that stops the check
// passing vacuously: skipped only halfway, A must NOT agree.
//
// Mutation, RUN: make skip() advance only smoothed[0], which is what step 1b's version effectively
// did -> the four non-width legs fail, each ~1020 of 1024 samples out.
//==================================================================================================
void testSkipAdvancesEverySmoother()
{
    const Stereo in    = makeNoise (0x5A1Bu, 512, false);
    const int    steps = static_cast<int> (0.05 * testRate) + 8;   // past the end of the 50 ms window

    struct Leg { const char* name; WetChain::Params target; };

    auto withWidth    = neutralParams();  withWidth.widthPercent   = 40.0f;
    auto withLowCut   = neutralParams();  withLowCut.wetLowCutHz   = 400.0f;
    auto withHighCut  = neutralParams();  withHighCut.wetHighCutHz = 2000.0f;
    auto withTilt     = neutralParams();  withTilt.tiltDb          = -9.0f;
    auto withBassMono = neutralParams();  withBassMono.bassMonoHz  = 300.0f;

    const Leg legs[] = { { "width",      withWidth },
                         { "wetlowcut",  withLowCut },
                         { "wethighcut", withHighCut },
                         { "wettilt",    withTilt },
                         { "bassmono",   withBassMono } };

    for (const auto& leg : legs)
    {
        // B: snapped to the target.
        const Stereo reference = runChain (leg.target, in);

        // A: ramped from neutral, then skipped.
        auto runSkipped = [&] (int skipped)
        {
            WetChain chain;
            chain.prepare (testRate);
            chain.setParameters (neutralParams(), true);
            chain.setParameters (leg.target, false);
            chain.skip (skipped);

            Stereo out;
            out.l.resize (in.size());
            out.r.resize (in.size());

            for (size_t n = 0; n < in.size(); ++n)
                chain.processSample (0.0f, 0.0f, in.l[n], in.r[n], 1.0f, out.l[n], out.r[n]);

            return out;
        };

        const Stereo full = runSkipped (steps);
        const Stereo half = runSkipped (steps / 2);

        int zeros = 0;
        const int fullMismatch = bitMismatches (full.l, reference.l, zeros)
                                 + bitMismatches (full.r, reference.r, zeros);
        const int halfMismatch = bitMismatches (half.l, reference.l, zeros)
                                 + bitMismatches (half.r, reference.r, zeros);

        const std::string name = std::string ("W6. skip() advances ") + leg.name;

        check (name.c_str(),
               fullMismatch == 0 && halfMismatch > 0,
               fmt ("after skip(%.0f): mismatches vs snapped=%.0f (want 0)",
                    static_cast<double> (steps), static_cast<double> (fullMismatch))
               + fmt ("  half-skip control=%.0f (want > 0)", static_cast<double> (halfMismatch)));
    }
}

//==================================================================================================
// W7. Peak sweep with the wet chain randomised, through the real engine.
//
// POSITIVE TILT IS EXCLUDED FROM THE SWEEP, ON PURPOSE. Wet Tilt reaches +12 dB, which is +6 dB of
// high-shelf boost on the wet signal, so any absolute peak ceiling can be broken by construction by
// turning it up -- the same reasoning that has step 2A sweeping `output` only over -24..0 dB. The
// 2.0 bound is about the reverb not blowing ITSELF up, not about the user not boosting. Everything
// else in this class is loss or a redistribution: a low cut, a high cut, negative tilt, and a bass
// mono that removes the low band's side component.
//
// The base configuration is check 4b's corner (longest decay, largest size, widest bandwidth,
// maximum modulation, width 200 %, fully wet, unity trim), because that is where the headroom
// actually is: 4b measured 1.750 at 48 kHz against the 2.0 bound when 5B landed.
//==================================================================================================
void testWetChainPeakSweep()
{
    for (const double sampleRate : { 48000.0, 192000.0 })
    {
        float worstPeak = 0.0f;
        bool  finite    = true;
        int   trips     = 0;
        float worstBassMono = 0.0f, worstTilt = 0.0f;

        for (unsigned seed = 0; seed < 3; ++seed)
        {
            Harness h (sampleRate, 512, 2);
            std::mt19937 rng (0x5B5Bu ^ seed ^ static_cast<unsigned> (sampleRate));
            std::uniform_real_distribution<float> unit (0.0f, 1.0f);

            auto p = measurementParams (30.0f);
            p.size         = 200.0f;
            p.bandwidthHz  = 20000.0f;
            p.diffusion    = 100.0f;
            p.modDepth     = 100.0f;
            p.modRateHz    = 5.0f;
            p.width        = 200.0f;
            p.erMix        = 25.0f;
            p.density      = 45.0f;
            p.decayLowMult  = 1.4f;
            p.decayHighMult = 0.7f;

            for (int b = 0; b < h.blocksFor (8.0); ++b)
            {
                auto pick = [&] (float lo, float hi) { return lo + (hi - lo) * unit (rng); };

                p.bassMonoHz   = pick (0.0f, 400.0f);
                p.wetLowCutHz  = pick (20.0f, 1000.0f);
                p.wetHighCutHz = pick (1000.0f, 20000.0f);
                p.wetTiltDb    = pick (-12.0f, 0.0f);   // see the note above: no positive tilt

                h.fillNoise (rng, 1.0f);
                h.engine.setParameters (p);
                h.processBlock();

                finite = finite && h.allFinite();

                if (h.peakAbs() > worstPeak)
                {
                    worstPeak    = h.peakAbs();
                    worstBassMono = p.bassMonoHz;
                    worstTilt     = p.wetTiltDb;
                }
            }

            trips += h.guardTrips;
        }

        const std::string name = "W7. wet chain peak sweep @ " + fmt ("%.0f Hz", sampleRate);

        check (name.c_str(),
               finite && worstPeak <= 2.0f && trips == 0,
               fmt ("worst peak=%.3f (%.2f dBFS)", static_cast<double> (worstPeak),
                    20.0 * std::log10 (std::max (static_cast<double> (worstPeak), 1.0e-12)))
               + fmt ("  @ bassmono %.0f Hz, tilt %+.1f dB  guard trips=%.0f",
                      static_cast<double> (worstBassMono), static_cast<double> (worstTilt),
                      static_cast<double> (trips)));
    }
}

//==================================================================================================
// Informational: the wet chain's own CPU cost, measured by flipping ONLY the wet-chain parameters.
//
// The base configuration is the shipping default set (decay EQ 1.4/0.7, modulation 25 %, erMix 25 %,
// density 45 %) so the absolute figures line up with the ladder's "engaged" number, and the three
// legs differ from each other in nothing but this class's five parameters. "default" is what ships:
// bass mono at 130 Hz, wet EQ neutral and therefore not run at all.
//==================================================================================================
void reportWetChainCpu()
{
    struct Leg { const char* name; float bassMono, lowCut, highCut, tilt; };

    const Leg legs[] = { { "cpu, wet chain off",     0.0f,   20.0f, 20000.0f,  0.0f },
                         { "cpu, wet chain default", 130.0f, 20.0f, 20000.0f,  0.0f },
                         { "cpu, wet chain engaged", 300.0f, 200.0f, 8000.0f, -6.0f } };

    for (const auto& leg : legs)
    {
        Harness h (48000.0, 512, 2);

        auto p = measurementParams (2.5f);
        p.decayLowMult  = 1.4f;
        p.decayHighMult = 0.7f;
        p.modDepth      = 25.0f;
        p.erMix         = 25.0f;
        p.density       = 45.0f;
        p.bassMonoHz    = leg.bassMono;
        p.wetLowCutHz   = leg.lowCut;
        p.wetHighCutHz  = leg.highCut;
        p.wetTiltDb     = leg.tilt;

        h.engine.setParameters (p);

        std::mt19937 rng (1u);
        h.fillNoise (rng, 0.5f);

        for (int b = 0; b < 200; ++b)
        {
            h.engine.setParameters (p);
            h.processBlock();
        }

        constexpr int runs = 4000;
        const auto start = std::chrono::steady_clock::now();

        for (int b = 0; b < runs; ++b)
        {
            h.engine.setParameters (p);
            h.processBlock();
        }

        const auto elapsed = std::chrono::steady_clock::now() - start;
        const double microsPerBlock = std::chrono::duration<double, std::micro> (elapsed).count()
                                      / static_cast<double> (runs);

        std::printf ("[INFO] %-34s  %.2f us/block (%.2f%% of one core at 48 kHz / 512)\n",
                     leg.name,
                     microsPerBlock,
                     microsPerBlock / (512.0 / 48000.0 * 1.0e6) * 100.0);
        std::fflush (stdout);
    }
}

} // namespace

//==================================================================================================

#include "TestSuites.h"

int runWetPathTests()
{
    // TestHarness.h's failureCount is shared by every suite in this target: reset on entry, read
    // back at the end, exactly as runEngineTests() does.
    failureCount = 0;

    std::printf ("\nWet path tests\n");
    std::printf ("--------------\n");

    testBassMonoMonoIdentity();
    testBassMonoMonosTheBass();
    testBassMonoBypass();
    testWetEqBypass();
    testWetEqOneJobEach();
    testSkipAdvancesEverySmoother();
    testWetChainPeakSweep();
    reportWetChainCpu();

    std::printf ("--------------\n");
    std::printf ("%s (%d failure%s)\n",
                 failureCount == 0 ? "ALL CHECKS PASSED" : "FAILURES",
                 failureCount,
                 failureCount == 1 ? "" : "s");

    return failureCount;
}
