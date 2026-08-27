#pragma once

namespace reverb
{

/** Returns true if n is prime. Trial division; only ever called from prepare(). */
bool isPrime (int n) noexcept;

/** Returns the largest prime <= n, or 2 if n < 2. */
int nextPrimeAtOrBelow (int n) noexcept;

/** Returns the prime closest to n, searching outwards in both directions.

    Delay-line lengths that are mutually prime give the tank a much longer period before its
    modes line up again, which is what stops a short FDN from ringing metallically.
*/
int nearestPrime (int n) noexcept;

} // namespace reverb
