#include "Parameters.h"

#include <limits>

#include <array>

namespace reverb::param
{

namespace
{

//==================================================================================================
// Value formatting for host parameter displays.
//==================================================================================================
juce::String percentString (float value, int)   { return juce::String (value, 1) + " %"; }
juce::String msString      (float value, int)   { return juce::String (value, 1) + " ms"; }
juce::String rateString    (float value, int)   { return juce::String (value, 2) + " Hz"; }
juce::String decibelString (float value, int)   { return juce::String (value, 1) + " dB"; }
juce::String multiplierString (float value, int) { return juce::String (value, 2) + "x"; }

juce::String secondsString (float value, int)
{
    return value < 1.0f ? juce::String (value * 1000.0f, 0) + " ms"
                        : juce::String (value, 2) + " s";
}

juce::String frequencyString (float value, int)
{
    return value >= 1000.0f ? juce::String (value / 1000.0f, 2) + " kHz"
                            : juce::String (value, 1) + " Hz";
}

/** NormalisableRange with the given value sitting at the middle of the knob's travel. */
juce::NormalisableRange<float> skewedRange (float minimum, float maximum, float centre)
{
    juce::NormalisableRange<float> range { minimum, maximum };
    range.setSkewForCentre (centre);
    return range;
}

//==================================================================================================
// Preset table. Order defines both presetNames() and the AU factory program list. Covers all 29
// parameters (PLAN-R2 3.1/3.2/3.3) -- these sixteen ship as the AU factory program
// list, so a preset that is silent about a parameter is not an option: the user's ducker, wet EQ,
// and density settings would otherwise ride along uncontrolled underneath whatever the preset picks
// for the 13 legacy IDs. Field order mirrors Parameters.h's allIds.
//
// The set is voiced by USE, not by room type -- what a house producer reaches for on drums, stabs,
// a vocal, bass, pads, or a DJ move -- and the names carry a category prefix so a host's
// alphabetical program menu groups them that way. `description` is the plain-language line the
// editor shows under the preset menu (presetDescription()); it is stored here rather than in a
// parallel array so a name and its description cannot drift apart.
//==================================================================================================
struct Preset
{
    const char* name;
    const char* description;
    float mix, preDelay, size, decay, damping, lowCut, bandwidth,
          diffusion, modDepth, modRate, width;
    bool  freeze;
    float output;
    float decayLow, decayHigh, erMix, erSize, erSpread, density;
    float duckAmount, duckThresh, duckRelease;
    int   duckChar;
    bool  preDelaySync;
    int   preDelayDiv;
    float bassMono, wetLowCut, wetHighCut, wetTilt;
};

// Duck character indices (duckCharChoices below): 0 = Gentle, 1 = Pump.
// Pre-delay division index 5 = "1/8" (pdDivisionDefaultIndex), matching the layout default;
// index 8 = "1/4", which "Pads -- Slow Bloom" uses (see its note for why not something longer).
//
// Two set-wide rules, so the per-preset notes below only have to argue what is unusual:
//
//   - Ducker OFF (duckAmount 0) everywhere except the two presets whose whole point is ducking:
//     "Vocal -- Ducked Send" (Gentle, modest) and "DJ -- Slam Verb" (Pump, extreme). Those two are
//     also how a user discovers the ducker exists at all. Anywhere else, a preset that quietly
//     gain-rides the tail is a surprise, not a voicing.
//   - pdsync FALSE everywhere except "Pads -- Slow Bloom", where a tempo-locked arrival IS the
//     effect being sold. Elsewhere a preset silently enabling tempo sync would be a surprise.
//
// damping/lowCut are the Decay EQ's crossovers (D1a), and decayLow/decayHigh are the multipliers
// they gate; every preset sets the pair together, because a decayLow of 2x means nothing without
// knowing how far up the spectrum "low" reaches. Wet EQ (wetLowCut/wetHighCut/wetTilt) sits near
// neutral (20 Hz / 20000 Hz / 0 dB) unless a preset's job wants tone shaping on top.
const Preset presets[] =
{
    // 0. Default -- literally the parameter layout's own defaults, so a user who never touched a
    // preset and one who explicitly picks "Default" land in the same place. Kept first and kept
    // exact: it is the only entry in the table whose values are not a voicing choice.
    { "Default", "The plugin's own defaults — always a way back",
      35.0f, 10.0f, 100.0f,  2.5f, 3500.0f, 250.0f, 16000.0f, 70.0f, 25.0f, 0.40f, 100.0f, false,  0.0f,
      1.4f, 0.7f, 25.0f, 100.0f, 60.0f, 45.0f,
      0.0f, -24.0f, 250.0f, 0, false, 5, 250.0f, 20.0f, 20000.0f, 0.0f },

    // 1. Drums -- Tight Room. "Space with no wash" is an early-reflection statement, not a tail
    // one: erMix 65 and a 0.40 s decay mean most of what you hear is the discrete cluster, so the
    // kit reads as being in a room without anything ringing between hits. decayHigh 0.45 under a
    // high crossover of 6500 gives the dead, woody top a small treated room has. modDepth 6 is
    // near-zero on purpose -- audible modulation on a close-miked kit is detuning, not width.
    // wetLowCut 120 keeps the kick out of the reverb entirely, which is what stops "weight" from
    // becoming "mud".
    { "Drums — Tight Room", "Weight and space on drums, with no wash at all",
      20.0f,  4.0f,  38.0f, 0.40f, 6500.0f, 140.0f, 14000.0f, 55.0f,  6.0f, 0.90f,  85.0f, false,  0.0f,
      1.0f, 0.45f, 65.0f, 40.0f, 70.0f, 40.0f,
      0.0f, -24.0f, 250.0f, 0, false, 5, 200.0f, 120.0f, 15000.0f, -1.0f },

    // 2. Drums -- Snare Plate. A plate has no discrete early cluster, so this inverts preset 1:
    // erMix 20, diffusion 82, density 78. decayLow 0.75 / decayHigh 1.15 is the plate shape --
    // little bass, a top that sustains -- and the high crossover at 9500 keeps that sustaining band
    // up in the sizzle rather than the presence region, so it stays bright instead of harsh.
    // Pre-delay 12 ms leaves the snare crack in front of its own tail. wetLowCut 180 because a
    // snare verb has no business below that.
    { "Drums — Snare Plate", "Bright medium plate for snares and claps",
      26.0f, 12.0f,  55.0f, 1.30f, 9500.0f, 220.0f, 18000.0f, 82.0f, 14.0f, 1.10f, 110.0f, false,  0.0f,
      0.75f, 1.15f, 20.0f, 50.0f, 35.0f, 78.0f,
      0.0f, -24.0f, 250.0f, 0, false, 5, 220.0f, 180.0f, 20000.0f, 2.5f },

    // 3. Stabs -- Short & Bright. The most-used setting in the set, so every value is a decision.
    // decay 0.70 s: long enough to hear the space, short enough to be gone before the next eighth
    // at 124 bpm. decayLow 0.55 with the low crossover at 260 hands the bottom octaves back to the
    // bass and kick almost immediately -- the low end is not this reverb's job. wetLowCut 250 is
    // the single most important number here: it means the tail literally cannot muddy the low mid,
    // which is what lets you use more of it. width 130 spreads the stab around a mono kick/bass.
    // Plate rather than Room so there is no discrete slapback ticking behind the chord.
    { "Stabs — Short & Bright", "The house chord stab: quick, bright, out of the way",
      24.0f, 15.0f,  52.0f, 0.70f, 8500.0f, 260.0f, 17000.0f, 74.0f, 10.0f, 0.80f, 130.0f, false,  0.0f,
      0.55f, 0.85f, 35.0f, 55.0f, 65.0f, 62.0f,
      0.0f, -24.0f, 250.0f, 0, false, 5, 260.0f, 250.0f, 19000.0f, 2.0f },

    // 4. Stabs -- Deep & Warm. Preset 3's mirror image, which is the point of shipping both.
    // decayLow 1.7 with the low crossover pushed up to 400 puts the sustain right where deep-house
    // chords live, so the bass genuinely lingers instead of just being present. The high crossover
    // at 3200 drops nearly everything above it into the fast decayHigh 0.5 band, and bandwidth
    // 10000 plus wetTilt -2.5 finish the tape-ish top. wetLowCut stays down at 60: this is the one
    // chord preset where low warmth is the feature rather than the risk.
    { "Stabs — Deep & Warm", "Deep-house chords: warmer, with a longer, fuller tail",
      32.0f, 22.0f,  90.0f, 1.90f, 3200.0f, 400.0f, 10000.0f, 76.0f, 22.0f, 0.30f, 118.0f, false,  0.0f,
      1.7f, 0.5f, 22.0f, 95.0f, 60.0f, 70.0f,
      0.0f, -24.0f, 250.0f, 0, false, 5, 250.0f, 60.0f, 11000.0f, -2.5f },

    // 5. Vocal -- Ducked Send. One of the two ducked presets: Gentle character, duckAmount 40 and
    // threshold -28 so it pulls the tail down during phrases and lets it bloom in the gaps, with a
    // 320 ms release slow enough that the recovery is not audible as pumping. mix 100 because the
    // name says send -- and the description says so too, so landing it on an insert reads as a
    // stated choice rather than a bug; output -1 trims for the full-wet level. Pre-delay 40 ms is
    // the classic vocal gap: consonants land before anything comes back. erMix 15 -- a vocal wants
    // tail, not slap. decayLow 0.8 and wetLowCut 160 keep the tail out of the vocal's chest.
    { "Vocal — Ducked Send", "Full-wet send that ducks under the vocal",
      100.0f, 40.0f,  85.0f, 2.10f, 7000.0f, 240.0f, 16000.0f, 80.0f, 20.0f, 0.55f, 115.0f, false, -1.0f,
      0.8f, 0.9f, 15.0f, 80.0f, 55.0f, 76.0f,
      40.0f, -28.0f, 320.0f, 0, false, 5, 250.0f, 160.0f, 17000.0f, 0.5f },

    // 6. Vocal -- Wide Plate. The un-ducked vocal option, and the glossy one: decayHigh 1.05 above
    // a 9000 crossover means the air band actually sustains, which is the whole "glossy" character,
    // while decayLow 0.7 and wetLowCut 200 keep the low end out as asked. diffusion 88 / density 85
    // / erMix 12 is a plate with no discrete reflections at all. width 145 is the widest thing in
    // the set that is not a DJ effect; at 3 s of decay that is where a lead vocal wants it.
    { "Vocal — Wide Plate", "Big glossy plate, very wide, low end kept out",
      30.0f, 25.0f, 105.0f, 3.00f, 9000.0f, 230.0f, 19000.0f, 88.0f, 28.0f, 0.75f, 145.0f, false,  0.0f,
      0.7f, 1.05f, 12.0f, 90.0f, 45.0f, 85.0f,
      0.0f, -24.0f, 250.0f, 0, false, 5, 240.0f, 200.0f, 20000.0f, 3.0f },

    // 7. Bass -- Club Safe. Everything here is subtraction. decayLow 0.3 with the low crossover at
    // 700 -- the highest in the set -- means the entire low region dies almost immediately, so what
    // is left is the sense of a space rather than a low-frequency tail. bassMono 400 is the
    // parameter's maximum: below 400 Hz the wet is mono, so there is no low-end phase mess to
    // collapse on a big rig. width 60 is the narrowest in the set for the same reason. mix 14 and
    // erMix 55 keep it a hint. modDepth 4 because modulating a bass tail is audible detuning.
    { "Bass — Club Safe", "Space on bass without mud on a big system",
      14.0f,  8.0f,  60.0f, 0.80f, 5000.0f, 700.0f,  9000.0f, 60.0f,  4.0f, 0.35f,  60.0f, false, -1.0f,
      0.3f, 0.6f, 55.0f, 60.0f, 30.0f, 45.0f,
      0.0f, -24.0f, 250.0f, 0, false, 5, 400.0f, 220.0f,  9000.0f, -2.0f },

    // 8. Pads -- Big Wash. The blur is built, not incidental: diffusion 90, density 88, erSpread 85
    // and erMix 8 leave nothing discrete to latch onto. decayLow 1.9 / decayHigh 0.35 across a
    // 450 Hz and 2600 Hz pair is the classic dark-and-huge shape -- the bottom sustains for well
    // over ten seconds while the top is gone in two. bassMono 300 is doing real work here: this is
    // one of the presets long and low enough to smear a club system if the sub stayed stereo.
    { "Pads — Big Wash", "Long, dark and wide — everything blurs together",
      45.0f, 55.0f, 155.0f, 6.50f, 2600.0f, 450.0f,  9000.0f, 90.0f, 40.0f, 0.20f, 140.0f, false, -1.0f,
      1.9f, 0.35f, 8.0f, 160.0f, 85.0f, 88.0f,
      0.0f, -24.0f, 250.0f, 0, false, 5, 300.0f, 45.0f,  9000.0f, -4.0f },

    // 9. Pads -- Slow Bloom. The one tempo-synced preset. Division "1/4" (index 8) rather than
    // something longer on purpose: TempoSync clamps the synced pre-delay at 500 ms (tests/
    // HouseTests.cpp H5b), and 1/4 at house tempo is 469-484 ms, so it is the LONGEST division that
    // still tracks the tempo instead of sitting permanently pinned at the clamp. The free predelay
    // is set to 450 ms as well, so a host that reports no tempo (H6's fallback) gives the same
    // character rather than a completely different preset. erMix 5 because nothing at all should
    // arrive before the bloom does; modDepth 50 at modRate 0.12 Hz is the slow swell.
    { "Pads — Slow Bloom", "Arrives late and swells; follows the track tempo",
      42.0f, 450.0f, 140.0f, 5.00f, 3000.0f, 380.0f, 12000.0f, 86.0f, 50.0f, 0.12f, 135.0f, false, -1.0f,
      1.5f, 0.55f, 5.0f, 150.0f, 80.0f, 82.0f,
      0.0f, -24.0f, 250.0f, 0, true, 8, 280.0f, 70.0f, 12000.0f, -2.0f },

    // 10. Atmos -- Glue. The most extreme early-reflection preset in the set: erMix 85, decay 0.30,
    // erSize 38, density 25, pre-delay 0. There is effectively no tail -- what it adds is a shared
    // set of very early reflections across whatever it is on, which is what makes separately-
    // recorded dry parts sound like they are in one place. The high crossover sits at 10000 and
    // wetTilt at 0 deliberately: glue that recolours the mix is not glue.
    { "Atmos — Glue", "Very short room that makes a dry mix sit together",
      15.0f,  0.0f,  35.0f, 0.30f, 10000.0f, 120.0f, 18000.0f, 40.0f,  5.0f, 0.60f, 120.0f, false,  0.0f,
      1.0f, 0.7f, 85.0f, 38.0f, 75.0f, 25.0f,
      0.0f, -24.0f, 250.0f, 0, false, 5, 200.0f, 140.0f, 18000.0f, 0.0f },

    // 11. Space -- Cathedral. The extreme of the table on every "big" axis: size 190, decay 11 s,
    // diffusion 92, density 92, erSpread 95. The biggest bass/treble split too -- decayLow 2.4,
    // decayHigh 0.3 -- with the low crossover at 500 so the rumble reaches well into the low mids,
    // as it genuinely does in a stone building, and the high crossover at 2200 standing in for the
    // air absorption over that distance. bandwidth 8000 and wetTilt -5 keep it dark rather than
    // merely loud; output -2 because 50% mix at this length is a lot of energy.
    { "Space — Cathedral", "The biggest, darkest, longest one here",
      50.0f, 60.0f, 190.0f, 11.00f, 2200.0f, 500.0f,  8000.0f, 92.0f, 32.0f, 0.15f, 150.0f, false, -2.0f,
      2.4f, 0.3f, 8.0f, 190.0f, 95.0f, 92.0f,
      0.0f, -24.0f, 250.0f, 0, false, 5, 320.0f, 35.0f,  8000.0f, -5.0f },

    // 12. Space -- Dub Chamber. Not just a darker Cathedral: it is shorter (4.5 s) and much more
    // heavily processed. modDepth 75 is the deepest in the set, bandwidth 4500 and wetHighCut 4500
    // together are the filtered part, and the high crossover at 1400 -- the lowest available region
    // of that control -- drops almost the whole spectrum into a decayHigh of 0.22. Pre-delay 90 ms
    // gives the dub-style gap before the chamber answers. wetTilt -7 is the darkest here.
    { "Space — Dub Chamber", "Long, dark, heavily filtered and wobbling",
      40.0f, 90.0f, 120.0f, 4.50f, 1400.0f, 620.0f,  4500.0f, 78.0f, 75.0f, 0.45f, 130.0f, false, -1.0f,
      1.8f, 0.22f, 18.0f, 115.0f, 55.0f, 66.0f,
      0.0f, -24.0f, 250.0f, 0, false, 5, 260.0f, 90.0f,  4500.0f, -7.0f },

    // 13. DJ -- Freeze Hold. freeze = true is the preset. decay 25 s and density 86 matter for the
    // moment BEFORE the hold and for the release out of it -- a sparse tank freezes into something
    // metallic. erMix 5 because freeze gates the early cluster anyway (tests/
    // EarlyReflectionsTests.cpp E5), so early reflections contribute nothing while held; spending
    // the mix on the tail instead is the only useful choice. Crossovers 4200/500 with decayHigh 0.9
    // keep an indefinite hold from accumulating harshness. output -3: a held wash only gets louder.
    { "DJ — Freeze Hold", "Endless tail — holds until you switch it off",
      60.0f,  0.0f, 145.0f, 25.00f, 4200.0f, 480.0f, 20000.0f, 90.0f, 24.0f, 0.28f, 125.0f, true,  -3.0f,
      1.2f, 0.9f, 5.0f, 140.0f, 60.0f, 86.0f,
      0.0f, -24.0f, 250.0f, 0, false, 5, 250.0f, 60.0f, 20000.0f, 0.0f },

    // 14. DJ -- Riser Wash. Big and bright at the same time, which needs the crossover rather than
    // the multiplier: decayHigh 0.8 is only slightly short, but the high crossover at 11000 puts it
    // above the presence region, so the whole audible top sustains and only the extreme sizzle dies
    // -- exactly what stops a 7.5 s wash at 55% mix from turning harsh over a build. decayLow 1.6
    // keeps the sub-swell that makes a riser feel like it is lifting. width 160 is the widest in the
    // set and bassMono 300 is what makes that safe. bandwidth 20000 and wetTilt +3.5 do the bright.
    { "DJ — Riser Wash", "Huge bright wash for build-ups and drops",
      55.0f, 20.0f, 170.0f, 7.50f, 11000.0f, 420.0f, 20000.0f, 90.0f, 45.0f, 0.60f, 160.0f, false, -2.0f,
      1.6f, 0.8f, 10.0f, 170.0f, 90.0f, 90.0f,
      0.0f, -24.0f, 250.0f, 0, false, 5, 300.0f, 50.0f, 20000.0f, 3.5f },

    // 15. DJ -- Slam Verb. The extreme ducker: amount 100, threshold -34 so essentially every hit
    // triggers it, Pump character for the fast attack, and a 90 ms release -- fast enough that the
    // tail is fully back before the next sixteenth at 125 bpm (120 ms), which is what turns ducking
    // into rhythm instead of just gain reduction. Everything else is chosen to be audible when it
    // slams back: mix 50, decay 2.60 s, erMix 30 and modRate 1.40 Hz for a fast shimmerless
    // movement. wetLowCut 130 so the pumping does not fight the kick it is pumping to.
    { "DJ — Slam Verb", "Hard pumping tail that slams in between hits",
      50.0f,  6.0f,  95.0f, 2.60f, 6000.0f, 300.0f, 15000.0f, 84.0f, 18.0f, 1.40f, 135.0f, false,  0.0f,
      1.1f, 0.6f, 30.0f, 90.0f, 70.0f, 74.0f,
      100.0f, -34.0f, 90.0f, 1, false, 5, 250.0f, 130.0f, 16000.0f, 1.0f }
};

constexpr int numPresetsValue = static_cast<int> (std::size (presets));

void setParameterNotifying (juce::AudioProcessorValueTreeState& state, const char* id, float value)
{
    if (auto* parameter = state.getParameter (id))
        parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
}

const juce::StringArray pdDivisionChoices { "1/32", "1/16T", "1/16", "1/8T", "1/16D", "1/8",
                                            "1/4T", "3/16", "1/4", "1/2", "1/1" };
constexpr int pdDivisionDefaultIndex = 5;   // "1/8", PLAN-R2 3.2

const juce::StringArray duckCharChoices { "Gentle", "Pump" };

} // namespace

//==================================================================================================

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    using Layout      = juce::AudioProcessorValueTreeState::ParameterLayout;
    using FloatParam  = juce::AudioParameterFloat;
    using FloatAttrs  = juce::AudioParameterFloatAttributes;
    using ChoiceParam = juce::AudioParameterChoice;
    using BoolParam   = juce::AudioParameterBool;

