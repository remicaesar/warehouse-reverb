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

    WetChain is "everything after the tank, before the dry/wet mix": wet EQ (low cut, high cut,
    tilt shelf), M/S width, D11 bass mono, then duck gain, in that order. All of it is live and
    measured -- W2/W2b/W2c cover the EQ and the bass mono, W5 the width and the duck multiply.
    The early/late blend is NOT part of WetChain: it happens once in processChunk(), ahead of
    everything WetChain does, which is why WetChain has no erMix of its own.

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

        // D1a "documented wart" (PLAN-R2 D1a, THE FOLD): these two fields ARE the Decay EQ's two
        // crossover frequencies -- dampingHz -> HIGH crossover (1000..12000 Hz), lowCutHz -> LOW
        // crossover (60..800 Hz). The fold has landed: the per-line damping and low-cut filters
        // are gone, replaced by one AttenuationFilter per line (FDNTank.h). Only the field names
        // were kept, which is the whole of the wart.
        //
        // Default member initialisers below are deliberately left at their OLD values (6000/120):
        // they only matter as a bare-Params fallback inside this header's own tests, and changing
        // them risks perturbing the frozen stability-sweep numbers that snap from a
        // default-constructed Params on the very first setParameters() call. The real,
        // product-facing default (3500/250) lives in src/Parameters.cpp's createParameterLayout(),
        // and test a2 names exactly these two fields as its one exemption when it holds Params
        // defaults equal to the shipping layout.
        //
        // setParameters() passes both through unclamped, so sanitiseCrossovers() below is what
        // holds them in range; PluginProcessor.cpp applies it on the way in.
        float dampingHz     = 6000.0f;
        float lowCutHz      = 120.0f;

        float bandwidthHz   = 16000.0f;
        float diffusion     = 70.0f;      // %
        float modDepth      = 25.0f;      // %
        float modRateHz     = 0.4f;
        float width         = 100.0f;     // %
        float outputGainDb  = 0.0f;
        bool  freeze        = false;

        // ---- Phase 1, 6 fields (PLAN-R2 3.2). ALL SIX ARE LIVE. ----
        //
        // These carried "no DSP reads this yet" / "1b no-op" notes describing the state of the tree
        // during phase 1b. Every one of them is now wired and measured, and the stale notes were
        // actively misleading: this struct is the plugin's central contract, so a reader trusting
        // them would conclude half the feature set was inert and plan against that. Each field below
        // names the code that consumes it, which is a fact that can be checked rather than a claim
        // about a development phase that has since moved on.
        float decayLowMult   = 1.4f;      // x, 0.1..4.0, low-band decay multiplier (D1);
                                          //   -> FDNTank.cpp bands.lowMult, measured by T1
        float decayHighMult  = 0.7f;      // x, 0.05..4.0, high-band decay multiplier (D1);
                                          //   -> FDNTank.cpp bands.highMult, measured by T1
        float erMix          = 25.0f;     // %, early/late blend (D4);
                                          //   -> ReverbEngine.cpp erMixSmoothed, measured by E1/E5/E7
        float erSizePercent  = 100.0f;    // %, 25..200, ER tap-time scale (D4);
                                          //   -> EarlyReflections erp.sizePercent, measured by E2/E4
        float erSpread       = 60.0f;     // %, L/R ER decorrelation (D4);
                                          //   -> EarlyReflections erp.spreadPercent, measured by E3
        float density        = 45.0f;     // %, in-line allpass density (D10);
                                          //   -> FDNTank.cpp densityMix/densityGain, measured by D6/D6b

        // ---- Phase 2, 10 fields. ALL LIVE except where noted. ----
        //
        // Same correction as above: these described Ducker and WetChain as no-ops and
        // preDelaySync/preDelayDivision as unread. The ducker is real and measured by the whole H1-H4
        // family, the wet chain by W1-W7, and tempo sync by H5-H7b.
        float duckAmount       = 0.0f;      // %; -> Ducker dp.amountPercent, measured by H1/H4
        float duckThresholdDb  = -24.0f;    // dB; -> Ducker dp.thresholdDb, measured by H1/H2
        float duckReleaseMs    = 250.0f;    // ms; -> Ducker dp.releaseMs, measured by H3/H3d/H3e
        int   duckCharacter    = 0;         // 0 = Gentle, 1 = Pump (D5); -> Ducker, measured by H3b
        bool  preDelaySync     = false;     // -> ReverbEngine.cpp pre-delay crossfade, measured by H5/H6
        int   preDelayDivision = 5;         // index into the 11-entry division list, D6; 1/8 default
                                            //   -> TempoSync, measured by H5b/H7/H7b
        // Raised 130 -> 250 Hz on measurement: the side response is r^2/sqrt(1+r^4), so 130 Hz left
        // 20-100 Hz side energy only -11.1 dB down, i.e. bass mono did not do the one thing its name
        // promises. 250 Hz measures -21.8 dB.
        //
        // THE SAME NUMBER LIVES IN THREE PLACES -- here, createParameterLayout()'s idBassMono, and
        // the fallback literal in PluginProcessor::currentParameters. This used to say "keep the
        // three in step", which is a request, not a mechanism, and it did not hold: changing only the
        // APVTS default to 130 left every one of the project's 121 checks green, including the one
        // named "and it monos it at the shipping default". W2b in WetPathTests.cpp has to read THIS
        // value, because the DSP test target cannot reach Parameters.h (that pulls in
        // juce_audio_processors), so the struct default is the only default it can see.
        //
        // a2 in ProcessorTests.cpp is the mechanism that replaced the request: it runs in the target
        // that can see both and fails if this default and the APVTS default ever disagree.
        float bassMonoHz       = 250.0f;    // Hz, 0 = off (D11); -> WetChain, measured by W2/W2b/W2c
        float wetLowCutHz      = 20.0f;     // Hz; -> WetChain wcp.wetLowCutHz, measured by W5
        float wetHighCutHz     = 20000.0f;  // Hz; -> WetChain wcp.wetHighCutHz, measured by W5
        float wetTiltDb        = 0.0f;      // dB; -> WetChain wcp.tiltDb, measured by W5

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

    Per the plan this belongs inside ReverbEngine::setParameters() (ReverbEngine.cpp). THE FOLD HAS
    LANDED AND DID NOT MOVE IT: setParameters() still assigns dampingHz/lowCutHz straight through
    unclamped, so this header-visible free function remains the only thing holding them in range and
    is not retired. PluginProcessor::engineParameters() is the one caller that applies it, on every
    parameter update. Anything else that builds a Params by hand -- a test, a future caller -- is
    unclamped unless it applies this itself.
*/
inline ReverbEngine::Params sanitiseCrossovers (ReverbEngine::Params p) noexcept
{
    p.dampingHz = juce::jlimit (1000.0f, 12000.0f, p.dampingHz);
    p.lowCutHz  = juce::jlimit (60.0f, 800.0f, p.lowCutHz);
    return p;
}

} // namespace reverb
