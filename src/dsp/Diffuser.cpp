#include "Diffuser.h"

#include "Primes.h"

#include <algorithm>
#include <cmath>

namespace reverb
{

namespace
{
    // Deliberately different, mutually irrational-ish length sets per channel.
    constexpr float leftLengthsMs[Diffuser::numStages]  = { 4.77f,  3.59f, 12.73f,  9.31f };
    constexpr float rightLengthsMs[Diffuser::numStages] = { 5.31f,  4.13f, 11.17f, 10.73f };
}

void Diffuser::prepare (double sampleRate, Channel channel)
{
    const float* lengthsMs = (channel == Channel::left ? leftLengthsMs : rightLengthsMs);

    for (int i = 0; i < numStages; ++i)
    {
        const auto idx     = static_cast<size_t> (i);
        const int  wanted  = static_cast<int> (std::round (lengthsMs[idx] * 0.001 * sampleRate));
        const int  snapped = nearestPrime (std::max (4, wanted));

        delaySamples[idx] = static_cast<float> (snapped);
        stages[idx].prepare (snapped + 4);
    }
}

void Diffuser::reset() noexcept
{
    for (auto& stage : stages)
        stage.clear();
}

} // namespace reverb