    Layout layout;

    // ---- the 13 existing parameters (PLAN-R2 3.1) ----
    layout.add (std::make_unique<FloatParam> (juce::ParameterID { idMix, 1 }, "Mix",
                                              juce::NormalisableRange<float> { 0.0f, 100.0f }, 35.0f,
                                              FloatAttrs().withLabel ("%")
                                                          .withStringFromValueFunction (percentString)));

    layout.add (std::make_unique<FloatParam> (juce::ParameterID { idPreDelay, 1 }, "Pre-Delay",
                                              skewedRange (0.0f, 500.0f, 100.0f), 10.0f,
                                              FloatAttrs().withLabel ("ms")
                                                          .withStringFromValueFunction (msString)));

    layout.add (std::make_unique<FloatParam> (juce::ParameterID { idSize, 1 }, "Size",
                                              juce::NormalisableRange<float> { 10.0f, 200.0f }, 100.0f,
                                              FloatAttrs().withLabel ("%")
                                                          .withStringFromValueFunction (percentString)));

    layout.add (std::make_unique<FloatParam> (juce::ParameterID { idDecay, 1 }, "Decay",
                                              skewedRange (0.1f, 30.0f, 3.0f), 2.5f,
                                              FloatAttrs().withLabel ("s")
                                                          .withStringFromValueFunction (secondsString)));

