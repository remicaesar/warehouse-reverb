#include "PluginProcessor.h"

#include "Parameters.h"
#include "PluginEditor.h"

#include <algorithm>
#include <cmath>

namespace
{

// Must match ReverbAudioProcessorEditor::defaultWidth/Height (ruling A2: 900x620). A fresh,
// never-saved instance opens at these, and WarehouseSnapshot derives its "default" render size from
// the editor's actual bounds -- so a stale value here silently renders and verifies the GUI at the
// wrong size, which is exactly the failure the derived-size change was meant to remove.
constexpr int defaultEditorWidth  = 900;
constexpr int defaultEditorHeight = 620;

// Reported as the tail length while Freeze is engaged; the real tail is unbounded.
constexpr double frozenTailSeconds = 60.0;

const juce::Identifier editorWidthProperty  { "editorWidth" };
const juce::Identifier editorHeightProperty { "editorHeight" };
const juce::Identifier programProperty      { "currentProgram" };
const juce::Identifier stateVersionProperty { "stateVersion" };

// How AudioProcessorValueTreeState stores each parameter under the state tree.
const juce::Identifier parameterTag        { "PARAM" };
const juce::Identifier parameterIdProperty { "id" };
const juce::Identifier valueProperty       { "value" };

/** True if the tree carries at least one child this plugin recognises as one of its parameters. */
bool containsKnownParameter (const juce::AudioProcessorValueTreeState& apvts,
                             const juce::ValueTree& tree)
{
    for (const auto& child : tree)
        if (child.hasType (parameterTag)
            && apvts.getParameter (child.getProperty (parameterIdProperty).toString()) != nullptr)
            return true;

    return false;
}

/** True if `tree` carries a PARAM child for `id` with a "value" property -- i.e. the document
    actually specifies this parameter, as opposed to merely mentioning its id. */
bool incomingTreeSpecifies (const juce::ValueTree& tree, const char* id)
{
    for (const auto& child : tree)
        if (child.hasType (parameterTag)
            && child.getProperty (parameterIdProperty).toString() == id
            && child.hasProperty (valueProperty))
            return true;

    return false;
}

/** Which of PLAN-R2 3.3's known parameter IDs the incoming document specifies, indexed the same
    as reverb::param::allIds. MUST be captured BEFORE apvts.replaceState() runs -- see
    resetParametersMissingFrom()'s doc comment for why reading the tree afterwards is wrong. */
using SpecifiedFlags = std::array<bool, static_cast<size_t> (reverb::param::numParameters)>;

SpecifiedFlags whichParametersAreSpecified (const juce::ValueTree& incomingTree)
{
    SpecifiedFlags specified {};

    for (size_t i = 0; i < reverb::param::allIds.size(); ++i)
        specified[i] = incomingTreeSpecifies (incomingTree, reverb::param::allIds[i]);

    return specified;
}

/** PLAN-R2 3.3: any known parameter the incoming document does not specify is reset to ITS OWN
    default, never left at whatever the previously-loaded preset set it to.

    `specified` MUST be captured from the incoming document BEFORE apvts.replaceState() runs, not
    after: juce::ValueTree has reference semantics, so `apvts.replaceState (tree)` makes
    `apvts.state` and the caller's `tree` variable the SAME underlying shared object, and
    updateParameterConnectionsToChildTrees() (C14) then APPENDS a fresh child --
    counter-intuitively sometimes carrying a "value" property, when the parameter's own in-memory
    value differed from its default at the moment of the call -- for every parameter the document
    did not mention. Re-inspecting `tree` for "does it specify this id" AFTER replaceState() would
    therefore see JUCE's own just-appended children instead of what the document originally said,
    which is silently self-defeating for exactly the reused-instance case this rule exists for:
    verified empirically while mutation-testing S3 (step 1a report) -- disabling this function
    changed nothing, because JUCE's own reentrant reset already happens to land on the correct
    default for a parameter that was mid-flight to a fresh value when the load arrived, and that
    default value's arrival is precisely what made incomingTreeSpecifies() see it as "already
    specified" and skip it. The bug cancelled a bug; the fix is to never re-read the mutated tree.
*/
void resetParametersMissingFrom (juce::AudioProcessorValueTreeState& apvts,
                                 const SpecifiedFlags& specified)
{
    for (size_t i = 0; i < reverb::param::allIds.size(); ++i)
    {
        if (specified[i])
            continue;

        if (auto* parameter = apvts.getParameter (reverb::param::allIds[i]))
            parameter->setValueNotifyingHost (parameter->getDefaultValue());
    }
}

} // namespace

