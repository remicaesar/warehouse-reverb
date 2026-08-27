#include "FDNTank.h"

#include "Primes.h"

#include <algorithm>
#include <cmath>

namespace reverb
{

namespace
{
    // SIXTEEN base lengths spread over 18-95 ms, roughly log-distributed with a jittered ratio
    // around 1.12 so no two are related by a simple factor, and snapped to primes in prepare().
    // Total 750.20 ms at Size 100%, against 380.28 ms for the eight-line table this replaces --
    // twice the delay, twice the modes, at every Size (PLAN-R2 D2).
    constexpr float baseLengthsMs[FDNTank::numLines] =
    {
        18.03f, 20.29f, 22.31f, 25.13f, 27.91f, 31.37f, 34.79f, 39.23f,
        43.51f, 48.91f, 54.29f, 61.03f, 67.79f, 76.03f, 84.67f, 94.91f
    };

    constexpr float maxSizeScale   = 2.0f;   // Size = 200%
    constexpr float minSizeScale   = 0.1f;   // Size = 10%, the bottom of the parameter range
    constexpr float hadamardScale  = 0.25f;  // 1 / sqrt(16)
    constexpr float outputScale    = 0.35355339059327376f;  // 1 / sqrt(8 lines per output tap)
    constexpr float twoPi          = 6.283185307179586f;
    constexpr float wetHeadroom    = 0.70794578f;   // -3 dB of peak headroom on the wet path

    /** D2, the WARPED Size map. Below Size 100% the length scale is
            floor + (1 - floor) * ((s - sMin) / (1 - sMin))^p
        and above it the scale is s itself; the two branches meet at exactly 1.0 by construction.

        Why warped rather than clamped. The 0.125 s colorlessness floor has to hold at Size 10%, and
        the sixteen-line table only reaches 0.750 s * 0.10 = 0.075 s there, so SOME floor is needed.
        A plain max(s, floor) would give the bottom of the knob a dead zone -- every Size below the
        floor sounding identical -- which is a worse defect than the one it fixes and is what §7.3
        D2 exists to reject. Warping instead compresses the whole lower half of the travel into
        [floor, 1], so every Size still changes the sound, monotonically, and the smallest step
        anywhere on the 20-point D2 grid is 5.3%.

        sizeFloorScale is derived from the table, not tuned: 0.125 / 0.7502 = 0.1666 is the minimum
        that clears the floor, and 0.20 is that with 20% of margin for the prime snapping and for
        8A's per-mode tables. It buys the floor at the cost of the very smallest rooms, which D2
        move 3 answers: perceived smallness belongs to the early field, and the ER stage follows the
        RAW Size parameter over its full range.
    */
    constexpr float sizeFloorScale  = 0.20f;
    constexpr float sizeWarpExponent = 1.5f;

    float sizeScaleFor (float size) noexcept
    {
        const float s = std::min (std::max (size, minSizeScale), maxSizeScale);

        if (s >= 1.0f)
            return s;

        const float u = (s - minSizeScale) / (1.0f - minSizeScale);

        return sizeFloorScale + (1.0f - sizeFloorScale) * std::pow (std::max (u, 0.0f),
                                                                    sizeWarpExponent);
    }

    /** D10: the nested allpass delay, as a fraction of that line's base length at Size 100%.

        Fixed in prepare() and NOT scaled by Size, so the read is always at an exact integer offset
        (see the densityAp comment in the header for why a fractional read inside the loop is
        unacceptable). 0.10 puts the sixteen allpasses at 1.8-9.5 ms, which is the range Dattorro's
        tank allpasses occupy and short enough that the loop-length bookkeeping below stays small.

        The consequence of not scaling with Size, stated rather than hidden: the allpass is 10% of a
        line at Size 100% and 50% of one at Size 10%, so Density colours a small room more than a
        large one. That is the right direction for a density control -- D2 move 3 wants density to
        RISE as Size falls -- but it is a side effect of the fixed delay, not a designed curve.
    */
    constexpr float densityFraction = 0.10f;

    /** D10: allpass coefficient at density = 100%.

        A Schroeder allpass has |H| = 1 for any |g| < 1, so this number cannot threaten the
        stability argument; what it costs is group-delay spread. The mean group delay of a
        D-sample allpass is exactly D whatever g is, but at a given frequency it runs from
        D(1-g)/(1+g) to D(1+g)/(1-g) -- so the added loop delay, and with it T60, varies with
        frequency about a correct mean. At 0.6 that spread is +/-0.25 x the allpass's share of the
        loop, i.e. a few per cent of T60, and it is fine-grained in frequency (one cycle every
        fs/D Hz) rather than a broad tilt.
    */
    constexpr float densityMaxGain = 0.6f;