    // D1a: repurposed as the Decay EQ's high crossover. ID and version hint stay put deliberately
    // (idDamping doc comment, Parameters.h) so host automation lanes keep working.
    layout.add (std::make_unique<FloatParam> (juce::ParameterID { idDamping, 1 }, "High X-over",
                                              skewedRange (1000.0f, 12000.0f, 3500.0f), 3500.0f,
                                              FloatAttrs().withLabel ("Hz")
                                                          .withStringFromValueFunction (frequencyString)));

    // D1a: repurposed as the Decay EQ's low crossover. Same rationale as idDamping above.
    layout.add (std::make_unique<FloatParam> (juce::ParameterID { idLowCut, 1 }, "Low X-over",
                                              skewedRange (60.0f, 800.0f, 250.0f), 250.0f,
                                              FloatAttrs().withLabel ("Hz")
                                                          .withStringFromValueFunction (frequencyString)));

    layout.add (std::make_unique<FloatParam> (juce::ParameterID { idBandwidth, 1 }, "Bandwidth",
                                              skewedRange (1000.0f, 20000.0f, 8000.0f), 16000.0f,
                                              FloatAttrs().withLabel ("Hz")
                                                          .withStringFromValueFunction (frequencyString)));

    layout.add (std::make_unique<FloatParam> (juce::ParameterID { idDiffusion, 1 }, "Diffusion",
                                              juce::NormalisableRange<float> { 0.0f, 100.0f }, 70.0f,
                                              FloatAttrs().withLabel ("%")
                                                          .withStringFromValueFunction (percentString)));

