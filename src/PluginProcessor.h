#pragma once

#include "dsp/ReverbEngine.h"
#include "dsp/TempoSync.h"

#include <array>
#include <atomic>
#include <juce_audio_processors/juce_audio_processors.h>

//==================================================================================================
/** Algorithmic reverb plugin.

    Parameter IDs, ranges and defaults are the contract with the editor; see createParameterLayout().
*/
class ReverbAudioProcessor : public juce::AudioProcessor
{
public:
    ReverbAudioProcessor();
    ~ReverbAudioProcessor() override;

    //==============================================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void reset() override;

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override;

    //==============================================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;

    bool   acceptsMidi() const override;
    bool   producesMidi() const override;
    bool   isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    //==============================================================================================
    int  getNumPrograms() override;
    int  getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==============================================================================================
    // GUI-facing API.
    juce::AudioProcessorValueTreeState apvts;

    /** Preset names, in the same order as the index taken by applyPreset(). */
    static juce::StringArray getPresetNames();

    /** Writes a preset into the parameters, notifying the host so automation follows. */
    void applyPreset (int index);

    /** Post-mix output peak for channel 0 or 1, linear, peak-hold with a ~10 dB/s release. */
    float getOutputLevel (int channel) const noexcept;

    /** Duck gain reduction in dB, negative for a reduction, mirrored off the audio thread once per
        block (PLAN-R2 D5, for the GUI's duck indicator). */
    float getDuckReductionDb() const noexcept;

    /** The pre-delay the engine is actually using, in ms: the synced division's length when Pre-Delay
        Sync is on and a tempo is known, otherwise the `predelay` parameter. Already CLAMPED to
        0..500 ms, which is the value D6 requires the GUI to show. */
    float getEffectivePreDelayMs() const noexcept;

    int  getSavedEditorWidth()  const noexcept;
    int  getSavedEditorHeight() const noexcept;
    void saveEditorSize (int w, int h);

private:
    //==============================================================================================
    /** Snapshot of the current parameter values, ready for the engine. */
    reverb::ReverbEngine::Params currentParameters() const noexcept;

    /** currentParameters() plus the two things that must happen between the APVTS and the engine:
        the D1a crossover clamp, and D6's pre-delay resolution against the last known tempo. */
    reverb::ReverbEngine::Params engineParameters() const noexcept;

    std::atomic<float>* raw (const char* id) const noexcept;

    reverb::ReverbEngine engine;

    // D6: the only reader of the playhead, updated once per processBlock. Held here rather than in
    // the engine so the tempo never enters the DSP -- see TempoSync.h.
    reverb::temposync::PreDelaySync preDelaySyncState;

    // ---- the 13 existing parameters (PLAN-R2 3.1) ----
    std::atomic<float>* mixParam       = nullptr;
    std::atomic<float>* preDelayParam  = nullptr;
    std::atomic<float>* sizeParam      = nullptr;
    std::atomic<float>* decayParam     = nullptr;
    std::atomic<float>* dampingParam   = nullptr;   // D1a: Decay EQ high crossover, see Parameters.h
    std::atomic<float>* lowCutParam    = nullptr;   // D1a: Decay EQ low crossover, see Parameters.h
    std::atomic<float>* bandwidthParam = nullptr;
    std::atomic<float>* diffusionParam = nullptr;
    std::atomic<float>* modDepthParam  = nullptr;
    std::atomic<float>* modRateParam   = nullptr;
    std::atomic<float>* widthParam     = nullptr;
    std::atomic<float>* freezeParam    = nullptr;
    std::atomic<float>* outputParam    = nullptr;

    // ---- Phase 1, 6 new parameters (PLAN-R2 3.2) ----
    std::atomic<float>* decayLowParam  = nullptr;
    std::atomic<float>* decayHighParam = nullptr;
    std::atomic<float>* erMixParam     = nullptr;
    std::atomic<float>* erSizeParam    = nullptr;
    std::atomic<float>* erSpreadParam  = nullptr;
    std::atomic<float>* densityParam   = nullptr;

    // ---- Phase 2, 10 new parameters ----
    std::atomic<float>* duckAmountParam      = nullptr;
    std::atomic<float>* duckThreshParam      = nullptr;
    std::atomic<float>* duckReleaseParam     = nullptr;
    std::atomic<float>* duckCharParam        = nullptr;
    std::atomic<float>* preDelaySyncParam    = nullptr;
    std::atomic<float>* preDelayDivParam     = nullptr;
    std::atomic<float>* bassMonoParam        = nullptr;
    std::atomic<float>* wetLowCutParam       = nullptr;
    std::atomic<float>* wetHighCutParam      = nullptr;
    std::atomic<float>* wetTiltParam         = nullptr;

    // Written from processBlock, read from the editor's timer. Never locked.
    std::array<std::atomic<float>, 2> outputLevel;
    std::atomic<float> duckReductionDb { 0.0f };
    std::atomic<float> effectivePreDelayMs { 10.0f };

    int currentProgram = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReverbAudioProcessor)
};
