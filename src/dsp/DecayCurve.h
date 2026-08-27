#pragma once

#include "Filters.h"
#include "Shelf.h"

#include <algorithm>
#include <cmath>

namespace reverb::decaycurve
{

/** Parameter ranges for the two crossovers (PLAN-R2 3.1) and the two band multipliers (3.2).

    Held here rather than in Parameters.h because the DSP must be safe when driven directly --
    ReverbEngine::setParameters passes Params::dampingHz / Params::lowCutHz through unclamped
    [verified, ReverbEngine.cpp's tp.dampingHz = p.dampingHz], and that file belongs to another
    assignment. sanitise() below is where the clamp lands instead, which also makes it the one
    place a test can drive out of range and still get a defined filter.

    The multiplier floor is the wider of the two parameter ranges (decayhigh's 0.05) for both
    bands: the parameter layer already stops `decaylow` below 0.1, and a lower floor here can only
    ever make the DSP more forgiving of a caller that does not.
*/
constexpr float minXoverLowHz  =    60.0f;
constexpr float maxXoverLowHz  =   800.0f;
constexpr float minXoverHighHz =  1000.0f;
constexpr float maxXoverHighHz = 12000.0f;
constexpr float minMultiplier  =     0.05f;
constexpr float maxMultiplier  =     4.0f;

/** The three facts the curve needs about FDNTank's shipped line-length table, in SECONDS at Size
    100% rather than samples so they mean the same thing at every sample rate: the mean, which is
    the length the single reference curve is drawn for, and the shortest and longest, which bound
    how far an individual line can disagree with it.

    Why the curve needs a length at all: the shelves carry m_i x a dB-per-sample rate, and a
    first-order shelf's transition width grows with its own depth (6 dB/octave is all it has, so a
    40 dB shelf must span ~7 octaves). The band asymptotes are therefore identical for every line
    -- that is the fold's guarantee and what §7.1 T7 measures -- but inside a deep transition the
    lines differ, because their shelves differ in depth by the 5.26:1 length ratio. No single curve
    can be exact for all of them at once; this is the one it is exact for.

    Restated here rather than derived because there is nothing to derive them from at compile time:
    FDNTank keeps its table in an anonymous namespace inside FDNTank.cpp and exposes lengths only
    through getLineLengthSamples(), a non-constexpr accessor on a prepared instance. What stops them
    drifting is not this comment -- the eight-line table's 47.5 ms mean and 3.4:1 ratio both
    survived the move to sixteen lines on a comment alone, which is the defect this block replaces.
    §7.1 T6b now reads all sixteen lengths back off a running tank and FAILS if any of these three
    is more than 1% out, so the next table change cannot land quietly.

    Numbers: 750.20 / 16 = 46.8875 ms mean, 18.03 ms shortest, 94.91 ms longest.
*/
constexpr float referenceLengthSeconds = 0.0468875f;
constexpr float shortestLineSeconds    = 0.01803f;
constexpr float longestLineSeconds     = 0.09491f;

/** Corner of the tank's fixed per-line DC blocker, in Hz -- a MIRROR of FDNTank::dcBlockerHz.

    Mirrored rather than read because including FDNTank.h from here would close an include cycle:
    FDNTank.h includes AttenuationFilter.h, which includes this header. The §7.1 suite
    static_asserts the two are equal, so the copy cannot drift silently either.
*/
constexpr float dcBlockerHz = 5.0f;

/** The Decay EQ's two crossovers and the two band multipliers either side of them (PLAN-R2 D1).
    Pure data -- no state, no allocation.
*/
struct Bands
{
    float lowMult = 1.0f, highMult = 1.0f, xoverLowHz = 250.0f, xoverHighHz = 3500.0f;
};

/** Clamps every field to its parameter range, then xoverHighHz to >= 4 * xoverLowHz. Single
    source of truth for that rule (D1 trap 2): overlapping shelves are what could push the
    composite above unity, and 4:1 separation is what keeps the two transitions from adding
    constructively. Idempotent, so calling it twice is harmless.
*/
inline Bands sanitise (Bands b) noexcept
{
    b.lowMult     = std::min (std::max (b.lowMult,  minMultiplier),  maxMultiplier);
    b.highMult    = std::min (std::max (b.highMult, minMultiplier),  maxMultiplier);
    b.xoverLowHz  = std::min (std::max (b.xoverLowHz,  minXoverLowHz),  maxXoverLowHz);
    b.xoverHighHz = std::min (std::max (b.xoverHighHz, minXoverHighHz), maxXoverHighHz);
    b.xoverHighHz = std::max (b.xoverHighHz, 4.0f * b.xoverLowHz);

    return b;
}

/** dB of attenuation per sample for one band, i.e. that band's asymptote. Always <= 0.

    This is the whole decay law: gamma_dB = -60 / (fs * T60), with T60 = decay * multiplier
    (STATE.md research finding 1). A line of m samples then has band gain 10^(m * gamma / 20),
    which is exactly the scalar law the tank shipped with -- see AttenuationFilter::targetFor.
*/
inline float bandDbPerSample (float multiplier, float baseT60Seconds, double sampleRate) noexcept
{
    const double fs   = sampleRate > 0.0 ? sampleRate : 44100.0;
    const double mult = std::min (std::max (static_cast<double> (multiplier),
                                            static_cast<double> (minMultiplier)),
                                 static_cast<double> (maxMultiplier));
    const double t60  = std::max (static_cast<double> (baseT60Seconds), 0.01) * mult;

    return static_cast<float> (-60.0 / (fs * t60));
}

/** dB the tank's fixed DC blocker takes off every PASS at hz. Always <= 0.

    Built from production constants only: dcBlockerHz above, OnePoleHP's own coefficient mapping,
    and the same discrete magnitude evaluation the shelves use. `x - lp(x)` is m0 = 1, m1 = -1 over
    the TPT lowpass [verified, Filters.h's OnePoleHP::process], so TptShelf::magnitudeFor gives the
    response of the filter that actually runs, not of an analogue prototype.

    A fixed dB per PASS, unlike the attenuation filter's m_i x dB per SAMPLE -- which is why it
    needs dividing by m to join the rest of the curve, why its share of the decay GROWS with the
    decay time, and why it differs between lines. 0.3% of a 0.7 s band decay at 100 Hz, and 12% of
    a 14 s one.
*/
inline float blockerDbPerPass (float hz, double sampleRate) noexcept
{
    const double fs = sampleRate > 0.0 ? sampleRate : 44100.0;

    TptShelf::Coeffs c;
    c.G  = OnePoleHP::coefficientForCutoff (dcBlockerHz, fs);
    c.m0 =  1.0f;
    c.m1 = -1.0f;

    return 20.0f * std::log10 (std::max (TptShelf::magnitudeFor (c, std::max (hz, 0.0f), fs),
                                         1.0e-30f));
}

/** dB of attenuation per sample at hz. Always <= 0.

    The prototype curve: one dB-per-sample response scaled by a line's length in samples (D1a
    argument 3 -- after the fold there is one T60(f) curve, not a per-line average). It is the
    same arithmetic AttenuationFilter runs, evaluated analytically:

        db(f)  =  gamma_mid  +  [ 2*20log10|lowShelf(f)| + 2*20log10|highShelf(f)| ] / m
                             +  blockerDbPerPass(f) / m

    with the two shelves at m*(gamma_band - gamma_mid) dB, split into the two half-dB sections the
    filter actually cascades. Evaluating the REALISED shelf magnitude rather than an idealised
    dB-linear shape matters: at decayhigh = 0.05 a long line's high shelf is ~45 dB deep, which
    drags its corner down by sqrt(A) to nearly a third of the crossover frequency, and an idealised
    shape mispredicts the 1 kHz decay rate by ~50% there.

    lineLengthSamples is REQUIRED, and each term above needs it for a different reason. The shelf dB
    is m x a per-sample rate, so the depth of the transition a line sees is its own: every line's
    asymptotes still sit exactly on the same curve -- that is the fold's guarantee -- and a line
    deviates only inside a transition, only to the extent its shelf depth differs. The blocker takes
    a fixed dB per PASS, so converting that to per-sample is a division by m and nothing else.

    It used to default to "the reference line"; pass referenceLengthSeconds * sampleRate explicitly
    if that is what you want. It stopped being a default because for one revision omitting it also
    meant "model the shelves and not the blocker" -- two different physics behind one name -- and
    because dividing a per-PASS cost by the length of a line that does not exist is not a curve
    anyone should be able to get by accident.

    THE DC BLOCKER. The real feedback path is the attenuation filter AND the fixed highpass -- see
    FDNTank::processFeedbackPath, which is that pair and nothing else -- so this is the COMPLETE
    path for the named line. The blocker's bias is one-sided, it only ever shortens the tail, so it
    belongs folded in here rather than drawn as uncertainty; §7.1 T6 bounds what is left over.
*/
inline float dbPerSampleAt (float hz, float baseT60Seconds, const Bands& bands, double sampleRate,
                            float lineLengthSamples) noexcept
{
    const Bands  b      = sanitise (bands);
    const double fs     = sampleRate > 0.0 ? sampleRate : 44100.0;
    const float  dbMid  = bandDbPerSample (1.0f,       baseT60Seconds, fs);
    const float  dbLow  = bandDbPerSample (b.lowMult,  baseT60Seconds, fs);
    const float  dbHigh = bandDbPerSample (b.highMult, baseT60Seconds, fs);

    // A line shorter than one sample is not a line; the floor keeps a caller that passes 0 (or a
    // negative, which the old default-argument sentinel used to mean) out of a division by zero.
    const float m = std::max (lineLengthSamples, 1.0f);

    const float lowShelfDb  = (dbLow  - dbMid) * m;
    const float highShelfDb = (dbHigh - dbMid) * m;

    const float f       = std::max (hz, 0.0f);
    const float magLow  = TptShelf::magnitudeAt (TptShelf::Kind::low,  f, b.xoverLowHz,
                                                 lowShelfDb * 0.5f, fs);
    const float magHigh = TptShelf::magnitudeAt (TptShelf::Kind::high, f, b.xoverHighHz,
                                                 highShelfDb * 0.5f, fs);

    const float shelfDb = 40.0f * (std::log10 (std::max (magLow,  1.0e-20f))
                                   + std::log10 (std::max (magHigh, 1.0e-20f)));

    const float attenDb = std::min (dbMid + shelfDb / m, 0.0f);

    return attenDb + blockerDbPerPass (f, fs) / m;
}

/** T60 in seconds at hz for one line. The inverse of dbPerSampleAt; what the GUI plots.
    lineLengthSamples is required for the reasons dbPerSampleAt gives.
*/
inline float t60At (float hz, float baseT60Seconds, const Bands& bands, double sampleRate,
                    float lineLengthSamples) noexcept
{
    const double fs = sampleRate > 0.0 ? sampleRate : 44100.0;
    const double db = static_cast<double> (dbPerSampleAt (hz, baseT60Seconds, bands, sampleRate,
                                                         lineLengthSamples));

    if (db >= -1.0e-12)
        return 1.0e6f;   // no attenuation at all: an infinite tail, reported as a large finite T60.

    return static_cast<float> (-60.0 / (fs * db));
}

} // namespace reverb::decaycurve