//==================================================================================================

ReverbAudioProcessor::ReverbAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMETERS", reverb::param::createParameterLayout())
{
    using namespace reverb::param;

    mixParam       = raw (idMix);
    preDelayParam  = raw (idPreDelay);
    sizeParam      = raw (idSize);
    decayParam     = raw (idDecay);
    dampingParam   = raw (idDamping);
    lowCutParam    = raw (idLowCut);
    bandwidthParam = raw (idBandwidth);
    diffusionParam = raw (idDiffusion);
    modDepthParam  = raw (idModDepth);
    modRateParam   = raw (idModRate);
    widthParam     = raw (idWidth);
    freezeParam    = raw (idFreeze);
    outputParam    = raw (idOutput);

    decayLowParam  = raw (idDecayLow);
    decayHighParam = raw (idDecayHigh);
    erMixParam     = raw (idErMix);
    erSizeParam    = raw (idErSize);
    erSpreadParam  = raw (idErSpread);
    densityParam   = raw (idDensity);

    duckAmountParam   = raw (idDuckAmount);
    duckThreshParam   = raw (idDuckThresh);
    duckReleaseParam  = raw (idDuckRelease);
    duckCharParam     = raw (idDuckChar);
    preDelaySyncParam = raw (idPreDelaySync);
    preDelayDivParam  = raw (idPreDelayDiv);
    bassMonoParam     = raw (idBassMono);
    wetLowCutParam    = raw (idWetLowCut);
    wetHighCutParam   = raw (idWetHighCut);
    wetTiltParam      = raw (idWetTilt);

    // Stamped on construction so a freshly-created session already carries the current layout
    // revision; setStateInformation() re-stamps it after every load too (PLAN-R2 3.3).
    apvts.state.setProperty (stateVersionProperty, currentStateVersion, nullptr);

    for (auto& level : outputLevel)
        level.store (0.0f, std::memory_order_relaxed);
}

ReverbAudioProcessor::~ReverbAudioProcessor() = default;

//==================================================================================================

std::atomic<float>* ReverbAudioProcessor::raw (const char* id) const noexcept
{
    return apvts.getRawParameterValue (id);
}

