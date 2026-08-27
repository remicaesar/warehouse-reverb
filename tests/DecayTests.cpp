/*
    Decay Rate EQ and THE FOLD -- PLAN-R2 7.1, checks T1..T8, plus T3b, T6b and T9. Step 2A, with
    the measurement geometry rebuilt after 3A took the tank to 16 lines.

    What is under test: the per-line AttenuationFilter (broadband scalar + two shelves, every dB
    scaled by that line's length in samples), the decaycurve prototype the GUI plots, and the fold
    itself -- the per-line damping lowpass and low-cut highpass are gone from the feedback path,
    `damping`/`lowcut` are the two crossover frequencies, and a fixed DC blocker replaces the
    guarantee the old highpass used to provide.

    HOW THE BAND MEASUREMENTS WORK, because every tolerance in this file depends on it. There are two
    instruments, both impulse-excited, differing only in analysis bandwidth, and the choice between
    them is forced by the mode count: an analysis band holds (its width in Hz) x (the tank's total
    delay in seconds) modes, which is ~1400 in a third of an octave at 8 kHz and ~17 at 100 Hz.
    Seventeen modes beat, so a low-frequency band must be wide in Hz to average them -- and no wider
    than the range over which the decay time is flat, or it averages decay times instead. Those
    requirements agree wherever the measurement sits in a band's asymptote (T1, T9, which place their
    crossovers to make that so) and conflict inside a shelf transition (T6, which measures there on
    purpose). See the note above asymptoticBandRt60/transitionBandRt60 for the measured numbers.

    Every band check is validated against the filter's OWN analytic prediction -- decaycurve's law
    evaluated per line, over the frequencies the instrument actually looks at -- rather than against
    a nominal target alone. That law is the COMPLETE feedback path whenever a line length is named:
    the DC blocker's per-pass loss used to be computed in this file and added on top, and now lives
    in decaycurve::blockerDbPerPass, folded in by dbPerSampleAt, so the display draws it too. Where
    prediction and measurement still disagree the reason is named: the shelf asymptote is approached
    and not met (up to -3.0% at 100 Hz), and the analysis filter rings (+3.0% on a 0.4 s decay at
    100 Hz). T6 sizes what is left.

    Deliberately NOT included: a check that the shelf transition width is independent of its own
    depth. It is not, it cannot be for a first-order shelf, and T6 quantifies the consequence
    instead -- the per-line prediction envelope, up to 42% wide at the extreme corners -- rather than
    asserting it away.
*/

#include "TestHarness.h"

#include "dsp/AttenuationFilter.h"
#include "dsp/DecayCurve.h"
#include "dsp/FDNTank.h"
#include "dsp/Filters.h"
#include "dsp/Shelf.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace
{

using namespace reverb;

constexpr double pi = 3.14159265358979323846;

//==================================================================================================
// Measurement plumbing.
//==================================================================================================

/** Renders the engine's left-channel impulse response. measurementParams already pins mix = 100,
    pre-delay 0, erMix 0 and every P2/P3 field neutral, so what comes out is the tank.
*/
std::vector<double> renderImpulseResponse (const reverb::ReverbEngine::Params& p,
                                           double sampleRate, double seconds)
{
    const int blockSize = 512;
    Harness   h (sampleRate, blockSize, 2);

    h.engine.setParameters (p);

    const int totalBlocks = h.blocksFor (seconds);

    std::vector<double> ir;
    ir.reserve (static_cast<size_t> (totalBlocks * blockSize));

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
            ir.push_back (static_cast<double> (h.left[static_cast<size_t> (n)]));
    }

    return ir;
}

/** One RBJ constant-peak-gain bandpass section, in doubles. */
struct Biquad
{
    void setBandpass (double f0, double fs, double q) noexcept
    {
        const double w0    = 2.0 * pi * std::min (f0, fs * 0.45) / fs;
        const double alpha = std::sin (w0) / (2.0 * q);
        const double a0    = 1.0 + alpha;

        b0 = alpha / a0;
        b1 = 0.0;
        b2 = -alpha / a0;
        a1 = -2.0 * std::cos (w0) / a0;
        a2 = (1.0 - alpha) / a0;
    }

    double process (double x) noexcept
    {
        const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x;
        y2 = y1; y1 = y;
        return y;
    }

    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
};

/** Eight cascaded 2nd-order bandpasses at f0, Q = 1.4 each.

    Not the plan's single "2nd-order, +/- 1 octave", and the reason is the whole point of a Decay EQ:
    the decay time VARIES across the analysis band, so a wide band does not measure "the RT60 at f0",
    it measures an energy-weighted average over whatever it passes -- and because slower components
    dominate a tail, that average is biased towards the slow side. With four sections at Q = 2/3
    (+/- 1 octave, what this file used until the 3A follow-up) the bias reached +21% at 1 kHz with the
    crossovers at 250/3500 and decaylow 3 / decayhigh 0.05, where T60 falls from ~7 s at 500 Hz to
    ~2.4 s at 1 kHz. Eight sections at Q = 1.4 bring the same measurement inside the per-line
    prediction envelope (2.444 s against 2.321..2.534 s).

    Skirt rejection, measured: -3 dB at +/-0.1 octave, -25 dB at +/-0.5 octave, -60 dB at +/-1
    octave. Narrow enough that the decay barely moves across the passband, and steep enough that a
    neighbouring band decaying ten times slower cannot overtake the leak inside the fit window.

    The cost of narrowing is the analysis filter's OWN ringing, which inflates a short decay: at
    100 Hz the cascade rings for ~0.15 s, which is +3.0% on a 0.4 s decay (the shortest this file
    measures at that frequency, T1's decaylow = 0.1 leg). Q = 1.4 rather than 2.0 is that trade-off
    -- Q = 2.0 measured +4.4% there while gaining nothing at 1 kHz.
*/
std::vector<double> bandpassed (const std::vector<double>& ir, double f0, double fs)
{
    Biquad sections[8];

    for (auto& s : sections)
        s.setBandpass (f0, fs, 1.4);

    std::vector<double> out (ir.size(), 0.0);

    for (size_t i = 0; i < ir.size(); ++i)
    {
        double v = ir[i];

        for (auto& s : sections)
            v = s.process (v);

        out[i] = v;
    }

    return out;
}

/** Four cascaded 2nd-order bandpasses at f0, +/- 1 octave (Q = 2/3): the wide instrument's band.

    Skirts 24 dB/octave, which is what keeps a neighbouring band that decays ten times slower from
    overtaking the leak inside the fit window. Wide enough to hold ~112 modes at 100 Hz, which is
    why the asymptotic instrument uses it -- see the instrument note below.
*/
std::vector<double> bandpassedWide (const std::vector<double>& ir, double f0, double fs)
{
    Biquad sections[4];

    for (auto& s : sections)
        s.setBandpass (f0, fs, 2.0 / 3.0);

    std::vector<double> out (ir.size(), 0.0);

    for (size_t i = 0; i < ir.size(); ++i)
    {
        double v = ir[i];

        for (auto& s : sections)
            v = s.process (v);

        out[i] = v;
    }

    return out;
}

