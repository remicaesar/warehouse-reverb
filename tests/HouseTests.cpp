/*
    House workflow -- PLAN-R2 7.4 H1-H6, plus H7. Step 5A: the dry-driven ducker (D5) and
    tempo-synced pre-delay (D6).

    Three of these checks are written differently from the way 7.4 phrases them, in every case
    because the plan's literal version would let its own named mutation survive. Each deviation is
    argued at the check itself:

      - H2 gains a SECOND leg. The plan's version (freeze a loud tail, feed silence, assert no
        ducking) passes trivially on an implementation that switches the ducker off while frozen,
        which is a plausible reading of Freeze and the opposite of what D5 wants. The second leg
        feeds a loud dry signal into the same frozen engine and requires the wet to duck, so the
        pair pins "the detector reads the dry, and it is still live while frozen".
      - H5 measures the pre-delay as a DIFFERENCE of two onsets rather than as an absolute time.
        "First non-zero wet sample at 250 ms" cannot be right to +/-1 ms: the earliest wet path is
        the first early-reflection tap, 4.3-4.9 ms after the pre-delay. Everything downstream is
        identical between the two renders, so the onset difference IS the pre-delay difference, and
        both named mutations still fail.
      - H7 is not in the plan at all; it covers D6's "a division change crossfades, it does not
        glide", which no H check names. Its instrument is the output's zero-crossing rate, because
        the artefact a glide produces is a pitch sweep and not a click -- see the check.

    All timing figures are printed, not just compared, so a later change that keeps a check green
    while moving a time constant is visible in the log.
*/

#include "TestHarness.h"

#include "dsp/Ducker.h"
#include "dsp/TempoSync.h"

#include <cstring>

namespace
{

constexpr double pi = 3.14159265358979323846;

//==================================================================================================
// Shared fixtures.
//==================================================================================================

/** measurementParams with the ducker's own test threshold. -40 dB is the plan's figure for H1/H2:
    against a full-scale source it puts the detector 40 dB over, which is exactly where `over`
    saturates, so the reduction sits at its deepest and the measurement is not reading the knee. */
reverb::ReverbEngine::Params duckParams (float decaySeconds)
{
    auto p = measurementParams (decaySeconds);
    p.duckThresholdDb = -40.0f;
    p.duckReleaseMs   = 250.0f;
    p.duckCharacter   = 0;      // Gentle
    return p;
}

/** Stub host transport. AudioPlayHead lives in juce_audio_basics (C10) and getPosition() is a pure
    virtual, so the whole playhead-facing half of D6 is testable from this DSP-only target -- no
    processor, no host, no juce_audio_processors. */
class StubPlayHead final : public juce::AudioPlayHead
{
public:
    /** Reports a tempo. */
    void setBpm (double bpm)
    {
        juce::AudioPlayHead::PositionInfo info;
        info.setBpm (bpm);
        position = info;
    }

    /** Reports a position that carries no tempo at all -- a real case: some hosts fill in only the
        sample position. */
    void setPositionWithoutTempo()
    {
        juce::AudioPlayHead::PositionInfo info;
        info.setTimeInSamples (0);
        position = info;
    }

    /** Reports nothing, which is what many hosts do while stopped and what an offline render may
        do (D6). */
    void setNoPosition()   { position = juce::Optional<juce::AudioPlayHead::PositionInfo>(); }

