#pragma once

#include <cmath>
#include <complex>

namespace reverb
{

/** First-order TPT (topology-preserving-transform) shelf: monotonic magnitude, no resonance
    (PLAN-R2 D1). Two cascaded instances, each carrying half the target dB, make one shelf band
    of AttenuationFilter (D1 trap 1b).

    Structure. A one-pole TPT lowpass supplies the state variable; the shelf is a linear
    combination of the input and that lowpass:

        low  shelf:  H(s) = 1 + (A-1) * LP(s),   corner prewarped by 1/sqrt(A)
        high shelf:  H(s) = A - (A-1) * LP(s),   corner prewarped by   sqrt(A)

    with A the linear gain (10^(dB/20)). The sqrt(A) prewarp is what makes |H(cornerHz)| exactly
    sqrt(A) -- the geometric midpoint of the shelf -- for every gain, which is why the crossover
    frequency keeps meaning something and why decaycurve's prototype curve is exact AT the corner
    however deep the shelf is. |H| is monotonic between 1 and A, so a cascade of these can never
    exceed max(1, A) at any frequency: that bound is what AttenuationFilter's passivity argument
    rests on (D1 trap 2), and it survives even the Nyquist clamp below.

    Both magnitude of the response and the coefficients are derived from the DISCRETE transfer
    function, not its analogue prototype, so magnitudeAt() is exactly the filter that runs -- tests
    and the GUI curve cannot drift from the DSP.
*/
class TptShelf
{
public:
    enum class Kind { low, high };

    /** G is the TPT integrator coefficient g/(1+g) (same convention as OnePoleLP), so process()
        needs no division. m0/m1 mix the input and the lowpass state.
    */
    struct Coeffs { float G = 0.5f, m0 = 1.0f, m1 = 0.0f; };

    /** Calls tan()/pow(); control rate only. */
    static Coeffs coeffsFor (Kind kind, float cornerHz, float gainDb, double sampleRate) noexcept
    {
        const double fs = sampleRate > 0.0 ? sampleRate : 44100.0;
        const double A  = std::pow (10.0, static_cast<double> (gainDb) * 0.05);
        const double warp = kind == Kind::low ? 1.0 / std::sqrt (A) : std::sqrt (A);

        // Prewarped corner, kept clear of both DC and Nyquist. A deep shelf shifts the corner by
        // sqrt(A), which at the [1e-5, 0.9999] band-gain extremes can land either side of the
        // audio band; clamping degrades the shelf towards "flat at its asymptote" rather than
        // putting the TPT pole at -1 (G = 1 rings at Nyquist forever, cf. Filters.cpp).
        const double corner  = std::min (std::max (static_cast<double> (cornerHz) * warp, 0.01),
                                         fs * 0.4975);
        const double g       = std::tan (3.14159265358979323846 * corner / fs);

        Coeffs c;
        c.G  = static_cast<float> (g / (1.0 + g));
        c.m0 = static_cast<float> (kind == Kind::low ? 1.0 : A);
        c.m1 = static_cast<float> (kind == Kind::low ? A - 1.0 : 1.0 - A);
        return c;
    }

    /** Magnitude of the discrete response at hz. Control rate / tests / GUI only. */
    static float magnitudeAt (Kind kind, float hz, float cornerHz, float gainDb, double sampleRate) noexcept
    {
        const double fs = sampleRate > 0.0 ? sampleRate : 44100.0;

        return magnitudeFor (coeffsFor (kind, cornerHz, gainDb, fs), hz, fs);
    }

    /** Magnitude of an already-derived coefficient set. Splitting it out keeps the composite
        response in AttenuationFilter honest: it evaluates the coefficients that are running.
    */
    static float magnitudeFor (const Coeffs& c, float hz, double sampleRate) noexcept
    {
        const double fs = sampleRate > 0.0 ? sampleRate : 44100.0;
        const double w  = 6.283185307179586 * static_cast<double> (hz) / fs;

        // TPT one-pole lowpass: LP(z) = G(z + 1) / (z - (1 - 2G)). Unity at DC, zero at Nyquist.
        const std::complex<double> z (std::cos (w), std::sin (w));
        const std::complex<double> num = static_cast<double> (c.G) * (z + 1.0);
        const std::complex<double> den = z - (1.0 - 2.0 * static_cast<double> (c.G));

        const std::complex<double> lp = std::abs (den) > 1.0e-300 ? num / den
                                                                  : std::complex<double> (0.0, 0.0);

        return static_cast<float> (std::abs (static_cast<double> (c.m0)
                                             + static_cast<double> (c.m1) * lp));
    }

    void setCoeffs (const Coeffs& c) noexcept   { coeffs = c; }
    void reset() noexcept                       { z = 0.0f; }

    float process (float x) noexcept   { return processWith (coeffs, z, x); }

    /** The section, with its coefficients and its state variable passed in.

        Exists so a cascade can hold ONE copy of the coefficients for several sections -- both
        sections of a shelf carry identical coefficients (half the dB each), and AttenuationFilter
        runs four of these per line per sample, where reloading four private copies of the same
        three floats is measurable (0.9 us per 512-sample block at eight lines). Same arithmetic as
        process(), defined once.
    */
    static float processWith (const Coeffs& c, float& z, float x) noexcept
    {
        const float v  = (x - z) * c.G;
        const float lp = v + z;
        z = lp + v;

        return c.m0 * x + c.m1 * lp;
    }

private:
    Coeffs coeffs {};
    float  z = 0.0f;
};

} // namespace reverb