    layout.add (std::make_unique<FloatParam> (juce::ParameterID { idModDepth, 1 }, "Mod Depth",
                                              juce::NormalisableRange<float> { 0.0f, 100.0f }, 25.0f,
                                              FloatAttrs().withLabel ("%")
                                                          .withStringFromValueFunction (percentString)));

    layout.add (std::make_unique<FloatParam> (juce::ParameterID { idModRate, 1 }, "Mod Rate",
                                              skewedRange (0.01f, 5.0f, 0.5f), 0.4f,
                                              FloatAttrs().withLabel ("Hz")
                                                          .withStringFromValueFunction (rateString)));

    layout.add (std::make_unique<FloatParam> (juce::ParameterID { idWidth, 1 }, "Width",
                                              juce::NormalisableRange<float> { 0.0f, 200.0f }, 100.0f,
                                              FloatAttrs().withLabel ("%")
                                                          .withStringFromValueFunction (percentString)));

    layout.add (std::make_unique<BoolParam> (juce::ParameterID { idFreeze, 1 }, "Freeze", false));

    layout.add (std::make_unique<FloatParam> (juce::ParameterID { idOutput, 1 }, "Output",
                                              juce::NormalisableRange<float> { -24.0f, 12.0f }, 0.0f,
                                              FloatAttrs().withLabel ("dB")
                                                          .withStringFromValueFunction (decibelString)));

