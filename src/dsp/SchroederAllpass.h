#pragma once

#include "DelayLineFrac.h"

namespace reverb
{

/** Single Schroeder allpass section.

    Output is delayed - g * v with v = x + g * delayed, i.e. -g * x + (1 - g^2) * delayed, which is
    the standard allpass numerator/denominator pair. The delay and gain are passed per sample so the
    caller can smooth them.
*/
class SchroederAllpass
{
public:
    void prepare (int maxDelaySamples)   { delay.prepare (maxDelaySamples); }
    void clear() noexcept                { delay.clear(); }

    float process (float x, float delaySamples, float g) noexcept
    {
        const float delayed = delay.read (delaySamples);
        const float v       = x + g * delayed;

        delay.push (v);

        return delayed - g * v;
    }

private:
    DelayLineFrac delay;
};

} // namespace reverb
