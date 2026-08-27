#include "KnobComponent.h"
#include "ReverbLookAndFeel.h"

KnobComponent::KnobComponent (juce::AudioProcessorValueTreeState& apvts,
                              const juce::String& parameterID,
                              const juce::String& caption)
    : boundParameterID (parameterID)
{
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 68, 18);
    slider.setVelocityBasedMode (false);
    addAndMakeVisible (slider);

    captionLabel.setText (caption, juce::dontSendNotification);
    captionLabel.setJustificationType (juce::Justification::centred);
    captionLabel.setColour (juce::Label::textColourId, ReverbLookAndFeel::colourTextDim);
    captionLabel.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (captionLabel);

    // Verified (juce_ParameterAttachments.cpp, SliderParameterAttachment
    // constructor): the attachment wires slider.textFromValueFunction to
    // the parameter's own getText(), and calls
    // slider.setDoubleClickReturnValue (true, defaultValue) itself. Neither
    // needs to be duplicated here.
    attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        apvts, parameterID, slider);

    // Every float parameter supplies its own unit via stringFromValue (see
    // PluginProcessor's parameter layout), so no suffix is set here.
}

KnobComponent::~KnobComponent() = default;

void KnobComponent::resized()
{
    auto bounds = getLocalBounds();
    auto captionHeight = juce::jmax (14, bounds.getHeight() / 8);

    // Caption font now actually reaches the screen (ReverbLookAndFeel::
    // getLabelFont no longer discards it), so it needs to be set explicitly
    // and scaled with the cell rather than relying on Label's own default.
    captionLabel.setFont (ReverbLookAndFeel::getScaledFont (juce::jmin (13.0f, captionHeight * 0.62f)));
    captionLabel.setBounds (bounds.removeFromTop (captionHeight));
    slider.setBounds (bounds);
}
