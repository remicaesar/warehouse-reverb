#pragma once

#include "AttenuationFilter.h"
#include "DelayLineFrac.h"
#include "Filters.h"
#include "PitchShifter.h"
#include "Saturator.h"

#include <algorithm>
#include <array>
#include <juce_audio_basics/juce_audio_basics.h>
#include <vector>

namespace reverb
{

/** Sixteen-line feedback delay network with an orthogonal Hadamard mixing matrix.

    Line lengths are prime numbers of samples at Size = 100%, then scaled by Size at runtime through
    the WARPED map of PLAN-R2 D2 (see sizeScaleFor() in the .cpp). The matrix is a Sylvester 16x16
    Hadamard applied as an in-place fast Walsh-Hadamard butterfly and scaled by 1/sqrt(16), which
    makes it exactly orthogonal - the loop therefore never has gain above the per-line feedback
    gain, and Freeze (gain exactly 1) holds forever rather than creeping up or down.

    STEP 3A, THE DENSITY REWORK (PLAN-R2 D2/D3/D10). Four changes, and the first two only work
    together:

      - SIXTEEN lines rather than eight, spanning 18..95 ms, which doubles the mode count at every
        Size. The fast Walsh-Hadamard butterfly generalises to any power of two unchanged.
      - A WARPED Size map with a FLOOR, so no reachable Size drops below 0.125 SECONDS of total
        delay -- the colorlessness threshold, which is a time and not a sample count, so the floor
        holds at 44.1 kHz and at 192 kHz alike. 16 lines alone does not clear it: Size 10% on a
        linear map reaches 0.150 s only because the floor puts it there.
      - PER-LINE ENERGY COMPENSATION. A line's modal energy for a unit injection goes as
        1/(1 - g_i^2), so the short lines carry ~3.1x the energy of the long ones and ring as comb
        peaks. Each line's injection is scaled by sqrt(1 - g_i^2), normalised to unit mean.
      - A DENSITY allpass nested inside every line's feedback path. |H| = 1, so it raises echo
        density without touching the stability argument at all (D10).

    Measured before the change, on the eight-line tank at 48 kHz (PLAN-R2 7.3): normalised echo
    density reached 0.9 at 39 ms at Size 10%, 166 ms at Size 100% and 328 ms at Size 200%. The
    interesting half of that is the LARGE end, not the small one: echo density is a time-domain
    measure, and Size 10% packs its (too few) modes into a short period, so it mixes fast while
    still being spectrally comb-y. Size and mode count are covered by the 0.125 s floor; mixing time
    is covered by §7.3 D3.

    Every quantity that a user can move is behind a juce::SmoothedValue, including the per-line
    delay lengths: because the reads are fractional, a gliding length is a clean pitch glide
    rather than a click.

    THE FOLD (PLAN-R2 D1a), step 2A. The per-line HF damping lowpass and low-cut highpass are
    GONE from the feedback path, and with them the per-line scalar feedback gain. All three are
    replaced by one AttenuationFilter per line -- broadband scalar plus two shelves, every dB
    scaled by that line's length -- so frequency-dependent decay has exactly one mechanism and the
    tank has exactly one T60(f) curve, the same one for every line. `damping` and `lowcut` are now
    the two CROSSOVER frequencies of that curve (the documented wart: the parameter IDs are stale,
    the meaning is not).

    The old filters applied a fixed dB per PASS, and a pass takes m_i samples, so their dB per
    SECOND went as 1/m_i and the eight lines decayed at different rates wherever they acted. That
    is the defect the fold deletes and §7.1 T7 is the test that keeps it deleted.

    THE SAME DEFECT ARRIVED THROUGH THE DELAY LINES, and that is why the line reads below go
    through DelayLineFrac::readLagrange() rather than read(). Linear interpolation is a lowpass with
    |H| = cos(pi f / fs) - 1.25 dB at 8 kHz / 48 kHz at a half-sample offset - taken once per pass,
    i.e. a fixed dB per PASS with dB per second going as 1/m_i, exactly the shape the fold exists to
    remove. It is invisible only at Size 100% and 200%, where halving or doubling a prime leaves an
    integer offset; every other Size, and any modulation at all including the default 25%, made the
    read fractional. Measured before the change: the 8 kHz T60 was 58% short of target at Size 50%
    and 20% short at the shipping default. This is the ONE read in the plugin that needed the wider
    kernel - the diffuser, early reflections and pre-delay are feed-forward and take the loss once,
    so they stay on read(). §7.1 T9 is the test that keeps this deleted.

    What the fold adds back, deliberately: a fixed ~5 Hz one-pole highpass per line, always on and
    not exposed (D1a(a)). Losing `lowcut` would otherwise turn an unconditional no-DC-accumulation
    guarantee into a user-controllable one -- at decaylow = 4.0 DC would decay four times slower
    than everything else, and DC in a feedback loop eats headroom without self-cancelling.

    Freeze got SIMPLER, not harder. There is no outer gain multiply left, so a bit-exact frozen
    loop needs one condition (filterMix == 0) instead of the two it used to need, and the single
    crossfade covers the attenuation filter and the DC blocker together.
*/
class FDNTank
{
public:
    static constexpr int numLines = 16;   // D2; must stay a power of two for hadamard()

