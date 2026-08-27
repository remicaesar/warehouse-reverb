#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

// A caption + rotary slider + value textbox, bound to one APVTS parameter.
// Owns the SliderAttachment that keeps it in sync with the parameter, so
// host automation, undo and gesture begin/end all go through JUCE's own
// plumbing rather than anything this class does itself.
class KnobComponent : public juce::Component
{
public:
    KnobComponent (juce::AudioProcessorValueTreeState& apvts,
                   const juce::String& parameterID,
                   const juce::String& caption);
    ~KnobComponent() override;

    void resized() override;

    juce::Slider& getSlider() noexcept { return slider; }

    // The APVTS parameter ID this knob is bound to, so a page can find "its" Pre-Delay knob (or
    // any other) by ID rather than by a fragile positional index into the panel's knob list.
    const juce::String& getParameterID() const noexcept { return boundParameterID; }

private:
    juce::String boundParameterID;
    juce::Slider slider;
    juce::Label captionLabel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KnobComponent)
};
