#include "AttenuationFilter.h"

#include <cstring>

namespace reverb
{

namespace
{
    /** Band gain for a line of m samples at that band's dB-per-sample rate, clamped per D1 trap 1.
        -100 dB per pass is inaudible, and the upper clamp is what the scalar law already did.
    */
    float bandGainFor (float lineLengthSamples, float dbPerSample,
                       float lowestGain, float highestGain) noexcept
    {
        const float m  = std::max (lineLengthSamples, 1.0f);
        const float db = dbPerSample * m;

        return std::min (std::max (std::pow (10.0f, db * 0.05f), lowestGain), highestGain);
    }

    /** Reciprocal stored energy per unit injection for a per-pass gain g: 1 - g^2. Floored well
        above zero because it lands in a denominator -- g is clamped to 0.9999, so the true floor
        is 2e-4 and 1e-8 only guards a caller that hands over an unclamped gain.
    */
    float energyReciprocalFor (float g) noexcept
    {
        return std::max (1.0f - g * g, 1.0e-8f);
    }
}

//==================================================================================================

AttenuationFilter::Target AttenuationFilter::targetFor (float lineLengthSamples, float baseT60Seconds,
                                                        const decaycurve::Bands& bands,
                                                        double sampleRate) noexcept
{
    const auto b = decaycurve::sanitise (bands);

    Target t;
    t.gainMid     = bandGainFor (lineLengthSamples,
                                 decaycurve::bandDbPerSample (1.0f, baseT60Seconds, sampleRate),
                                 minBandGain, maxBandGain);
    t.gainLow     = bandGainFor (lineLengthSamples,
                                 decaycurve::bandDbPerSample (b.lowMult, baseT60Seconds, sampleRate),
                                 minBandGain, maxBandGain);
    t.gainHigh    = bandGainFor (lineLengthSamples,
                                 decaycurve::bandDbPerSample (b.highMult, baseT60Seconds, sampleRate),
                                 minBandGain, maxBandGain);
    t.xoverLowHz  = b.xoverLowHz;
    t.xoverHighHz = b.xoverHighHz;

    return t;
}

void AttenuationFilter::prepare (double newSampleRate) noexcept
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;

    // 50 ms, the same ramp length every other smoothed quantity in the tank uses.
    rampLength = std::max (1, static_cast<int> (0.05 * sampleRate));

    target = Target();
    reset();
    setTarget (target, true);
}

void AttenuationFilter::reset() noexcept
{
    for (int i = 0; i < 2; ++i)
    {
        zLow[i]  = 0.0f;
        zHigh[i] = 0.0f;
    }

    scalar        = target.gainMid;
    scalarStep    = 0.0f;
    rampRemaining = 0;
}

