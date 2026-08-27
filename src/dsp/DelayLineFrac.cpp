#include "DelayLineFrac.h"

#include <algorithm>

namespace reverb
{

void DelayLineFrac::prepare (int maxSamples)
{
    size = std::max (8, maxSamples);
    buffer.assign (static_cast<size_t> (size), 0.0f);
    writeIndex = 0;
}

void DelayLineFrac::clear() noexcept
{
    std::fill (buffer.begin(), buffer.end(), 0.0f);
    writeIndex = 0;
}

float DelayLineFrac::read (float delaySamples) const noexcept
{
    const int maxDelay = size - 2;

    // Negated comparison on purpose: a NaN delay lands on the lower bound instead of propagating
    // into the index arithmetic.
    const float d = ! (delaySamples > 1.0f) ? 1.0f
                                            : std::min (delaySamples, static_cast<float> (maxDelay));

    // The wrap is done in integers on purpose. Computing (writeIndex - d) as a float and adding
    // size back can round up to exactly size - float has no resolution finer than ~0.0005 at these
    // magnitudes - which indexes one past the end of the buffer and returns whatever is next on the
    // heap. In a feedback network that garbage then circulates forever.
    const int   whole = std::min (static_cast<int> (d), maxDelay);
    const float frac  = d - static_cast<float> (whole);

    int i0 = writeIndex - whole;          // the sample 'whole' steps ago
    int i1 = i0 - 1;                      // one step older, for the interpolation

    if (i0 < 0) i0 += size;
    if (i1 < 0) i1 += size;

    const float a = buffer[static_cast<size_t> (i0)];
    const float b = buffer[static_cast<size_t> (i1)];

    // frac == 0 returns a bit for bit, which is what Freeze relies on.
    return a + frac * (b - a);
}

float DelayLineFrac::readLagrange (float delaySamples) const noexcept
{
    const int   maxDelay = getMaxDelayLagrange();
    const float minDelay = static_cast<float> (minLagrangeDelay);

    // Negated comparison, same as read() and for the same reason: a NaN delay lands on the lower
    // bound instead of propagating into the index arithmetic. The lower bound is 1 + lagrangeAhead
    // rather than read()'s 1, because this kernel also reads lagrangeAhead samples NEWER than the
    // requested delay and "1 sample ago" is already the newest sample the line holds.
    const float d = ! (delaySamples > minDelay) ? minDelay
                                                : std::min (delaySamples, static_cast<float> (maxDelay));

    // Integer wrap, as in read(): computing (writeIndex - d) in float and adding size back can
    // round up to exactly size and index one past the end of the buffer.
    //
    // BOUNDS. whole lands in [1 + lagrangeAhead, size - 1 - lagrangeBehind] = [3, size - 3], so the
    // five taps sit at (whole - 2) .. (whole + 2) samples ago, i.e. within [1, size - 1] - every one
    // of them a real stored sample, none of them the slot the write head is about to overwrite.
    // Pre-wrap the oldest index is (writeIndex - whole - 2) >= -(size - 1), so a single addition of
    // size brings it into [0, size - 1]. The single-wrap invariant needs whole + lagrangeBehind
    // <= size; the clamp above leaves one slot of margin on top of that, which is the same margin
    // read()'s size - 2 leaves for its own two taps.
    const int   whole = std::min (static_cast<int> (d), maxDelay);
    const float frac  = d - static_cast<float> (whole);

    // The five taps are CONSECUTIVE buffer slots, oldest first, so one wrap serves all of them.
    // base indexes the oldest, (whole + 2) samples ago:
    //
    //   base + 0 -> (whole + 2) ago    base + 3 -> (whole - 1) ago
    //   base + 1 -> (whole + 1) ago    base + 4 -> (whole - 2) ago
    //   base + 2 -> (whole    ) ago
    //
    // Pre-wrap base = writeIndex - whole - 2 lies in [-(size - 1), size - 6], so one addition of
    // size lands it in [0, size - 1] - in bounds unconditionally, and that is the only clamp the
    // fast path below relies on.
    int base = writeIndex - whole - lagrangeBehind;

    if (base < 0)
        base += size;

    float xm2, xm1, x0, x1, x2;

    if (base + kernelWidth - 1 <= size - 1)
    {
        // The window does not straddle the wrap, which is the case for all but (kernelWidth - 1) of
        // the size possible write positions, so the branch predicts and the five loads sit in one or
        // two adjacent cache lines.
        const float* p = buffer.data() + static_cast<size_t> (base);

        x2 = p[0]; x1 = p[1]; x0 = p[2]; xm1 = p[3]; xm2 = p[4];
    }
    else
    {
        // base >= size - (kernelWidth - 1), so base + k is at most size + kernelWidth - 2 and one
        // subtraction of size is enough for each.
        const int i1w = base + 1 < size ? base + 1 : base + 1 - size;
        const int i0w = base + 2 < size ? base + 2 : base + 2 - size;
        const int im1 = base + 3 < size ? base + 3 : base + 3 - size;
        const int im2 = base + 4 < size ? base + 4 : base + 4 - size;

        x2  = buffer[static_cast<size_t> (base)];
        x1  = buffer[static_cast<size_t> (i1w)];
        x0  = buffer[static_cast<size_t> (i0w)];
        xm1 = buffer[static_cast<size_t> (im1)];
        xm2 = buffer[static_cast<size_t> (im2)];
    }

    // 5-point 4th-order Lagrange, written as a polynomial in frac rather than as the five Lagrange
    // basis functions. Same arithmetic to within rounding, but a0..a4 depend only on the five loaded
    // samples so they compute while the next line's loads are still in flight, and the frac-dependent
    // part is four fused multiply-adds instead of a twenty-multiply dependency chain.
    //
    // FIVE points, not four and not six. The number that matters is the loss at frac = 0.5: that is
    // where EVERY line sits at Size 50%, because halving an odd prime lands exactly on a half
    // sample, and Size 50% also halves m_i so the loss is taken twice as often per second as at
    // Size 100%. At 8 kHz / 48 kHz the loss at frac = 0.5 is 0.2263 dB for 4 points, 0.0466 dB for
    // 5 and 0.0472 dB for 6 - the sixth tap is very slightly WORSE at 8 kHz, not better, because
    // going from 4 to 5 taps is what raises the order from 3 to 4 and 5 to 6 does not. So five is
    // not a compromise between four and six here; it is the whole of what six would buy, one tap and
    // two polynomial terms cheaper. Measured on the tank, the 8 kHz T60 at Size 50% lands 15.3%
    // short of target with 4 points and 0.4% short with 5. See T9.
    //
    // |H| <= 1 for every frac and every frequency, attained only at DC, so the kernel cannot add
    // gain to the feedback loop and T3's composite-magnitude argument is untouched.
    //
    // e1/o1/e2/o2 are the even and odd parts of the tap pairs about x0: the even-order coefficients
    // depend only on the sums and the odd-order ones only on the differences. Pure common
    // subexpression sharing, not an approximation.
    const float e1 = x1 + xm1;
    const float o1 = x1 - xm1;
    const float e2 = x2 + xm2;
    const float o2 = x2 - xm2;

    // Each of these appears in two coefficients, and the 1/6 terms are a quarter of the 2/3 ones,
    // so eight multiplies cover all four coefficients.
    const float o1s = (2.0f / 3.0f) * o1;
    const float o2s = (1.0f / 12.0f) * o2;
    const float e1s = (2.0f / 3.0f) * e1;
    const float e2s = (1.0f / 24.0f) * e2;

    const float a0 = x0;
    const float a1 = o1s - o2s;
    const float a2 = e1s - e2s - 1.25f * x0;
    const float a3 = o2s - 0.25f * o1s;
    const float a4 = e2s - 0.25f * e1s + 0.25f * x0;

    // frac == 0 collapses to a0 = x0, bit for bit - which is what Freeze relies on, and Freeze snaps
    // every line length to a whole sample to get it. Exact by CONSTRUCTION: the polynomial is
    // centred on x0, so no coefficient has to happen to evaluate to zero.
    return (((a4 * frac + a3) * frac + a2) * frac + a1) * frac + a0;
}

} // namespace reverb