    static_assert ((numLines & (numLines - 1)) == 0,
                   "the fast Walsh-Hadamard butterfly needs a power-of-two line count");

    struct Params
    {
        float size          = 1.0f;   // 0.1 .. 2.0
        float decaySeconds  = 2.5f;

        // D1a, THE FOLD: these two are the Decay EQ's two CROSSOVER frequencies now --
        // dampingHz is the HIGH crossover (1000..12000 Hz), lowCutHz the LOW one (60..800 Hz).
        // Neither is a filter cutoff any more. The field names stay because the parameter IDs
        // stay (renaming those is a state-breaking change, not a cleanup -- see ReverbEngine.h);
        // decaycurve::sanitise() clamps both to the ranges above, since ReverbEngine passes them
        // through unclamped. The default member initialisers are left at the OLD 6000/120 for the
        // same reason as in ReverbEngine::Params: they are only a bare-Params fallback, they land
        // in range once sanitised, and moving them would perturb the frozen stability-sweep
        // numbers. The product defaults (3500/250) live in src/Parameters.cpp.
        float dampingHz     = 6000.0f;
        float lowCutHz      = 120.0f;

        float modDepth      = 0.25f;  // 0 .. 1
        float modRateHz     = 0.4f;
        bool  freeze        = false;

        // ---- P1-P3 fields the tank owns structurally (D1, D7, D8, D9, D10). decayLowMult and
        // decayHighMult are REAL as of 2A (the Decay EQ); density is REAL as of 3A. algorithm: no
        // .cpp reads it yet (8A). drive/shimmerMode/shimmerAmount: wired as of 1b into Saturator/
        // PitchShifter, both no-ops (identity / exactly silent) -- see their call sites in
        // processSample(). ----
        float decayLowMult  = 1.4f;   // x, 0.1..4.0, low-band decay multiplier (D1)
        float decayHighMult = 0.7f;   // x, 0.05..4.0, high-band decay multiplier (D1)
        float density       = 45.0f;  // %, in-line allpass density, per line (D10)
        float drive         = 0.0f;   // %, ADAA cubic soft clip per line (D7)
        int   shimmerMode   = 0;      // 0=Off,1=+12,2=+7,3=-12,4=Dual (D9)
        float shimmerAmount = 0.0f;   // %
        int   algorithm     = 2;      // 0=Room,1=Plate,2=Hall,3=Ambience,4=Space; default Hall (D8)
    };

    void prepare (double newSampleRate);
    void reset() noexcept;
    /** snap = true applies the values immediately instead of ramping (use after prepare()). */
    void setParameters (const Params& p, bool snap = false) noexcept;

    void processSample (float inL, float inR, float& outL, float& outR) noexcept;

    /** Fixed corner of the per-line DC blocker, in Hz. NOT a parameter and not derived from one:
        it is a compile-time constant precisely so no user setting can move it (D1a(a), and the
        second half of §7.1 T8).

        Why 5 Hz and not the plan's nominal 15 Hz: an `x - lp(x)` highpass has a steady-state gain
        of exactly 0 at DC for ANY corner [verified, Filters.h:51 -- constraint C15], so the
        unconditional guarantee is independent of this number, and the only thing the corner buys
        is how much of the audio band it colours. It is a FIXED dB per pass, so its dB per second
        goes as 1/m_i -- the very defect the fold removes. At 15 Hz that costs a measured ~19% of
        the low-band decay time at 100 Hz on a 6 s LF tail, which would put §7.1 T1's low band
        outside tolerance on the DC blocker alone; at 5 Hz it costs ~2.5%. Sub-audio rejection is
        still emphatic (~0.27 dB per pass at 20 Hz, tens of dB across any real tail), and 5 Hz sits
        further below the audio band than the 20 Hz floor the old `lowcut` could reach, so it
        colours strictly less than the filter it replaces.
    */
    static constexpr float dcBlockerHz = 5.0f;

    /** The colorlessness threshold of PLAN-R2 D2, in SECONDS of total delay summed over every line.

        A time, not a sample count: the number of FDN modes below a fixed frequency scales with the
        total delay in seconds, so reading the published ~6000-mode figure as samples would demand
        twice as much delay at 96 kHz for no perceptual reason. The Size map's floor is derived from
        this number in sizeScaleFor(), and §7.3 D1 asserts the result at four sample rates.
    */
    static constexpr double colorlessTotalDelaySeconds = 0.125;