reverb::ReverbEngine::Params ReverbAudioProcessor::currentParameters() const noexcept
{
    reverb::ReverbEngine::Params p;

    auto value = [] (const std::atomic<float>* source, float fallback)
    {
        return source != nullptr ? source->load (std::memory_order_relaxed) : fallback;
    };

    // Choice/bool parameters are stored denormalised too (C13): a choice's raw value is its
    // index, a bool's is 0.0f/1.0f.
    auto intValue = [&value] (const std::atomic<float>* source, int fallback)
    {
        return source != nullptr ? static_cast<int> (value (source, static_cast<float> (fallback)) + 0.5f)
                                 : fallback;
    };

    auto boolValue = [&value] (const std::atomic<float>* source, bool fallback)
    {
        return value (source, fallback ? 1.0f : 0.0f) > 0.5f;
    };

    // ---- the 13 existing parameters (PLAN-R2 3.1) ----
    p.mix          = value (mixParam,       35.0f);
    p.preDelayMs   = value (preDelayParam,  10.0f);
    p.size         = value (sizeParam,      100.0f);
    p.decaySeconds = value (decayParam,     2.5f);
    p.dampingHz    = value (dampingParam,   3500.0f);    // D1a: high crossover default
    p.lowCutHz     = value (lowCutParam,    250.0f);     // D1a: low crossover default
    p.bandwidthHz  = value (bandwidthParam, 16000.0f);
    p.diffusion    = value (diffusionParam, 70.0f);
    p.modDepth     = value (modDepthParam,  25.0f);
    p.modRateHz    = value (modRateParam,   0.4f);
    p.width        = value (widthParam,     100.0f);
    p.outputGainDb = value (outputParam,    0.0f);
    p.freeze       = boolValue (freezeParam, false);

    // ---- Phase 1, 6 new parameters (PLAN-R2 3.2). No DSP reads these yet (step 1a). ----
    p.decayLowMult   = value (decayLowParam,  1.4f);
    p.decayHighMult  = value (decayHighParam, 0.7f);
    p.erMix          = value (erMixParam,     25.0f);
    p.erSizePercent  = value (erSizeParam,    100.0f);
    p.erSpread       = value (erSpreadParam,  60.0f);
    p.density        = value (densityParam,   45.0f);

    // ---- Phase 2, 10 new parameters. No DSP reads these yet (step 1a). ----
    p.duckAmount       = value (duckAmountParam,  0.0f);
    p.duckThresholdDb  = value (duckThreshParam,  -24.0f);
    p.duckReleaseMs    = value (duckReleaseParam, 250.0f);
    p.duckCharacter    = intValue (duckCharParam, 0);
    p.preDelaySync     = boolValue (preDelaySyncParam, false);
    p.preDelayDivision = intValue (preDelayDivParam, 5);
    p.bassMonoHz       = value (bassMonoParam,   250.0f);   // keep in step with Parameters.cpp + ReverbEngine.h
    p.wetLowCutHz      = value (wetLowCutParam,  20.0f);
    p.wetHighCutHz     = value (wetHighCutParam, 20000.0f);
    p.wetTiltDb        = value (wetTiltParam,    0.0f);

    // Phase 3's shimmerMode/shimmerAmount/drive/algorithm are no longer driven by a host
    // parameter (see the "Phase 3's four IDs" comment in Parameters.h) -- p's own field defaults
    // (0 / 0.0f / 0.0f / 2, ReverbEngine.h) already match what these used to be clamped to, so
    // leaving them unset here is exact, not an approximation.

    return p;
}

reverb::ReverbEngine::Params ReverbAudioProcessor::engineParameters() const noexcept
{
    auto p = reverb::sanitiseCrossovers (currentParameters());

    // D6: substitute the synced length for the typed one. The engine is handed a plain millisecond
    // value either way and still receives preDelaySync / preDelayDivision unchanged, which is how
    // it tells a discrete grid jump (crossfade) from a continuous change (glide) -- see
    // ReverbEngine::setParameters. When sync is off, or no tempo has ever been reported, this
    // returns the typed value untouched; it never returns 0 because of sync.
    p.preDelayMs = preDelaySyncState.preDelayMsFor (p.preDelaySync,
                                                    reverb::temposync::divisionForIndex (p.preDelayDivision),
                                                    p.preDelayMs);

    return p;
}

//==================================================================================================

void ReverbAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate, samplesPerBlock,
                    juce::jmax (1, getTotalNumInputChannels(), getTotalNumOutputChannels()));

    // The playhead is deliberately NOT read here: AudioPlayHead::getPosition() is documented as
    // callable only from processBlock. Whatever tempo has been seen so far still applies.
    const auto params = engineParameters();

    effectivePreDelayMs.store (params.preDelayMs, std::memory_order_relaxed);
    engine.setParameters (params);

    for (auto& level : outputLevel)
        level.store (0.0f, std::memory_order_relaxed);
}

void ReverbAudioProcessor::releaseResources()
{
}

void ReverbAudioProcessor::reset()
{
    engine.reset();

    for (auto& level : outputLevel)
        level.store (0.0f, std::memory_order_relaxed);
}

bool ReverbAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& mainIn  = layouts.getMainInputChannelSet();
    const auto& mainOut = layouts.getMainOutputChannelSet();

    if (mainIn.isDisabled() || mainOut.isDisabled())
        return false;

    if (mainIn == mainOut)
        return mainIn.size() == 1 || mainIn.size() == 2;

    // Mono in, stereo out: a stereo reverb on a mono source is the commonest way a reverb is used,
    // and it is exactly where the diffuser's left/right decorrelation earns its keep. processBlock
    // duplicates the input into the second channel so both chains run.
    return mainIn.size() == 1 && mainOut.size() == 2;
}

void ReverbAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                         juce::MidiBuffer& midiMessages)
{
    juce::ignoreUnused (midiMessages);
    juce::ScopedNoDenormals noDenormals;

    const int totalIn   = getTotalNumInputChannels();
    const int totalOut  = getTotalNumOutputChannels();
    const int numSamples = buffer.getNumSamples();

    if (numSamples <= 0)
        return;

    // Mono in, stereo out: duplicate the input so the engine runs both diffuser chains and returns
    // a genuinely stereo tail instead of folding the wet signal back down to mono.
    const bool monoToStereo = totalIn == 1 && totalOut >= 2 && buffer.getNumChannels() >= 2;

    if (monoToStereo)
        buffer.copyFrom (1, 0, buffer, 0, 0, numSamples);

    for (int c = monoToStereo ? 2 : totalIn; c < totalOut; ++c)
        buffer.clear (c, 0, numSamples);

    const int numChannels = monoToStereo ? 2 : juce::jmin (totalIn, totalOut, 2);

    if (numChannels <= 0)
        return;

    // D6: ONE playhead read per block, never per sample, and only from here.
    preDelaySyncState.updateFromPlayHead (getPlayHead());

    const auto params = engineParameters();

    effectivePreDelayMs.store (params.preDelayMs, std::memory_order_relaxed);

    engine.setParameters (params);
    engine.process (buffer.getArrayOfWritePointers(), numChannels, numSamples);

    // D5: mirrored off the audio thread for the GUI indicator. Once per block, no lock.
    duckReductionDb.store (engine.getDuckReductionDb(), std::memory_order_relaxed);

    // Peak hold with a ~10 dB/s release, computed once per block so a 30 Hz GUI timer sees a
    // usable value without the audio thread ever taking a lock.
    const double sampleRate    = getSampleRate();
    const double blockSeconds  = sampleRate > 0.0 ? static_cast<double> (numSamples) / sampleRate
                                                  : 0.0;
    const float  releaseFactor = static_cast<float> (std::pow (10.0, -0.5 * blockSeconds));

    for (int c = 0; c < 2; ++c)
    {
        const int  source = juce::jmin (c, numChannels - 1);
        const float peak  = buffer.getMagnitude (source, 0, numSamples);
        const float held  = outputLevel[static_cast<size_t> (c)].load (std::memory_order_relaxed)
                            * releaseFactor;

        outputLevel[static_cast<size_t> (c)].store (std::max (peak, held), std::memory_order_relaxed);
    }
}

float ReverbAudioProcessor::getOutputLevel (int channel) const noexcept
{
    if (channel < 0 || channel > 1)
        return 0.0f;

    return outputLevel[static_cast<size_t> (channel)].load (std::memory_order_relaxed);
}

float ReverbAudioProcessor::getDuckReductionDb() const noexcept
{
    return duckReductionDb.load (std::memory_order_relaxed);
}

float ReverbAudioProcessor::getEffectivePreDelayMs() const noexcept
{
    return effectivePreDelayMs.load (std::memory_order_relaxed);
}

//==================================================================================================

juce::AudioProcessorEditor* ReverbAudioProcessor::createEditor()
{
    return new ReverbAudioProcessorEditor (*this);
}

bool ReverbAudioProcessor::hasEditor() const                { return true; }

const juce::String ReverbAudioProcessor::getName() const    { return JucePlugin_Name; }

bool ReverbAudioProcessor::acceptsMidi() const              { return false; }
bool ReverbAudioProcessor::producesMidi() const             { return false; }
bool ReverbAudioProcessor::isMidiEffect() const             { return false; }

double ReverbAudioProcessor::getTailLengthSeconds() const
{
    // A frozen tail is infinite by design. Reporting decay + pre-delay would let the host stop
    // pulling audio and cut it off, so claim a length no host will truncate in practice.
    if (freezeParam != nullptr && freezeParam->load (std::memory_order_relaxed) > 0.5f)
        return frozenTailSeconds;

    const float decay    = decayParam    != nullptr ? decayParam->load (std::memory_order_relaxed)    : 2.5f;
    const float preDelay = preDelayParam != nullptr ? preDelayParam->load (std::memory_order_relaxed) : 10.0f;

    return static_cast<double> (decay) + static_cast<double> (preDelay) * 0.001;
}

