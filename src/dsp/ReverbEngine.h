#pragma once

#include "DelayLineFrac.h"
#include "Diffuser.h"
#include "Ducker.h"
#include "EarlyReflections.h"
#include "FDNTank.h"
#include "Filters.h"
#include "WetChain.h"

#include <array>
#include <juce_audio_basics/juce_audio_basics.h>
#include <vector>

namespace reverb
{

/** Top-level stereo reverb.

    Signal chain, per channel (PLAN-R2 D11 gives the wet-path order):

        in -> pre-delay -+-> EarlyReflections -----------------------+
                          |                                          v
                          +-> bandwidth lowpass -> diffuser -> FDN tank -> WetChain -+
        in (dry mono) ------------------------------------> Ducker ---^             |
                                                                                     v
        in ------------------------------------------------------------------- dry/wet mix -> trim

    WetChain is "everything after the tank, before the dry/wet mix": ER blend, wet EQ, M/S width,
    D11 bass mono, then duck gain, in that order. As of step 1b only width and the duck multiply
    are real (both bit-identical to what used to live inline here); the rest are no-ops until 2B/
    5A/5B (PLAN-R2 step 6).

    prepare() is the only function that allocates. process() takes no locks and does no I/O.
*/
class ReverbEngine
{
public:
    struct Params
    {
        // ---- the 13 existing fields (PLAN-R2 3.1) ----
        float mix           = 35.0f;      // %
        float preDelayMs    = 10.0f;
        float size          = 100.0f;     // %
        float decaySeconds  = 2.5f;

        // D1a "documented wart" (PLAN-R2 D1a, THE FOLD): these two fields are being repurposed as
        // the Decay EQ's two crossover frequencies -- dampingHz -> HIGH crossover (1000..12000 Hz),
        // lowCutHz -> LOW crossover (60..800 Hz). Field names stay as they are today because the
        // fold itself (deleting the per-line damping/low-cut filters in favour of an
        // AttenuationFilter) happens in ReverbEngine.cpp/FDNTank.cpp, which step 2A owns and this
        // step does not touch. Default member initialisers below are deliberately left at their
        // OLD values (6000/120): they only matter as a bare-Params fallback inside this header's
        // own tests, and changing them risks perturbing the frozen stability-sweep numbers that
        // snap from a default-constructed Params on the very first setParameters() call. The real,
        // product-facing default (3500/250) lives in src/Parameters.cpp's createParameterLayout().
        // sanitiseCrossovers() below clamps both to their new range until 2A's setParameters() does
        // it directly (PLAN-R2 step 6, "ReverbEngine::setParameters must also clamp...").
        float dampingHz     = 6000.0f;
        float lowCutHz      = 120.0f;

        float bandwidthHz   = 16000.0f;
        float diffusion     = 70.0f;      // %
        float modDepth      = 25.0f;      // %
        float modRateHz     = 0.4f;
        float width         = 100.0f;     // %
        float outputGainDb  = 0.0f;
        bool  freeze        = false;

        // ---- Phase 1, 6 new fields (PLAN-R2 3.2). decayLowMult/decayHighMult: no DSP reads these
        // yet -- AttenuationFilter isn't wired into FDNTank until 2A. The rest are wired as of 1b
        // into classes whose process()/processSample() are no-ops, so none of them changes the
        // sound yet either. ----
        float decayLowMult   = 1.4f;      // x, 0.1..4.0, low-band decay multiplier (D1)
        float decayHighMult  = 0.7f;      // x, 0.05..4.0, high-band decay multiplier (D1)
        float erMix          = 25.0f;     // %, early/late blend (D4); WetChain 1b no-op
        float erSizePercent  = 100.0f;    // %, 25..200, ER tap-time scale (D4); EarlyReflections 1b no-op
        float erSpread       = 60.0f;     // %, L/R ER decorrelation (D4); EarlyReflections 1b no-op
        float density        = 45.0f;     // %, in-line allpass density (D10); no DSP reads this yet

        // ---- Phase 2, 10 new fields. Wired as of 1b into Ducker/WetChain, both no-ops except the
        // duck-gain multiply (always exactly 1.0f) and M/S width (real, moved from here into
        // WetChain -- see the class doc comment). preDelaySync/preDelayDivision: no DSP reads these
        // yet -- TempoSync isn't wired until 5A. ----
        float duckAmount       = 0.0f;      // %; Ducker 1b no-op
        float duckThresholdDb  = -24.0f;    // dB; Ducker 1b no-op
        float duckReleaseMs    = 250.0f;    // ms; Ducker 1b no-op
        int   duckCharacter    = 0;         // 0 = Gentle, 1 = Pump (D5); Ducker 1b no-op
        bool  preDelaySync     = false;
        int   preDelayDivision = 5;         // index into the 11-entry division list, D6; 1/8 default
        // MUST match the APVTS default in Parameters.cpp (idBassMono) and the fallback literal in
        // PluginProcessor::currentParameters. Raised 130 -> 250 Hz on measurement: the side
        // response is r^2/sqrt(1+r^4), so 130 Hz left 20-100 Hz side energy only -11.1 dB down,
        // i.e. bass mono did not do the one thing its name promises. 250 Hz measures -21.8 dB.
        // This struct default is what a bare Params gives, which is what the DSP test target sees
        // (it cannot reach Parameters.h -- that pulls in juce_audio_processors), so a test asserting
        // "the shipping default monos the bass" reads THIS value. Keep the three in step.
        float bassMonoHz       = 250.0f;    // Hz, 0 = off (D11)
        float wetLowCutHz      = 20.0f;     // Hz; WetChain 1b no-op
        float wetHighCutHz     = 20000.0f;  // Hz; WetChain 1b no-op
        float wetTiltDb        = 0.0f;      // dB; WetChain 1b no-op