/** RT60 by Schroeder backward integration and a least-squares fit over [dbHigh, dbLow].

    Same method as ReverbEngineTests check 3, plus two things a BAND measurement needs and a
    broadband one does not, both of them standard (Chu 1978 / ISO 3382 noise compensation):

      - subtract the residual power floor, estimated from the last 10% of the record, and
      - truncate the integration there rather than integrating the floor all the way back.

    Without them a fast band reads systematically LONG, and silently: the backward integral carries
    every later sample into every earlier one, so slow-decaying skirt leakage from a neighbouring
    band -- 45 dB down instantaneously, but decaying ten times slower and therefore only ~35 dB down
    in ENERGY -- flattens the fitted slope. Measured effect before the compensation: +26% on a
    0.2 s low band and +58% on a 0.1 s high band, i.e. large enough to fail a 15% test on a filter
    that was correct. Band windows are -5 .. -25 dB for the same reason.

    Returns 0 when it cannot fit.
*/
double schroederRt60 (const std::vector<double>& x, double fs, double dbHigh, double dbLow,
                      double skipSeconds = 0.0)
{
    const size_t length = x.size();

    if (length < 64)
        return 0.0;

    const size_t skip = std::min (static_cast<size_t> (std::max (skipSeconds, 0.0) * fs), length / 2);
    const size_t floorStart = length - length / 10;

    double floorPower = 0.0;

    for (size_t i = floorStart; i < length; ++i)
        floorPower += x[i] * x[i];

    floorPower /= static_cast<double> (length - floorStart);

    std::vector<double> edc (length, 0.0);
    double running = 0.0;

    for (size_t i = floorStart; i-- > 0;)
    {
        running += std::max (x[i] * x[i] - floorPower, 0.0);
        edc[i] = running;
    }

    if (edc[skip] <= 0.0)
        return 0.0;

    const double reference = edc[skip];

    double sumX = 0.0, sumY = 0.0, sumXX = 0.0, sumXY = 0.0;
    int    count = 0;

    for (size_t i = skip; i < length; ++i)
    {
        if (edc[i] <= 0.0)
            break;

        const double db = 10.0 * std::log10 (edc[i] / reference);

        if (db > dbHigh)
            continue;

        if (db < dbLow)
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

    if (slope >= -1.0e-9)
        return 0.0;

    return -60.0 / slope;
}

/** Two band-RT60 instruments, and the rule for which one a check uses.

    Both take the engine's IMPULSE response -- not the 20 ms Hann burst this file used until the 3A
    follow-up. The burst was chosen to beat a wide band's slow-side bias, and it did, but it excites
    only the handful of modes inside its own ~+/-0.07 octave spectrum, so it measures a small random
    sample of the mode field rather than the band. That surfaced the moment 3A took the tank from 8
    lines to 16: same total delay, opposite sign of error (8 lines at Size 200% read -7.9%, 16 lines
    at Size 100% read +12.9%). Mode selection looks like that; a decay defect does not.

    What differs between the two is the ANALYSIS bandwidth, and the choice is forced by geometry, not
    taste. The number of modes in an analysis band is (its width in Hz) x (the tank's total delay in
    seconds) -- 0.75 modes/Hz at Size 100%. So at 8 kHz a 1/3-octave band holds ~1400 modes and at
    100 Hz it holds ~17, and seventeen modes beat: successive 10 dB windows of one 0.69 s decay
    fitted 0.63 / 0.35 / 1.14 / 0.85 s. A band therefore has to be wide enough in HERTZ to average
    the beating -- and no wider than the range over which the decay time is flat, or it averages
    decay times instead and biases towards the slow side (+21% at 1 kHz, measured).

    Those two requirements do not conflict as long as the measurement sits in a band's ASYMPTOTE,
    which is where T1 and T9 deliberately place their crossovers. They do conflict inside a shelf
    TRANSITION, which is where T6 deliberately measures. Hence two instruments:

      asymptoticBandRt60  -- one +/-1 octave band (4 sections, Q = 2/3), fit -5 .. -25 dB.
      transitionBandRt60  -- five 1/12-octave-spaced narrow slices (8 sections, Q = 1.4) summed in
                             POWER, fit -10 .. -40 dB.

    The late window belongs to the narrow instrument for the same mode-count reason: with few modes
    the first 10 dB is onset and analysis-filter ring (77 ms at 100 Hz for that cascade), and on
    T6's hardest configuration -- 100 Hz with decaylow 0.1, the low band decaying nine times faster
    than everything above it -- the windows read 0.507 s (-5..-25), 0.637 s (-5..-35) and 0.701 s
    (-10..-40) against a predicted 0.661..0.707 s. The wide instrument must NOT use a late window:
    on a 0.4 s decay at 100 Hz the tank has run out of echoes by -30 dB and the curve steepens, so
    -5..-25 reads 0.404 s against a predicted 0.402..0.407 s where -10..-40 reads 0.360 s.

    Every configuration used by T1, T6 and T9 was measured with both instruments against the
    filter's own analytic prediction before this split was adopted; the table is in the step report.

    The 50 ms onset skip is about one circulation of the longest delay line: until every first echo
    has arrived the response is the injected impulse spreading out, not a decay.
*/
double asymptoticBandRt60 (const reverb::ReverbEngine::Params& p, double fs, double f0, double seconds)
{
    const auto ir = renderImpulseResponse (p, fs, seconds);

    return schroederRt60 (bandpassedWide (ir, f0, fs), fs, -5.0, -25.0, 0.05);
}

double transitionBandRt60 (const reverb::ReverbEngine::Params& p, double fs, double f0, double seconds)
{
    constexpr int slices = 5;

    const auto ir = renderImpulseResponse (p, fs, seconds);

    std::vector<double> summed (ir.size(), 0.0);

    for (int k = 0; k < slices; ++k)
    {
        const double f    = f0 * std::pow (2.0, (static_cast<double> (k) - 0.5 * (slices - 1)) / 12.0);
        const auto   band = bandpassed (ir, f, fs);

        for (size_t i = 0; i < ir.size(); ++i)
            summed[i] += band[i] * band[i];
    }

    for (auto& v : summed)
        v = std::sqrt (v);

    return schroederRt60 (summed, fs, -10.0, -40.0, 0.05);
}

/** The blocker's per-pass loss is decaycurve's now, not this file's.

    It used to be computed here and added on top of decaycurve::dbPerSampleAt, because the curve --
    and therefore the display -- modelled the attenuation filter and nothing else. That made the
    drawn curve optimistic by up to 12%, always in the same direction, so the arithmetic moved into
    DecayCurve.h and dbPerSampleAt folds it in whenever a line length is named. The two constants it
    now mirrors are pinned here rather than trusted:
*/
// Two inequalities rather than ==, which is exactly as strict and does not trip -Wfloat-equal.
static_assert (decaycurve::dcBlockerHz <= reverb::FDNTank::dcBlockerHz
               && decaycurve::dcBlockerHz >= reverb::FDNTank::dcBlockerHz,
               "decaycurve mirrors the blocker corner because it cannot include FDNTank.h "
               "(FDNTank.h -> AttenuationFilter.h -> DecayCurve.h); the two must stay equal");

reverb::ReverbEngine::Params decayEqParams (float decaySeconds, float lowMult, float highMult,
                                            float xoverLowHz, float xoverHighHz)
{
    auto p = measurementParams (decaySeconds);
    p.decayLowMult  = lowMult;
    p.decayHighMult = highMult;
    p.lowCutHz      = xoverLowHz;
    p.dampingHz     = xoverHighHz;
    return p;
}

decaycurve::Bands bandsFor (float lowMult, float highMult, float xoverLowHz, float xoverHighHz)
{
    decaycurve::Bands b;
    b.lowMult     = lowMult;
    b.highMult    = highMult;
    b.xoverLowHz  = xoverLowHz;
    b.xoverHighHz = xoverHighHz;
    return b;
}

/** Line lengths in samples the tank is actually running, read back from the tank itself. */
std::vector<float> tankLineLengths (double fs, const reverb::FDNTank::Params& tp)
{
    reverb::FDNTank tank;
    tank.prepare (fs);
    tank.setParameters (tp, true);

    std::vector<float> m;

    for (int i = 0; i < reverb::FDNTank::numLines; ++i)
        m.push_back (tank.getLineLengthSamples (i));

    return m;
}

/** What bandRt60 should measure at f0, as an envelope.

    Taken over the FIVE slice frequencies the instrument looks at and the SIXTEEN line lengths the
    tank is running, because both spreads are real: a first-order shelf's transition width scales
    with its own depth, so lines of different length disagree inside a transition, and the decay time
    varies across the slice span for the same reason. Collapses to a point wherever the shelves are
    shallow.

    One envelope, not two: t60At with a named line length is the COMPLETE feedback path now
    (attenuation filter plus the fixed DC blocker, see DecayCurve.h), which is what the tank does AND
    what the display draws. The `withBlocker` flag this used to take existed only because those were
    two different curves.
*/
std::pair<double, double> predictedEnvelope (const reverb::ReverbEngine::Params& p, double fs,
                                             double f0)
{
    reverb::FDNTank::Params tp;
    tp.size          = p.size * 0.01f;
    tp.decaySeconds  = p.decaySeconds;
    tp.decayLowMult  = p.decayLowMult;
    tp.decayHighMult = p.decayHighMult;
    tp.lowCutHz      = p.lowCutHz;
    tp.dampingHz     = p.dampingHz;

    const auto bands   = bandsFor (p.decayLowMult, p.decayHighMult, p.lowCutHz, p.dampingHz);
    const auto lengths = tankLineLengths (fs, tp);

    double lo = 1.0e30, hi = 0.0;

    for (int k = 0; k < 5; ++k)
    {
        const auto f = static_cast<float> (f0 * std::pow (2.0, (static_cast<double> (k) - 2.0) / 12.0));

        for (const float m : lengths)
        {
            const double v = static_cast<double> (decaycurve::t60At (f, p.decaySeconds, bands, fs, m));

            lo = std::min (lo, v);
            hi = std::max (hi, v);
        }
    }

    return { lo, hi };
}

//==================================================================================================
// T1. Each shelved band's RT60 is decay x that band's multiplier, and does not depend on Size.
//
// THE product-level claim about the law: an attenuation filter whose shelves carry a fixed dB per
// PASS instead of m_i x a dB per SAMPLE colours the tail just as plausibly, but its dB per SECOND
// goes as 1/m_i, so the decay time stops being what the knob says and starts depending on the line
// lengths. The Size leg is what actually catches that -- see it below.
//
// Three geometry choices, each forced by a measured limit rather than chosen for convenience:
//
//   - Crossovers are placed PER BAND so the measurement frequency sits a few octaves past that
//     band's corner. A first-order shelf has 6 dB/octave and nothing more, so its asymptote -- which
//     is what "decay x mult" names -- is not reached one octave out. Even so the asymptote is only
//     approached, not met: at 100 Hz with the low crossover at 800 Hz the law itself predicts
//     11.64 s where decay x mult says 12 s (-3.0%), and that gap is in this check's error budget.
//     The frequencies are the plan's own 100 Hz and 8 kHz.
//   - Base decay per band. A band target must be several circulations of the tank's longest delay
//     line or the tank cannot express it at all; and the fixed DC blocker takes a constant dB off
//     every pass, so its share of the decay GROWS with the decay time (~0.38% of RT60 per second of
//     RT60, at 100 Hz). The first limit rules out short targets, the second rules out long ones, and
//     they do not overlap for both bands at once: 4 s for the low band (0.4 .. 12 s of target),
//     6 s for the high band (0.3 .. 18 s).
//   - Tolerances are per band because their error floors are different physics. The high band's
//     worst measured error is -2.8%, all of it the shelf asymptote, so 5%. The low band's is -7.2%:
//     -3.0% shelf asymptote plus -4.3% DC blocker, both understood and both real, so 10%. Tightening
//     the low band further would mean either measuring lower (which makes the blocker's share worse,
//     3x at 60 Hz) or modelling the blocker in the target, which would stop this being a check on
//     the product claim.
//==================================================================================================
void testBandRt60TracksMultiplier()
{
    constexpr double fs                = 48000.0;
    constexpr double lowBandTolerance  = 0.10;
    constexpr double highBandTolerance = 0.05;
    constexpr double sizeTolerance     = 0.05;

    // Worst error against decay x mult (what is asserted) and against the law's own per-line
    // prediction (reported, so that instrument drift and DSP drift stay distinguishable).
    struct Leg
    {
        double worstError = 0.0, worstLawError = 0.0, measured = 0.0;
        float  at = 0.0f;
        bool   measuredAll = true;

        void record (float where, double rt60, double target, double law)
        {
            measuredAll = measuredAll && rt60 > 0.0;

            const double error = rt60 > 0.0 ? std::abs (rt60 - target) / target : 1.0;

            if (rt60 > 0.0 && law > 0.0)
                worstLawError = std::max (worstLawError, std::abs (rt60 - law) / law);

            if (error > worstError)
            {
                worstError = error;
                measured   = rt60;
                at         = where;
            }
        }
    };

    // The law's own prediction for the complete feedback path, midpoint of the envelope. Reported,
    // not asserted: what T1 asserts is the PRODUCT claim (the knob says 4 s, the band decays in
    // 4 s), and the two differ by understood amounts -- the shelf asymptote and the DC blocker --
    // which is exactly why both numbers are printed.
    auto lawPrediction = [&] (const reverb::ReverbEngine::Params& pp, float hz)
    {
        const auto env = predictedEnvelope (pp, fs, hz);
        return 0.5 * (env.first + env.second);
    };

    Leg low, high, size;

    for (const float lowMult : { 0.1f, 1.0f, 3.0f })
    {
        const auto   p      = decayEqParams (4.0f, lowMult, 1.0f, 800.0f, 3200.0f);
        const double target = 4.0 * static_cast<double> (lowMult);

        low.record (lowMult, asymptoticBandRt60 (p, fs, 100.0, target * 2.0 + 0.4), target,
                    lawPrediction (p, 100.0f));
    }

    for (const float highMult : { 0.05f, 1.0f, 3.0f })
    {
        const auto   p      = decayEqParams (6.0f, 1.0f, highMult, 250.0f, 1000.0f);
        const double target = 6.0 * static_cast<double> (highMult);

        high.record (highMult, asymptoticBandRt60 (p, fs, 8000.0, target * 2.0 + 0.4), target,
                     lawPrediction (p, 8000.0f));
    }

    // The Size leg, and the one that makes this check see a missing m_i scaling. m_i scales with
    // Size and the law puts m_i in the exponent, so dB per SECOND comes out Size-independent -- that
    // IS the law. Give the shelves a fixed dB per PASS instead and dB per second goes as 1/m_i, so
    // doubling Size doubles the damped band's decay time. A composite band RT60 at ONE Size cannot
    // see it: the Hadamard matrix mixes every line into every mode, which averages the per-line
    // rates back together, and the two legs above stay inside 13% even with the scaling removed.
    //
    // Size 100% and 200%, tolerance 5%. Those two are the Sizes at which every line length is an
    // integer number of samples (prime x 1 and prime x 2), so the delay read interpolates nothing
    // and this leg measures Size-invariance of the LAW and nothing else. The fractional Sizes carry
    // the interpolator's own residual loss on top -- -1.7% at Size 50%, -5.9% at Size 25%, four
    // times the pass rate of Size 100% -- and that belongs to T9, which measures it against the
    // same prediction and asserts it. Keeping the two concerns in separate checks is what lets this
    // one hold 5%.
    //
    // (The 8-line tank appeared to show a -7.1% "residual size dependence at integer offsets" here.
    // It was the burst instrument selecting modes, not the tank: with an impulse the same two Sizes
    // read +0.9% and +1.3%.)
    const double sizeTarget = 6.0 * 0.25;

    for (const float sizePercent : { 100.0f, 200.0f })
    {
        auto p = decayEqParams (6.0f, 1.0f, 0.25f, 250.0f, 1000.0f);
        p.size = sizePercent;

        size.record (sizePercent, asymptoticBandRt60 (p, fs, 8000.0, sizeTarget * 2.0 + 0.4), sizeTarget,
                     lawPrediction (p, 8000.0f));
    }

    check ("T1. band RT60 = decay x mult",
           low.measuredAll && high.measuredAll && size.measuredAll
           && low.worstError <= lowBandTolerance
           && high.worstError <= highBandTolerance
           && size.worstError <= sizeTolerance,
           fmt ("100 Hz %.1f%% @ mult=%.2f (%.3f s, tol 10%%)",
                low.worstError * 100.0, static_cast<double> (low.at), low.measured)
           + fmt ("  8 kHz %.1f%% @ mult=%.2f (%.3f s, tol 5%%)",
                  high.worstError * 100.0, static_cast<double> (high.at), high.measured)
           + fmt ("  Size %.1f%% @ %.0f%% (%.3f s, tol 5%%)",
                  size.worstError * 100.0, static_cast<double> (size.at), size.measured)
           + fmt ("  vs law: %.1f%% / %.1f%% / %.1f%%", low.worstLawError * 100.0,
                  high.worstLawError * 100.0, size.worstLawError * 100.0));
}

//==================================================================================================
// T2. Broadband RT60, multipliers flat, tolerance 15% (was 30%).
//
// The mid band is where "T60 = the Decay knob" is the claim, and it is the one band whose target is
// not a shelf asymptote. ReverbEngineTests check 3 measures the same thing at the same tolerance;
// this repeats it here so the decay suite stands on its own and so a mis-scaled MID gain (as
// opposed to a mis-scaled shelf) is caught in the suite that owns the filter.
//==================================================================================================
void testBroadbandRt60()
{
    constexpr double fs = 48000.0;
    double worstError = 0.0, worstMeasured = 0.0, worstRequested = 0.0;

    for (const float requested : { 1.0f, 4.0f })
    {
        const auto   p        = measurementParams (requested);
        const auto   ir       = renderImpulseResponse (p, fs, static_cast<double> (requested) * 2.5 + 0.5);
        const double measured = schroederRt60 (ir, fs, -5.0, -35.0);
        const double error    = std::abs (measured - static_cast<double> (requested))
                                / static_cast<double> (requested);

        if (error > worstError)
        {
            worstError     = error;
            worstMeasured  = measured;
            worstRequested = static_cast<double> (requested);
        }
    }

    check ("T2. broadband RT60 within 15%",
           worstError <= 0.15,
           fmt ("worst %.1f%% (requested %.1f s, measured %.3f s)",
                worstError * 100.0, worstRequested, worstMeasured));
}

//==================================================================================================
// T3. Composite passivity. A filter in a feedback loop that exceeds unity anywhere is divergence,
// not a 1 dB error.
//
// Grid per the plan, including mult = 0.05 and the minimum-separation crossover corner, evaluated
// at 256 log-spaced frequencies. Also asserts decaycurve::sanitise's 4:1 rule directly: the
// composite stays below unity even with the rule removed (see the report), so the sweep alone
// cannot see that mutation and the invariant is asserted where it lives.
//==================================================================================================
void testCompositePassivity()
{
    constexpr double fs = 48000.0;

    double worst = 0.0;
    float  worstDecay = 0.0f, worstLow = 0.0f, worstHigh = 0.0f, worstLength = 0.0f;
    float  worstXoverLow = 0.0f, worstXoverHigh = 0.0f;

    const std::pair<float, float> crossovers[] =
    {
        { 60.0f,  240.0f },    // minimum separation at the bottom of the low range
        { 800.0f, 3200.0f },   // minimum separation at the top of it
        { 250.0f, 3500.0f },   // product defaults
        { 60.0f,  12000.0f },  // maximum separation
        { 800.0f, 1000.0f },   // requests 1.25:1; sanitise must widen it to 4:1
    };

    for (const float decay : { 0.1f, 1.0f, 30.0f })
      for (const float lowMult : { 0.05f, 0.1f, 1.0f, 4.0f })
        for (const float highMult : { 0.05f, 0.1f, 1.0f, 4.0f })
          for (const auto& xover : crossovers)
            // Down to 100 samples: at Size 10% the shortest line is 111 samples at 48 kHz, and a
            // short line at a long decay is the ONLY place the 0.9999 upper band-gain clamp binds
            // (10^(m*gamma/20) only reaches it when m*gamma is tiny). Starting the grid at 500 left
            // the clamp untested and its mutation invisible.
            for (const float length : { 100.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f, 8000.0f })
            {
                reverb::AttenuationFilter f;
                f.prepare (fs);
                f.setTarget (reverb::AttenuationFilter::targetFor (
                                 length, decay,
                                 bandsFor (lowMult, highMult, xover.first, xover.second), fs),
                             true);

                for (int k = 0; k < 256; ++k)
                {
                    const auto  hz  = static_cast<float> (10.0 * std::pow (2400.0, k / 255.0));
                    const double mag = static_cast<double> (f.magnitudeAt (hz));

                    if (mag > worst)
                    {
                        worst          = mag;
                        worstDecay     = decay;
                        worstLow       = lowMult;
                        worstHigh      = highMult;
                        worstLength    = length;
                        worstXoverLow  = xover.first;
                        worstXoverHigh = xover.second;
                    }
                }
            }

    const auto separated = decaycurve::sanitise (bandsFor (1.0f, 1.0f, 800.0f, 1000.0f));
    const bool ruleHolds = separated.xoverHighHz >= 4.0f * separated.xoverLowHz
                           && decaycurve::sanitise (bandsFor (1.0f, 1.0f, 60.0f, 12000.0f)).xoverHighHz
                              <= decaycurve::maxXoverHighHz;

    check ("T3. composite magnitude < 1",
           worst < 1.0 && ruleHolds,
           fmt ("worst=%.6f @ decay=%.2f low=%.2f", worst, static_cast<double> (worstDecay),
                static_cast<double> (worstLow))
           + fmt (" high=%.2f m=%.0f xover=%.0f/", static_cast<double> (worstHigh),
                  static_cast<double> (worstLength), static_cast<double> (worstXoverLow))
           + fmt ("%.0f  4:1 rule=%.0f", static_cast<double> (worstXoverHigh), ruleHolds ? 1.0 : 0.0));
}

//==================================================================================================
// T3b. Stability sweep with the Decay EQ randomised, fully wet.
//
// NOT in the plan, and here for two reasons. First, check 4 in ReverbEngineTests randomises only
// the original thirteen parameters -- it leaves both decay multipliers at their defaults, so the
// mechanism this step adds is never swept. That matters more than usual: the broadband scalar ramps
// over 50 ms while the shelf coefficients snap, so a transition can put an old high scalar under a
// new boosting shelf, and AttenuationFilter::setTarget's scalarCeiling exists to bound exactly
// that; this sweep is what would catch it if the bound were wrong. Second, check 4's reported peak
// is dry-path energy and not the tank's (see the note on the pinned mix below), so it cannot be the
// number a stability claim rests on. Check 4 itself is deliberately left untouched so its figures
// stay comparable with the pre-fold measurement.
//==================================================================================================
void testDecayEqStabilitySweep()
{
    constexpr float ceilingLinear = 2.0f;   // +6 dBFS, same ceiling as check 4

    int numSeeds = 4;

    if (const char* override = std::getenv ("REVERB_SWEEP_SEEDS"))
        numSeeds = std::max (1, std::atoi (override));

    for (const double sampleRate : { 48000.0, 192000.0 })
    {
        float worstPeak = 0.0f;
        bool  finite = true;
        int   totalTrips = 0;

        for (int seed = 0; seed < numSeeds; ++seed)
        {
            Harness h (sampleRate, 512, 2);

            std::mt19937 rng (0xDECA1u ^ static_cast<unsigned> (sampleRate)
                              ^ (static_cast<unsigned> (seed) * 2654435761u));

            std::uniform_real_distribution<float> unit (0.0f, 1.0f);
            auto pick = [&] (float lo, float hi) { return lo + unit (rng) * (hi - lo); };

            reverb::ReverbEngine::Params p;

            // Prime with the PINNED values so the first setParameters after prepare snaps at
            // mix = 100 / erMix = 0. Priming with a bare default Params (mix = 35) instead makes
            // the first in-loop call RAMP 35 -> 100 over 50 ms, and with full-scale input the dry
            // leakage during that ramp is ~0.65 -- which was the entire reported peak. Verified:
            // that is why this sweep's peak was 0.645 both with the tank silenced and with erMix
            // pinned to 0. Same blind spot as check 4, reached by a different route.
            p.mix          = 100.0f;
            p.outputGainDb = 0.0f;
            p.erMix        = 0.0f;
            h.engine.setParameters (p);

            for (int b = 0; b < h.blocksFor (30.0); ++b)
            {
                // Mix and output trim are PINNED, not swept, and that is the point of this sweep
                // existing beside check 4. Check 4's reported worst peak is dry-path energy: with
                // the tank's output forced to silence it still reports 0.648 at 48 kHz, identical to
                // five figures [verified by probe]. It is a real test of finiteness and of the
                // non-finite guard, but its PEAK cannot see the feedback network at all. Fully wet
                // at unity trim, the peak here is the tank's own.
                p.mix           = 100.0f;
                p.outputGainDb  = 0.0f;
                // erMix PINNED TO 0 (CTO): Params' own default is 25, so without this the wet
                // signal is a quarter early cluster and the reported peak is ER-dominated, not the
                // tank's. Verified by probe: with the tank's output forced silent this sweep's peak
                // was UNCHANGED to three decimals (0.645 / 0.649), i.e. it was measuring the same
                // blind spot it was written to replace. With erMix pinned to 0 the peak is the
                // feedback network's alone. The ER cluster's own peak is covered by the 4b-corner
                // INFO sweep in EarlyReflectionsTests.
                p.erMix         = 0.0f;
                p.preDelayMs    = pick (0.0f, 500.0f);
                p.size          = pick (10.0f, 200.0f);
                p.decaySeconds  = pick (0.1f, 30.0f);
                p.dampingHz     = pick (1000.0f, 12000.0f);
                p.lowCutHz      = pick (60.0f, 800.0f);
                p.bandwidthHz   = pick (1000.0f, 20000.0f);
                p.diffusion     = pick (0.0f, 100.0f);
                p.modDepth      = pick (0.0f, 100.0f);
                p.modRateHz     = pick (0.01f, 5.0f);
                p.width         = pick (0.0f, 200.0f);
                p.freeze        = unit (rng) < 0.15f;
                p.decayLowMult  = pick (0.1f, 4.0f);
                p.decayHighMult = pick (0.05f, 4.0f);

                h.fillNoise (rng, 1.0f);
                h.engine.setParameters (p);
                h.processBlock();

                finite    = finite && h.allFinite();
                worstPeak = std::max (worstPeak, h.peakAbs());
            }

            totalTrips += h.guardTrips;
        }

        const std::string name = "T3b. decay-EQ sweep @ " + fmt ("%.0f Hz", sampleRate);

        check (name.c_str(),
               finite && worstPeak <= ceilingLinear && totalTrips == 0,
               fmt ("worst peak=%.3f (%.2f dBFS)  seeds=%.0f",
                    static_cast<double> (worstPeak),
                    20.0 * std::log10 (std::max (static_cast<double> (worstPeak), 1.0e-12)),
                    static_cast<double> (numSeeds))
               + fmt ("  guard trips=%.0f", static_cast<double> (totalTrips)));
    }
}

//==================================================================================================
// T4. Flat is exactly the old scalar.
//
// Both multipliers at 1.0 must reproduce the plain per-line gain the tank shipped with, bit for
// bit, so the fold cannot have quietly moved the neutral setting. The sample set includes -0.0f and
// denormals on purpose: those are where a "harmless" extra multiply or add shows up.
//==================================================================================================
void testFlatIsTheScalar()
{
    constexpr double fs = 48000.0;

    reverb::AttenuationFilter f;
    f.prepare (fs);
    f.setTarget (reverb::AttenuationFilter::targetFor (2003.0f, 2.0f,
                                                      bandsFor (1.0f, 1.0f, 250.0f, 3500.0f), fs),
                 true);

    const float gain = f.getMidGain();

    std::mt19937 rng (0x5CA1A2u);
    std::uniform_real_distribution<float> dist (-1.0f, 1.0f);

    int mismatches = 0;

    const float specials[] = { 0.0f, -0.0f, 1.0f, -1.0f, 1.0e-40f, -1.0e-40f,
                               1.0e-30f, 0.5f, -0.5f, 3.0f };

    for (int i = 0; i < 10000; ++i)
    {
        const float x        = i < 10 ? specials[static_cast<size_t> (i)] : dist (rng);
        const float actual   = f.process (x);
        const float expected = gain * x;

        if (std::memcmp (&actual, &expected, sizeof (float)) != 0)
            ++mismatches;
    }

    check ("T4. flat == plain scalar, bit-exact",
           mismatches == 0 && f.isFlat() && gain > 0.0f && gain < 1.0f,
           fmt ("mismatches=%.0f  isFlat=%.0f  gain=%.6f",
                static_cast<double> (mismatches), f.isFlat() ? 1.0 : 0.0,
                static_cast<double> (gain)));
}

//==================================================================================================
// T5. Freeze bypasses the whole feedback path.
//
// Two halves. The engine half holds the frozen tail to +/- 0.1 dB over five seconds -- thirty times
// tighter than check 5's +/- 3 dB, which is affordable now that the frozen loop is an exact
// identity rather than a near-transparent filter. It runs with a deliberately NON-flat Decay EQ,
// so a filter left engaged during Freeze strips the tail instead of hiding behind unity gain.
//
// The unit half asserts the algebra the single crossfade rests on: with fMix at 0,
// raw + fMix * (dcBlock(atten(raw)) - raw) is bit-for-bit raw, for a fully charged filter chain and
// including signed zeros. That is the ONE condition Freeze now needs -- before the fold it also
// needed the outer gain multiply to be exactly 1.
//==================================================================================================
void testFreezeBypassesFeedbackPath()
{
    constexpr double fs = 48000.0;

    Harness h (fs, 512, 2);

    auto p = decayEqParams (4.0f, 3.0f, 0.1f, 250.0f, 3500.0f);
    p.modDepth = 25.0f;
    h.engine.setParameters (p);

    std::mt19937 rng (0xF00Du);

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

    // One-second windows, not check 5's quarter-second ones. The frozen loop is an exact identity,
    // so there is no drift to measure -- what a short window measures instead is its own sampling
    // variance on a quasi-periodic tail, which is +/- 0.12 dB at 0.25 s and would fail this bar for
    // no reason. A 1 s window averages that down.
    runSilence (0.5);
    const double referenceRms = measureRms (1.0);

    runSilence (5.0);
    const double laterRms = measureRms (1.0);

    const double drift = (referenceRms > 0.0 && laterRms > 0.0)
                       ? 20.0 * std::log10 (laterRms / referenceRms)
                       : -999.0;

    // The crossfade identity, on a charged non-flat chain.
    reverb::AttenuationFilter f;
    f.prepare (fs);
    f.setTarget (reverb::AttenuationFilter::targetFor (2003.0f, 4.0f,
                                                      bandsFor (3.0f, 0.1f, 250.0f, 3500.0f), fs),
                 true);

    reverb::OnePoleHP dcBlock;
    dcBlock.setCoefficient (reverb::OnePoleHP::coefficientForCutoff (reverb::FDNTank::dcBlockerHz, fs));

    std::uniform_real_distribution<float> dist (-1.0f, 1.0f);
    std::mt19937 rng2 (0xB1A5u);

    for (int i = 0; i < 4000; ++i)
        dcBlock.process (f.process (dist (rng2)));

    int mismatches = 0, signedZeroNormalisations = 0;

    for (int i = 0; i < 10000; ++i)
    {
        const float raw      = i == 0 ? 0.0f : (i == 1 ? -0.0f : dist (rng2));
        const float filtered = dcBlock.process (f.process (raw));
        const float fed      = raw + 0.0f * (filtered - raw);

        if (std::memcmp (&fed, &raw, sizeof (float)) == 0)
            continue;

        // The single deviation from bit-exactness, counted rather than ignored: for raw == -0.0f,
        // 0.0f * (filtered - raw) is +0.0f whenever filtered is positive, and -0.0f + 0.0f is
        // +0.0f. The magnitude is identical, so no energy moves and nothing is audible; it is the
        // sign of a zero. It is also not new -- the pre-fold path was `1.0f * (raw + fMix * ...)`
        // and did exactly the same thing. Any OTHER difference is a real failure.
        if (fed == 0.0f && raw == 0.0f)
            ++signedZeroNormalisations;
        else
            ++mismatches;
    }

    check ("T5. freeze bypasses feedback path",
           std::abs (drift) <= 0.1 && mismatches == 0 && h.guardTrips == 0 && ! f.isFlat(),
           fmt ("rms %.5f -> %.5f, drift=%.3f dB", referenceRms, laterRms, drift)
           + fmt ("  fMix=0 identity: mismatches=%.0f (signed-zero normalisations=%.0f)",
                  static_cast<double> (mismatches),
                  static_cast<double> (signedZeroNormalisations)));
}

//==================================================================================================
// T6. The displayed decay curve is the tank's decay curve.
//
// The GUI plots decaycurve::t60At, so this is the check that stops the display drifting from the
// DSP. It measures the running tank at the plan's three frequencies with the product-default
// crossovers over the full 3 x 3 multiplier grid, which is deliberately the awkward geometry: at
// 250 / 3500 Hz the 1 kHz band sits inside BOTH shelf transitions in the extreme corners, which is
// the region T1's crossover placement avoids and where an idealised dB-linear shelf shape
// mispredicts by up to 50% (measured -- the shape has to come from the shelf that is running).
//
// Three things are asserted against ONE prediction, the complete per-line feedback path -- the
// attenuation filter plus the fixed DC blocker, which is exactly what FDNTank::processFeedbackPath
// contains and, since the blocker was folded into decaycurve::dbPerSampleAt, exactly what the
// display draws:
//
//   (a) the MEASUREMENT lands inside the per-line prediction envelope, taken over the five slice
//       frequencies the instrument looks at and all sixteen line lengths, to 10%. This is the
//       tank-versus-law check.
//   (b) the CURVE lands inside that envelope. The curve is the same law at one reference line
//       length, so this asserts the reference length is inside the range of lengths the tank is
//       running, which is the assumption the display rests on.
//   (c) the curve does not OVER-promise the measured tail by more than 3%, and does not disagree
//       with it in either direction by more than 12%. Two bounds because the two directions are not
//       equally bad and no longer have the same cause.
//
//       Over-promising -- the display claiming a longer tail than an RT60 meter finds -- was the
//       blocker, which takes a fixed dB per PASS and so costs a share of the decay that grows with
//       the decay time. It reached +7.2% on this grid (100 Hz, decaylow = 3, a 13 s tail) and 12% on
//       a 14 s one, always in that one direction, and it is computable rather than uncertain, so it
//       is now folded into the curve instead of bounded. What is left in that direction is +1.6%.
//
//       The 12% is the other direction and never was the blocker: the worst point is 1 kHz with
//       decaylow = 3 / decayhigh = 0.05, where the curve reads 2.48 s against a measured 2.78 s and
//       folding the blocker in moved it by 0.01%. 1 kHz sits inside BOTH deep shelf transitions
//       there, which is where the sixteen lines disagree most -- the envelope is 2.03..2.88 s and the
//       measurement is inside it. So this bound sizes the reference line's distance from the
//       aggregate the tank actually resolves to, which is the same quantity (b) asserts and the
//       display draws as its confidence band; it is NOT a modelling error to be removed.
//
// The envelope is not slack: it collapses to a point wherever the shelves are shallow, which is
// most of the grid. Its width where it is wide -- 42% at the extreme corners, across the 5.26:1
// spread of 16 line lengths -- is a real property of D1's topology, because a first-order shelf's
// transition width scales with its own depth. The alternative is a filter whose order grows with its
// gain, which D1 rejected on stability grounds.
//
// T6b pins the three line-length constants decaycurve restates from FDNTank's table (the mean the
// reference curve is drawn for, and the shortest/longest the display's confidence band spans)
// against the lengths a running tank reports. Both of the eight-line table's numbers survived the
// move to sixteen lines on a comment asking the next author to update them, which is why this is a
// check and not a comment.
//==================================================================================================
void testCurveMatchesMeasurement()
{
    constexpr double fs = 48000.0;

    // 6 s rather than the plan's 2 s for the reason T1 documents: at 2 s the 8 kHz band with
    // decayhigh = 0.05 is asked for 0.118 s, which is below the tank's own circulation time and so
    // is not a measurable decay at all. Every band target here is at least four circulations.
    constexpr float decay = 6.0f;

    // t60At takes the line length as a required argument (DecayCurve.h), so this names the
    // reference length exactly as every call in the display names one of its own.
    const auto referenceSamples = decaycurve::referenceLengthSeconds * static_cast<float> (fs);

    double worstError = 0.0, worstSpread = 0.0, worstCurveGap = 0.0, worstOptimism = 0.0;
    float  worstLow = 0.0f, worstHigh = 0.0f, worstFreq = 0.0f;
    float  gapLow = 0.0f, gapHigh = 0.0f, gapFreq = 0.0f;
    double worstLo = 0.0, worstHi = 0.0, worstMeasured = 0.0, worstCurve = 0.0;
    bool   measuredAll = true, curveInsideEnvelope = true;

    for (const float lowMult : { 0.1f, 1.0f, 3.0f })
      for (const float highMult : { 0.05f, 1.0f, 3.0f })
      {
          const auto bands = bandsFor (lowMult, highMult, 250.0f, 3500.0f);
          const auto p     = decayEqParams (decay, lowMult, highMult, 250.0f, 3500.0f);

          const double longest = static_cast<double> (decay)
                                 * std::max (1.0, std::max (static_cast<double> (lowMult),
                                                            static_cast<double> (highMult)));

          for (const float hz : { 100.0f, 1000.0f, 8000.0f })
          {
              const double curve = static_cast<double> (decaycurve::t60At (hz, decay, bands, fs,
                                                                          referenceSamples));

              const auto full = predictedEnvelope (p, fs, hz);

              const double band = transitionBandRt60 (p, fs, static_cast<double> (hz),
                                                      longest * 2.0 + 0.4);

              measuredAll = measuredAll && band > 0.0;

              // (b) the curve's reference line must sit inside the range of lines the tank runs.
              curveInsideEnvelope = curveInsideEnvelope
                                    && curve >= full.first * 0.999 && curve <= full.second * 1.001;

              // (c) how far the curve is from the tail the tank measurably delivers, signed. This is
              //     the number a user could notice: the display says one thing, an RT60 measurement
              //     of the output says another. Positive means the display OVER-promises, which is
              //     the direction that matters and is bounded far tighter below.
              if (band > 0.0)
              {
                  const double gap = curve / band - 1.0;

                  worstOptimism = std::max (worstOptimism, gap);

                  if (std::abs (gap) > worstCurveGap)
                  {
                      worstCurveGap = std::abs (gap);
                      gapFreq       = hz;
                      gapLow        = lowMult;
                      gapHigh       = highMult;
                  }
              }

              worstSpread = std::max (worstSpread, full.second / full.first - 1.0);

              // (a) distance from the complete-path envelope, zero when inside it.
              const double error = band <= 0.0 ? 1.0
                                 : (band < full.first  ? (full.first - band) / full.first
                                 : (band > full.second ? (band - full.second) / full.second : 0.0));

              if (error > worstError)
              {
                  worstError    = error;
                  worstLow      = lowMult;
                  worstHigh     = highMult;
                  worstFreq     = hz;
                  worstLo       = full.first;
                  worstHi       = full.second;
                  worstMeasured = band;
                  worstCurve    = curve;
              }
          }
      }

    // (c)'s two bounds, both sized to what is measured now that the blocker is modelled rather than
    // to the blocker itself. Measured worst on this grid: +1.6% optimistic, 10.8% either way.
    constexpr double optimismBound = 0.03;
    constexpr double curveGapBound = 0.12;

    check ("T6. curve matches the measured tank",
           measuredAll && worstError <= 0.10 && curveInsideEnvelope
           && worstOptimism <= optimismBound && worstCurveGap <= curveGapBound,
           fmt ("worst %.1f%% outside the full-path envelope @ %.0f Hz, low=%.2f",
                worstError * 100.0, static_cast<double> (worstFreq), static_cast<double> (worstLow))
           + fmt (" high=%.2f: measured %.3f s vs %.3f", static_cast<double> (worstHigh),
                  worstMeasured, worstLo)
           + fmt ("..%.3f s (curve %.3f)", worstHi, worstCurve)
           + fmt ("  curve over-promises by at worst %+.1f%% (bound %.0f%%)",
                  worstOptimism * 100.0, optimismBound * 100.0)
           + fmt (", disagrees either way by %.1f%% @ %.0f Hz low=%.2f",
                  worstCurveGap * 100.0, static_cast<double> (gapFreq), static_cast<double> (gapLow))
           + fmt (" high=%.2f (bound %.0f%%)  widest envelope on the grid %.1f%%",
                  static_cast<double> (gapHigh), curveGapBound * 100.0, worstSpread * 100.0));

    // T6b. The three constants decaycurve restates from FDNTank's shipped table, against the
    // lengths a tank prepared at Size 100% reports. 1% covers the prime snapping in prepare()
    // (a few samples on each line) and nothing else -- a table change moves these by percent.
    const auto lengths = tankLineLengths (fs, reverb::FDNTank::Params {});

    double sum = 0.0, shortest = 1.0e30, longest = 0.0;

    for (const float m : lengths)
    {
        const double seconds = static_cast<double> (m) / fs;
        sum      += seconds;
        shortest  = std::min (shortest, seconds);
        longest   = std::max (longest, seconds);
    }

    const double mean = sum / static_cast<double> (lengths.size());

    const auto off = [] (double actual, float stated)
    {
        return std::abs (actual / static_cast<double> (stated) - 1.0);
    };

    check ("T6b. curve line lengths match the table",
           off (mean, decaycurve::referenceLengthSeconds) <= 0.01
           && off (shortest, decaycurve::shortestLineSeconds) <= 0.01
           && off (longest, decaycurve::longestLineSeconds) <= 0.01,
           fmt ("tank mean/short/long %.2f / %.2f / %.2f ms", mean * 1000.0, shortest * 1000.0,
                longest * 1000.0)
           + fmt (" vs stated %.2f / %.2f",
                  static_cast<double> (decaycurve::referenceLengthSeconds) * 1000.0,
                  static_cast<double> (decaycurve::shortestLineSeconds) * 1000.0)
           + fmt (" / %.2f ms  (worst %.2f%%, tol 1%%)  ratio %.2f:1",
                  static_cast<double> (decaycurve::longestLineSeconds) * 1000.0,
                  100.0 * std::max (off (mean, decaycurve::referenceLengthSeconds),
                                    std::max (off (shortest, decaycurve::shortestLineSeconds),
                                              off (longest, decaycurve::longestLineSeconds))),
                  longest / shortest));
}

//==================================================================================================
// T7. Per-line decay-rate uniformity at a damped frequency. THE test that the fold achieved its
// purpose, and the one that fails if anybody reverts it.
//
// Before the fold, the per-line lowpass took a fixed dB off every PASS, and a pass takes m_i
// samples -- so dB per SECOND went as 1/m_i and the lines decayed at rates spanning the whole length
// ratio at every damped frequency (3.4:1 on the eight-line table this predates, 5.26:1 on the
// sixteen-line one shipping now). After it, the shelf dB is m_i x a dB per sample, so dB per second
// is identical for every line by construction.
//
// Measured two ways, because neither alone is enough:
//   (a) directly, through FDNTank::processFeedbackPath -- the real per-line chain, one line at a
//       time, which is unobservable once the Hadamard matrix has mixed the lines. Assertion: the
//       max/min ratio of dB per second at 8 kHz is below 1.15.
//   (b) through the tank's own 8 kHz band decay, as the ratio of the early slope to the late slope.
//       A sum of exponentials with different rates is curved on a dB plot; one rate is a straight
//       line. This is the half that still fails if a fixed-dB filter is added back somewhere other
//       than inside the factored chain.
//==================================================================================================
void testPerLineUniformity()
{
    constexpr double fs        = 48000.0;
    constexpr float  decay     = 2.0f;
    constexpr float  highMult  = 0.25f;
    constexpr double tolerance = 1.15;

    // (a) per-line chain, driven with an 8 kHz sine until the state settles.
    reverb::FDNTank::Params tp;
    tp.decaySeconds  = decay;
    tp.decayLowMult  = 1.0f;
    tp.decayHighMult = highMult;
    tp.lowCutHz      = 250.0f;
    tp.dampingHz     = 3500.0f;
    tp.modDepth      = 0.0f;

    const auto lengths = tankLineLengths (fs, tp);

    reverb::FDNTank tank;
    tank.prepare (fs);
    tank.setParameters (tp, true);

    double slowest = 1.0e30, fastest = -1.0e30;

    for (int line = 0; line < reverb::FDNTank::numLines; ++line)
    {
        const double w = 2.0 * pi * 8000.0 / fs;

        double sumIn = 0.0, sumOut = 0.0;

        for (int n = 0; n < 8000; ++n)
        {
            const auto  x = static_cast<float> (std::sin (w * static_cast<double> (n)));
            const float y = tank.processFeedbackPath (line, x);

            if (n >= 2000)   // let the filter state settle before measuring
            {
                sumIn  += static_cast<double> (x) * x;
                sumOut += static_cast<double> (y) * y;
            }
        }

        const double magnitude = sumIn > 0.0 ? std::sqrt (sumOut / sumIn) : 0.0;
        const double dbPerPass = 20.0 * std::log10 (std::max (magnitude, 1.0e-30));
        const double dbPerSec  = -dbPerPass / (static_cast<double> (lengths[static_cast<size_t> (line)]) / fs);

        slowest = std::min (slowest, dbPerSec);
        fastest = std::max (fastest, dbPerSec);
    }

    const double ratio = slowest > 0.0 ? fastest / slowest : 1.0e30;

    // (b) the tank's own 8 kHz band: early slope against late slope.
    const auto p    = decayEqParams (decay, 1.0f, highMult, 250.0f, 3500.0f);
    const auto band = bandpassed (renderImpulseResponse (p, fs, 4.0), 8000.0, fs);

    const double early = schroederRt60 (band, fs, -5.0, -20.0);
    const double late  = schroederRt60 (band, fs, -20.0, -35.0);
    const double curvature = (early > 0.0 && late > 0.0) ? std::max (early / late, late / early)
                                                         : 1.0e30;

    check ("T7. per-line 8 kHz rates agree",
           ratio < tolerance && curvature < tolerance,
           fmt ("per-line dB/s %.1f..%.1f  ratio=%.4f", slowest, fastest, ratio)
           + fmt ("  band T60 early=%.3f late=%.3f curvature=%.4f", early, late, curvature));
}

//==================================================================================================
// T8. The DC blocker works, and it is unconditional.
//
// (a) Feed the tank a constant 0.5 with decaylow at its maximum -- the slowest LF decay reachable
//     -- and the mean of its output must sit below -60 dBFS. Without the blocker the loop's DC gain
//     is the low-band gain, which for a long decay is a hair under 1, so DC accumulates towards
//     1/(1-g) instead of cancelling.
// (b) The corner is independent of every parameter. A DC test cannot see that: `x - lp(x)` has a
//     steady-state gain of exactly 0 at DC for ANY corner, so making the blocker follow `lowcut`
//     would leave (a) green. It IS visible just above DC, and only if the attenuation filter is
//     flat -- with both multipliers at 1.0 the crossover positions have no effect whatsoever on the
//     decay law, so any difference in the 20 Hz decay between lowcut = 60 and lowcut = 800 can only
//     have come from the blocker.
//==================================================================================================
void testDcBlocker()
{
    constexpr double fs = 48000.0;

    auto dcMean = [] (float lowMult, float lowCutHz)
    {
        reverb::FDNTank::Params tp;
        tp.decaySeconds  = 4.0f;
        tp.decayLowMult  = lowMult;
        tp.decayHighMult = 1.0f;
        tp.lowCutHz      = lowCutHz;
        tp.dampingHz     = 3500.0f;
        tp.modDepth      = 0.0f;

        reverb::FDNTank tank;
        tank.prepare (fs);
        tank.setParameters (tp, true);

        const int total   = static_cast<int> (5.0 * 4.0 * fs);   // 5 x the decay time
        const int measure = static_cast<int> (0.1 * fs);

        double sumL = 0.0, sumR = 0.0;
        float  peak = 0.0f;

        for (int n = 0; n < total; ++n)
        {
            float outL = 0.0f, outR = 0.0f;
            tank.processSample (0.5f, 0.5f, outL, outR);

            peak = std::max (peak, std::max (std::abs (outL), std::abs (outR)));

            if (n >= total - measure)
            {
                sumL += static_cast<double> (outL);
                sumR += static_cast<double> (outR);
            }
        }

        const double n = static_cast<double> (measure);
        return std::make_pair (std::max (std::abs (sumL / n), std::abs (sumR / n)),
                               static_cast<double> (peak));
    };

    const auto slowest = dcMean (4.0f, 250.0f);
    const auto neutral = dcMean (1.0f, 250.0f);

    // (b) 20 Hz decay must not move with the crossover while the filter is flat.
    auto subDecay = [] (float lowCutHz)
    {
        const auto p = decayEqParams (4.0f, 1.0f, 1.0f, lowCutHz, 3500.0f);
        return schroederRt60 (bandpassed (renderImpulseResponse (p, fs, 8.0), 20.0, fs), fs, -5.0, -25.0);
    };

    const double sub60  = subDecay (60.0f);
    const double sub800 = subDecay (800.0f);
    const double subDelta = sub60 > 0.0 && sub800 > 0.0 ? std::abs (sub60 - sub800) / sub60 : 1.0;

    check ("T8. DC blocker, unconditional",
           slowest.first < 1.0e-3 && neutral.first < 1.0e-3 && subDelta < 0.02,
           fmt ("mean DC out: decaylow=4 %.3e, decaylow=1 %.3e", slowest.first, neutral.first)
           + fmt ("  20 Hz T60 xover 60/800 Hz: %.3f / %.3f s (delta %.2f%%)",
                  sub60, sub800, subDelta * 100.0));
}

//==================================================================================================
// T9. The delay-line interpolator does not eat the high band.
//
// This was informational until the tank moved from DelayLineFrac::read() to readLagrange(). It is
// asserted now because it is the same defect class the fold removes, arriving through another door:
// the interpolator's loss is a fixed dB per PASS, and a pass takes m_i samples, so its dB per
// SECOND goes as 1/m_i. Nothing else in the tank does that any more except the DC blocker, which is
// deliberate and bounded.
//
// Line lengths are primes, so Size 100% and 200% read at integer offsets and interpolate nothing --
// those two are the controls, and they are asserted rather than reported now that the instrument is
// accurate enough to make them worth asserting. Every other Size reads fractionally, and any
// modulation at all does too, including the shipping default modDepth = 25%.
//
// Each leg is measured against the same complete per-line prediction T6 uses (attenuation filter
// plus DC blocker, over the instrument's five slices and every line length), not against the
// nominal 1.5 s: the shelf's asymptote at 8 kHz with the crossover at 1 kHz is 1.51 s, and folding
// that 1% into the budget would be sloppy.
//
// Size 25% gets 8% where the others get 4%, because that is where the residual genuinely is: a
// quarter-length line reads at frac 0.25 or 0.75 AND takes four times as many passes per second, so
// whatever the kernel loses per pass is multiplied by four. Measured -6.6%. Reference numbers with
// the old linear read, same probes: -58% at Size 50%, -20% at modDepth 25%.
//==================================================================================================
void testInterpolationLoss()
{
    constexpr double fs = 48000.0;

    struct Probe { float size, mod; double tolerance; const char* note; };

    for (const Probe& probe : { Probe { 25.0f,  0.0f,   0.08, "frac 0.25/0.75, 4x pass rate" },
                                Probe { 50.0f,  0.0f,   0.04, "frac 0.5, 2x pass rate" },
                                Probe { 100.0f, 0.0f,   0.04, "control, integer reads" },
                                Probe { 200.0f, 0.0f,   0.04, "control, integer reads" },
                                Probe { 100.0f, 25.0f,  0.04, "shipping default modulation" },
                                Probe { 100.0f, 100.0f, 0.04, "maximum modulation" } })
    {
        auto p = decayEqParams (6.0f, 1.0f, 0.25f, 250.0f, 1000.0f);
        p.size     = probe.size;
        p.modDepth = probe.mod;

        const auto   predicted = predictedEnvelope (p, fs, 8000.0);
        const double rt60      = asymptoticBandRt60 (p, fs, 8000.0, 3.4);

        // Signed: negative when the measurement falls SHORT of the prediction, which is the
        // direction an interpolation loss pushes it.
        const double signedError = rt60 <= 0.0 ? -1.0
                                 : (rt60 < predicted.first  ? (rt60 - predicted.first) / predicted.first
                                 : (rt60 > predicted.second ? (rt60 - predicted.second) / predicted.second
                                                            : 0.0));
        const double error = std::abs (signedError);

        const auto label = fmt ("T9. 8 kHz T60, size=%3.0f%% mod=%3.0f%%",
                                static_cast<double> (probe.size), static_cast<double> (probe.mod));

        check (label.c_str(),
               rt60 > 0.0 && error < probe.tolerance,
               fmt ("%.3f s vs predicted %.3f..%.3f s", rt60, predicted.first, predicted.second)
               + fmt (" (%+.1f%% outside, tol %.0f%%)", signedError * 100.0, probe.tolerance * 100.0)
               + "  " + probe.note);
    }
}

//==================================================================================================
// Informational: CPU with the Decay EQ actually engaged.
//
// reportCpu() in ReverbEngineTests measures measurementParams, whose multipliers are pinned flat --
// which takes AttenuationFilter's fast path and so measures the old scalar plus the DC blocker. The
// shipping defaults are 1.4 / 0.7, i.e. four TPT shelf sections per line running all the time, and
// that is the number the CPU ladder has to be judged against.
//==================================================================================================
void reportNonFlatCpu()
{
    for (const bool nonFlat : { false, true })
    {
        Harness h (48000.0, 512, 2);

        auto p = nonFlat ? decayEqParams (2.5f, 1.4f, 0.7f, 3500.0f, 250.0f)
                         : measurementParams (2.5f);
        p.lowCutHz  = 250.0f;
        p.dampingHz = 3500.0f;
        p.modDepth  = 25.0f;
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
                     nonFlat ? "cpu, decay EQ engaged (1.4/0.7)" : "cpu, decay EQ flat (1.0/1.0)",
                     microsPerBlock,
                     microsPerBlock / (512.0 / 48000.0 * 1.0e6) * 100.0);
        std::fflush (stdout);
    }
}


} // namespace

//==================================================================================================

#include "TestSuites.h"

int runDecayTests()
{
    // TestHarness.h's failureCount is shared by every suite in this target: reset on entry, read
    // back at the end, exactly as runEngineTests() does.
    failureCount = 0;

    std::printf ("\nDecay Rate EQ tests\n");
    std::printf ("-------------------\n");

    testBandRt60TracksMultiplier();
    testBroadbandRt60();
    testCompositePassivity();
    testDecayEqStabilitySweep();
    testFlatIsTheScalar();
    testFreezeBypassesFeedbackPath();
    testCurveMatchesMeasurement();
    testPerLineUniformity();
    testDcBlocker();
    testInterpolationLoss();
    reportNonFlatCpu();

    std::printf ("-------------------\n");
    std::printf ("%s (%d failure%s)\n",
                 failureCount == 0 ? "ALL CHECKS PASSED" : "FAILURES",
                 failureCount,
                 failureCount == 1 ? "" : "s");

    return failureCount;
}
