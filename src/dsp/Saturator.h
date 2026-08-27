#pragma once

namespace reverb
{

/** First-order ADAA (antiderivative antialiasing) cubic soft clip, per FDN line, in the feedback
    path (PLAN-R2 D7): `f(x) = x - x^3/3` for |x| < 1, +-2/3 beyond, applied via its antiderivative
    so the nonlinearity does not alias inside the recursive loop.

    1b no-op: process() is an unconditional identity -- there is no clip curve or antiderivative
    state yet, so drive01 has no effect regardless of its value. TODO(7B): real ADAA cubic soft
    clip, `F(x) = x^2/2 - x^4/12` for |x| < 1 with linear continuation beyond, and the small-
    denominator fallback to `f((x_n + x_{n-1}) / 2)`, PLAN-R2 D7.
*/
class Saturator
{
public:
    void reset() noexcept   { }

    void setDrive (float drive01) noexcept   { drive = drive01; }

    /** Returns x bit-exactly in 1b (drive01 has no effect yet). TODO(7B): real ADAA clip. */
    float process (float x) noexcept
    {
        (void) drive;
        return x;
    }

private:
    float drive = 0.0f;
};

} // namespace reverb
