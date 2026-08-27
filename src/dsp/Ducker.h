#pragma once

#include <algorithm>
#include <cmath>

namespace reverb
{

/** Wet-only gain reduction driven by the dry signal (PLAN-R2 D5: "dry-driven only, no sidechain
    bus"). Character sets the detector, knee AND attack together -- Gentle: mean-square envelope,
    6 dB knee, 30 ms attack; Pump: peak envelope, 0 dB knee, 2 ms attack -- release stays the one
    user-controlled time constant, because release is what decides how the reverb recovers between
    kicks.

    Signal flow, per sample (D5's ordering, which is what makes this a ducker rather than a
    tremolo -- the attack/release smooth the GAIN REDUCTION, not the envelope):

        dry mono -> level detector -> soft knee -> reduction in dB -> attack/release -> linear gain

    Three things about the structure are worth knowing before changing it:

    - **The detector is deliberately fast and is NOT the release.** Its time constant is
      `min (10 ms, releaseMs / 4)`: 10 ms bridges the trough of a 50 Hz kick's half-cycle so the
      detector does not ripple at the kick's own fundamental, and staying at least 4x faster than
      the release pole is what makes `duckrelease` measure as the time the user asked for. A
      detector as slow as the release would cascade with it and the knob would read ~3x long.
      Only the detector's TYPE is a character property (D5); its time constant is not.
    - **Gentle's S-curve is two identical poles, each carrying tau/2.1462.** For two cascaded
      one-poles the step response is `1 - (1 + x) e^-x` with `x = t/tau_pole`, so 63 % of the way
      up -- and, by the same equation, 37 % of the way back down -- is reached at
      `x = 2.1462`. Scaling each pole by that factor makes the stated 30 ms attack and the
      user's release time mean the same measured thing in Gentle as they do in Pump, instead of
      running ~2.1x long. One constant, derived, not tuned.
    - **`amountPercent == 0` is a hard bypass returning exactly 1.0f** (D5), not a smoothed
      approach to unity: at the shipping default the ducker must be bit-transparent. Entering
      bypass also clears the reduction state, so re-engaging attacks from unity rather than
      jumping out of a stale reduction.

    prepare() is the only function that touches the sample rate. setParameters() is control rate
    and calls exp()/pow(); processSample() is audio rate and calls neither -- it does spend one
    log10 and one exp2 per sample, but only while the dry signal is above the knee, and nothing at
    all while the ducker is bypassed.
*/
class Ducker
{
public:
    enum class Character { gentle, pump };

    struct Params
    {
        float amountPercent = 0.0f, thresholdDb = -24.0f, releaseMs = 250.0f;
        Character character = Character::gentle;
    };

    void prepare (double sampleRate);
    void reset() noexcept;

    /** Control rate: recomputes every coefficient, so exp()/pow() here are fine. */
    void setParameters (const Params& p) noexcept;

    /** Feed the DRY sample; returns the linear gain for the WET. Exactly 1.0f when the amount is
        0, and exactly 1.0f again once a reduction has fully recovered. */
    float processSample (float dryMono) noexcept
    {
        if (! active)
            return 1.0f;   // D5: exact bypass at amount == 0, the shipping default

        // Negated comparison, the DelayLineFrac::read() idiom: a NaN lands on the ceiling instead
        // of propagating into the follower, where it would stick forever. The ceiling itself is
        // overflow protection for the mean-square path -- x*x overflows to inf around 1.8e19, and
        // inf - inf in the follower below is NaN. +60 dBFS is 20 dB past the loudest input that
        // can still change the output, since `over` saturates at maxOverDb above a threshold whose
        // own maximum is 0 dBFS.
        const float magnitude = std::abs (dryMono);
        const float level     = ! (magnitude < detectorCeiling) ? detectorCeiling : magnitude;

        float envelope;

        if (peakDetector)
        {
            // D5: Pump = peak follower. Instantaneous attack, so the reduction snaps onto the
            // transient and the 2 ms gain pole is what shapes it.
            detectorState = std::max (level, detectorState * detectorReleaseCoeff);
            envelope      = detectorState;
        }
        else
        {
            // D5: Gentle = mean-square follower then sqrt. Symmetric in both directions, which is
            // half of why Gentle breathes rather than snaps.
            detectorState += detectorAttackCoeff * (level * level - detectorState);
            envelope       = std::sqrt (detectorState);
        }

        // Below the knee's lower edge the soft knee is exactly zero, so the log10 is skipped
        // rather than approximated. This is the whole cost of the detector at low levels.
        float over = 0.0f;

        if (envelope > kneeStartLinear)
        {
            const float overshootDb = 20.0f * std::log10 (envelope) - thresholdDb;

            over = overshootDb >= halfKneeDb
                       ? overshootDb
                       : (overshootDb + halfKneeDb) * (overshootDb + halfKneeDb) * kneeScale;

            over = std::min (over, maxOverDb);
        }

        const float targetDb = -depth * over;

        // Attack when the reduction is deepening, release when it is recovering (D5).
        const float coeff = targetDb < stage1Db ? attackCoeff : releaseCoeff;

        stage1Db += (1.0f - coeff) * (targetDb - stage1Db);

        if (twoPole)
        {
            stage2Db += (1.0f - coeff) * (stage1Db - stage2Db);
            reductionDb = stage2Db;
        }
        else
        {
            reductionDb = stage1Db;
        }

        // A one-pole only approaches its target, so without this the wet would stay multiplied by
        // 0.99999x forever after a single kick -- and the tail of a one-pole toward zero is a
        // denormal factory. Settling both the state and the return value at 1e-4 dB (a factor of
        // 1.00001) costs nothing audible and makes "recovered" mean exactly unity.
        if (targetDb > -settleDb && reductionDb > -settleDb)
        {
            stage1Db = stage2Db = reductionDb = 0.0f;
            return 1.0f;
        }

        // exp2 rather than pow (10, x): one transcendental, and the cheapest of the family.
        // log2 (10) / 20 converts dB to a base-2 exponent.
        return std::exp2 (reductionDb * 0.166096404744368f);
    }

