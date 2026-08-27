#pragma once

#include "DelayLineFrac.h"
#include "SchroederAllpass.h"

#include <array>
#include <juce_audio_basics/juce_audio_basics.h>

namespace reverb
{

/** Early-reflection cluster: a 10-tap fractional multitap per channel followed by a two-stage
    Schroeder allpass smear, decorrelated L/R, blended against the FDN tank's late output before
    the dry/wet mix (PLAN-R2 D4).

        in -> preDelay -> bandwidth -+-> diffuser -> FDN tank ----> late
                                     +-> ER multitap -> smear ----> early

    D4 chose the PARALLEL topology over series specifically so that `erMix = 0` can be an exact
    identity on the late path: ReverbEngine::processChunk skips the blend entirely at that value,
    so the late samples reach WetChain bit-for-bit. A series topology cannot offer that, because
    DelayLineFrac::read clamps its delay to >= 1 sample.

    Tap times are fractional and smoothed, so an ER Size sweep glides rather than clicking, and
    both the tap times and the smear lengths interpolate between a shared MEAN row (spread 0 -> a
    mono early field) and each channel's own DIVERGENT row (spread 100 -> decorrelated). D4:
    interpolating toward a shared mean rather than scaling one channel's times avoids introducing a
    pitch offset between the channels.

    The tap table is per-algorithm data (D8), so this class can take one from the outside via
    setVoicing(); until a Voicing actually supplies one (step 8A) the D4 default table below is
    used. prepare() is the only allocator and always sizes for the worst case: the longest tap any
    voicing may specify, at ER Size 200 %, at the current sample rate.
*/
class EarlyReflections
{
public:
    struct Params { float sizePercent = 100.0f, spreadPercent = 60.0f; bool freeze = false; };

    /** Longest tap time, in ms at ER Size 100 %, that any voicing may specify. prepare() sizes the
        tap buffers from this, so a voicing table is clamped to it in setVoicing() rather than being
        allowed to read past the end of the line. */
    static constexpr float maxTapMs = 80.0f;

    /** ER Size caps the tap-time scale at 2.0x (parameter range 25..200 %). */
    static constexpr float maxSizeScale = 2.0f;

    static constexpr int maxTaps = 16;

    // ---- D4's tap and smear tables. Public because tests reconstruct the smear cascade to
    // deconvolve the ER impulse response back into its tap train (see EarlyReflectionsTests.cpp),
    // and assert the tap positions against these exact numbers. ----

    static constexpr int numDefaultTaps = 10;

    /** ms at ER Size 100 %: the shared row both channels collapse onto at ER Spread 0. */
    static constexpr float defaultTapsMeanMs[numDefaultTaps] =
        { 4.9f, 9.6f, 14.4f, 20.4f, 26.6f, 32.9f, 40.5f, 48.8f, 58.3f, 69.0f };

    /** ms at ER Size 100 %, ER Spread 100 %: the fully decorrelated per-channel rows. */
    static constexpr float defaultTapsLeftMs[numDefaultTaps] =
        { 4.3f, 8.9f, 13.7f, 19.1f, 25.3f, 31.7f, 39.1f, 47.3f, 56.9f, 67.1f };
    static constexpr float defaultTapsRightMs[numDefaultTaps] =
        { 5.7f, 10.3f, 15.1f, 21.7f, 27.9f, 34.1f, 41.9f, 50.3f, 59.7f, 70.9f };

    static constexpr int   numSmearStages = 2;
    static constexpr float smearGain      = 0.35f;
    static constexpr float smearLeftMs[numSmearStages]  = { 7.3f, 11.9f };
    static constexpr float smearRightMs[numSmearStages] = { 8.7f, 13.1f };

    /** The L/R divergence applied to a voicing-supplied tap table, as a fraction of each tap time.
        The default table above carries its own explicit rows and never uses this. */
    static constexpr float voicingSpreadFraction = 0.05f;

