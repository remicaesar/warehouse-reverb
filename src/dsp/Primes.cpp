#include "Primes.h"

namespace reverb
{

bool isPrime (int n) noexcept
{
    if (n < 2)      return false;
    if (n < 4)      return true;
    if (n % 2 == 0) return false;

    for (int d = 3; d * d <= n; d += 2)
        if (n % d == 0)
            return false;

    return true;
}

int nextPrimeAtOrBelow (int n) noexcept
{
    for (int candidate = n; candidate >= 2; --candidate)
        if (isPrime (candidate))
            return candidate;

    return 2;
}

int nearestPrime (int n) noexcept
{
    if (n <= 2)
        return 2;

    for (int offset = 0; offset <= n; ++offset)
    {
        if (isPrime (n - offset))
            return n - offset;

        if (isPrime (n + offset))
            return n + offset;
    }

    return 2;
}

} // namespace reverb
