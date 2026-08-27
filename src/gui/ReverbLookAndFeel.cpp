#include "ReverbLookAndFeel.h"

const juce::Colour ReverbLookAndFeel::colourBackground   { 0xff121316 };
const juce::Colour ReverbLookAndFeel::colourPanel        { 0xff1b1d21 };
const juce::Colour ReverbLookAndFeel::colourPanelBorder  { 0xff2c2e33 };
const juce::Colour ReverbLookAndFeel::colourAccent       { 0xffd79a4b };
const juce::Colour ReverbLookAndFeel::colourAccentDim    { 0xff5c4a30 };
const juce::Colour ReverbLookAndFeel::colourText         { 0xffe8e6e1 };
const juce::Colour ReverbLookAndFeel::colourTextDim      { 0xff8b8b90 };
const juce::Colour ReverbLookAndFeel::colourMeterGreen   { 0xff4caf6e };
const juce::Colour ReverbLookAndFeel::colourMeterAmber   { 0xffe0a83c };
const juce::Colour ReverbLookAndFeel::colourMeterRed     { 0xffe0524a };

ReverbLookAndFeel::ReverbLookAndFeel()
{
    setColour (juce::ResizableWindow::backgroundColourId, colourBackground);

    setColour (juce::Slider::rotarySliderFillColourId, colourAccent);
    setColour (juce::Slider::rotarySliderOutlineColourId, colourPanelBorder);
    setColour (juce::Slider::thumbColourId, colourAccent);
    setColour (juce::Slider::textBoxTextColourId, colourText);
    setColour (juce::Slider::textBoxBackgroundColourId, colourPanel);
    setColour (juce::Slider::textBoxOutlineColourId, colourPanelBorder);

    setColour (juce::Label::textColourId, colourTextDim);

    setColour (juce::ComboBox::backgroundColourId, colourPanel);
    setColour (juce::ComboBox::textColourId, colourText);
    setColour (juce::ComboBox::outlineColourId, colourPanelBorder);
    setColour (juce::ComboBox::buttonColourId, colourPanel);
    setColour (juce::ComboBox::arrowColourId, colourTextDim);

    setColour (juce::PopupMenu::backgroundColourId, colourPanel);
    setColour (juce::PopupMenu::textColourId, colourText);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, colourAccentDim);
    setColour (juce::PopupMenu::highlightedTextColourId, colourText);

    setColour (juce::TextButton::buttonColourId, colourPanel);
    setColour (juce::TextButton::textColourOffId, colourTextDim);
    setColour (juce::TextButton::textColourOnId, colourText);

    setColour (juce::ToggleButton::textColourId, colourText);
}

juce::Font ReverbLookAndFeel::getScaledFont (float height, bool bold)
{
    return juce::Font (juce::FontOptions (height, bold ? juce::Font::bold : juce::Font::plain));
}

int ReverbLookAndFeel::panelTitleHeight (int panelHeight) noexcept
{
    return juce::jmax (18, panelHeight / 6);
}

juce::Rectangle<float> ReverbLookAndFeel::drawPanelChrome (juce::Graphics& g, juce::Rectangle<int> bounds,
                                                           const juce::String& title)
{
    auto floatBounds = bounds.toFloat();

    g.setColour (colourPanel);
    g.fillRoundedRectangle (floatBounds, 8.0f);
    g.setColour (colourPanelBorder);
    g.drawRoundedRectangle (floatBounds.reduced (0.5f), 8.0f, 1.0f);

    auto titleHeight = (float) panelTitleHeight (bounds.getHeight());
    auto titleArea = floatBounds.removeFromTop (titleHeight).reduced (10.0f, 0.0f);

    g.setColour (colourTextDim);
    g.setFont (getScaledFont (juce::jmin (14.0f, titleHeight * 0.55f), true));
    g.drawText (title, titleArea, juce::Justification::centredLeft, false);

    return floatBounds;
}

juce::Font ReverbLookAndFeel::getLabelFont (juce::Label& label)
{
    // Honour whatever font the label was actually given (LookAndFeel_V2's
    // default is label.getFont()); a hard-coded size here would silently
    // discard every setFont() call made by the editor and its child
    // components, including the title and every scaled caption.
    return label.getFont();
}

juce::Font ReverbLookAndFeel::getComboBoxFont (juce::ComboBox&)
{
    return getScaledFont (15.0f);
}

