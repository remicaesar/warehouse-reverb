#pragma once

#include "Filters.h"
#include "Shelf.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <juce_audio_basics/juce_audio_basics.h>

namespace reverb
{

/** Everything after the tank and before the dry/wet mix: wet EQ, M/S width, D11 bass mono, then the
    duck gain -- in that order (PLAN-R2 D11: "tank -> (erMix blend) -> wet EQ -> width -> bass mono
    -> duck gain -> dry/wet -> trim"; the dry/wet mix and the output trim stay in ReverbEngine).

    The ER/late blend is NOT here: step 2B put it in ReverbEngine::processChunk, which is the single
    blend point, so lateL/lateR arrive already blended and erL/erR are dead parameters kept only so
    this fixed 1b signature does not change. Do NOT blend them again here.

    WET EQ IS NOT THE DECAY EQ. The Decay EQ (decaylow/decayhigh plus the two crossovers, inside
    FDNTank's per-line AttenuationFilter) changes how fast each band DECAYS. The wet EQ here changes
    the TIMBRE of the whole wet signal, once, and cannot change a decay time at all -- it sits
    outside the feedback loop. Three controls now touch the top end and each does exactly one thing:
    `bandwidth` sets the tail's initial timbre (input lowpass, ahead of the tank), `decayhigh` sets
    how fast the top dies (in the loop), `wethighcut` sets the wet tone (here, once).

    FOUR HARD BYPASSES, AND WHY A HARD GATE IS RIGHT HERE. Wet low cut at 20 Hz, wet high cut at
    20 kHz, wet tilt at 0 dB and bass mono at 0 Hz are each a BIT-EXACT bypass -- the stage is not
    run at all (PLAN-R2 7.4 W3/W4; 3.3 lists all four of those values as genuinely neutral). A gate
    that switches instead of crossfading is normally a click, and is not one here because every gate
    sits at a setting where its own stage is already an identity or within a hair of one: tilt at
    0 dB is m1 = 0, i.e. EXACTLY unity, and a one-pole at 20 Hz, a one-pole at 20 kHz and the
    bass-mono band split at 0 Hz are all within ~1e-3 of their pass-through coefficients at every
    supported sample rate. Two details make that argument hold in practice: the coefficient
    smoothers are targeted from the raw parameter whether or not their stage runs, so engaging one
    glides out of neutral rather than jumping to its new value; and a stage's filter state is
    cleared on the gate's RISING edge, because at a near-pass-through coefficient these filters
    return their state rather than swamping it (the one-pole highpass returns x - z, the bass-mono
    highpass returns x - s1*r2 - s2), so a state left over from an earlier engagement would arrive
    as a step.

    BASS MONO (D11), and the one place this class deviates from the plan -- see below. The plan's
    complementary form is

        outL = L - LOW(L) + (LOW(L) + LOW(R)) / 2

    which needs no separate highpass, therefore has no crossover phase error at all, and is an exact
    identity when L == R. Written in mid/side, where LOW is any linear low band, that expression is

        outL = mid + HIGH(side)      outR = mid - HIGH(side)      HIGH = identity - LOW

    -- the same thing, and cheaper: the mid is untouched (outL + outR == L + R, exactly, at every
    frequency, which is the property D11 bought) and ONE filter runs, on the side signal, instead of
    one per channel. This is the form implemented. Two notes on why it is written this way rather
    than literally:
      - `L - a + a` is not the identity in floating point, so the literal expression would not give
        W1's BIT-exact mono identity. In the form above a mono input makes the side signal exactly
        +0.0f, the filter's state stays exactly zero and its output is exactly +0.0f, so outL is mid
        is L, bit for bit.
      - HIGH is evaluated directly instead of as x - LOW(x), for the reason below.

    DEVIATION FROM D11, DELIBERATE: `LOW` is not two cascaded OnePoleLP, and the complement is taken
    from the filter rather than by subtraction. LOW/HIGH are the low and high bands of ONE
    second-order TPT state-variable filter at Q = 1/sqrt(2), and HIGH is its own highpass output --
    which is the exact complement of its own low band by construction (the SVF identity
    x = HP + BP/Q + LP holds sample by sample), so there is still no second filter and still nothing
    that sums two separately filtered bands.

    The reason is a fact about complementary crossovers that is easy to get backwards, and this file
    did get it backwards once: what bass mono DOES is set by HIGH(f), the side signal's response, and
    for ANY plain lowpass -- one pole, two cascaded poles, Butterworth, anything with LOW(0) = 1 --
    the subtraction x - LOW(x) is FIRST order at low frequency, not the lowpass's own order. Its
    small-s expansion is 1 - LOW(s) = (tau * s) + O(s^2) with tau the lowpass's group delay at DC,
    so the slope is 6 dB/oct whatever the lowpass is, and the order only changes the constant:
    tau = 2/w_c for two cascaded one-poles, sqrt(2)/w_c for a second-order Butterworth -- a 3 dB
    difference, not a slope change. Measured, with the subtraction form and a Butterworth low band:
    integrated side rejection over 20..100 Hz is 2.8 dB at a 130 Hz corner and 12.6 dB at 400 Hz,
    the top of the range, so PLAN-R2 7.4 W2 (>= 20 dB below 100 Hz, < 0.5 dB above 500 Hz) is
    unsatisfiable at EVERY bassmono value -- and, far more to the point, a bass mono set to 130 Hz
    would leave the side signal at 65 Hz only 1.7 dB down. The label would be a fiction.

    Taking the SVF's own highpass output instead makes the side response second order,
    |HIGH| = r^2 / sqrt (1 + r^4) with r = f/fc: 12.3 dB down at half the corner, 24 dB at a quarter
    of it, monotonic, and never above unity at any frequency (so bass mono cannot add peak level).
    Measured numbers, including the whole 0..400 Hz range rather than only the corner W2 asserts at,
    are printed by W2 in tests/WetPathTests.cpp.

    REAL-TIME SAFETY. prepare() is the only allocator, and it allocates nothing -- every member is
    fixed size. setParameters() is control rate and calls tan()/pow() through
    OnePoleLP::coefficientForCutoff and TptShelf::coeffsFor. processSample() calls no transcendental
    function, takes no lock, and does one division per sample, and only while bass mono is engaged
    (the SVF's 1/(1 + R2 g + g^2)).
*/
class WetChain
{
public:
    struct Params
    {
        float widthPercent = 100.0f,
              wetLowCutHz = 20.0f, wetHighCutHz = 20000.0f,
              tiltDb = 0.0f, bassMonoHz = 0.0f;
    };

