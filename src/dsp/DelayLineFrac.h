#pragma once

#include <vector>

namespace reverb
{

/** Circular delay line with interpolated fractional reads.

    Reads are expressed in samples-ago: after push (x), read (1.0f) returns x. Two kernels are
    offered and they are NOT interchangeable in cost or in bounds:

    - read() interpolates linearly between two adjacent samples. |H| = cos(pi f / fs) at a
      half-sample offset, i.e. -1.25 dB at 8 kHz / 48 kHz. Fine for a feed-forward tap, where the
      loss is taken exactly once.
    - readLagrange() interpolates with a 5-point (4th-order) Lagrange kernel: -0.047 dB at the same
      point, 26x less. It touches two samples either side of the interpolation point instead of one,
      so it has its own, narrower, delay bounds - see getMaxDelayLagrange().

    Which one to use is a question about ACCUMULATION, not about audibility of a single read. Inside
    a feedback loop the loss is taken once per pass and a pass takes m samples, so the loss in dB
    per second goes as 1/m: a fixed dB per pass is exactly the defect class FDNTank's attenuation
    fold exists to remove, and linear interpolation smuggles it back in. Feed-forward callers (the
    diffuser, early reflections, pre-delay) take it once and should stay on read().

    Both kernels clamp the read delay so a modulated or smoothed read position can never leave the
    buffer, and both do the wrap in integers - see the comments in the .cpp for why that matters.
    Both require prepare() to have been called; neither is safe on a default-constructed line.
*/
class DelayLineFrac
{
public:
    DelayLineFrac() = default;

    /** Allocates the buffer. The only allocating call; never invoke from a process function. */
    void prepare (int maxSamples);

    /** Zeroes the buffer and rewinds the write head. */
    void clear() noexcept;

    void push (float x) noexcept
    {
        buffer[static_cast<size_t> (writeIndex)] = x;

        if (++writeIndex >= size)
            writeIndex = 0;
    }

    float read (float delaySamples) const noexcept;

    /** Fractional read with a 5-point Lagrange kernel. Use inside a feedback loop; see the class
        comment for why, and getMaxDelayLagrange() / minLagrangeDelay for the bounds it needs.

        Out of line in the .cpp on purpose: with LTO on, the link-time inliner already inlines it into
        FDNTank's loop, and forcing it into the header made the measured cost WORSE by ~2 us per
        512-sample block, not better.
    */
    float readLagrange (float delaySamples) const noexcept;

    /** How far either side of the requested whole-sample delay the Lagrange kernel reaches. The
        taps are at (whole - lagrangeAhead) .. (whole + lagrangeBehind) samples ago.
    */
    static constexpr int lagrangeAhead  = 2;
    static constexpr int lagrangeBehind = 2;

    /** Number of taps the kernel reads. */
    static constexpr int kernelWidth = lagrangeAhead + lagrangeBehind + 1;

    /** Smallest delay readLagrange() will honour. The newest sample a line holds is "1 sample ago",
        and the kernel reads one sample newer than the requested delay, so 1 + lagrangeAhead is the
        floor. (read()'s floor is 1, which is why this is a separate constant rather than a shared
        one.)
    */
    static constexpr int minLagrangeDelay = 1 + lagrangeAhead;

    /** Largest delay read() will honour, in samples. */
    int getMaxDelay() const noexcept   { return size - 2; }

    /** Largest delay readLagrange() will honour, in samples. One slot tighter than the arithmetic
        strictly needs, for the same reason getMaxDelay() is: it keeps every tap in [1, size - 1],
        so no tap can land on the slot the write head is about to overwrite.
    */
    int getMaxDelayLagrange() const noexcept   { return size - 1 - lagrangeBehind; }

private:
    std::vector<float> buffer;
    int size = 0;
    int writeIndex = 0;
};

} // namespace reverb