juce::Font ReverbLookAndFeel::getTextButtonFont (juce::TextButton&, int buttonHeight)
{
    return getScaledFont (juce::jmin (16.0f, (float) buttonHeight * 0.6f), true);
}

void ReverbLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                          float sliderPosProportional, float rotaryStartAngle,
                                          float rotaryEndAngle, juce::Slider& slider)
{
    auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (4.0f);
    auto radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) / 2.0f;
    auto centreX = bounds.getCentreX();
    auto centreY = bounds.getCentreY();

    auto trackThickness = juce::jmax (2.0f, radius * 0.09f);
    auto valueThickness  = juce::jmax (3.0f, radius * 0.16f);
    auto arcRadius = radius - valueThickness * 0.5f - 1.0f;

    auto toAngle = rotaryStartAngle + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);

    // Thin unfilled track spanning the whole travel.
    juce::Path track;
    track.addCentredArc (centreX, centreY, arcRadius, arcRadius, 0.0f,
                          rotaryStartAngle, rotaryEndAngle, true);
    g.setColour (slider.findColour (juce::Slider::rotarySliderOutlineColourId));
    g.strokePath (track, juce::PathStrokeType (trackThickness, juce::PathStrokeType::curved,
                                                juce::PathStrokeType::rounded));

    // Thicker filled arc from the start of travel to the current value.
    if (slider.isEnabled())
    {
        juce::Path valueArc;
        valueArc.addCentredArc (centreX, centreY, arcRadius, arcRadius, 0.0f,
                                 rotaryStartAngle, toAngle, true);
        g.setColour (slider.findColour (juce::Slider::rotarySliderFillColourId));
        g.strokePath (valueArc, juce::PathStrokeType (valueThickness, juce::PathStrokeType::curved,
                                                       juce::PathStrokeType::rounded));
    }

    // Pointer line from the centre out toward the current angle.
    juce::Path pointer;
    auto pointerLength = radius * 0.62f;
    auto pointerThickness = juce::jmax (1.6f, radius * 0.05f);
    pointer.startNewSubPath (centreX, centreY);
    pointer.lineTo (centreX + pointerLength * std::sin (toAngle),
                     centreY - pointerLength * std::cos (toAngle));
    g.setColour (slider.isEnabled() ? colourText : colourTextDim);
    g.strokePath (pointer, juce::PathStrokeType (pointerThickness, juce::PathStrokeType::curved,
                                                  juce::PathStrokeType::rounded));
}

void ReverbLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button,
                                          bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
{
    juce::ignoreUnused (shouldDrawButtonAsDown);

    auto bounds = button.getLocalBounds().toFloat().reduced (1.0f);
    auto cornerSize = juce::jmin (6.0f, bounds.getHeight() * 0.25f);
    bool isOn = button.getToggleState();

    if (isOn)
    {
        g.setColour (shouldDrawButtonAsHighlighted ? colourAccent.brighter (0.15f) : colourAccent);
        g.fillRoundedRectangle (bounds, cornerSize);
    }
    else
    {
        g.setColour (shouldDrawButtonAsHighlighted ? colourPanel.brighter (0.1f) : colourPanel);
        g.fillRoundedRectangle (bounds, cornerSize);
        g.setColour (colourPanelBorder);
        g.drawRoundedRectangle (bounds, cornerSize, 1.2f);
    }

    g.setColour (isOn ? colourBackground : colourTextDim);
    g.setFont (getScaledFont (juce::jmin (15.0f, bounds.getHeight() * 0.5f), true));
    g.drawText (button.getButtonText(), bounds, juce::Justification::centred, false);
}

void ReverbLookAndFeel::drawLabel (juce::Graphics& g, juce::Label& label)
{
    g.fillAll (label.findColour (juce::Label::backgroundColourId));

    if (! label.isBeingEdited())
    {
        auto alpha = label.isEnabled() ? 1.0f : 0.5f;
        auto font = getLabelFont (label);

        g.setColour (label.findColour (juce::Label::textColourId).withMultipliedAlpha (alpha));
        g.setFont (font);

        auto textArea = getLabelBorderSize (label).subtractedFrom (label.getLocalBounds());

        g.drawFittedText (label.getText(), textArea, label.getJustificationType(),
                           juce::jmax (1, (int) ((float) textArea.getHeight() / font.getHeight())),
                           label.getMinimumHorizontalScale());
    }
}