    // ---- Phase 1, 6 new parameters (PLAN-R2 3.2) ----
    layout.add (std::make_unique<FloatParam> (juce::ParameterID { idDecayLow, 1 }, "Low Decay",
                                              skewedRange (0.1f, 4.0f, 1.0f), 1.4f,
                                              FloatAttrs().withLabel ("x")
                                                          .withStringFromValueFunction (multiplierString)));

    layout.add (std::make_unique<FloatParam> (juce::ParameterID { idDecayHigh, 1 }, "High Decay",
                                              skewedRange (0.05f, 4.0f, 1.0f), 0.7f,
                                              FloatAttrs().withLabel ("x")
                                                          .withStringFromValueFunction (multiplierString)));

    layout.add (std::make_unique<FloatParam> (juce::ParameterID { idErMix, 1 }, "Early / Late",
                                              juce::NormalisableRange<float> { 0.0f, 100.0f }, 25.0f,
                                              FloatAttrs().withLabel ("%")
                                                          .withStringFromValueFunction (percentString)));

    layout.add (std::make_unique<FloatParam> (juce::ParameterID { idErSize, 1 }, "ER Size",
                                              juce::NormalisableRange<float> { 25.0f, 200.0f }, 100.0f,
                                              FloatAttrs().withLabel ("%")
                                                          .withStringFromValueFunction (percentString)));