    juce::Optional<juce::AudioPlayHead::PositionInfo> getPosition() const override { return position; }

private:
    juce::Optional<juce::AudioPlayHead::PositionInfo> position;
};

double dbOf (double linear)
{
    return 20.0 * std::log10 (std::max (linear, 1.0e-30));
}

//==================================================================================================
// H1. Ducking attenuates the wet.
//
// Sustained full-scale noise, fully wet, measured once every smoother has settled. `duckamt = 0`
// renders the identical input for the comparison, so the only difference between the two numbers is
// the duck gain.
//
// Mutation: apply the duck gain to the dry instead of the wet -> at mix = 100 the dry contributes
// nothing, so the wet comes back un-ducked and the drop collapses to ~0 dB.
//==================================================================================================
double duckedWetRms (float duckAmount, int character)
{
    Harness h (48000.0, 512, 2);

    auto p = duckParams (2.0f);
    p.duckAmount    = duckAmount;
    p.duckCharacter = character;
    h.engine.setParameters (p);

    std::mt19937 rng (0x115Eu);

    const int total       = h.blocksFor (2.0);
    const int measureFrom = h.blocksFor (1.5);

    double sum = 0.0;
    int    count = 0;

    for (int b = 0; b < total; ++b)
    {
        h.fillNoise (rng, 1.0f);
        h.engine.setParameters (p);
        h.processBlock();

        if (b >= measureFrom)
        {
            const double r = h.blockRms();
            sum += r * r;
            ++count;
        }
    }

    return count > 0 ? std::sqrt (sum / static_cast<double> (count)) : 0.0;
}

void testDuckAttenuatesWet()
{
    for (const int character : { 0, 1 })
    {
        const double open   = duckedWetRms (0.0f,   character);
        const double ducked = duckedWetRms (100.0f, character);
        const double dropDb = dbOf (open) - dbOf (ducked);

        const std::string name = std::string ("H1. ducking attenuates the wet (")
                                 + (character == 0 ? "Gentle)" : "Pump)");

        check (name.c_str(),
               dropDb >= 15.0 && open > 1.0e-3,
               fmt ("wet rms %.5f -> %.6f, drop=%.1f dB (bound 15 dB)", open, ducked, dropDb));
    }
}

//==================================================================================================
// H2. The detector reads the DRY, not the wet -- the ducker's defining property.
//
// Every leg builds the SAME tail from the same seeded noise, freezes it, then lets the smoothers
// settle before measuring. While frozen the tank's input gate is closed and Freeze forces the D4
// blend to late-only, so nothing the input does can reach the wet signal (step 2B's E7 proved that
// as bit-identity). The output can therefore differ between these legs through the duck gain and
// nothing else.
//
//   leg A  duckamt = 0,   silence in   -- the reference tail
//   leg B  duckamt = 100, silence in   -- must match A within 0.5 dB: a WET-driven detector would
//                                         hear the loud tail and duck it
//   leg C  duckamt = 100, full-scale dry in -- must drop >= 15 dB
//
// Leg C is not in the plan and the check is worth little without it: without C, an implementation
// that simply disables ducking while frozen passes H2 outright. It also states the Freeze ruling
// for this feature -- ducking stays fully live while frozen. That is deliberate: the ducker is
// driven by the dry signal, which still exists while frozen, its gain is applied to the wet OUTPUT
// rather than inside the feedback loop (so unlike D9's shimmer it cannot affect the frozen loop's
// gain), and "an infinite pad that gets out of the way of the kick" is the house gesture Freeze
// plus ducking exists for. Contrast the erMix override, which had to happen because a frozen early
// cluster has no material to hold; the ducker has both its source and its target.
//
// Mutation: feed the detector from the wet -> leg B ducks the frozen tail and fails.
//==================================================================================================
double frozenTailRms (float duckAmount, bool loudInputWhileFrozen)
{
    Harness h (48000.0, 512, 2);

    auto p = duckParams (4.0f);
    p.duckAmount = 0.0f;   // identical tank state in every leg: no ducking during the fill
    h.engine.setParameters (p);

    std::mt19937 rng (0x7A11u);

    for (int b = 0; b < h.blocksFor (1.0); ++b)
    {
        h.fillNoise (rng, 0.5f);
        h.engine.setParameters (p);
        h.processBlock();
    }

    p.freeze = true;

    for (int b = 0; b < h.blocksFor (0.5); ++b)   // let the freeze crossfade settle
    {
        h.fillSilence();
        h.engine.setParameters (p);
        h.processBlock();
    }

    p.duckAmount = duckAmount;

    for (int b = 0; b < h.blocksFor (0.4); ++b)   // let the duck attack settle
    {
        if (loudInputWhileFrozen)
            h.fillNoise (rng, 1.0f);
        else
            h.fillSilence();

        h.engine.setParameters (p);
        h.processBlock();
    }

    double sum = 0.0;
    int    count = 0;

    for (int b = 0; b < h.blocksFor (0.5); ++b)
    {
        if (loudInputWhileFrozen)
            h.fillNoise (rng, 1.0f);
        else
            h.fillSilence();

        h.engine.setParameters (p);
        h.processBlock();

        const double r = h.blockRms();
        sum += r * r;
        ++count;
    }

    return count > 0 ? std::sqrt (sum / static_cast<double> (count)) : 0.0;
}

void testDetectorReadsDry()
{
    const double reference = frozenTailRms (0.0f,   false);
    const double silent    = frozenTailRms (100.0f, false);
    const double driven    = frozenTailRms (100.0f, true);

    const double silentErrorDb = std::abs (dbOf (silent) - dbOf (reference));
    const double drivenDropDb  = dbOf (reference) - dbOf (driven);

    check ("H2. detector reads the dry, not the wet",
           silentErrorDb <= 0.5 && reference > 1.0e-3,
           fmt ("frozen tail rms %.5f, silence + duckamt 100 -> %.5f, error=%.3f dB (bound 0.5)",
                reference, silent, silentErrorDb));

    check ("H2b. and it still ducks while frozen",
           drivenDropDb >= 15.0,
           fmt ("same frozen tail, full-scale dry in -> rms %.6f, drop=%.1f dB (bound 15 dB)",
                driven, drivenDropDb));
}

//==================================================================================================
// H3. Release is the time requested, and character sets the attack.
//
// Measured on the Ducker directly: a DC step to full scale, held until the reduction settles, then
// removed. Attack = time to 63 % of the settled reduction, release = time back to 37 % of it. Both
// are read off getReductionDb(), so this also pins the value the GUI indicator will show.
//
// H3 IS A CHECK AT ONE POINT, AND SAYS SO IN ITS NAME. The plan's +/-30 % holds at duckrelease's
// shipping default of 250 ms and does NOT hold across the range -- at 50 ms and the deepest
// reduction the ducker can produce the measured release is 103 ms, 2.1x the knob. H3d and H3e below
// carry the rest of the range: H3d against a release time COMPUTED from the structure, H3e against
// a stated bound on how far the structure can be from the knob. This file used to print those legs
// as INFO, which is the same as not checking them.
//
// The bounds are the SPEC's numbers with a stated allowance, not the measurement's:
//   - release within +/-30 % of duckrelease at 250 ms, both characters (the plan's own bound);
//   - Pump attack 2 ms +/-50 %, Gentle attack 30 ms +/-50 % (D5's two figures);
//   - and the ratio between them at least 5x.
// The ratio bound is what catches the plan's second mutation ("make duckchar change only the
// knee"): with the attack selection removed the two characters still differ in DETECTOR, which on
// its own moves the measured attack by a factor of ~2, so a check that only asserted "they differ"
// would survive it.
//==================================================================================================
struct DuckerTiming
{
    double attackMs = 0.0, releaseMs = 0.0, settledDb = 0.0;
};

DuckerTiming measureDuckerTiming (reverb::Ducker::Character character, float releaseMs,
                                  float thresholdDb = -40.0f, double fs = 48000.0)
{
    reverb::Ducker ducker;
    ducker.prepare (fs);

    reverb::Ducker::Params p;
    p.amountPercent = 100.0f;
    p.thresholdDb   = thresholdDb;
    p.releaseMs     = releaseMs;
    p.character     = character;
    ducker.setParameters (p);

    const int holdSamples    = static_cast<int> (fs * 1.5);
    const int releaseSamples = static_cast<int> (fs * 3.0);

    std::vector<float> attackTrace (static_cast<size_t> (holdSamples)),
                       releaseTrace (static_cast<size_t> (releaseSamples));

    for (int n = 0; n < holdSamples; ++n)
    {
        ducker.processSample (1.0f);
        attackTrace[static_cast<size_t> (n)] = ducker.getReductionDb();
    }

    const double settled = attackTrace.back();

    for (int n = 0; n < releaseSamples; ++n)
    {
        ducker.processSample (0.0f);
        releaseTrace[static_cast<size_t> (n)] = ducker.getReductionDb();
    }

    DuckerTiming timing;
    timing.settledDb = settled;

    const double toMs = 1000.0 / fs;

    for (size_t n = 0; n < attackTrace.size(); ++n)
        if (attackTrace[n] <= 0.63 * settled)
        {
            timing.attackMs = static_cast<double> (n + 1) * toMs;
            break;
        }

    for (size_t n = 0; n < releaseTrace.size(); ++n)
        if (releaseTrace[n] >= 0.37 * settled)
        {
            timing.releaseMs = static_cast<double> (n + 1) * toMs;
            break;
        }

    return timing;
}

//==================================================================================================
// H3d. The release LAW, computed rather than sampled at one point.
//
// The whole derivation, all of it from Ducker.h's published law and nothing tuned:
//
//   1. The dry step is removed at t = 0, so the detector starts decaying with time constant
//      Ducker::envelopeTauMs (release, character) -- 20 ms for Gentle at any release from 40 ms up,
//      because Gentle's follower is mean-square and the envelope is its square root. An exponential
//      decay is a STRAIGHT LINE in dB, walking down at (20 / ln 10) / tau: 0.434 dB/ms for Gentle,
//      0.869 for Pump.
//   2. `over` -- and therefore the target reduction -- is that line, clamped at both ends. It holds
//      at the settled depth while the overshoot is still above Ducker::maxOverDb (a dead time, and
//      only for thresholds below -40 dBFS), then walks up to 0 at the same dB/ms, then stays there.
//      A clamped ramp is one ramp minus the same ramp delayed by depth / rate, which is what makes
//      the next step closed form.
//   3. The gain pole then filters that target: one pole of Ducker::poleTauMs for Pump, two cascaded
//      for Gentle. The response of n identical cascaded one-poles to a ramp is closed form, so the
//      entire recovery curve is, and the 37 % crossing is one bisection away.
//
// This bounds the MECHANISM rather than a point: the check goes red if the mean-square/sqrt pairing
// changes, if the pole count per character changes, or if any of the three time constants is fed to
// the wrong stage -- each of which changes the measured release without changing the requested one,
// which is exactly the class of change a +/-30 % tolerance at 250 ms cannot see. What it does NOT
// see is a change to one of Ducker.h's published constants, because the prediction is computed from
// those constants and moves with them; H3 and H3e are what hold those, and they have literal bounds
// for that reason. The mutation list below draws the line with measured numbers.
//
// Residual of the model against the implementation, swept over release 20..1000 ms x threshold
// -60..-4 dB x 44.1/48/96/192 kHz x both characters: worst 2.2 %, at the -4 dB corner where the
// 6 dB soft knee the model ignores is most of the reduction. Over the twelve legs asserted here it
// is at most 0.6 %, so the bound is 5 %.
//
// Four mutations, all RUN, with the measured worst leg:
//
//   1. Feed the detector the release time constant instead of detectorTauMs (the cascade the class
//      comment warns about) -> H3d fails at 467 %, H3 at 573 % (Gentle 250 ms measures 1683 ms),
//      H3e on all four of its bounds.
//   2. Drop the twoPoleTimeScale scaling from releaseCoeff -> H3d fails at 113 %, H3 at 133 %.
//   3. Drop the sqrt from Gentle's mean-square follower, i.e. let the envelope BE the mean square
//      -> H3d fails at 27 % and NOTHING ELSE IN THE SUITE NOTICES: H1, H2, H2b, H3, H3b and H3e all
//      stay green, because the settled depth, the attack and the 250 ms release are all unchanged
//      and only the walk-down rate doubles. That mutation is the reason this check exists.
//   4. Raise detectorMaxMs from 10 ms to 40 ms -> H3d PASSES at 0.73 %, as designed: it is a
//      published constant, so the prediction follows it. H3 fails at 83 % and H3e fails at 1.83x on
//      its >= 250 ms bound.
//==================================================================================================

/** Response, in dB, of `poles` cascaded identical one-poles of `tauMs` to a ramp of slope
    `rateDbPerMs` starting at t = 0. Both are the standard closed forms; the two-pole one is the
    integral of the same 1 - (1 + x) e^-x step response Ducker.h's second bullet quotes. */
double rampResponseDb (double tMs, double rateDbPerMs, double tauMs, int poles)
{
    if (tMs <= 0.0)
        return 0.0;

    const double decay = std::exp (-tMs / tauMs);

    return poles == 2 ? rateDbPerMs * (tMs - 2.0 * tauMs + (2.0 * tauMs + tMs) * decay)
                      : rateDbPerMs * (tMs - tauMs + tauMs * decay);
}

/** The reduction the ducker settles at under a full-scale dry signal at amount 100: the overshoot
    over the threshold, saturated at Ducker::maxOverDb. Valid while the overshoot is clear of the
    soft knee's upper edge, which every leg below is. */
double predictedDuckDepthDb (float thresholdDb)
{
    return std::min (static_cast<double> (reverb::Ducker::maxOverDb),
                     -static_cast<double> (thresholdDb));
}

/** The release time the structure predicts -- time for the reduction to recover to 37 % of its
    settled depth once a full-scale dry step is removed. See the block comment above. */
double predictedDuckReleaseMs (reverb::Ducker::Character character, float releaseMs,
                              float thresholdDb)
{
    constexpr double dbPerNeper = 8.685889638065035;   // 20 / ln 10

    const int    poles   = character == reverb::Ducker::Character::gentle ? 2 : 1;
    const double rate    = dbPerNeper / reverb::Ducker::envelopeTauMs (releaseMs, character);
    const double tauPole = reverb::Ducker::poleTauMs (releaseMs, character);

    const double depthDb = predictedDuckDepthDb (thresholdDb);
    const double deadMs  = std::max (0.0, -static_cast<double> (thresholdDb) - depthDb) / rate;
    const double rampMs  = depthDb / rate;
    const double wantDb  = 0.63 * depthDb;   // 37 % of the way back down IS 63 % recovered

    // The recovery is strictly increasing in t (the difference of two shifted ramp responses of a
    // cascade whose own step response is monotonic), so a bisection cannot land on a false root.
    double lo = 0.0, hi = deadMs + rampMs + 24.0 * tauPole + 100.0;

    for (int i = 0; i < 120; ++i)
    {
        const double mid = 0.5 * (lo + hi);
        const double recovered = rampResponseDb (mid - deadMs,          rate, tauPole, poles)
                                 - rampResponseDb (mid - deadMs - rampMs, rate, tauPole, poles);

        if (recovered < wantDb)
            lo = mid;
        else
            hi = mid;
    }

    return 0.5 * (lo + hi);
}

void testDuckerTiming()
{
    const auto gentle = measureDuckerTiming (reverb::Ducker::Character::gentle, 250.0f);
    const auto pump   = measureDuckerTiming (reverb::Ducker::Character::pump,   250.0f);

    const double gentleReleaseError = std::abs (gentle.releaseMs - 250.0) / 250.0;
    const double pumpReleaseError   = std::abs (pump.releaseMs   - 250.0) / 250.0;

    check ("H3. release = duckrelease +/-30% at 250 ms",
           gentleReleaseError <= 0.30 && pumpReleaseError <= 0.30,
           fmt ("requested 250 ms: Gentle %.1f ms (%.1f%%), Pump %.1f ms",
                gentle.releaseMs, gentleReleaseError * 100.0, pump.releaseMs)
           + fmt (" (%.1f%%)", pumpReleaseError * 100.0));

    const double ratio = pump.attackMs > 0.0 ? gentle.attackMs / pump.attackMs : 0.0;

    check ("H3b. character sets the attack",
           pump.attackMs   >= 1.0  && pump.attackMs   <= 3.0
           && gentle.attackMs >= 15.0 && gentle.attackMs <= 45.0
           && ratio >= 5.0,
           fmt ("attack to 63%%: Pump %.2f ms (spec 2), Gentle %.2f ms (spec 30), ratio=%.1fx",
                pump.attackMs, gentle.attackMs, ratio));

    // ---- H3d: every leg asserted against the computed law, none of them printf-only -------------
    // The three releases are the bottom, the default and the top of duckrelease's range, crossed
    // with the deepest reduction the ducker can produce (40 dB, the D5 `over` cap) and a musical
    // 12 dB -- which is the crossing that shows the excess over the knob is the DEPTH and not the
    // release law. Both characters, because the depth term differs between them by exactly the
    // factor of two in envelopeTauMs.
    struct LawLeg { reverb::Ducker::Character character; float releaseMs, thresholdDb; };

    double      worstLawErrorPercent = 0.0, worstDepthErrorDb = 0.0;
    std::string worstLawLeg;

    for (const LawLeg leg : { LawLeg { reverb::Ducker::Character::gentle,   50.0f, -40.0f },
                              LawLeg { reverb::Ducker::Character::gentle,  250.0f, -40.0f },
                              LawLeg { reverb::Ducker::Character::gentle, 1000.0f, -40.0f },
                              LawLeg { reverb::Ducker::Character::gentle,   50.0f, -12.0f },
                              LawLeg { reverb::Ducker::Character::gentle,  250.0f, -12.0f },
                              LawLeg { reverb::Ducker::Character::gentle, 1000.0f, -12.0f },
                              LawLeg { reverb::Ducker::Character::pump,     50.0f, -40.0f },
                              LawLeg { reverb::Ducker::Character::pump,    250.0f, -40.0f },
                              LawLeg { reverb::Ducker::Character::pump,   1000.0f, -40.0f },
                              LawLeg { reverb::Ducker::Character::pump,     50.0f, -12.0f },
                              LawLeg { reverb::Ducker::Character::pump,    250.0f, -12.0f },
                              LawLeg { reverb::Ducker::Character::pump,   1000.0f, -12.0f } })
    {
        const bool isGentle = leg.character == reverb::Ducker::Character::gentle;

        const auto   measured  = measureDuckerTiming (leg.character, leg.releaseMs, leg.thresholdDb);
        const double predicted = predictedDuckReleaseMs (leg.character, leg.releaseMs, leg.thresholdDb);
        const double depthDb   = predictedDuckDepthDb (leg.thresholdDb);

        const double errorPercent = std::abs (measured.releaseMs - predicted) / predicted * 100.0;
        const double depthErrorDb = std::abs (measured.settledDb + depthDb);

        const std::string label = std::string (isGentle ? "Gentle" : "Pump")
                                  + fmt (" %.0f ms, %.0f dB deep", static_cast<double> (leg.releaseMs),
                                         depthDb);

        if (errorPercent > worstLawErrorPercent)
        {
            worstLawErrorPercent = errorPercent;
            worstLawLeg          = label;
        }

        worstDepthErrorDb = std::max (worstDepthErrorDb, depthErrorDb);

        std::printf ("[INFO] %-34s  measured %7.1f ms  predicted %7.1f ms  (model %+.2f%%,"
                     " knob %+.1f%%)\n",
                     label.c_str(), measured.releaseMs, predicted, errorPercent,
                     (measured.releaseMs - static_cast<double> (leg.releaseMs))
                         / static_cast<double> (leg.releaseMs) * 100.0);
    }

    std::fflush (stdout);

    check ("H3d. release matches the computed release law",
           worstLawErrorPercent <= 5.0 && worstDepthErrorDb <= 0.05,
           fmt ("12 legs, 50/250/1000 ms x 40/12 dB x both characters: worst %.2f%% (bound 5%%) @ ",
                worstLawErrorPercent)
           + worstLawLeg
           + fmt ("; worst settled depth error %.3f dB (bound 0.05)", worstDepthErrorDb));

    //==============================================================================================
    // H3e. The honest limit of "release = the number on the knob", stated as a bound.
    //
    // H3d says the release is the time the structure predicts. This says how far that can be from
    // the time the user asked for, so the worst case cannot get worse in silence. All of the excess
    // is step 1 of H3d's derivation -- the envelope has to walk the overshoot back down at a fixed
    // dB/ms before the release pole can finish -- so it is worst at the SHORTEST release and the
    // DEEPEST reduction, and it is a fixed number of milliseconds rather than a percentage, which is
    // why the percentage grows as the knob comes down.
    //
    // This is a musical limit, not a rounding. At 128 BPM a 16th note is 117 ms, so duckrelease at
    // 50 ms with Duck Amount 100 and a low threshold -- the pumping setting a house producer reaches
    // for -- recovers in 103 ms on Gentle and 75 ms on Pump, so on Gentle the wet is still
    // substantially ducked when the next 16th lands. It is asserted and
    // documented rather than fixed: the detector's 10 ms floor exists to stop it rippling at a 50 Hz
    // kick's fundamental, and trading that for release accuracy is a sonic decision, not a bug fix.
    //
    // Bounds, from the measured worst cases with a stated allowance:
    //   - at the deepest reduction, never more than 2.5x the knob (measured worst 2.36x, Gentle at
    //     20 ms, the bottom of the range);
    //   - at a musical 12 dB, never more than 1.5x (measured worst 1.37x, same leg);
    //   - from 250 ms up, inside the plan's own +/-30 % at BOTH depths (measured worst 1.19x);
    //   - and never SHORT, because the walk-down can only add time -- a release that measured short
    //     would be a different bug. The bound is 0.99 rather than 1.00 for one measured case: Pump
    //     at 1000 ms and 12 dB comes in at 0.9998x, because there the target ramp is 14 ms against a
    //     1000 ms pole, so the pole gets a head start of about that much on its own step response.
    //==============================================================================================
    double worstDeepRatio = 0.0, worstShallowRatio = 0.0, worstLongRatio = 0.0, worstShortRatio = 9.9;

    std::string worstDeepLeg;

    for (const auto character : { reverb::Ducker::Character::gentle, reverb::Ducker::Character::pump })
        for (const float requested : { 20.0f, 50.0f, 100.0f, 250.0f, 500.0f, 1000.0f })
            for (const float thresholdDb : { -40.0f, -12.0f })
            {
                const auto   measured  = measureDuckerTiming (character, requested, thresholdDb);
                const double overshoot = measured.releaseMs / static_cast<double> (requested);

                if (thresholdDb <= -40.0f)
                {
                    if (overshoot > worstDeepRatio)
                    {
                        worstDeepRatio = overshoot;
                        worstDeepLeg   = std::string (character == reverb::Ducker::Character::gentle
                                                          ? "Gentle" : "Pump")
                                         + fmt (" %.0f ms", static_cast<double> (requested));
                    }
                }
                else
                {
                    worstShallowRatio = std::max (worstShallowRatio, overshoot);
                }

                if (requested >= 250.0f)
                    worstLongRatio = std::max (worstLongRatio, overshoot);

                worstShortRatio = std::min (worstShortRatio, overshoot);
            }

    check ("H3e. release overshoot over the knob is bounded",
           worstDeepRatio <= 2.5 && worstShallowRatio <= 1.5 && worstLongRatio <= 1.30
           && worstShortRatio >= 0.99,
           fmt ("20..1000 ms x both characters: deepest %.2fx (bound 2.5) @ ", worstDeepRatio)
           + worstDeepLeg
           + fmt ("; 12 dB %.2fx (bound 1.5), >=250 ms %.2fx (bound 1.30), shortest %.4fx (bound 0.99)",
                  worstShallowRatio, worstLongRatio, worstShortRatio));

    // Every coefficient is exp (-1 / (tau * fs)), so a rate-independence check is the cheap guard
    // against the classic version of getting that wrong. The suite-level stability sweep runs at
    // four rates but leaves duckamt at 0, so nothing else in the target exercises the ducker away
    // from 48 kHz.
    double worstAttackErrorPercent = 0.0, worstReleaseErrorPercent = 0.0;
    double worstRate = 0.0;

    for (const double rate : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        const auto timing = measureDuckerTiming (reverb::Ducker::Character::gentle, 250.0f,
                                                 -40.0f, rate);

        const double attackError  = std::abs (timing.attackMs - gentle.attackMs)
                                    / gentle.attackMs * 100.0;
        const double releaseError = std::abs (timing.releaseMs - gentle.releaseMs)
                                    / gentle.releaseMs * 100.0;

        if (attackError > worstAttackErrorPercent || releaseError > worstReleaseErrorPercent)
            worstRate = rate;

        worstAttackErrorPercent  = std::max (worstAttackErrorPercent, attackError);
        worstReleaseErrorPercent = std::max (worstReleaseErrorPercent, releaseError);
    }

    check ("H3c. time constants are rate-independent",
           worstAttackErrorPercent <= 2.0 && worstReleaseErrorPercent <= 2.0,
           fmt ("44.1/48/96/192 kHz vs the 48 kHz figures: worst attack %.2f%%, release %.2f%%",
                worstAttackErrorPercent, worstReleaseErrorPercent)
           + fmt (" (bound 2%%) @ %.0f Hz", worstRate));

    std::fflush (stdout);
}

//==================================================================================================
// H4. duckamt = 0 returns exactly 1.0f.
//
// Bit-compared, not compared with a tolerance, and measured on a ducker whose state has just been
// driven deep -- returning to zero must be a hard bypass, not a smoothed approach to unity. Check 2
// (mix = 0 bit-identical) is the cross-guard the plan names; this is the direct assertion.
//
// Mutation: return 0.9999f -> fails here, and check 2 fails too.
//==================================================================================================
void testDuckAmountZeroIsUnity()
{
    const float one = 1.0f;

    reverb::Ducker ducker;
    ducker.prepare (48000.0);

    reverb::Ducker::Params p;
    p.amountPercent = 100.0f;
    p.thresholdDb   = -40.0f;
    ducker.setParameters (p);

    for (int n = 0; n < 24000; ++n)
        ducker.processSample (1.0f);

    const double deepDb = ducker.getReductionDb();

    p.amountPercent = 0.0f;
    ducker.setParameters (p);

    int nonUnity = 0;

    for (int n = 0; n < 48000; ++n)
    {
        const float g = ducker.processSample (n % 2 == 0 ? 1.0f : -0.75f);

        if (std::memcmp (&g, &one, sizeof (float)) != 0)
            ++nonUnity;
    }

    // A fresh, never-engaged ducker too, since that is the shipping default.
    reverb::Ducker fresh;
    fresh.prepare (48000.0);
    fresh.setParameters (reverb::Ducker::Params {});

    for (int n = 0; n < 4800; ++n)
    {
        const float g = fresh.processSample (1.0f);

        if (std::memcmp (&g, &one, sizeof (float)) != 0)
            ++nonUnity;
    }

    check ("H4. duckamt=0 is exactly 1.0f",
           nonUnity == 0 && deepDb < -20.0,
           fmt ("non-unity gains=%.0f of 52800 (state was %.1f dB deep, reduction now %.1f dB)",
                static_cast<double> (nonUnity), deepDb,
                static_cast<double> (ducker.getReductionDb())));
}

//==================================================================================================
// H5/H6. Synced pre-delay.
//
// The onset scan finds the first wet sample above a floor 4 decades under the impulse. Fully wet, so
// the dry impulse itself is not in the output; erMix = 100 so the earliest wet path is the first ER
// tap, a few ms after the pre-delay, which cancels in the difference.
//==================================================================================================
int wetOnsetSamples (const reverb::ReverbEngine::Params& params)
{
    Harness h (48000.0, 512, 2);
    h.engine.setParameters (params);

    const int totalBlocks = h.blocksFor (1.2);

    for (int b = 0; b < totalBlocks; ++b)
    {
        h.fillSilence();

        if (b == 0)
        {
            h.left[0]  = 1.0f;
            h.right[0] = 1.0f;
        }

        h.engine.setParameters (params);
        h.processBlock();

        for (int n = 0; n < h.blockSize; ++n)
        {
            const auto sn = static_cast<size_t> (n);

            if (std::abs (h.left[sn]) > 1.0e-4f || std::abs (h.right[sn]) > 1.0e-4f)
                return b * h.blockSize + n;
        }
    }

    return -1;
}

void testSyncedPreDelayLength()
{
    reverb::temposync::PreDelaySync sync;

    StubPlayHead playHead;
    playHead.setBpm (120.0);
    sync.updateFromPlayHead (&playHead);

    // 1/8 at 120 bpm: half a beat, 500 ms to the beat -> 250 ms. The typed 40 ms must be ignored.
    const auto division = reverb::temposync::divisionForIndex (5);
    const float syncedMs = sync.preDelayMsFor (true, division, 40.0f);

    auto p = duckParams (2.0f);
    p.erMix = 100.0f;

    p.preDelaySync     = false;
    p.preDelayDivision = 5;
    p.preDelayMs       = 0.0f;
    const int zeroOnset = wetOnsetSamples (p);

    p.preDelaySync = true;
    p.preDelayMs   = syncedMs;
    const int syncedOnset = wetOnsetSamples (p);

    const double measuredMs = static_cast<double> (syncedOnset - zeroOnset) / 48.0;

    check ("H5. synced pre-delay = 250 ms @ 120 bpm",
           std::abs (static_cast<double> (syncedMs) - 250.0) < 1.0e-3
           && zeroOnset >= 0 && syncedOnset >= 0
           && std::abs (measuredMs - 250.0) <= 1.0,
           fmt ("division 1/8 -> %.3f ms; onset %.0f -> %.0f samples",
                static_cast<double> (syncedMs),
                static_cast<double> (zeroOnset), static_cast<double> (syncedOnset))
           + fmt (" = %.3f ms of pre-delay (bound 250 +/-1 ms)", measuredMs));

    // The whole division list, against D6's own table written out as LITERALS -- 500 ms to the beat
    // at 120 bpm, the last two clamped at the pre-delay parameter's 500 ms ceiling. Deriving the
    // expectation from beatsFor() instead (the obvious way to write this) would make the check
    // tautological: the plan's second H5 mutation is exactly a wrong entry in that table, and both
    // sides of the comparison would move together.
    struct Expected { const char* name; double ms; };

    const Expected expected[]
    {
        { "1/32",   62.5 },        { "1/16T",  500.0 / 6.0 },  { "1/16",  125.0 },
        { "1/8T",  500.0 / 3.0 },  { "1/16.", 187.5 },         { "1/8",   250.0 },
        { "1/4T",  1000.0 / 3.0 }, { "3/16",  375.0 },         { "1/4",   500.0 },
        { "1/2",   500.0 },        { "1/1",   500.0 }
    };

    const int divisionCount = static_cast<int> (reverb::temposync::Division::count);

    double      worstErrorMs = 0.0;
    std::string worstName;
    int         nameMismatches = 0;

    for (int i = 0; i < divisionCount && i < static_cast<int> (std::size (expected)); ++i)
    {
        const auto   d      = reverb::temposync::divisionForIndex (i);
        const double actual = static_cast<double> (sync.preDelayMsFor (true, d, 40.0f));
        const double error  = std::abs (actual - expected[static_cast<size_t> (i)].ms);

        if (std::string (reverb::temposync::nameFor (d)) != expected[static_cast<size_t> (i)].name)
            ++nameMismatches;

        if (error > worstErrorMs)
        {
            worstErrorMs = error;
            worstName    = expected[static_cast<size_t> (i)].name;
        }
    }

    check ("H5b. all 11 divisions, clamped at 500 ms",
           divisionCount == static_cast<int> (std::size (expected))
           && worstErrorMs < 1.0e-3 && nameMismatches == 0,
           fmt ("divisions=%.0f  worst error %.6f ms", static_cast<double> (divisionCount),
                worstErrorMs)
           + (worstName.empty() ? std::string() : (" @ " + worstName))
           + fmt ("  name mismatches=%.0f", static_cast<double> (nameMismatches)));
}

void testNoTempoFallback()
{
    const auto division = reverb::temposync::divisionForIndex (5);

    // No playhead at all -- an offline render, or a host that never provides one.
    reverb::temposync::PreDelaySync noHost;
    const float noPlayHeadMs = noHost.preDelayMsFor (true, division, 40.0f);

    // A playhead that reports nothing.
    reverb::temposync::PreDelaySync stopped;
    StubPlayHead silentHead;
    silentHead.setNoPosition();
    stopped.updateFromPlayHead (&silentHead);
    const float noPositionMs = stopped.preDelayMsFor (true, division, 40.0f);

    // A playhead that reports a position carrying no tempo.
    reverb::temposync::PreDelaySync untimed;
    StubPlayHead untimedHead;
    untimedHead.setPositionWithoutTempo();
    untimed.updateFromPlayHead (&untimedHead);
    const float noBpmMs = untimed.preDelayMsFor (true, division, 40.0f);

    check ("H6. no tempo falls back to the free ms",
           std::abs (noPlayHeadMs - 40.0f) < 1.0e-3f
           && std::abs (noPositionMs - 40.0f) < 1.0e-3f
           && std::abs (noBpmMs - 40.0f) < 1.0e-3f,
           fmt ("predelay 40 ms with pdsync on: no playhead %.3f, no position %.3f, no bpm %.3f",
                static_cast<double> (noPlayHeadMs),
                static_cast<double> (noPositionMs),
                static_cast<double> (noBpmMs)));

    // D6's intermittent-host case: once a tempo has been seen, losing it must NOT move the
    // pre-delay -- not to the free value and certainly not to zero.
    reverb::temposync::PreDelaySync intermittent;
    StubPlayHead host;
    host.setBpm (124.0);
    intermittent.updateFromPlayHead (&host);
    const float playing = intermittent.preDelayMsFor (true, division, 40.0f);

    host.setNoPosition();
    intermittent.updateFromPlayHead (&host);
    const float stoppedMs = intermittent.preDelayMsFor (true, division, 40.0f);

    // And sync off is the typed value at any tempo.
    const float syncOffMs = intermittent.preDelayMsFor (false, division, 40.0f);

    check ("H6b. a known tempo survives losing it",
           std::abs (stoppedMs - playing) < 1.0e-3f
           && stoppedMs > 1.0f
           && std::abs (syncOffMs - 40.0f) < 1.0e-3f,
           fmt ("124 bpm 1/8 -> %.3f ms, transport stops -> %.3f ms, sync off -> %.3f ms",
                static_cast<double> (playing), static_cast<double> (stoppedMs),
                static_cast<double> (syncOffMs)));
}

//==================================================================================================
// H7. A division change crossfades; it does not glide (D6). NOT one of the plan's H checks.
//
// The artefact a glide produces is not a click, so a click test cannot see it. Gliding 250 ms of
// delay to 125 ms over the 50 ms smoothing window moves the read position at 2.5x real time, i.e.
// backwards through the buffer faster than time advances, which is a large FREQUENCY sweep. So the
// instrument here is the output's zero-crossing rate through the transition, with a click bound
// alongside it.
//
// The tone is 402 Hz for a specific reason. Everything from the pre-delay onward is LTI and static
// (modulation off), so a single input frequency stays a single output frequency and zero-crossing
// counting is exact. 402 Hz x 125 ms = 50.25 cycles, so the two read positions sit a QUARTER cycle
// apart: far enough from anti-phase that the crossfade cannot null (its worst dip is 0.707 over
// 30 ms), and far enough from in-phase that a hard jump between the two positions really does step
// the waveform -- at a multiple of 8 Hz the two reads would be IDENTICAL and this test would pass on
// an implementation with no crossfade at all.
//
// Mutations: (a) drop the discrete-change detection so the position glides -> the frequency
// assertion fails; (b) jump the read position with no crossfade -> the click assertion fails.
//==================================================================================================
struct TransitionMeasurement
{
    double frequencyHz = 0.0, maxDelta = 0.0, peak = 0.0;
};

TransitionMeasurement measureDivisionChange (bool changeDivision)
{
    const double fs        = 48000.0;
    const int    blockSize = 512;
    const double toneHz    = 402.0;

    reverb::temposync::PreDelaySync sync;
    StubPlayHead playHead;
    playHead.setBpm (120.0);
    sync.updateFromPlayHead (&playHead);

    Harness h (fs, blockSize, 2);

    auto p = duckParams (2.0f);
    p.erMix            = 100.0f;   // the early cluster, so the pre-delayed signal dominates
    p.modDepth         = 0.0f;     // the tank's own modulation is the only other time variance
    p.preDelaySync     = true;
    p.preDelayDivision = 5;        // 1/8 = 250 ms
    p.preDelayMs       = sync.preDelayMsFor (true, reverb::temposync::divisionForIndex (5), 10.0f);
    h.engine.setParameters (p);

    const int totalBlocks = h.blocksFor (1.4);
    const int changeBlock = h.blocksFor (0.7);

    std::vector<float> out;
    out.reserve (static_cast<size_t> (totalBlocks * blockSize));

    double phase = 0.0;
    const double increment = 2.0 * pi * toneHz / fs;

    for (int b = 0; b < totalBlocks; ++b)
    {
        for (int n = 0; n < blockSize; ++n)
        {
            const auto s = static_cast<float> (std::sin (phase));
            phase += increment;

            h.left[static_cast<size_t> (n)]  = s;
            h.right[static_cast<size_t> (n)] = s;
        }

        if (b == changeBlock && changeDivision)
        {
            p.preDelayDivision = 2;   // 1/16 = 125 ms
            p.preDelayMs = sync.preDelayMsFor (true, reverb::temposync::divisionForIndex (2), 10.0f);
        }

        h.engine.setParameters (p);
        h.processBlock();

        out.insert (out.end(), h.left.begin(), h.left.end());
    }

    // 60 ms from the change: covers both the 30 ms crossfade and the 50 ms glide it replaces.
    const size_t from   = static_cast<size_t> (changeBlock * blockSize);
    const size_t window  = static_cast<size_t> (0.06 * fs);
    const size_t to     = std::min (from + window, out.size());

    TransitionMeasurement m;
    int crossings = 0;

    for (size_t n = from + 1; n < to; ++n)
    {
        const float previous = out[n - 1];
        const float current  = out[n];

        if ((previous <= 0.0f && current > 0.0f) || (previous >= 0.0f && current < 0.0f))
            ++crossings;

        m.maxDelta = std::max (m.maxDelta, static_cast<double> (std::abs (current - previous)));
        m.peak     = std::max (m.peak, static_cast<double> (std::abs (current)));
    }

    m.frequencyHz = 0.5 * static_cast<double> (crossings) / (static_cast<double> (to - from) / fs);

    return m;
}

void testDivisionChangeCrossfades()
{
    const auto control    = measureDivisionChange (false);
    const auto transition = measureDivisionChange (true);

    const double frequencyError = std::abs (transition.frequencyHz - control.frequencyHz)
                                  / std::max (control.frequencyHz, 1.0);
    const double deltaRatio = transition.maxDelta / std::max (control.maxDelta, 1.0e-12);

    check ("H7. division change does not pitch-bend",
           control.peak > 1.0e-3 && frequencyError <= 0.05,
           fmt ("402 Hz in: steady %.1f Hz, through the change %.1f Hz (%.2f%% error, bound 5%%)",
                control.frequencyHz, transition.frequencyHz, frequencyError * 100.0));

    check ("H7b. division change does not click",
           deltaRatio <= 3.0,
           fmt ("max |sample step| %.3e -> %.3e = %.2fx (bound 3x)",
                control.maxDelta, transition.maxDelta, deltaRatio)
           + fmt ("  window peak %.4f", transition.peak));
}

//==================================================================================================
// Informational: what the ducker costs. The suite-level CPU probes all run at the shipping default
// (duckamt = 0), where the ducker is one branch, so its engaged cost would otherwise never be
// reported anywhere.
//==================================================================================================
void reportDuckerCpu()
{
    const double blockDurationUs = 512.0 / 48000.0 * 1.0e6;

    struct Leg { const char* label; float amount; int character; bool decayEq; };

    for (const Leg leg : { Leg { "cpu, duck off (default)",   0.0f,   0, false },
                           Leg { "cpu, duck Gentle 100%",     100.0f, 0, false },
                           Leg { "cpu, duck Pump 100%",       100.0f, 1, false },
                           Leg { "cpu, decay EQ engaged",     0.0f,   0, true },
                           Leg { "cpu, decay EQ + duck 100%", 100.0f, 0, true } })
    {
        Harness h (48000.0, 512, 2);

        auto p = duckParams (2.5f);
        p.modDepth      = 25.0f;
        p.duckAmount    = leg.amount;
        p.duckCharacter = leg.character;

        // The two shipping-default multipliers, i.e. the same configuration DecayTests reports as
        // "cpu, decay EQ engaged" -- so the last leg is the worst case this step can produce.
        if (leg.decayEq)
        {
            p.decayLowMult  = 1.4f;
            p.decayHighMult = 0.7f;
        }

        h.engine.setParameters (p);

        std::mt19937 rng (1u);
        h.fillNoise (rng, 0.5f);

        constexpr int warmup = 200;
        constexpr int runs   = 2000;

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

        std::printf ("[INFO] %-34s  %.2f us/block (%.2f%% of one core at 48 kHz / 512)\n",
                     leg.label, microsPerBlock, microsPerBlock / blockDurationUs * 100.0);
        std::fflush (stdout);
    }
}

} // namespace

//==================================================================================================

#include "TestSuites.h"

int runHouseTests()
{
    // TestHarness.h's failureCount is shared by every suite in this target: reset on entry, read
    // back at the end, exactly as runEngineTests() does.
    failureCount = 0;

    std::printf ("House workflow tests\n");
    std::printf ("--------------------\n");

    testDuckAttenuatesWet();
    testDetectorReadsDry();
    testDuckerTiming();
    testDuckAmountZeroIsUnity();
    testSyncedPreDelayLength();
    testNoTempoFallback();
    testDivisionChangeCrossfades();
    reportDuckerCpu();

    std::printf ("--------------------\n");
    std::printf ("%s (%d failure%s)\n",
                 failureCount == 0 ? "ALL CHECKS PASSED" : "FAILURES",
                 failureCount,
                 failureCount == 1 ? "" : "s");
    std::fflush (stdout);

    return failureCount;
}