        // ---- Phase 3, 4 new fields. drive/shimmerMode/shimmerAmount are wired as of 1b into
        // FDNTank's Saturator/PitchShifter, both no-ops (identity / exactly silent). algorithm is
        // wired into EarlyReflections::setVoicing(); no DSP reads it beyond that until 3A/8A.
        //
        // As of v1.0, none of these four is driven by a host parameter any more (removed from the
        // public parameter surface -- see the "Phase 3's four IDs" comment in Parameters.h): they
        // hold these defaults unconditionally until the phase-3 DSP (7A/7B/3A/8A) lands and they
        // are reintroduced. ----
        int   shimmerMode    = 0;         // 0=Off,1=+12,2=+7,3=-12,4=Dual (D9)
        float shimmerAmount  = 0.0f;      // %
        float drive          = 0.0f;      // %, ADAA cubic soft clip per line (D7)
        int   algorithm      = 2;         // 0=Room,1=Plate,2=Hall,3=Ambience,4=Space; default Hall (D8)
    };

    void prepare (double sampleRate, int maxBlockSize, int numChannels);
    void reset() noexcept;

    void setParameters (const Params& p) noexcept;

    /** Processes in place. numChannels may be 1 or 2. */
    void process (float* const* io, int numChannels, int numSamples) noexcept;

    /** True if the previous block tripped the non-finite guard. Informational, for tests. */
    bool didRecoverFromNonFinite() const noexcept   { return recovered; }

    /** Current duck gain reduction in dB, negative for a reduction (D5). Read once per block by
        PluginProcessor, which mirrors it into an atomic for the GUI indicator. */
    float getDuckReductionDb() const noexcept   { return ducker.getReductionDb(); }

private:
    using Smoothed = juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>;

    static constexpr float smoothingSeconds = 0.05f;
    static constexpr float maxPreDelayMs    = 500.0f;

    /** D6: a division or sync-toggle change crossfades between two read positions of the ONE
        pre-delay line over this long, instead of gliding the read position. Gliding a discrete jump
        sweeps the read position faster than time (125 ms of delay over the 50 ms smoothing window
        is a rate of 2.5), which is not a smoothed parameter change but a pitch artefact. */
    static constexpr float preDelayCrossfadeSeconds = 0.03f;

    /** Processes at most wetL.size() samples. process() splits oversized blocks into these. */
    void processChunk (float* const* ch, int numChannels, int numSamples) noexcept;

    double sampleRate = 44100.0;

    // The first update after prepare() jumps straight to its values instead of ramping up from
    // the defaults, so a host that sets state before the first block gets it exactly.
    bool firstUpdate = true;

    std::array<DelayLineFrac, 2> preDelay {};
    std::array<OnePoleLP, 2>     bandwidth {};
    std::array<Diffuser, 2>      diffuser {};
    FDNTank                      tank;
    Ducker                       ducker;
    EarlyReflections             earlyReflections;
    WetChain                     wetChain;

    // widthSmoothed used to live here; PLAN-R2 step 5B note: "moved in from ReverbEngine by 1b"
    // -- it is now WetChain's own member, fed via wetChain.setParameters().
    // erMixSmoothed is the D4 early/late blend, applied in processChunk at the blend point 1b
    // wired inert (see the comment there): at a settled target of 0 the late samples reach WetChain
    // untouched, which is the exact-identity guarantee D4 chose the parallel topology for.
    Smoothed mixSmoothed, outputGainSmoothed,
             preDelaySamples, bandwidthCoeff, diffusionSmoothed, erMixSmoothed;

    // D6's crossfade. preDelaySamples still carries the (possibly gliding) CURRENT read position;
    // during a crossfade the OLD position is frozen here and the two are read from the same line --
    // "two reads from one line, no extra memory, no allocation". The previous discrete selection is
    // kept so setParameters() can tell a division/toggle change (crossfade) from a continuous
    // pre-delay or tempo change (glide, as before).
    Smoothed preDelayCrossfade;
    float    preDelayFadeFromSamples = 0.0f;
    bool     preDelayFading          = false;
    bool     lastPreDelaySync        = false;
    int      lastPreDelayDivision    = -1;

    std::vector<float> wetL, wetR;

    // Per-sample scratch for the classes wired in by 1b, filled in processChunk's first loop and
    // consumed by WetChain in its second loop -- same two-stage shape wetL/wetR already used, so
    // the non-finite guard between the two loops covers them too.
    std::vector<float> erL, erR, duckGain;

    bool recovered = false;
};

//==================================================================================================
/** D1a: clamps the two D1a-repurposed crossover fields (Params::dampingHz -> 1000..12000 Hz,
    Params::lowCutHz -> 60..800 Hz) to their new range, PLAN-R2 3.1.

    Per the plan this belongs inside ReverbEngine::setParameters() (ReverbEngine.cpp), which
    currently passes both through unclamped -- but that file is step 2A's, not step 1a's, so it is
    a header-visible free function a caller can apply first. PluginProcessor::currentParameters()
    is that caller today; 2A's fold should fold this clamp into setParameters() directly and this
    function can then be retired.
*/
inline ReverbEngine::Params sanitiseCrossovers (ReverbEngine::Params p) noexcept
{
    p.dampingHz = juce::jlimit (1000.0f, 12000.0f, p.dampingHz);
    p.lowCutHz  = juce::jlimit (60.0f, 800.0f, p.lowCutHz);
    return p;
}

} // namespace reverb