    layout.add (std::make_unique<FloatParam> (juce::ParameterID { idErSpread, 1 }, "ER Spread",
                                              juce::NormalisableRange<float> { 0.0f, 100.0f }, 60.0f,
                                              FloatAttrs().withLabel ("%")
                                                          .withStringFromValueFunction (percentString)));

    layout.add (std::make_unique<FloatParam> (juce::ParameterID { idDensity, 1 }, "Density",
                                              juce::NormalisableRange<float> { 0.0f, 100.0f }, 45.0f,
                                              FloatAttrs().withLabel ("%")
                                                          .withStringFromValueFunction (percentString)));

    // ---- Phase 2, 10 new parameters ----
    layout.add (std::make_unique<FloatParam> (juce::ParameterID { idDuckAmount, 1 }, "Duck Amount",
                                              juce::NormalisableRange<float> { 0.0f, 100.0f }, 0.0f,
                                              FloatAttrs().withLabel ("%")
                                                          .withStringFromValueFunction (percentString)));

    layout.add (std::make_unique<FloatParam> (juce::ParameterID { idDuckThresh, 1 }, "Duck Threshold",
                                              juce::NormalisableRange<float> { -60.0f, 0.0f }, -24.0f,
                                              FloatAttrs().withLabel ("dB")
                                                          .withStringFromValueFunction (decibelString)));

    layout.add (std::make_unique<FloatParam> (juce::ParameterID { idDuckRelease, 1 }, "Duck Release",
                                              skewedRange (20.0f, 1000.0f, 250.0f), 250.0f,
                                              FloatAttrs().withLabel ("ms")
                                                          .withStringFromValueFunction (msString)));