//==================================================================================================
// Programs are the preset table, so Logic lists them as AU factory presets.
//==================================================================================================

juce::StringArray ReverbAudioProcessor::getPresetNames()
{
    return reverb::param::presetNames();
}

void ReverbAudioProcessor::applyPreset (int index)
{
    if (index < 0 || index >= reverb::param::numPresets())
        return;

    reverb::param::applyPreset (apvts, index);

    currentProgram = index;

    // Stored on the state tree, not just in the member, so a reopened session shows the right name
    // in the host's own preset menu instead of falling back to "Default".
    apvts.state.setProperty (programProperty, index, nullptr);

    updateHostDisplay();
}

int ReverbAudioProcessor::getNumPrograms()      { return reverb::param::numPresets(); }
int ReverbAudioProcessor::getCurrentProgram()   { return currentProgram; }

void ReverbAudioProcessor::setCurrentProgram (int index)
{
    if (index < 0 || index >= reverb::param::numPresets())
        return;

    applyPreset (index);
}

const juce::String ReverbAudioProcessor::getProgramName (int index)
{
    return reverb::param::presetName (index);
}

void ReverbAudioProcessor::changeProgramName (int index, const juce::String& newName)
{
    // The preset table is fixed; hosts that offer renaming get a no-op.
    juce::ignoreUnused (index, newName);
}

//==================================================================================================

void ReverbAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    const auto state = apvts.copyState();

    if (auto xml = std::unique_ptr<juce::XmlElement> (state.createXml()))
        copyXmlToBinary (*xml, destData);
}

void ReverbAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes <= 0)
        return;

    // Anything unrecognised is ignored rather than trusted: hosts do hand over garbage.
    auto xml = std::unique_ptr<juce::XmlElement> (getXmlFromBinary (data, sizeInBytes));

    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    const auto tree = juce::ValueTree::fromXml (*xml);

    if (! tree.isValid())
        return;

    // The root tag alone is not enough. replaceState() on a document carrying no recognised
    // parameters makes APVTS rebuild every child from scratch, and appending a child with no
    // "value" property resets that parameter to its default - so a stale or foreign document that
    // merely shares our root tag would silently wipe every setting the user had.
    if (! containsKnownParameter (apvts, tree))
        return;

    // Captured BEFORE replaceState(): see resetParametersMissingFrom()'s doc comment. `tree` and
    // apvts.state become the same shared ValueTree the moment replaceState() runs, so this is the
    // only point at which "what did the incoming document specify" can still be asked honestly.
    const auto specified = whichParametersAreSpecified (tree);

    apvts.replaceState (tree);

    // PLAN-R2 3.3: any parameter the incoming document did not specify is reset to its own
    // default rather than left at whatever the previously-loaded preset set it to (C14).
    resetParametersMissingFrom (apvts, specified);

    // Re-stamped unconditionally: whatever version (if any) `tree` carried, the state now active
    // in this build IS the current layout.
    apvts.state.setProperty (stateVersionProperty, reverb::param::currentStateVersion, nullptr);

    const int storedProgram = static_cast<int> (apvts.state.getProperty (programProperty,
                                                                         currentProgram));
    if (storedProgram >= 0 && storedProgram < reverb::param::numPresets())
        currentProgram = storedProgram;
}

//==================================================================================================

void ReverbAudioProcessor::saveEditorSize (int w, int h)
{
    apvts.state.setProperty (editorWidthProperty,  juce::jlimit (200, 8000, w), nullptr);
    apvts.state.setProperty (editorHeightProperty, juce::jlimit (200, 8000, h), nullptr);
}

int ReverbAudioProcessor::getSavedEditorWidth() const noexcept
{
    const int stored = static_cast<int> (apvts.state.getProperty (editorWidthProperty,
                                                                  defaultEditorWidth));
    return stored >= 200 && stored <= 8000 ? stored : defaultEditorWidth;
}

int ReverbAudioProcessor::getSavedEditorHeight() const noexcept
{
    const int stored = static_cast<int> (apvts.state.getProperty (editorHeightProperty,
                                                                  defaultEditorHeight));
    return stored >= 200 && stored <= 8000 ? stored : defaultEditorHeight;
}

//==================================================================================================

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ReverbAudioProcessor();
}