void AttenuationFilter::setTarget (const Target& t, bool snap) noexcept
{
    // An unchanged target is the common case -- ReverbEngine calls setParameters every block
    // whether or not anything moved -- and it must not restart the ramp: recomputing
    // step = (target - scalar) / rampLength every block turns a 50 ms linear glide into an
    // asymptotic approach that never arrives. Skipping also keeps 48 transcendental calls per block
    // (tan/pow/log10, eight lines x two shelves) out of the control path. Same early-out
    // juce::SmoothedValue::setTargetValue makes, for the same reason. memcmp rather than == because
    // this is a bit-pattern question, not a numeric one (and -Wfloat-equal is on).
    if (! snap && rampInitialised && std::memcmp (&t, &target, sizeof (Target)) == 0)
        return;

    const bool wasFlat = flat;

    rampInitialised = true;
    target = t;

    // The shelves carry the RATIO of a band's gain to the mid gain, so the mid gain factors out
    // into the broadband scalar. 1e-20 only keeps the logs defined for a silent target (gainMid 0,
    // which is how prepare() leaves the filter until the first setParameters call): all three gains
    // are then equal, the ratio is 1, and the shelves come out flat rather than NaN.
    const float safeMid = std::max (target.gainMid, 1.0e-20f);
    const float aLow    = std::max (target.gainLow,  1.0e-20f) / safeMid;
    const float aHigh   = std::max (target.gainHigh, 1.0e-20f) / safeMid;

    const float lowDb  = 20.0f * std::log10 (aLow);
    const float highDb = 20.0f * std::log10 (aHigh);

    // 1e-4 dB is far below anything audible and far above float noise in the log above, so a
    // genuinely flat request (both multipliers exactly 1.0, hence all three gains the identical
    // float) lands on the fast path while a real 0.01 dB shelf still runs.
    flat = std::abs (lowDb) < 1.0e-4f && std::abs (highDb) < 1.0e-4f;

    // Half the dB per section, twice (D1 trap 1b). Both sections of a shelf are identical, so one
    // coefficient derivation feeds both -- and magnitudeAt() can square one section's response.
    lowCoeffs  = TptShelf::coeffsFor (TptShelf::Kind::low,  target.xoverLowHz,  lowDb  * 0.5f, sampleRate);
    highCoeffs = TptShelf::coeffsFor (TptShelf::Kind::high, target.xoverHighHz, highDb * 0.5f, sampleRate);

    // The band-energy-equivalent gain FDNTank's injection normalisation reads -- the derivation and
    // both of its deliberate properties are in getEnergyGain()'s doc comment. The flat early-out is
    // not just a saving: it is what makes the flat case gainMid to the last bit, since the three
    // weights only sum to exactly 1.0f in exact arithmetic.
    if (flat)
    {
        energyGain = target.gainMid;
    }
    else
    {
        const float band   = std::min (energyBandHz, 0.5f * static_cast<float> (sampleRate));
        const float wLow   = std::min (std::max (target.xoverLowHz  / band, 0.0f), 1.0f);
        const float wHigh  = std::min (std::max ((band - target.xoverHighHz) / band, 0.0f), 1.0f);
        const float wMid   = std::max (1.0f - wLow - wHigh, 0.0f);

        const float energy = wLow  / energyReciprocalFor (target.gainLow)
                           + wMid  / energyReciprocalFor (target.gainMid)
                           + wHigh / energyReciprocalFor (target.gainHigh);

        const float equivalent = std::sqrt (std::max (1.0f - 1.0f / std::max (energy, 1.0f), 0.0f));

        energyGain = std::min (std::max (equivalent, target.gainMid), maxBandGain);
    }

    // Entering the flat state parks the four sections, so their state variables would be stale
    // whenever a multiplier later moves off 1.0. Clearing them means the shelves re-engage from
    // silence -- a few milliseconds of settling -- instead of from an arbitrary old sample.
    if (flat && ! wasFlat)
    {
        for (int i = 0; i < 2; ++i)
        {
            zLow[i]  = 0.0f;
            zHigh[i] = 0.0f;
        }
    }

    // Instantaneous passivity during the scalar ramp. |shelf(w)| <= max(1, A) for each shelf, so
    // scalar * max(1, aLow) * max(1, aHigh) <= 0.9999 is sufficient. When both shelves boost that
    // endpoint bound is loose (the two peaks are at opposite ends of the spectrum and cannot add),
    // and insisting on it would clamp the steady state and change the decay time -- so the ceiling
    // never drops below the target itself, which is passive by T3. The effect in that corner is
    // that a DOWNWARD gain step stops gliding and snaps, which is inaudible next to the alternative
    // of a loop gain briefly above 1.
    const float shelfBound = std::max (1.0f, aLow) * std::max (1.0f, aHigh);
    scalarCeiling = std::max (target.gainMid, maxBandGain / shelfBound);

    // Clamping the ramp's STARTING point is enough, and keeps process() free of a per-sample min:
    // a linear ramp between two values that both satisfy the bound cannot leave it, and the ramp's
    // destination (target.gainMid) satisfies it by construction above.
    scalar = std::min (scalar, scalarCeiling);

    if (snap)
    {
        scalar        = target.gainMid;
        scalarStep    = 0.0f;
        rampRemaining = 0;
    }
    else
    {
        rampRemaining = rampLength;
        scalarStep    = (target.gainMid - scalar) / static_cast<float> (rampLength);
    }
}

float AttenuationFilter::magnitudeAt (float hz) const noexcept
{
    const float low  = TptShelf::magnitudeFor (lowCoeffs,  hz, sampleRate);
    const float high = TptShelf::magnitudeFor (highCoeffs, hz, sampleRate);

    return target.gainMid * low * low * high * high;
}

} // namespace reverb
