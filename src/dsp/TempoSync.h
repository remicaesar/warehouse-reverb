#pragma once

#include <algorithm>
#include <juce_audio_basics/juce_audio_basics.h>

namespace reverb::temposync
{

/** Pre-delay-only tempo sync (PLAN-R2 D6): eleven musical divisions, ms computed from a live BPM
    read once per processBlock. Header-only, juce_audio_basics only (C10), which is what lets the
    playhead-reading half below be unit-tested from the DSP-only test target with a stub
    AudioPlayHead.

    Step 5A made this real. Two halves, deliberately separated:

    - the pure division arithmetic (nameFor / beatsFor / millisecondsFor / divisionForIndex), and
    - PreDelaySync, which owns the ONE piece of state tempo sync needs: the last tempo the host
      reported. PluginProcessor holds an instance and feeds it the playhead once per block.

    The CROSSFADE half of D6 ("a division change crossfades, it does not glide") is not here: it is
    a property of the delay-line read, so it lives in ReverbEngine::processChunk, which is where the
    two read positions are. This class only answers "how many milliseconds", and it answers with a
    plain float precisely so the engine cannot tell a synced value from a typed one -- the engine
    detects the DISCRETE change from Params::preDelaySync / preDelayDivision instead, which keeps
    the playhead out of the DSP entirely.
*/
enum class Division { d1_32, d1_16T, d1_16, d1_8T, d1_16D, d1_8, d1_4T, d3_16, d1_4, d1_2, d1_1, count };

inline const char* nameFor (Division d) noexcept
{
    switch (d)
    {
        case Division::d1_32:  return "1/32";
        case Division::d1_16T: return "1/16T";
        case Division::d1_16:  return "1/16";
        case Division::d1_8T:  return "1/8T";
        case Division::d1_16D: return "1/16.";
        case Division::d1_8:   return "1/8";
        case Division::d1_4T:  return "1/4T";
        case Division::d3_16:  return "3/16";
        case Division::d1_4:   return "1/4";
        case Division::d1_2:   return "1/2";
        case Division::d1_1:   return "1/1";
        case Division::count:  break;
    }

    return "";
}

/** Division length in beats (a quarter note is 1.0), so millisecondsFor() is one multiply. */
inline float beatsFor (Division d) noexcept
{
    switch (d)
    {
        case Division::d1_32:  return 0.125f;
        case Division::d1_16T: return 1.0f / 6.0f;
        case Division::d1_16:  return 0.25f;
        case Division::d1_8T:  return 1.0f / 3.0f;
        case Division::d1_16D: return 0.375f;
        case Division::d1_8:   return 0.5f;
        case Division::d1_4T:  return 2.0f / 3.0f;
        case Division::d3_16:  return 0.75f;
        case Division::d1_4:   return 1.0f;
        case Division::d1_2:   return 2.0f;
        case Division::d1_1:   return 4.0f;
        case Division::count:  break;
    }

    return 1.0f;
}

/** ms for a division at bpm; 0 when bpm <= 0 so the caller can detect the no-tempo case. */
inline float millisecondsFor (Division d, double bpm) noexcept
{
    if (bpm <= 0.0)
        return 0.0f;

    return beatsFor (d) * static_cast<float> (60000.0 / bpm);
}

/** Clamps an unvalidated parameter index (Params::preDelayDivision is a plain int) onto the list. */
inline Division divisionForIndex (int index) noexcept
{
    const int last = static_cast<int> (Division::count) - 1;

    return static_cast<Division> (std::min (std::max (index, 0), last));
}

/** The pre-delay parameter's own upper bound, PLAN-R2 3.1 (0..500 ms), which D6 deliberately does
    NOT widen: 1/4 at 120 bpm is exactly 500 ms, and longer divisions clamp. This constant lives
    here rather than being read off ReverbEngine because the authority is the PARAMETER range -- the
    engine's identical bound is a consequence of it, and the GUI has to show the clamped value
    without asking the engine. */
constexpr float maxPreDelayMs = 500.0f;

//==================================================================================================
/** The host-facing half of D6: remembers the last tempo the host reported and turns a division into
    a pre-delay in milliseconds.

    THE FALLBACK LADDER, which is the part hosts make interesting:

      1. a tempo was reported this block          -> use it
      2. no tempo this block but one was seen before (a host that reports intermittently, or is
         stopped) -> use the last one seen, so the sound does not change under the user
      3. no tempo has EVER been seen              -> use the free `predelay` value in ms

    Step 3 is where PLAN-R2's own text and its test H6 disagree: D6 says "else 120" bpm, H6 says
    "falls back to the free ms value, not to zero" and asserts the typed 40 ms. H6's reading is
    implemented, because a value the user typed is a real intention where 120 bpm is a guess, and
    because it is the reading the plan made testable. Both agree on the thing that actually matters
    and that the mutation targets: NEVER fall back to 0, which would collapse the pre-delay to
    DelayLineFrac's 1-sample clamp and change the sound on a host that reports tempo intermittently.
*/
class PreDelaySync
{
public:
    /** Control rate, once per processBlock and nowhere else -- AudioPlayHead::getPosition() is
        documented as callable only from processBlock. A null playhead, an empty position or a
        position carrying no tempo all leave the remembered tempo untouched. */
    void updateFromPlayHead (const juce::AudioPlayHead* playHead) noexcept
    {
        if (playHead == nullptr)
            return;

        const auto position = playHead->getPosition();

        if (! position.hasValue())
            return;

        const auto bpm = position->getBpm();

        if (bpm.hasValue() && *bpm > 0.0)
            lastKnownBpm = *bpm;
    }

    /** The pre-delay to hand the engine, in ms, clamped to the parameter's own range. */
    float preDelayMsFor (bool syncEnabled, Division division, float freeMs) const noexcept
    {
        if (! syncEnabled || lastKnownBpm <= 0.0)
            return freeMs;

        return std::min (millisecondsFor (division, lastKnownBpm), maxPreDelayMs);
    }

    /** 0 until the host has reported a tempo at least once. */
    double getLastKnownBpm() const noexcept   { return lastKnownBpm; }

private:
    // Deliberately NOT cleared by a host reset(): the tempo is knowledge about the session, not
    // audio state, and forgetting it would move the pre-delay on a transport stop.
    double lastKnownBpm = 0.0;
};

} // namespace reverb::temposync
