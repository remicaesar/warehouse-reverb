#include "WetChain.h"

#include <cmath>

namespace reverb
{

namespace
{
    float clampf (float v, float lo, float hi) noexcept
    {
        return std::min (std::max (v, lo), hi);
    }

    /** g = tan (pi * fc / fs) for the bass-mono SVF, with fc kept clear of DC and Nyquist for the
        same reason OnePoleLP::coefficientForCutoff does it (tan runs away at Nyquist). Calls tan();
        control rate only. */
    float svfGainForCutoff (float hz, double sampleRate) noexcept
    {
        const double fs      = sampleRate > 0.0 ? sampleRate : 44100.0;
        const double clamped = std::min (std::max (static_cast<double> (hz), 0.01), fs * 0.4975);

        return static_cast<float> (std::tan (3.14159265358979323846 * clamped / fs));
    }
}

//==================================================================================================

void WetChain::prepare (double newSampleRate)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;

    const int steps = std::max (1, static_cast<int> (smoothingSeconds * sampleRate));

    for (auto& s : smoothed)
        s.reset (steps);

    // Every smoother starts at the value its own bypass/neutral setting maps to, so the first
    // engagement of a stage glides out of neutral instead of out of whatever prepare() left behind.
    const Params defaults;

    const auto low  = TptShelf::coeffsFor (TptShelf::Kind::low,  tiltPivotHz, -0.5f * defaults.tiltDb, sampleRate);
    const auto high = TptShelf::coeffsFor (TptShelf::Kind::high, tiltPivotHz,  0.5f * defaults.tiltDb, sampleRate);

    smoothed[smWidth]     .setCurrentAndTargetValue (defaults.widthPercent * 0.01f);
    smoothed[smLowCut]    .setCurrentAndTargetValue (OnePoleHP::coefficientForCutoff (defaults.wetLowCutHz, sampleRate));
    smoothed[smHighCut]   .setCurrentAndTargetValue (OnePoleLP::coefficientForCutoff (defaults.wetHighCutHz, sampleRate));
    smoothed[smTiltLowG]  .setCurrentAndTargetValue (low.G);
    smoothed[smTiltLowM0] .setCurrentAndTargetValue (low.m0);
    smoothed[smTiltLowM1] .setCurrentAndTargetValue (low.m1);
    smoothed[smTiltHighG] .setCurrentAndTargetValue (high.G);
    smoothed[smTiltHighM0].setCurrentAndTargetValue (high.m0);
    smoothed[smTiltHighM1].setCurrentAndTargetValue (high.m1);
    smoothed[smBassMono]  .setCurrentAndTargetValue (svfGainForCutoff (defaults.bassMonoHz, sampleRate));

    lowCutActive = highCutActive = tiltActive = bassMonoActive = false;

    reset();
}

void WetChain::reset() noexcept
{
    // Filter state is cleared; the smoothers deliberately are NOT, so they ramp across a reset()
    // exactly like ReverbEngine's own widthSmoothed used to. ReverbEngine::reset() runs on the
    // non-finite recovery path, so this is also what keeps a poisoned sample from living on in the
    // wet EQ or the bass-mono band split after the engine has cleaned everything upstream of it.
    for (int c = 0; c < 2; ++c)
    {
        lowCut[c].reset();
        highCut[c].reset();

        tiltLowZ[c]  = 0.0f;
        tiltHighZ[c] = 0.0f;
    }

    bassMonoSvf.reset();
}

void WetChain::setSmoothed (size_t index, float value, bool snap) noexcept
{
    if (snap)
        smoothed[index].setCurrentAndTargetValue (value);
    else
        smoothed[index].setTargetValue (value);
}

void WetChain::setParameters (const Params& p, bool snap) noexcept
{
    // Clamped here rather than trusted: ReverbEngine passes the wet-EQ and bass-mono fields through
    // unclamped, so the bypass tests below have to be made from a value that is in range -- a
    // requested 10 Hz low cut is a 20 Hz low cut, which is off, not a 10 Hz filter.
    const float widthScale = clampf (p.widthPercent, 0.0f, 200.0f) * 0.01f;
    const float lowCutHz   = clampf (p.wetLowCutHz,  lowCutMinHz,  lowCutMaxHz);
    const float highCutHz  = clampf (p.wetHighCutHz, highCutMinHz, highCutMaxHz);
    const float tiltDb     = clampf (p.tiltDb,       -tiltMaxDb,   tiltMaxDb);
    const float bassHz     = clampf (p.bassMonoHz,   0.0f,         bassMonoMaxHz);

    // A tilt of t dB is -t/2 below the pivot and +t/2 above it, so the parameter reads as the full
    // top-to-bottom tilt. Both shelves prewarp their corner by sqrt(A) in opposite directions
    // (Shelf.h), which is what makes the product exactly unity AT the pivot for every t.
    const auto low  = TptShelf::coeffsFor (TptShelf::Kind::low,  tiltPivotHz, -0.5f * tiltDb, sampleRate);
    const auto high = TptShelf::coeffsFor (TptShelf::Kind::high, tiltPivotHz,  0.5f * tiltDb, sampleRate);

    setSmoothed (smWidth,      widthScale, snap);
    setSmoothed (smLowCut,     OnePoleHP::coefficientForCutoff (lowCutHz, sampleRate), snap);
    setSmoothed (smHighCut,    OnePoleLP::coefficientForCutoff (highCutHz, sampleRate), snap);
    setSmoothed (smTiltLowG,   low.G,  snap);
    setSmoothed (smTiltLowM0,  low.m0, snap);
    setSmoothed (smTiltLowM1,  low.m1, snap);
    setSmoothed (smTiltHighG,  high.G,  snap);
    setSmoothed (smTiltHighM0, high.m0, snap);
    setSmoothed (smTiltHighM1, high.m1, snap);
    setSmoothed (smBassMono,   svfGainForCutoff (bassHz, sampleRate), snap);

    // Gates, and the rising-edge state clear the class comment argues for. Falling edges leave the
    // state alone: nothing reads it while the stage is bypassed, and clearing it on the way out
    // would make the clear depend on the order two parameters happened to be moved in.
    const bool lowCutNow   = lowCutHz > lowCutMinHz;
    const bool highCutNow  = highCutHz < highCutMaxHz;
    const bool tiltNow     = tiltDb != 0.0f;
    const bool bassMonoNow = bassHz > 0.0f;

    if (lowCutNow && ! lowCutActive)
    {
        lowCut[0].reset();
        lowCut[1].reset();
    }

    if (highCutNow && ! highCutActive)
    {
        highCut[0].reset();
        highCut[1].reset();
    }

    if (tiltNow && ! tiltActive)
        tiltLowZ[0] = tiltLowZ[1] = tiltHighZ[0] = tiltHighZ[1] = 0.0f;

    if (bassMonoNow && ! bassMonoActive)
        bassMonoSvf.reset();

    lowCutActive   = lowCutNow;
    highCutActive  = highCutNow;
    tiltActive     = tiltNow;
    bassMonoActive = bassMonoNow;
}

} // namespace reverb