    layout.add (std::make_unique<ChoiceParam> (juce::ParameterID { idDuckChar, 1 }, "Duck Character",
                                               duckCharChoices, 0));

    layout.add (std::make_unique<BoolParam> (juce::ParameterID { idPreDelaySync, 1 },
                                             "Pre-Delay Sync", false));

    layout.add (std::make_unique<ChoiceParam> (juce::ParameterID { idPreDelayDiv, 1 },
                                               "Pre-Delay Division", pdDivisionChoices,
                                               pdDivisionDefaultIndex));

    // Default raised from 130 to 250 Hz (measured, not guessed): the side response is
    // r^2/sqrt(1+r^4), so a 130 Hz corner leaves measured 20..100 Hz side energy at only -11.1 dB --
    // nowhere near mono. The research note behind this control says keep everything below ~150 Hz
    // mono, which at this filter's 12 dB/oct slope needs a corner around 250 Hz; measured points
    // confirm it (200 Hz -> -18.0 dB, still short; 250 Hz -> -21.8 dB, clears the -20 dB bar; 400 Hz
    // -> -29.9 dB). See tests/WetPathTests.cpp W2, whose asserted leg uses 300 Hz for the same reason.
    layout.add (std::make_unique<FloatParam> (juce::ParameterID { idBassMono, 1 }, "Bass Mono",
                                              skewedRange (0.0f, 400.0f, 250.0f), 250.0f,
                                              FloatAttrs().withLabel ("Hz")
                                                          .withStringFromValueFunction (frequencyString)));

    layout.add (std::make_unique<FloatParam> (juce::ParameterID { idWetLowCut, 1 }, "Wet Low Cut",
                                              skewedRange (20.0f, 1000.0f, 150.0f), 20.0f,
                                              FloatAttrs().withLabel ("Hz")
                                                          .withStringFromValueFunction (frequencyString)));

    layout.add (std::make_unique<FloatParam> (juce::ParameterID { idWetHighCut, 1 }, "Wet High Cut",
                                              skewedRange (1000.0f, 20000.0f, 8000.0f), 20000.0f,
                                              FloatAttrs().withLabel ("Hz")
                                                          .withStringFromValueFunction (frequencyString)));

    layout.add (std::make_unique<FloatParam> (juce::ParameterID { idWetTilt, 1 }, "Wet Tilt",
                                              juce::NormalisableRange<float> { -12.0f, 12.0f }, 0.0f,
                                              FloatAttrs().withLabel ("dB")
                                                          .withStringFromValueFunction (decibelString)));

    // Phase 3's four parameters (Shimmer mode/amount, Drive, Algorithm) were removed from the
    // layout for v1.0 -- see the "Phase 3's four IDs" comment in Parameters.h.

    return layout;
}

//==================================================================================================

// All three string accessors below wrap the stored `const char*` in CharPointer_UTF8, and that is
// load-bearing rather than defensive. juce::String's const char* constructor decodes as
// **CharPointer_ASCII**, not UTF-8 (juce_String.cpp:306-308), so an em dash (E2 80 94) is passed
// through byte-by-byte as three Latin-1 codepoints: "Drums — Tight Room" renders as
// "Drums â€ Tight Room". Fifteen of these sixteen names carry an em dash, so without this the
// factory-preset menu in every host shows mojibake.
//
// The trap is that JUCE *asserts* on bytes above 127 in that constructor, so a Debug build catches
// it loudly and a Release build does not catch it at all. This was found by looking at a rendered
// screenshot, not by any test.
//
// Convert here, in the accessors, rather than at the call sites: a future preset with an accent or
// a typographic apostrophe then cannot reintroduce it.
juce::StringArray presetNames()
{
    juce::StringArray names;

    for (const auto& preset : presets)
        names.add (juce::String (juce::CharPointer_UTF8 (preset.name)));

    return names;
}

int numPresets()   { return numPresetsValue; }

