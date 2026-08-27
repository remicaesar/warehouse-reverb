#include "Filters.h"

#include <algorithm>
#include <cmath>

namespace reverb
{

float OnePoleLP::coefficientForCutoff (float hz, double sampleRate) noexcept
{
    // G = 1 would put the TPT pole at -1 (z_new = 2x - z), which rings at Nyquist forever. 0.5 is
    // the neutral mid-band value and is unconditionally stable, so a bad sample rate degrades the
    // cutoff rather than the filter.
    if (sampleRate <= 0.0)
        return 0.5f;

    // Keep the prewarped frequency clear of Nyquist, where tan() runs away.
    const double nyquist = sampleRate * 0.5;
    const double clamped = std::min (std::max (static_cast<double> (hz), 1.0), nyquist * 0.995);
    const double g       = std::tan (3.14159265358979323846 * clamped / sampleRate);

    return static_cast<float> (g / (1.0 + g));
}

} // namespace reverb