    /** Parameter ends, PLAN-R2 3.2. Public so a test gates on the same constant the DSP does
        instead of a literal that can drift away from it. The minimum low cut, the maximum high cut,
        zero tilt and zero bass mono are the four bypass points. */
    static constexpr float lowCutMinHz   = 20.0f,    lowCutMaxHz  = 1000.0f;
    static constexpr float highCutMinHz  = 1000.0f,  highCutMaxHz = 20000.0f;
    static constexpr float tiltMaxDb     = 12.0f;
    static constexpr float bassMonoMaxHz = 400.0f;

    /** Tilt pivot: unity gain here at every tilt setting, +tiltDb/2 above it and -tiltDb/2 below,
        so `wettilt` is the full top-to-bottom tilt in dB. 1 kHz is the round number nearest the
        geometric mean of the two Decay-EQ crossover defaults (sqrt (250 * 3500) = 935 Hz), so the
        two EQs hinge in the same place. */
    static constexpr float tiltPivotHz = 1000.0f;

    /** Bass mono's band-split Q. 1/sqrt(2) makes the side signal's response a Butterworth highpass:
        monotonic, never above unity, 12 dB/oct. */
    static constexpr float bassMonoQ = 0.70710678f;

    void prepare (double newSampleRate);
    void reset() noexcept;
    void setParameters (const Params& p, bool snap) noexcept;

    /** Advances the time base by numSamples WITHOUT processing, for the blocks ReverbEngine hands
        nothing (its fully-dry fast path and its non-finite early return).

        This exists because a bypass-capable engine has to be able to advance a sub-object's
        smoothers without running audio through it. Step 1b moved widthSmoothed in here from
        ReverbEngine and, having no skip() to call, dropped the two skip() calls that used to run on
        those paths -- so a Width change made during bypass glided from a stale position once bypass
        ended (PLAN-R2 risk 16). Width was only the first case; 5B added nine more smoothed values.
        Rather than trust nine more lines to be kept in step, every smoothed value in this class
        lives in ONE array, and both this function and processSample() walk all of it -- so a
        smoother that is not advanced here is not reachable without also removing it from the array.
        Checked by W6 in tests/WetPathTests.cpp, which fails if any single entry stops advancing. */
    void skip (int numSamples) noexcept
    {
        for (auto& s : smoothed)
            s.skip (numSamples);
    }

