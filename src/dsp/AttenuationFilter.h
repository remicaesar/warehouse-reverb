#pragma once

#include "DecayCurve.h"
#include "Shelf.h"

#include <algorithm>
#include <cmath>

namespace reverb
{

/** Per-FDN-line composite attenuation filter: the Decay EQ (PLAN-R2 D1).

    One broadband scalar at the mid-band rate, plus a low shelf and a high shelf, and EVERY dB
    value scaled by that line's length in samples m_i:

        |Gamma_i(w)| = 10^(m_i * gamma_dB(w) / 20),   gamma_dB(w) = -60 / (fs * T60(w))

    The m_i scaling is the whole point. A per-line shelf carrying a fixed dB per PASS colours the
    tail just as convincingly while making every line decay at a different rate at the shelved
    frequencies -- dB per second goes as 1/m_i -- which is exactly the defect THE FOLD exists to
    delete (D1a argument 1). §7.1 T1 and T7 are the two checks that can see the difference.

    Each shelf is TWO cascaded TptShelf sections carrying half the dB each (D1 trap 1b), which
    halves the pole shift per section and so keeps the transition from spreading at large gains.

    Passivity, which a filter inside a feedback loop has to earn (D1 trap 2):
      - every band gain is clamped to [1e-5, 0.9999] in targetFor();
      - |shelf| is monotonic between 1 and its band gain, so the composite target can only exceed
        unity if two BOOSTING shelves overlap -- which decaycurve::sanitise's 4:1 crossover
        separation is there to prevent, and §7.1 T3 sweeps;
      - the broadband scalar ramps (50 ms, so a decay change glides rather than steps) while the
        shelf coefficients snap at control rate, which would otherwise let an old, high scalar meet
        a new, boosting shelf mid-ramp and put the instantaneous loop gain above 1. scalarCeiling
        bounds the product at every sample -- see setTarget().
*/
class AttenuationFilter
{
public:
    struct Target
    {
        float gainMid = 0.0f, gainLow = 0.0f, gainHigh = 0.0f;
        float xoverLowHz = 250.0f, xoverHighHz = 3500.0f;
    };

    /** Per-line target. Each band gain is clamped to [1e-5, 0.9999] here, D1 trap 1.

        With both multipliers at 1.0 all three gains are the identical float and the result is
        bit-for-bit the plain scalar law the tank shipped with (§7.1 T4).
    */
    static Target targetFor (float lineLengthSamples, float baseT60Seconds,
                             const decaycurve::Bands& bands, double sampleRate) noexcept;

    void prepare (double newSampleRate) noexcept;
    void reset() noexcept;

    /** Control rate; calls tan()/pow()/log10(). snap = true skips the broadband ramp. */
    void setTarget (const Target& t, bool snap = false) noexcept;

    float process (float x) noexcept
    {
        if (rampRemaining > 0)
        {
            scalar += scalarStep;

            if (--rampRemaining == 0)
                scalar = target.gainMid;
        }

        if (flat)
            return scalar * x;

        float y = TptShelf::processWith (lowCoeffs,  zLow[0],  x);
        y = TptShelf::processWith (lowCoeffs,  zLow[1],  y);
        y = TptShelf::processWith (highCoeffs, zHigh[0], y);
        y = TptShelf::processWith (highCoeffs, zHigh[1], y);

        return y * scalar;
    }

    /** The target broadband gain, not the ramped one -- FDNTank's input-gain normalisation wants
        the steady state, exactly as it did when this was a plain per-line scalar.
    */
    float getMidGain() const noexcept   { return target.gainMid; }

    /** The single per-pass gain that stores as much BROADBAND energy as the three bands actually
        store, floored at getMidGain(). This is what FDNTank's injection normalisation must use.

        Why it has to exist. An orthogonal FDN driven by broadband noise settles at stored energy
        proportional to k^2 / (1 - g^2), so the injection k = sqrt(1 - g^2) is what makes the
        through-gain unity and independent of decay time. With ONE gain that is exact. With three
        bands it is not: energy is the bandwidth-weighted sum of the per-band figures,

            1 / (1 - g_eff^2)  =  sum_b  w_b / (1 - g_b^2),

        and reading g_mid alone -- which is what the tank did before this accessor -- ignores the
        multipliers entirely. `decaylow`/`decayhigh` above 1.0 then lengthen the real band decays
        while the injection gain does not compensate at all, and the extra energy comes out as level:
        at Size 200% / decay 30 s, multipliers 4.0/4.0 measured +2.03 dB over the flat corner and
        3.723 peak against a 2.0 bound (§7.1 4b's `true corner, mults 4.0` leg is that measurement).

        Two deliberate properties:

        - It NEVER returns less than gainMid, so folding it in can only ever inject LESS than the
          mid-band law did. A band whose multiplier is below 1.0 stores less energy and would
          otherwise ask for more injection -- i.e. this fix would ADD level at the shipping defaults
          (decaylow 1.4 / decayhigh 0.7, where the wide high band dominates and drags the weighted
          sum below the mid band). The floor is what keeps the defaults bit-for-bit unchanged and
          keeps a peak fix from being a level change.
        - With both multipliers at 1.0 it is gainMid exactly, by the same early-out as isFlat(), so
          T4's bit-exactness is untouched; and with both multipliers EQUAL it is that common band
          gain exactly, because the three weights sum to one. The 4.0/4.0 corner is therefore
          normalised exactly right rather than approximately.

        The weights are the band edges' share of a fixed 0..20 kHz reference band (the plugin's own
        `bandwidth` ceiling), not of Nyquist, so the injection level does not move with sample rate.
        They use the ASYMPTOTIC crossovers: a deep shelf drags its own corner, so the weighting is
        first-order inside a transition. That approximation costs nothing in the corner it exists
        for, where both bands move together and the weights cancel.
    */
    float getEnergyGain() const noexcept   { return energyGain; }

    /** Composite magnitude of the target response at hz: scalar x both shelves, evaluated from the
        coefficients that are actually running. What §7.1 T3 bounds and what the display plots.
    */
    float magnitudeAt (float hz) const noexcept;

    /** True while both shelves are exactly 0 dB, i.e. both multipliers are 1.0. process() then
        skips the four sections entirely, which is both the bit-exactness of T4 and the reason a
        flat Decay EQ costs what the old scalar cost.
    */
    bool isFlat() const noexcept   { return flat; }

private:
    static constexpr float minBandGain = 1.0e-5f;
    static constexpr float maxBandGain = 0.9999f;

    // The band the energy weighting is taken over -- see getEnergyGain(). Capped at Nyquist so a
    // sample rate below 40 kHz still gets weights that sum to one over a band that exists.
    static constexpr float energyBandHz = 20000.0f;

    double sampleRate = 44100.0;
    int    rampLength = 2205;

    Target target {};

    // Both sections of a shelf carry the same coefficients (half the target dB each), so one copy
    // feeds two state variables -- see TptShelf::processWith.
    TptShelf::Coeffs lowCoeffs {}, highCoeffs {};
    float zLow[2] { 0.0f, 0.0f }, zHigh[2] { 0.0f, 0.0f };

    float energyGain    = 0.0f;
    float scalar        = 0.0f;
    float scalarStep    = 0.0f;
    float scalarCeiling = 1.0f;
    int   rampRemaining = 0;
    bool  flat          = true;
    bool  rampInitialised = false;
};

} // namespace reverb