    /** Current gain reduction in dB, NEGATIVE for a reduction (0 = no ducking). Read once per
        block by the processor, which mirrors it into an atomic for the GUI indicator (D5). */
    float getReductionDb() const noexcept   { return reductionDb; }

    //==============================================================================================
    // THE PUBLISHED TIMING LAW. These four constants and three derived time constants are what
    // decide how long a release actually takes. They are public so that a test can COMPUTE the
    // release time it expects from the same definitions updateCoefficients() uses, instead of
    // restating them as literals that can drift -- see H3d in tests/HouseTests.cpp, which bounds
    // the mechanism rather than one measured point. None of them is a knob: changing one changes
    // what `duckrelease` means, and H3d will say by how much.
    //==============================================================================================

    /** `over` saturates here, so this is also the deepest reduction the ducker can produce -- at
        amount 100, a full-scale dry signal and any threshold at or below -40 dBFS. */
    static constexpr float maxOverDb = 40.0f;

    /** PLAN-R2 3.2's duckrelease range. Params carries plain unvalidated floats, so
        setParameters() clamps to this and every law below is written in terms of the clamped
        value. */
    static constexpr float releaseMinMs = 20.0f, releaseMaxMs = 1000.0f;

    /** Two cascaded identical one-poles reach 63 % (and fall back to 37 %) at t = 2.1462 * tau:
        the root of (1 + x) e^-x = 1/e. */
    static constexpr float twoPoleTimeScale = 2.1462f;

    /** The detector's ceiling -- see the class comment's first bullet for why it is 10 ms and why
        that is not a character property. */
    static constexpr float detectorMaxMs = 10.0f;

    static constexpr float clampedReleaseMs (float releaseMs) noexcept
    {
        return std::min (std::max (releaseMs, releaseMinMs), releaseMaxMs);
    }

    /** The GAIN pole's per-stage time constant: Gentle cascades two of these, Pump has one. */
    static constexpr float poleTauMs (float releaseMs, Character character) noexcept
    {
        return clampedReleaseMs (releaseMs)
               / (character == Character::gentle ? twoPoleTimeScale : 1.0f);
    }

    /** The DETECTOR's time constant, which is never allowed to be the slower pole. */
    static constexpr float detectorTauMs (float releaseMs) noexcept
    {
        return std::min (detectorMaxMs, 0.25f * clampedReleaseMs (releaseMs));
    }

    /** The time constant of the ENVELOPE, which for Gentle is NOT the detector's: its follower is
        mean-square and the envelope is that follower's square root, so the envelope decays at half
        the follower's rate. That factor of two is the whole reason a Gentle release runs long at a
        deep reduction -- the envelope has to walk the overshoot back down at a fixed dB/ms before
        the release pole can finish -- and it is why Pump, whose follower is already a peak and
        whose envelope therefore shares the detector's time constant, runs less long. */
    static constexpr float envelopeTauMs (float releaseMs, Character character) noexcept
    {
        return detectorTauMs (releaseMs) * (character == Character::gentle ? 2.0f : 1.0f);
    }

private:
    void updateCoefficients() noexcept;

    /** exp (-1 / (tau * fs)), the standard one-pole retention coefficient. Control rate only. */
    float retentionCoefficient (float tauMs) const noexcept;

    // D5's remaining fixed numbers, in one place so the .cpp reads as the spec. The four that a
    // test has to be able to reach are public, above.
    static constexpr float gentleAttackMs   = 30.0f;
    static constexpr float pumpAttackMs     = 2.0f;
    static constexpr float gentleKneeDb     = 6.0f;
    static constexpr float pumpKneeDb       = 0.0f;
    static constexpr float detectorCeiling  = 1000.0f; // +60 dBFS
    static constexpr float settleDb         = 1.0e-4f;

    double sampleRate = 44100.0;

    Params params {};

    bool  active       = false;   // amountPercent > 0
    bool  peakDetector = false;   // Pump
    bool  twoPole      = true;    // Gentle's S-curve

    float depth           = 0.0f;   // amountPercent / 100
    float thresholdDb     = -24.0f;
    float halfKneeDb      = 0.5f * gentleKneeDb;
    float kneeScale       = 1.0f / (2.0f * gentleKneeDb);
    float kneeStartLinear = 0.0f;

    float attackCoeff  = 0.0f;
    float releaseCoeff = 0.0f;
    float detectorAttackCoeff  = 0.0f;   // Gentle: the mean-square follower's 1 - coeff
    float detectorReleaseCoeff = 0.0f;   // Pump: the peak follower's decay

    float detectorState = 0.0f;   // |x| for Pump, x^2 for Gentle
    float stage1Db      = 0.0f;
    float stage2Db      = 0.0f;
    float reductionDb   = 0.0f;
};

} // namespace reverb
