#pragma once

namespace reverb
{

/** Shimmer voice: PLAN-R2 D9 is "one shifter on a tapped sum of the tank, injected back at the
    tank input" -- a granular/rotating-read overlap-add pitch shifter, NOT one per delay line.

    1b no-op: processSample() is an unconditional `return 0.0f` -- there is no grain engine yet, so
    amount has no effect regardless of its value. TODO(7A): real granular shifter (80 ms grains, 2
    at 50% overlap, Hann window, randomised restart offset via a member xorshift, allocation-free),
    PLAN-R2 D9.
*/
class PitchShifter
{
public:
    struct Params { float ratio = 2.0f, amount = 0.0f; };

    void prepare (double sampleRate)   { (void) sampleRate; }
    void reset() noexcept              { }

    void setParameters (const Params& p) noexcept
    {
        params = p;
    }

    /** Returns 0.0f exactly in 1b. TODO(7A): real granular pitch shift, PLAN-R2 D9. */
    float processSample (float x) noexcept
    {
        (void) x;
        return 0.0f;
    }

private:
    Params params {};
};

} // namespace reverb