    /** Target injection gain for the tank input, BEFORE the per-line energy compensation; exactly 0
        once Freeze has been applied. The per-line injection is this times getEnergyCompensation(i),
        and the compensation has unit mean, so this remains the level-setting quantity it was.
    */
    float getInputGain() const noexcept   { return inputGainTarget; }

    /** Total delay across all lines at the current Size, in seconds -- the quantity the 0.125 s
        colorlessness floor is expressed in (D2, §7.3 D1/D2). Reads the smoothers' TARGETS, i.e. the
        lengths the tank is heading for, which is what the Size map decides; while frozen those are
        the held lengths instead. Excludes the density allpasses on purpose: they are bypassed at
        density = 0, so counting them would make the floor conditional on a user parameter.
    */
    double getTotalDelaySeconds() const noexcept
    {
        double total = 0.0;

        for (const auto& length : lineLength)
            total += static_cast<double> (length.getTargetValue());

        return total / sampleRate;
    }

    /** D10's crossfade weight: exactly 0 while density = 0 and exactly 1 above it, which is what
        makes density = 0 a bit-exact bypass (§7.3 D6). The TARGET, not the ramped value -- the ramp
        only exists so a knob move across zero does not click.

        Why a switch and not a proportional control: see setParameters(). A partial crossfade between
        a signal and an allpassed copy of it is a comb, not a partial diffusion.
    */
    float getDensityMix() const noexcept   { return densityMix.getTargetValue(); }

    /** Per-line injection weight from the D2 energy compensation, sqrt(1 - g_i^2) normalised to
        unit mean over the lines -- and exactly 1.0 for every line while frozen (the D2 guard, §7.3
        D5). Control rate; this is the value the audio loop is using.
    */
    float getEnergyCompensation (int line) const noexcept
    {
        return energyComp[static_cast<size_t> (std::min (std::max (line, 0), numLines - 1))];
    }

    /** Line length in samples at the current Size, i.e. m_i -- the quantity every dB in the
        attenuation filter is scaled by. Exposed because converting a per-line per-PASS attenuation
        into dB per SECOND needs it, and dB per second is what the fold makes uniform (§7.1 T7).
    */
    float getLineLengthSamples (int line) const noexcept
    {
        const auto idx = static_cast<size_t> (std::min (std::max (line, 0), numLines - 1));
        return lineLength[idx].getTargetValue();
    }

    /** The complete per-line feedback filter chain: decay law, then the fixed DC blocker.

        Audio rate -- processSample() calls it once per line per sample; it is not a test hook
        bolted on beside the real path, it IS the real path. Factored out because per-line decay
        rates are unobservable from outside the tank once the Hadamard matrix has mixed the lines,
        and per-line uniformity is the property THE FOLD exists to deliver (§7.1 T7). Advances the
        line's filter state, so a caller measuring one line must drive it as the audio loop does.
    */
    float processFeedbackPath (int line, float x) noexcept
    {
        const auto idx = static_cast<size_t> (std::min (std::max (line, 0), numLines - 1));
        return dcBlock[idx].process (atten[idx].process (x));
    }

private:
    using Smoothed = juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>;

    /** D10's nested allpass: one Schroeder section on a FIXED, INTEGER, non-modulated delay.

        Why this and not SchroederAllpass, which is the same structure and already exists. That class
        reads through DelayLineFrac, whose read() earns its keep on a smoothed or modulated delay --
        NaN-guarded clamp, int split, two wrapped indices, two loads and an interpolation. None of
        that is reachable here: the delay is set once in prepare() and never moves, so the whole read
        collapses to one load from the slot that is about to be overwritten. Measured, which is the
        only reason this exists: SIXTEEN of these per sample cost 11.4 us per 512-sample block through
        SchroederAllpass against a 105 us ceiling for the whole engine, and the tank had 3 us of
        margin. SchroederAllpass stays exactly right for the diffuser, whose delays ARE fractional.

        The buffer length IS the delay: reading before writing means the slot holds the sample from
        size steps ago, which is why there is one index rather than a read and a write head.
    */
    struct DensityAllpass
    {
        void prepare (int delaySamples)
        {
            size = std::max (2, delaySamples);
            buffer.assign (static_cast<size_t> (size), 0.0f);
            index = 0;
        }

        void clear() noexcept
        {
            std::fill (buffer.begin(), buffer.end(), 0.0f);
            index = 0;
        }