    /** -5 dB of peak headroom on the early path, the exact counterpart of FDNTank's wetHeadroom
        (which buys 3 dB on the late path for the same class of reason) -- a documented constant
        rather than a limiter, PLAN-R2 A5.

        Why it is needed and why 3 dB is not enough: D4's gain law normalises sum (g_k^2) = 1, so
        the cluster carries its input's ENERGY -- but ten taps of one signal can align, and the
        worst-case PEAK gain is sum |g_k| = 3.00 (+9.5 dB). Unlike the tank, whose injection gain
        already spends sqrt (1 - g^2) on exactly this, the early path has no normalisation to lean
        on. Measured at ReverbEngineTests check 4b's corner (Size 200 %, decay 30 s, fully wet,
        width 200 %, unity trim, full-scale broadband in) at erMix = 100 %, WITHOUT this constant:
        3.025 at 48 kHz and 3.058 at 192 kHz, i.e. +9.6 dBFS, over the +2.99 dBFS the README
        documents as the worst case and over the peak <= 2.0 bound checks 4/4b assert. The peak
        scales exactly linearly with this constant (at erMix = 100 the wet IS the early signal), so
        -5 dB puts the same corner at ~1.72 -- inside 2.0 with margin.

        Consequence, deliberate and ruled on by the CTO: this lowers the early level at EVERY
        setting, not just at the extremes. */
    static constexpr float erHeadroom = 0.56234133f;   // -5 dB

    void  prepare (double sampleRate);
    void  reset() noexcept;
    void  setVoicing (const struct Voicing& voicing) noexcept;   // control rate; tap tables come from here
    void  setParameters (const Params& p, bool snap) noexcept;
    void  processSample (float inL, float inR, float& outL, float& outR) noexcept;
    float getMaxTapSeconds() const noexcept;

private:
    using Smoothed = juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>;

    // Must track ReverbEngine::smoothingSeconds: E4 (PLAN-R2 7.2) is a no-click requirement on the
    // ER Size sweep, and this is the ramp that buys it.
    static constexpr float smoothingSeconds = 0.05f;

    /** Loads a tap table (the D4 default row when ms == nullptr), derives D4's tap gains from it
        and re-derives the sample-domain copies. Control rate only: calls sqrt(). */
    void loadTaps (const float* ms, int count) noexcept;

    /** Re-derives the sample-domain tap/smear tables from the millisecond tables. Called from both
        prepare() (sample rate changed) and loadTaps() -- never from process(). */
    void updateSampleTables() noexcept;

    double sampleRate = 44100.0;

    // The table setVoicing() last accepted, so the per-block call can skip the work when the
    // voicing has not actually changed. Initially "the default table", which prepare() loads.
    const float* voicingTable = nullptr;
    int          voicingCount = 0;

    // Tap table in ms at ER Size 100 %, plus the sample-domain copies process() actually reads.
    int    numTaps = numDefaultTaps;
    std::array<float, maxTaps> tapMeanMs {}, tapLeftMs {}, tapRightMs {}, tapGain {};
    // Sample-domain tap positions as (mean, per-channel offset from the mean), which is the form
    // the audio loop wants: delay = mean * sizeScale + offset * (spread * sizeScale), two multiply
    // -adds per tap instead of a lerp followed by a scale.
    std::array<float, maxTaps> tapMeanSamples {}, tapOffsetLeftSamples {}, tapOffsetRightSamples {};

    std::array<float, numSmearStages> smearMeanSamples {}, smearOffsetLeftSamples {}, smearOffsetRightSamples {};

    std::array<DelayLineFrac, 2> tapLine {};
    std::array<std::array<SchroederAllpass, numSmearStages>, 2> smear {};

    // Freeze gates the early cluster the same way FDNTank gates its own input -- a smoothed
    // injection gain to exactly 0, so no new material reaches the wet path through the parallel ER
    // branch while the tail is held. Without it Freeze is audibly leaky at any erMix > 0.
    Smoothed sizeSmoothed, spreadSmoothed, inputGain;
};

} // namespace reverb
