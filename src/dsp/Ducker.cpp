#include "Ducker.h"

namespace reverb
{

namespace
{
    float clampf (float v, float lo, float hi) noexcept
    {
        return std::min (std::max (v, lo), hi);
    }
}

//==================================================================================================

void Ducker::prepare (double newSampleRate)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;

    reset();
    updateCoefficients();
}

void Ducker::reset() noexcept
{
    detectorState = 0.0f;
    stage1Db      = 0.0f;
    stage2Db      = 0.0f;
    reductionDb   = 0.0f;
}

void Ducker::setParameters (const Params& p) noexcept
{
    // Seed the incoming detector from the outgoing one on a character change, so switching
    // Gentle <-> Pump mid-tail does not re-attack from zero. Cheap here, impossible at audio rate.
    if (p.character != params.character)
        detectorState = p.character == Character::pump ? std::sqrt (std::max (detectorState, 0.0f))
                                                       : detectorState * detectorState;

    params = p;
    updateCoefficients();
}

void Ducker::updateCoefficients() noexcept
{
    const float amount = clampf (params.amountPercent, 0.0f, 100.0f);
    const bool  engaged = amount > 0.0f;

    // Leaving bypass behind a stale reduction would jump the wet gain the moment the amount comes
    // off zero; clearing here means re-engaging always attacks from unity.
    if (active && ! engaged)
        reset();

    active = engaged;
    depth  = amount * 0.01f;

    if (! engaged)
        return;   // every coefficient below is unused while processSample() returns 1.0f

    peakDetector = params.character == Character::pump;
    twoPole      = ! peakDetector;

    thresholdDb = clampf (params.thresholdDb, -60.0f, 0.0f);

    const float kneeDb = peakDetector ? pumpKneeDb : gentleKneeDb;

    halfKneeDb = 0.5f * kneeDb;
    kneeScale  = kneeDb > 0.0f ? 1.0f / (2.0f * kneeDb) : 0.0f;

    // The linear level at which the knee starts contributing. Below it the soft knee is exactly
    // zero, which is what lets processSample() skip its log10 rather than approximate it.
    kneeStartLinear = std::pow (10.0f, (thresholdDb - halfKneeDb) * 0.05f);

    const float attackMs = peakDetector ? pumpAttackMs : gentleAttackMs;

    // Per-pole time constants: Gentle's two identical poles each carry tau / 2.1462 so that the
    // COMPOSITE reaches 63 % (attack) and 37 % (release) at the stated time. See the header.
    const float poleScale = twoPole ? twoPoleTimeScale : 1.0f;

    // The release pole and the detector go through the header's published law rather than
    // recomputing it here, so the number H3d predicts against and the number that runs cannot be
    // two different numbers. Both clamp params.releaseMs to PLAN-R2 3.2's range internally, which
    // is where the clamp has to happen because Params carries plain unvalidated floats.
    attackCoeff  = retentionCoefficient (attackMs / poleScale);
    releaseCoeff = retentionCoefficient (poleTauMs (params.releaseMs, params.character));

    // The detector must never be the slower pole (header, second bullet).
    detectorReleaseCoeff = retentionCoefficient (detectorTauMs (params.releaseMs));
    detectorAttackCoeff  = 1.0f - detectorReleaseCoeff;
}

float Ducker::retentionCoefficient (float tauMs) const noexcept
{
    const double tauSamples = std::max (1.0e-3, static_cast<double> (tauMs) * 0.001 * sampleRate);

    return static_cast<float> (std::exp (-1.0 / tauSamples));
}

} // namespace reverb