/** The value the preset TABLE holds for one parameter, before any range clamping.

    Exists so a test can compare what the table asked for against what the parameter ended up
    holding. That distinction is the whole point: `setParameterNotifying` goes through
    `NormalisableRange::convertTo0to1`, which CLAMPS, so an out-of-range literal in the table is
    silently pulled to the range end and is then indistinguishable from a deliberate endpoint. A
    typo'd zero was demonstrated to be invisible to the build, both test binaries and the host.

    Returns NaN for an unknown id or an out-of-range preset index, so a caller cannot mistake
    "no such value" for a real one.
*/
float presetTableValue (int index, const char* parameterId)
{
    if (index < 0 || index >= numPresetsValue || parameterId == nullptr)
        return std::numeric_limits<float>::quiet_NaN();

    const auto& p = presets[static_cast<size_t> (index)];
    const juce::String id { parameterId };

    if (id == idMix)          return p.mix;
    if (id == idPreDelay)     return p.preDelay;
    if (id == idSize)         return p.size;
    if (id == idDecay)        return p.decay;
    if (id == idDamping)      return p.damping;
    if (id == idLowCut)       return p.lowCut;
    if (id == idBandwidth)    return p.bandwidth;
    if (id == idDiffusion)    return p.diffusion;
    if (id == idModDepth)     return p.modDepth;
    if (id == idModRate)      return p.modRate;
    if (id == idWidth)        return p.width;
    if (id == idFreeze)       return p.freeze ? 1.0f : 0.0f;
    if (id == idOutput)       return p.output;
    if (id == idDecayLow)     return p.decayLow;
    if (id == idDecayHigh)    return p.decayHigh;
    if (id == idErMix)        return p.erMix;
    if (id == idErSize)       return p.erSize;
    if (id == idErSpread)     return p.erSpread;
    if (id == idDensity)      return p.density;
    if (id == idDuckAmount)   return p.duckAmount;
    if (id == idDuckThresh)   return p.duckThresh;
    if (id == idDuckRelease)  return p.duckRelease;
    if (id == idDuckChar)     return static_cast<float> (p.duckChar);
    if (id == idPreDelaySync) return p.preDelaySync ? 1.0f : 0.0f;
    if (id == idPreDelayDiv)  return static_cast<float> (p.preDelayDiv);
    if (id == idBassMono)     return p.bassMono;
    if (id == idWetLowCut)    return p.wetLowCut;
    if (id == idWetHighCut)   return p.wetHighCut;
    if (id == idWetTilt)      return p.wetTilt;

    return std::numeric_limits<float>::quiet_NaN();
}

void applyPreset (juce::AudioProcessorValueTreeState& apvts, int index)
{
    if (index < 0 || index >= numPresetsValue)
        return;

    const auto& preset = presets[static_cast<size_t> (index)];

    // ---- the 13 legacy parameters ----
    setParameterNotifying (apvts, idMix,       preset.mix);
    setParameterNotifying (apvts, idPreDelay,  preset.preDelay);
    setParameterNotifying (apvts, idSize,      preset.size);
    setParameterNotifying (apvts, idDecay,     preset.decay);
    setParameterNotifying (apvts, idDamping,   preset.damping);
    setParameterNotifying (apvts, idLowCut,    preset.lowCut);
    setParameterNotifying (apvts, idBandwidth, preset.bandwidth);
    setParameterNotifying (apvts, idDiffusion, preset.diffusion);
    setParameterNotifying (apvts, idModDepth,  preset.modDepth);
    setParameterNotifying (apvts, idModRate,   preset.modRate);
    setParameterNotifying (apvts, idWidth,     preset.width);
    setParameterNotifying (apvts, idFreeze,    preset.freeze ? 1.0f : 0.0f);
    setParameterNotifying (apvts, idOutput,    preset.output);

    // ---- Phase 1 ----
    setParameterNotifying (apvts, idDecayLow,  preset.decayLow);
    setParameterNotifying (apvts, idDecayHigh, preset.decayHigh);
    setParameterNotifying (apvts, idErMix,     preset.erMix);
    setParameterNotifying (apvts, idErSize,    preset.erSize);
    setParameterNotifying (apvts, idErSpread,  preset.erSpread);
    setParameterNotifying (apvts, idDensity,   preset.density);

    // ---- Phase 2 ----
    setParameterNotifying (apvts, idDuckAmount,   preset.duckAmount);
    setParameterNotifying (apvts, idDuckThresh,   preset.duckThresh);
    setParameterNotifying (apvts, idDuckRelease,  preset.duckRelease);
    setParameterNotifying (apvts, idDuckChar,     static_cast<float> (preset.duckChar));
    setParameterNotifying (apvts, idPreDelaySync, preset.preDelaySync ? 1.0f : 0.0f);
    setParameterNotifying (apvts, idPreDelayDiv,  static_cast<float> (preset.preDelayDiv));
    setParameterNotifying (apvts, idBassMono,     preset.bassMono);
    setParameterNotifying (apvts, idWetLowCut,    preset.wetLowCut);
    setParameterNotifying (apvts, idWetHighCut,   preset.wetHighCut);
    setParameterNotifying (apvts, idWetTilt,      preset.wetTilt);
}

juce::String presetName (int index)
{
    if (index < 0 || index >= numPresetsValue)
        return {};

    return juce::String (juce::CharPointer_UTF8 (presets[static_cast<size_t> (index)].name));
}

juce::String presetDescription (int index)
{
    if (index < 0 || index >= numPresetsValue)
        return {};

    return juce::String (juce::CharPointer_UTF8 (presets[static_cast<size_t> (index)].description));
}

} // namespace reverb::param