        /** Output is delayed - g*v with v = x + g*delayed: the standard allpass pair, so |H| = 1 for
            any |g| < 1 and this section cannot add gain to the feedback loop (D10).
        */
        float process (float x, float g) noexcept
        {
            const auto  slot    = static_cast<size_t> (index);
            const float delayed = buffer[slot];
            const float v       = x + g * delayed;

            buffer[slot] = v;

            if (++index >= size)
                index = 0;

            return delayed - g * v;
        }

        std::vector<float> buffer;
        int size = 0;
        int index = 0;
    };

    static constexpr float smoothingSeconds = 0.05f;
    static constexpr float maxModSamples    = 8.0f;

    /** In-place H16 * x, scaled by 1/sqrt(16). Four add/subtract butterfly stages. */
    static void hadamard (std::array<float, numLines>& x) noexcept;

    double sampleRate = 44100.0;

    std::array<DelayLineFrac, numLines> lines {};

    // D1 + D1a: the whole feedback-path filter chain, per line. The attenuation filter carries the
    // decay law (broadband scalar + two shelves, all scaled by m_i); the highpass is the fixed DC
    // blocker. Both sit inside the one filterMix crossfade, so Freeze bypasses them together.
    std::array<AttenuationFilter, numLines> atten {};
    std::array<OnePoleHP, numLines> dcBlock {};

    // D1a(a) again, at the tank's INPUT this time, one per channel at the same fixed 5 Hz corner.
    //
    // Added in 3A, and it closes a hole the per-line energy compensation opened. The per-line
    // blockers above cannot touch the injected signal on its FIRST pass -- they sit after the read --
    // so each line's output carries an unfiltered copy of the input's DC. That used to cancel
    // exactly in the output taps, because every line got the same injection gain and the taps
    // alternate sign; give the lines DIFFERENT injection weights and the cancellation goes, leaving
    // a DC offset in the wet output at 4% of a DC input (measured: 2.0e-2 out for 0.5 in, against
    // 6.5e-6 before). It is a leak rather than an accumulation -- it does not grow with decay time --
    // but a reverb should not put DC in the wet path at all, and depending on an accidental
    // cancellation for that is exactly the kind of guarantee that breaks silently. Blocking the
    // input makes it unconditional and costs two one-poles per sample. §7.1 T8 is the check.
    std::array<OnePoleHP, 2> inputDcBlock {};

    // D7: per-line drive, in the feedback path, before the attenuation filter. 1b no-op: identity
    // at every drive value, so this call site is inert until 7B.
    std::array<Saturator, numLines> saturators {};

    // D10: one Schroeder allpass nested inside each line's feedback path, ahead of the attenuation
    // filter. Its delay is a fixed prime number of samples set in prepare() -- so the read is at an
    // exact integer offset and interpolates nothing, which is the same reason the line reads moved
    // to readLagrange(): a fractional read inside a feedback loop is a fixed dB per PASS, and that
    // is the defect class THE FOLD exists to delete. The allpass delay therefore does NOT scale
    // with Size; the consequence is that its share of the loop grows as Size shrinks, which is
    // stated at densityFraction in the .cpp.
    std::array<DensityAllpass, numLines> densityAp {};
    std::array<int, numLines> densityDelaySamples { {} };

    // D9: ONE shimmer voice, not per line -- fed a tapped sum of the tank, injected back at every
    // line's input alongside the dry signal. 1b no-op: always 0.0f, so this call site is inert
    // until 7A.
    PitchShifter shimmer;

    // Prime line lengths in samples at Size = 100%.
    std::array<int, numLines> primeBaseSamples { {} };

    std::array<Smoothed, numLines> lineLength {};
    std::array<float, numLines> lfoPhase { {} };

    // D2: per-line injection, i.e. the broadband normalisation times that line's unit-mean energy
    // compensation. Smoothed per line rather than one shared smoother times an unsmoothed weight,
    // because the weight moves with Decay and Size and an unsmoothed step in injection gain is a
    // click. energyComp holds the weight itself so §7.3 D4/D5 can read what the loop is using.
    std::array<Smoothed, numLines> lineInputGain {};
    std::array<float, numLines> energyComp { {} };
    float inputGainTarget = 0.0f;

    // feedbackGain[] is gone: the per-line broadband gain lives inside AttenuationFilter now, which
    // is what leaves Freeze with a single condition to satisfy. dampingCoeff/lowCutCoeff are gone
    // with the filters they drove.
    Smoothed filterMix, modDepthSamples;

    // D10: densityMix is the crossfade that makes density = 0 a bit-exact bypass; densityGain is
    // the allpass coefficient. See setParameters() for why the mix is a 0/1 switch and the gain is
    // what carries the parameter.
    Smoothed densityMix, densityGain;

    float lfoIncrement = 0.0f;
    bool  frozen = false;
};

} // namespace reverb