    void processSample (float erL, float erR, float lateL, float lateR,
                        float duckGain, float& outL, float& outR) noexcept
    {
        // erL/erR are dead: the ER blend happens in ReverbEngine::processChunk, upstream of this
        // call. See the class comment.
        (void) erL;
        (void) erR;

        // Every smoother advances on every sample, whether or not the stage reading it runs, for
        // the same reason skip() advances all of them: a bypassed stage whose coefficient stood
        // still would jump when it was switched back on.
        std::array<float, numSmoothed> s;

        for (size_t i = 0; i < numSmoothed; ++i)
            s[i] = smoothed[i].getNextValue();

        float l = lateL;
        float r = lateR;

        // ---- wet EQ: low cut -> high cut -> tilt, all pre-width so it is plain L/R -------------
        if (lowCutActive)
        {
            lowCut[0].setCoefficient (s[smLowCut]);
            lowCut[1].setCoefficient (s[smLowCut]);

            l = lowCut[0].process (l);
            r = lowCut[1].process (r);
        }

        if (highCutActive)
        {
            highCut[0].setCoefficient (s[smHighCut]);
            highCut[1].setCoefficient (s[smHighCut]);

            l = highCut[0].process (l);
            r = highCut[1].process (r);
        }

        if (tiltActive)
        {
            // One coefficient set per shelf, shared by both channels, each channel keeping its own
            // state variable -- the idiom TptShelf::processWith exists for (see Shelf.h).
            const TptShelf::Coeffs low  { s[smTiltLowG],  s[smTiltLowM0],  s[smTiltLowM1]  };
            const TptShelf::Coeffs high { s[smTiltHighG], s[smTiltHighM0], s[smTiltHighM1] };

            l = TptShelf::processWith (low,  tiltLowZ[0],  l);
            r = TptShelf::processWith (low,  tiltLowZ[1],  r);
            l = TptShelf::processWith (high, tiltHighZ[0], l);
            r = TptShelf::processWith (high, tiltHighZ[1], r);
        }

        // ---- M/S width, then D11 bass mono, which is the same mid/side pair --------------------
        // Width is the M/S maths ReverbEngine::processChunk used to hold, moved here verbatim by
        // step 1b. Bass mono then takes the low band out of the SIDE signal and leaves the mid
        // alone, which is D11's complementary form written in mid/side -- see the class comment.
        // D11's order (width, then bass mono) is what runs: the side is scaled first and filtered
        // after, which matters only while width is gliding.
        const float mid = (l + r) * 0.5f;
        float side      = (l - r) * 0.5f * s[smWidth];

        if (bassMonoActive)
        {
            const float g = s[smBassMono];
            const float h = 1.0f / (1.0f + g * (g + r2));

            side = bassMonoSvf.highpass (g, h, r2, side);
        }

        outL = (mid + side) * duckGain;
        outR = (mid - side) * duckGain;
    }

private:
    using Smoothed = juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>;

    /** Second-order TPT state-variable filter, highpass output (Zavalishin's topology, the same one
        juce::dsp::StateVariableTPTFilter implements). Hand-rolled and local because the coefficient
        g arrives already smoothed, per sample, which a class owning its own cutoff cannot do. It
        belongs beside OnePoleLP in Filters.h, which is outside step 5B's file set -- move it there
        the next time that file is open.

        The state updates are the full SVF's, not a highpass-only reduction, because `hp` is only the
        exact complement of this filter's own low band while the identity x = hp + bp/Q + lp holds. */
    struct Svf
    {
        void reset() noexcept   { s1 = 0.0f; s2 = 0.0f; }

        /** g = tan (pi * fc / fs), r2 = 1/Q, h = 1 / (1 + g*(g + r2)); all three at control rate. */
        float highpass (float g, float h, float r2In, float x) noexcept
        {
            const float hp = h * (x - s1 * (g + r2In) - s2);
            const float bp = hp * g + s1;
            s1 = hp * g + bp;
            const float lp = bp * g + s2;
            s2 = bp * g + lp;

            return hp;
        }

        float s1 = 0.0f, s2 = 0.0f;
    };

    // Indices into `smoothed`. Kept as constants over one array rather than as ten named members so
    // that skip() and processSample() can walk the lot -- see skip()'s comment.
    static constexpr size_t smWidth      = 0;
    static constexpr size_t smLowCut     = 1;
    static constexpr size_t smHighCut    = 2;
    static constexpr size_t smTiltLowG   = 3;
    static constexpr size_t smTiltLowM0  = 4;
    static constexpr size_t smTiltLowM1  = 5;
    static constexpr size_t smTiltHighG  = 6;
    static constexpr size_t smTiltHighM0 = 7;
    static constexpr size_t smTiltHighM1 = 8;
    static constexpr size_t smBassMono   = 9;
    static constexpr size_t numSmoothed  = 10;

    // Must track ReverbEngine::smoothingSeconds: width was relocated from there and its 50 ms
    // window is part of what makes that relocation bit-identical (PLAN-R2 step 5B).
    static constexpr float smoothingSeconds = 0.05f;
    static constexpr float r2               = 1.0f / bassMonoQ;

    void setSmoothed (size_t index, float value, bool snap) noexcept;

    double sampleRate = 44100.0;

    std::array<Smoothed, numSmoothed> smoothed;

    OnePoleHP  lowCut[2];
    OnePoleLP  highCut[2];
    float      tiltLowZ[2] { 0.0f, 0.0f }, tiltHighZ[2] { 0.0f, 0.0f };
    Svf        bassMonoSvf;   // on the side signal only -- see processSample()

    // Gate state, all control rate. Each is true only when its stage is off its bypass value.
    bool lowCutActive = false, highCutActive = false, tiltActive = false, bassMonoActive = false;
};

} // namespace reverb
