#pragma once

namespace reverb
{

/** Topology-preserving-transform (zero-delay-feedback) one-pole lowpass.

    The cutoff enters as a single coefficient G, which is monotonic in frequency and safe to
    interpolate per sample. That lets the engine smooth a cutoff sweep without calling tan() in
    the audio loop: map hz -> G once per block with coefficientForCutoff(), then ramp G.
*/
class OnePoleLP
{
public:
    /** Maps a cutoff in Hz to the TPT coefficient. Calls tan(); use at control rate only. */
    static float coefficientForCutoff (float hz, double sampleRate) noexcept;

    void setCutoff (float hz, double sampleRate) noexcept   { G = coefficientForCutoff (hz, sampleRate); }
    void setCoefficient (float newG) noexcept               { G = newG; }

    void reset() noexcept                                   { z = 0.0f; }

    float process (float x) noexcept
    {
        const float v = (x - z) * G;
        const float y = v + z;
        z = y + v;
        return y;
    }

private:
    float G = 0.5f;
    float z = 0.0f;
};

//==================================================================================================
/** One-pole highpass, built as x - lowpass(x) so it shares the lowpass coefficient mapping. */
class OnePoleHP
{
public:
    static float coefficientForCutoff (float hz, double sampleRate) noexcept
    {
        return OnePoleLP::coefficientForCutoff (hz, sampleRate);
    }

    void setCutoff (float hz, double sampleRate) noexcept    { lp.setCutoff (hz, sampleRate); }
    void setCoefficient (float newG) noexcept                { lp.setCoefficient (newG); }

    void reset() noexcept                                    { lp.reset(); }

    float process (float x) noexcept                         { return x - lp.process (x); }

private:
    OnePoleLP lp;
};

} // namespace reverb
