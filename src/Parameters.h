#pragma once

/*
    Parameter identity: the 29 IDs (PLAN-R2 3.1/3.2), the APVTS layout, and the factory preset
    table. This is the contract with the editor and with every saved host session, so it is kept
    in one place rather than duplicated between PluginProcessor and the GUI.

    Step 1a extracted this out of PluginProcessor -- see PLAN-R2 step 6, "Step 1a". State
    save/restore (including the PLAN-R2 3.3 missing-parameter rule and the stateVersion stamp)
    stays in PluginProcessor.cpp: this file is about WHAT the parameters are, not how a session is
    serialised.
*/

#include <array>
#include <juce_audio_processors/juce_audio_processors.h>

namespace reverb::param
{

//==================================================================================================
// Parameter IDs. Kept as constants so applyPreset() and PluginProcessor::currentParameters()
// cannot drift apart. No ID collides with any other -- see PLAN-R2 3, which is why the count below
// is asserted rather than assumed.
//==================================================================================================

// ---- The 13 existing IDs (PLAN-R2 3.1). Range, default and skew unchanged except the two below. ----
constexpr const char* idMix       = "mix";
constexpr const char* idPreDelay  = "predelay";
constexpr const char* idSize      = "size";
constexpr const char* idDecay     = "decay";

// D1a "THE FOLD" (PLAN-R2 D1a) -- Tank Damping and Tank Low Cut are removed from the feedback path
// and the attenuation filter becomes the only mechanism for frequency-dependent decay. These two
// IDs are REPURPOSED as its two crossover frequencies rather than retired, deliberately: it
// preserves host automation lanes and session compatibility for any future version.
//
//     idDamping ("damping") is now the Decay EQ HIGH CROSSOVER -- 1000..12000 Hz, default 3500.
//     idLowCut  ("lowcut")  is now the Decay EQ LOW CROSSOVER  --   60..800  Hz, default 250.
//
// Renaming these two ID strings later is a state-breaking change, not a cleanup -- do not "fix"
// them. Every OTHER identifier that refers to what they now do (labels, struct fields once 2A
// executes the fold) uses the crossover naming; only these two string literals are deliberately
// stale.
constexpr const char* idDamping   = "damping";
constexpr const char* idLowCut    = "lowcut";

constexpr const char* idBandwidth = "bandwidth";
constexpr const char* idDiffusion = "diffusion";
constexpr const char* idModDepth  = "moddepth";
constexpr const char* idModRate   = "modrate";
constexpr const char* idWidth     = "width";
constexpr const char* idFreeze    = "freeze";
constexpr const char* idOutput    = "output";

// ---- Phase 1, the 6 new IDs (PLAN-R2 3.2). xoverlow/xoverhigh were deleted by the fold. ----
constexpr const char* idDecayLow  = "decaylow";
constexpr const char* idDecayHigh = "decayhigh";
constexpr const char* idErMix     = "ermix";
constexpr const char* idErSize    = "ersize";
constexpr const char* idErSpread  = "erspread";
constexpr const char* idDensity   = "density";

// ---- Phase 2, the 10 new IDs. duckattack was cut; duckchar now sets the attack too (D5). ----
constexpr const char* idDuckAmount   = "duckamt";
constexpr const char* idDuckThresh   = "duckthresh";
constexpr const char* idDuckRelease  = "duckrelease";
constexpr const char* idDuckChar     = "duckchar";
constexpr const char* idPreDelaySync = "pdsync";
constexpr const char* idPreDelayDiv  = "pddiv";
constexpr const char* idBassMono     = "bassmono";
constexpr const char* idWetLowCut    = "wetlowcut";
constexpr const char* idWetHighCut   = "wethighcut";
constexpr const char* idWetTilt      = "wettilt";

// Phase 3's four IDs (shimmermode/shimmeramt/drive/algomode) were removed from the public
// parameter surface for the v1.0 release: PitchShifter::processSample() and Saturator::process()
// are still 1b no-ops (unconditional 0.0f / bit-exact identity) and voicingFor() still returns one
// shared Voicing regardless of algorithm, so all four controlled nothing audible. The DSP classes,
// the ReverbEngine::Params fields they used to drive, and the voicingFor() call site stay in place
// (see their own comments) so phase 3 can resume without re-deriving this; only the host-facing
// parameter, its GUI controls, and its preset-table columns are gone.

constexpr int numParameters = 29;

/** Every parameter ID above, for code that must walk the whole set (PLAN-R2 3.3's missing-
    parameter rule, and PluginProcessor's containsKnownParameter() guard). */
inline constexpr std::array<const char*, static_cast<size_t> (numParameters)> allIds
{
    idMix, idPreDelay, idSize, idDecay, idDamping, idLowCut, idBandwidth, idDiffusion,
    idModDepth, idModRate, idWidth, idFreeze, idOutput,
    idDecayLow, idDecayHigh, idErMix, idErSize, idErSpread, idDensity,
    idDuckAmount, idDuckThresh, idDuckRelease, idDuckChar, idPreDelaySync, idPreDelayDiv,
    idBassMono, idWetLowCut, idWetHighCut, idWetTilt
};

static_assert (allIds.size() == static_cast<size_t> (numParameters),
              "allIds must list exactly numParameters IDs");

/** On-disk layout revision, stamped into every saved session (PluginProcessor::getStateInformation
    / setStateInformation). PLAN-R2 3.3: nothing keys a migration off it yet -- that is this stamp's
    documented job, deferred to whichever future round adds a parameter with no neutral default. */
constexpr int currentStateVersion = 2;

//==================================================================================================

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

//==================================================================================================
// Factory presets. All 29 parameters are covered: these sixteen ship as the AU factory program
// list, so a preset that stayed silent about the ducker, wet EQ, or Decay EQ would let a user's own
// settings for those ride along uncontrolled underneath whatever the preset picks for the 13
// legacy IDs -- a state no preset describes and no way back to. See the Preset table in
// Parameters.cpp for the per-preset values and the reasoning behind each.
//
// The set is organised by WHAT PART OF A TRACK a preset is for, not by room type: the names carry a
// "Drums --"/"Stabs --"/"Vocal --"/"Bass --"/"Pads --"/"Atmos --"/"Space --"/"DJ --" prefix so a
// host's alphabetical program menu groups them by use. presetDescription() below carries the
// one-line plain-language version of the same thing for the editor.
//==================================================================================================

/** Preset names, in the same order applyPreset() takes an index. */
juce::StringArray presetNames();

int numPresets();

/** Writes preset `index` into apvts, notifying the host so automation follows. Out-of-range index
    is a no-op; the caller (PluginProcessor::applyPreset) still owns currentProgram and
    updateHostDisplay(). */
void applyPreset (juce::AudioProcessorValueTreeState& apvts, int index);

/** Empty string for an out-of-range index. */
juce::String presetName (int index);

/** One short, plain-language line per preset, in the same order as presetNames().
    Shown in the editor under the preset menu. Keep each under ~60 characters so it
    fits at the 700x480 minimum window size.
*/
juce::String presetDescription (int index);

/** The value the preset TABLE holds for one parameter, before range clamping. NaN if the index or
    the id is unknown. For tests: comparing this against what the parameter actually holds is the
    only way to catch an out-of-range table literal, because writing one clamps silently. */
float presetTableValue (int index, const char* parameterId);
// NOTE: intended for tests. It constructs a juce::String and does up to 29 string comparisons per
// call, so it is not suitable for an audio thread or any hot path. It is also the one place in
// Parameters.cpp that must be kept in step with the Preset struct by hand -- d3's `checked` count
// is what catches a missing mapping.

} // namespace reverb::param
