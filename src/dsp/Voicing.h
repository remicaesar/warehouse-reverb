#pragma once

#include <cstddef>

namespace reverb
{

/** Per-algorithm-mode data: everything that changes between Room/Plate/Hall/Ambience/Space is a
    field here, never a branch in the audio loop (PLAN-R2 D8, "modes are DATA"). prepare() always
    allocates for the worst case across every Voicing, so switching modes is a parameter retarget,
    not a reallocation.
*/
struct Voicing
{
    const float* lineLengthsMs;
    int   numLines;
    int   numActiveLines;

    float sizeMapExponent, sizeFloorScale;

    int   diffuserStages;
    float diffuserGainScale;

    const float* erTapsMs;
    int   numErTaps;

    float densityScale, decayLowBias, decayHighBias;
};

enum class Algorithm { room, plate, hall, ambience, space, count };   // five, D8

namespace detail
{
    // The current 8-line tank's base lengths in ms at Size = 100% (PLAN-R2 D8, "consistent with
    // the current 8-line tank"). Mirrors FDNTank.cpp's baseLengthsMs verbatim; kept as its own
    // copy rather than shared because FDNTank.cpp holds its table in an anonymous namespace and
    // reads it directly in prepare() -- 3A is the step that actually threads Voicing through.
    inline constexpr float kLineLengthsMs[8] =
    {
        23.17f, 29.71f, 34.13f, 41.53f, 49.79f, 57.23f, 66.41f, 78.31f
    };

    inline constexpr const char* kNames[] = { "Room", "Plate", "Hall", "Ambience", "Space" };
}

/** 1b no-op: every algorithm returns this SAME Voicing, describing today's 8-line tank exactly --
    sizeMapExponent = 1 is today's linear Size map, there are no ER taps (EarlyReflections is a 1b
    no-op too), and every scale/bias is neutral (1.0). diffuserStages mirrors Diffuser::numStages
    (src/dsp/Diffuser.h). TODO(3A): differentiate numLines/numActiveLines and the Size map per
    algorithm. TODO(8A): differentiate diffuserStages, erTapsMs, densityScale, decayLowBias/
    decayHighBias per algorithm.
*/
inline const Voicing& voicingFor (Algorithm algorithm) noexcept
{
    (void) algorithm;

    static constexpr Voicing sharedVoicing
    {
        detail::kLineLengthsMs, 8, 8,
        1.0f, 0.0f,
        4, 1.0f,
        nullptr, 0,
        1.0f, 1.0f, 1.0f
    };

    return sharedVoicing;
}

inline const char* nameFor (Algorithm algorithm) noexcept
{
    const auto index = static_cast<int> (algorithm);

    if (index < 0 || index >= static_cast<int> (Algorithm::count))
        return "Hall";

    return detail::kNames[static_cast<std::size_t> (index)];
}

} // namespace reverb
