#pragma once

#include "SchroederAllpass.h"

#include <array>

namespace reverb
{

/** Four cascaded Schroeder allpasses that smear the input before it reaches the tank.

    The left and right instances are prepared from different length sets (Channel::left /
    Channel::right), so the two channels decorrelate here rather than in the tank. That
    decorrelation is where the stereo image of the reverb comes from.
*/
class Diffuser
{
public:
    static constexpr int numStages = 4;

    enum class Channel { left, right };

    void prepare (double sampleRate, Channel channel);
    void reset() noexcept;

    /** diffusion is 0..1; it maps onto an allpass gain of 0 .. 0.7. */
    float process (float x, float diffusion) noexcept
    {
        const float g = 0.7f * diffusion;

        for (int i = 0; i < numStages; ++i)
            x = stages[static_cast<size_t> (i)].process (x, delaySamples[static_cast<size_t> (i)], g);

        return x;
    }

private:
    std::array<SchroederAllpass, numStages> stages {};
    std::array<float, numStages> delaySamples { { 0.0f, 0.0f, 0.0f, 0.0f } };
};

} // namespace reverb