    // Slots allocated on top of the widest read a line can be asked for. readLagrange() honours
    // delays up to (size - 1 - lagrangeBehind), so 1 + lagrangeBehind = 3 of these are load-bearing
    // and the rest is the slack the tank has always carried. Held at 8 so the allocation does not
    // move: it was already wide enough for the five-point window.
    constexpr int bufferTailSlots = 8;

    static_assert (bufferTailSlots >= 1 + DelayLineFrac::lagrangeBehind,
                   "the delay-line buffers must have room for the Lagrange kernel's oldest tap");

    template <typename SmoothedType>
    void assignSmoothed (SmoothedType& s, float value, bool snap) noexcept
    {
        if (snap)
            s.setCurrentAndTargetValue (value);
        else
            s.setTargetValue (value);
    }

    /** D9 ratios: +12 = 2.0, +7 = 1.4983, -12 = 0.5. shimmerMode: 0=Off,1=+12,2=+7,3=-12,4=Dual.
        Dual is still a single PitchShifter instance in 1b (see the FDNTank::setParameters comment),
        so it reads as +12 here; irrelevant while amount is 0 regardless.
    */
    float shimmerRatioFor (int shimmerMode) noexcept
    {
        switch (shimmerMode)
        {
            case 1:  return 2.0f;
            case 2:  return 1.4983f;
            case 3:  return 0.5f;
            case 4:  return 2.0f;
            default: return 2.0f;
        }
    }
}

//==================================================================================================

void FDNTank::prepare (double newSampleRate)
{
    // Guarded here as well as in ReverbEngine::prepare so the tank is safe to prepare directly:
    // every length, coefficient and LFO increment below divides by or scales with this.
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;

    const int smoothingSteps = std::max (1, static_cast<int> (smoothingSeconds * sampleRate));

    for (int i = 0; i < numLines; ++i)
    {
        const auto idx    = static_cast<size_t> (i);
        const int  wanted = static_cast<int> (std::round (baseLengthsMs[idx] * 0.001 * sampleRate));

        primeBaseSamples[idx] = nearestPrime (std::max (16, wanted));

        // Room for Size = 200%, the modulation swing, and the interpolator's window.
        //
        // The widest read any reachable parameter combination can request is
        // primeBaseSamples * maxSizeScale + maxModSamples: setParameters clamps size to
        // maxSizeScale and modDepth to [0, 1] before scaling it by maxModSamples, and the LFO is a
        // sine, so |depth * sin| <= maxModSamples. lineLength is linearly smoothed between two
        // in-range targets and so cannot overshoot either. Frozen, the target is only ever rounded
        // down towards 2, never up.
        //
        // readLagrange() honours delays up to size - 1 - lagrangeBehind, so the requirement is
        // maxRead + 1 + lagrangeBehind <= size, i.e. 3 slots of tail on top of maxRead. The 8 slots
        // below are the pre-existing allocation slack and cover that with 5 to spare at every sample
        // rate, since the requirement is a constant and does not scale with fs -- bufferTailSlots
        // carries the static_assert that keeps the two in step.
        const float maxRead  = static_cast<float> (primeBaseSamples[idx]) * maxSizeScale + maxModSamples;
        const int   maxDelay = static_cast<int> (std::ceil (maxRead)) + bufferTailSlots;

        lines[idx].prepare (maxDelay);

        // D10: a prime allpass delay, so the sixteen nested allpasses never share a period with each
        // other or with their own line. The buffer length IS the delay -- see DensityAllpass.
        densityDelaySamples[idx] = nearestPrime (std::max (8, static_cast<int> (
                                      std::round (densityFraction
                                                  * static_cast<float> (primeBaseSamples[idx])))));
        densityAp[idx].prepare (densityDelaySamples[idx]);

        lineLength[idx].reset (smoothingSteps);
        lineLength[idx].setCurrentAndTargetValue (static_cast<float> (primeBaseSamples[idx]));

        lineInputGain[idx].reset (smoothingSteps);
        lineInputGain[idx].setCurrentAndTargetValue (0.0f);

        energyComp[idx] = 1.0f;

        // Leaves the filter silent (target gain 0) until the first setParameters call, which is
        // exactly where the per-line feedbackGain smoother used to start.
        atten[idx].prepare (sampleRate);

        // Fixed corner, set once and never touched again: no parameter can reach it (D1a(a)).
        dcBlock[idx].setCoefficient (OnePoleHP::coefficientForCutoff (dcBlockerHz, sampleRate));

        if (idx < inputDcBlock.size())
            inputDcBlock[idx].setCoefficient (OnePoleHP::coefficientForCutoff (dcBlockerHz,
                                                                              sampleRate));

        // Evenly distributed LFO phases so the eight lines never modulate in lockstep.
        lfoPhase[idx] = static_cast<float> (i) / static_cast<float> (numLines);
    }

    for (auto* s : { &filterMix, &modDepthSamples, &densityMix, &densityGain })
        s->reset (smoothingSteps);

    filterMix.setCurrentAndTargetValue (1.0f);
    modDepthSamples.setCurrentAndTargetValue (0.0f);
    densityMix.setCurrentAndTargetValue (0.0f);
    densityGain.setCurrentAndTargetValue (0.0f);
    inputGainTarget = 0.0f;

    lfoIncrement = 0.4f / static_cast<float> (sampleRate);
    frozen = false;

    for (auto& s : saturators)
        s.reset();

    shimmer.prepare (sampleRate);

    reset();
}

void FDNTank::reset() noexcept
{
    for (int i = 0; i < numLines; ++i)
    {
        const auto idx = static_cast<size_t> (i);
        lines[idx].clear();
        atten[idx].reset();
        dcBlock[idx].reset();
        densityAp[idx].clear();
        saturators[idx].reset();
        lfoPhase[idx] = static_cast<float> (i) / static_cast<float> (numLines);
    }

    for (auto& hp : inputDcBlock)
        hp.reset();

    shimmer.reset();
}

void FDNTank::setParameters (const Params& p, bool snap) noexcept
{
    const float sizeScale = sizeScaleFor (p.size);
    const bool  wasFrozen = frozen;
    frozen = p.freeze;

    // D10, Density. TWO targets from one parameter, and the split is deliberate:
    //
    //   densityGain carries the parameter. It is the allpass coefficient, so it is what decides how
    //   much the nested allpass diffuses, and at 0 the allpass degenerates to a plain delay.
    //
    //   densityMix is a 0/1 SWITCH, not a proportional control, and it exists only to make
    //   density = 0 a bit-exact bypass (§7.3 D6). It cannot be proportional: crossfading a signal
    //   with an allpassed copy of itself gives |H| = |(1-mix) + mix*e^(j phi)|, which at mix = 0.5
    //   has full NULLS wherever the allpass is 180 degrees out of phase -- a 20 dB comb in the
    //   feedback loop, i.e. the exact opposite of the colorlessness this step is for. Held at 0 or
    //   1 the composite is a pure allpass (|H| = 1) or a pure bypass, and the 50 ms smoother only
    //   passes through the comb-y middle during a knob move across zero.
    const float density01   = std::min (std::max (p.density, 0.0f), 100.0f) * 0.01f;
    const bool  densityOn   = density01 > 0.0f;
    const float densityStep = densityOn ? 1.0f : 0.0f;

    assignSmoothed (densityMix,  densityStep, snap);
    assignSmoothed (densityGain, densityMaxGain * density01, snap);

    // D1a: `damping` is the high crossover and `lowcut` the low one now. sanitise() clamps both to
    // their parameter ranges AND enforces the 4:1 separation the passivity argument needs -- it is
    // the single source of truth for that rule, and it runs here because ReverbEngine passes both
    // frequencies through unclamped.
    decaycurve::Bands bands;
    bands.lowMult     = p.decayLowMult;
    bands.highMult    = p.decayHighMult;
    bands.xoverLowHz  = p.lowCutHz;
    bands.xoverHighHz = p.dampingHz;
    bands = decaycurve::sanitise (bands);

    const float decaySeconds = std::max (p.decaySeconds, 0.01f);

    float energySum = 0.0f, compSum = 0.0f;
    std::array<float, numLines> rawComp {};

    for (int i = 0; i < numLines; ++i)
    {
        const auto  idx        = static_cast<size_t> (i);
        const float freeLength = static_cast<float> (primeBaseSamples[idx]) * sizeScale;

        if (frozen)
        {
            // Snapping the target to a whole sample makes the interpolator return the stored
            // sample verbatim, so the frozen loop is bit-exact rather than slowly low-passed.
            const float held = wasFrozen ? lineLength[idx].getTargetValue()
                                         : std::round (lineLength[idx].getCurrentValue());
            assignSmoothed (lineLength[idx], std::max (2.0f, held), snap);
        }
        else
        {
            assignSmoothed (lineLength[idx], freeLength, snap);
        }

        // The per-line decay law, now a filter rather than a scalar. m_i is the line's length in
        // samples at the CURRENT Size -- the unrounded free length, exactly the quantity the old
        // scalar law used, so a flat Decay EQ reproduces the shipped RT60 calibration.
        //
        // D10 adds one term: a pass now takes freeLength PLUS the nested allpass's delay, because
        // the allpass sits in series inside the loop and the mean group delay of a D-sample
        // Schroeder allpass is exactly D samples for any gain. The decay law converts dB per SAMPLE
        // into dB per pass, so it must be scaled by the length of the pass the filter actually sits
        // in -- otherwise T60 would stretch with the Density knob (by 10% at Size 100% and 50% at
        // Size 10%, which is the whole reachable range of the parameter). Density is a texture
        // control; it must not move the decay time.
        //
        // The target is set even while frozen: filterMix crossfades the whole filter out, so the
        // value cannot be heard, and keeping it current means leaving Freeze does not step.
        const float passLength = freeLength
                                 + densityStep * static_cast<float> (densityDelaySamples[idx]);

        atten[idx].setTarget (AttenuationFilter::targetFor (passLength, decaySeconds, bands,
                                                            sampleRate), snap);

        // Read back through the filter's accessors rather than off the Target: the broadband gain
        // is the filter's business now, and these two normalisations are the consumers the accessors
        // exist for (PLAN-R2 D1a, "the tank needs AttenuationFilter::getMidGain()").
        //
        // TWO gains, and the difference between them is a bound, not a nicety. getMidGain() is the
        // mid-band per-pass gain; getEnergyGain() is the single gain that stores as much BROADBAND
        // energy as the three bands actually store, floored at the mid gain. The injection
        // normalisation below has to read the energy gain: reading the mid gain ignores the band
        // multipliers entirely, so `decaylow`/`decayhigh` above 1.0 lengthened the real band decays
        // with nothing compensating the injection, and the surplus energy came out as LEVEL --
        // measured 3.723 peak against §7.1's 2.0 bound at multipliers 4.0/4.0, Size 200%, decay 30 s.
        // The floor inside getEnergyGain() is what keeps that a peak fix rather than a level change:
        // the two gains are the identical float wherever no band asks for more energy than the mid
        // band, which includes every multiplier at or below 1.0 and the shipping 1.4/0.7 defaults.
        const float lineGain   = atten[idx].getMidGain();
        const float lineEnergy = atten[idx].getEnergyGain();

        energySum += lineEnergy;

        // D2, per-line energy compensation. A line fed a unit injection stores energy in proportion
        // to 1/(1 - g_i^2), so the SHORT lines -- whose per-pass gain is closest to 1 -- carry
        // several times the energy of the long ones and stand out as comb peaks. Measured on the
        // eight-line tank at decay 4 s: 3.09x at Size 100%, 3.35x at Size 10%. Scaling each
        // injection by sqrt(1 - g_i^2) cancels that exactly, and normalising to unit mean below
        // leaves the overall level where the broadband normalisation put it.
        rawComp[idx] = std::sqrt (std::max (1.0f - lineGain * lineGain, 0.0f));
        compSum += rawComp[idx];
    }

    if (frozen)
    {
        // Fully transparent feedback path: the ONE crossfade takes the attenuation filter and the
        // DC blocker out together, which is the whole of it now -- there is no outer gain multiply
        // left to also have to be exactly 1 (D1a, "one condition instead of two"). No modulation
        // and no input either. The Hadamard matrix is orthogonal, so the tail holds indefinitely.
        assignSmoothed (filterMix, 0.0f, snap);
        assignSmoothed (modDepthSamples, 0.0f, snap);
        inputGainTarget = 0.0f;
    }
    else
    {
        assignSmoothed (filterMix, 1.0f, snap);
        assignSmoothed (modDepthSamples,
                        std::min (std::max (p.modDepth, 0.0f), 1.0f) * maxModSamples, snap);

        // Injection gain that keeps the steady-state wet level roughly independent of decay time.
        // gMean reads the per-line filters' band-energy-equivalent gain -- see the two-gain comment
        // in the loop above, and AttenuationFilter::getEnergyGain() for the derivation. It equals
        // the old mid-band gain (the quantity that used to be feedbackGain[i]) at every setting
        // where no band stores more energy than the mid band, which is what leaves the shipped
        // levels alone.
        // For an orthogonal FDN driven by broadband noise the stored energy settles at
        // sigma^2 * k^2 / (1 - g^2) per stored sample, so k = sqrt(1 - g^2) gives unity RMS
        // through-gain and stops a 30 s decay from accumulating tens of dB above the input.
        //
        // wetHeadroom then buys back 3 dB. The tank turns a uniform-distributed input into a
        // Gaussian-distributed tail, which raises the crest factor by roughly 7 dB, so at unity RMS
        // gain a full-scale broadband input peaks right on +6 dBFS with nothing to spare.
        const float gMean = energySum / static_cast<float> (numLines);
        const float norm  = wetHeadroom * std::sqrt (std::max (1.0f - gMean * gMean, 0.0f));
        inputGainTarget = std::min (std::max (norm, 0.02f), 1.0f);
    }

    // D2's freeze guard, and the per-line injection targets.
    //
    // THE GUARD. The compensation is sqrt(1 - g^2), so any regime where the per-line gain reaches
    // exactly 1 makes every factor 0 -- and then the unit-mean normalisation is 0/0. Forcing the
    // weights to exactly 1 while frozen removes that possibility by construction. Note what it does
    // and does not protect TODAY, because the difference matters if anyone changes Freeze: after THE
    // FOLD, Freeze holds the tail by crossfading the whole feedback path out (filterMix -> 0), NOT
    // by setting the loop gain to 1, so getMidGain() while frozen is an ordinary sub-unity decay
    // gain and the 0/0 cannot currently arise. The guard is therefore belt-and-braces for the
    // mechanism the plan (D2) describes, and it is also what makes the frozen state simple to state:
    // no injection at all, and no per-line weighting on it. §7.3 D5 asserts the weights, not the
    // arithmetic that would follow from removing them.
    const float compMean = compSum / static_cast<float> (numLines);
    const bool  compUsable = ! frozen && compMean > 1.0e-6f;

    for (int i = 0; i < numLines; ++i)
    {
        const auto idx = static_cast<size_t> (i);

        energyComp[idx] = compUsable ? rawComp[idx] / compMean : 1.0f;

        assignSmoothed (lineInputGain[idx], inputGainTarget * energyComp[idx], snap);
    }

    lfoIncrement = std::min (std::max (p.modRateHz, 0.0f), 5.0f) / static_cast<float> (sampleRate);

    // D7: control-rate drive, 0..100% -> 0..1. 1b no-op (Saturator::process is an identity
    // regardless), but the value is threaded through for real now so 7B only has to fill in the
    // curve.
    const float drive01 = std::min (std::max (p.drive, 0.0f), 100.0f) * 0.01f;

    for (auto& s : saturators)
        s.setDrive (drive01);

    // D9: shimmerMode -> ratio (Dual is still a single voice in 1b -- TODO(7A): a second instance
    // for the +7 half of Dual). shimmerAmount 0..100% -> 0..1.
    PitchShifter::Params shimmerParams;
    shimmerParams.ratio  = shimmerRatioFor (p.shimmerMode);
    shimmerParams.amount = std::min (std::max (p.shimmerAmount, 0.0f), 100.0f) * 0.01f;
    shimmer.setParameters (shimmerParams);
}

/** Four butterfly stages now rather than three, and not one line of it changed for the extra one:
    the loop is stride-doubling over numLines, so it generalises to any power of two. Only the scale
    moved, 1/sqrt(8) -> 1/sqrt(16).
*/
void FDNTank::hadamard (std::array<float, numLines>& x) noexcept
{
    for (int stride = 1; stride < numLines; stride <<= 1)
    {
        for (int base = 0; base < numLines; base += stride * 2)
        {
            for (int j = base; j < base + stride; ++j)
            {
                const auto lo = static_cast<size_t> (j);
                const auto hi = static_cast<size_t> (j + stride);

                const float a = x[lo];
                const float b = x[hi];

                x[lo] = a + b;
                x[hi] = a - b;
            }
        }
    }

    for (auto& v : x)
        v *= hadamardScale;
}

void FDNTank::processSample (float inL, float inR, float& outL, float& outR) noexcept
{
    const float fMix  = filterMix.getNextValue();
    const float depth = modDepthSamples.getNextValue();

    // D1a(a): the input's own DC, removed once for both channels before injection. See the
    // inputDcBlock declaration for why the per-line blockers cannot cover this.
    const float dcInL = inputDcBlock[0].process (inL);
    const float dcInR = inputDcBlock[1].process (inR);
    const float dMix  = densityMix.getNextValue();
    const float dGain = densityGain.getNextValue();

    std::array<float, numLines> tapped {};

    for (int i = 0; i < numLines; ++i)
    {
        const auto idx = static_cast<size_t> (i);

        float readPos = lineLength[idx].getNextValue();

        lfoPhase[idx] += lfoIncrement;

        if (lfoPhase[idx] >= 1.0f)
            lfoPhase[idx] -= 1.0f;

        if (depth > 0.0f)
            readPos += depth * std::sin (twoPi * lfoPhase[idx]);

        // readLagrange(), not read(): this is the ONE read in the plugin that sits inside a
        // feedback loop, so it is the one place where the interpolator's HF loss is taken once per
        // pass and compounds. Every feed-forward caller stays on read() -- see DelayLineFrac.h.
        tapped[idx] = lines[idx].readLagrange (readPos);
    }

    // Alternating signs decorrelate the two output taps. Eight lines each now, so the scale is
    // 1/sqrt(8) rather than 1/sqrt(4).
    outL = (tapped[0] - tapped[2] + tapped[4] - tapped[6]
            + tapped[8] - tapped[10] + tapped[12] - tapped[14]) * outputScale;
    outR = (tapped[1] - tapped[3] + tapped[5] - tapped[7]
            + tapped[9] - tapped[11] + tapped[13] - tapped[15]) * outputScale;

    std::array<float, numLines> fed {};

    for (int i = 0; i < numLines; ++i)
    {
        const auto idx = static_cast<size_t> (i);

        // D7: drive sits before the attenuation filter. 1b no-op: driven == raw bit-exactly,
        // since Saturator::process is an identity.
        const float raw      = tapped[idx];
        const float driven   = saturators[idx].process (raw);

        // D10: the nested density allpass, inside the loop and ahead of the attenuation filter. It
        // is fed UNCONDITIONALLY -- at density = 0 its output is discarded but its state stays warm,
        // so raising Density mixes in a filled allpass rather than a silent one. The delay is an
        // exact integer, so read() interpolates nothing and the section is exactly allpass:
        // |H| = 1, which is why this stage does not enter the stability argument at all (D10).
        //
        // dMix == 0 leaves driven untouched bit for bit -- §7.3 D6.
        const float diffused = densityAp[idx].process (driven, dGain);
        const float dense    = driven + dMix * (diffused - driven);

        // The entire feedback-path filter chain: decay law, then the fixed DC blocker. The
        // broadband gain is inside atten[], so there is no multiply after the crossfade.
        const float filtered = processFeedbackPath (i, dense);

        // fMix == 0 leaves dense untouched bit for bit, which is what Freeze needs -- and it is
        // now the ONLY condition Freeze needs. Frozen with Density on, the loop is delay x allpass x
        // Hadamard: every factor norm-preserving, so the tail still holds indefinitely; frozen with
        // Density at 0 it is bit-exact, which is the regime §7.1 T5 measures.
        fed[idx] = dense + fMix * (filtered - dense);
    }

    hadamard (fed);

    // D9: one shimmer voice fed a tapped sum of the tank, injected back at every line's input
    // alongside the dry signal. 1b no-op: shimmerOut is 0.0f exactly (PitchShifter::processSample
    // is an unconditional identity-to-silence), so this is inert. TODO(7A)/TODO(3A): revisit which
    // taps feed the sum once the tank is 16 lines and the real grain engine exists, PLAN-R2 D9.
    float shimmerSum = 0.0f;

    for (const float t : tapped)
        shimmerSum += t;

    const float shimmerOut = shimmer.processSample (shimmerSum);

    for (int i = 0; i < numLines; ++i)
    {
        const auto  idx      = static_cast<size_t> (i);

        // D2: the injection gain is per line now -- the broadband normalisation times that line's
        // unit-mean energy compensation, smoothed together as one value.
        const float injected = (i % 2 == 0 ? dcInL : dcInR) * lineInputGain[idx].getNextValue();

        lines[idx].push (injected + fed[idx] + shimmerOut);
    }
}

} // namespace reverb
