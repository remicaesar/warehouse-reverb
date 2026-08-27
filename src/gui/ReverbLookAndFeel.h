#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// Dark, single-accent look and feel for the Reverb plugin. All colours are
// named constants here so every custom-drawn component (knobs, meters,
// panels, the freeze button) pulls from the same palette.
class ReverbLookAndFeel : public juce::LookAndFeel_V4
{
public:
    // Palette -----------------------------------------------------------
    static const juce::Colour colourBackground;   // window background, near-black
    static const juce::Colour colourPanel;        // section panel fill
    static const juce::Colour colourPanelBorder;  // section panel outline
    static const juce::Colour colourAccent;       // value arcs, freeze-on state
    static const juce::Colour colourAccentDim;    // unfilled/inactive accent uses
    static const juce::Colour colourText;         // primary text
    static const juce::Colour colourTextDim;      // captions, secondary text
    static const juce::Colour colourMeterGreen;
    static const juce::Colour colourMeterAmber;
    static const juce::Colour colourMeterRed;

    ReverbLookAndFeel();

    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                            float sliderPosProportional, float rotaryStartAngle,
                            float rotaryEndAngle, juce::Slider&) override;

    void drawToggleButton (juce::Graphics&, juce::ToggleButton&,
                            bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

    void drawLabel (juce::Graphics&, juce::Label&) override;

    juce::Font getLabelFont (juce::Label&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;

    // Shared font helper so every custom-drawn component (panel titles,
    // header, meters) asks for the same construction path rather than each
    // building its own juce::Font.
    static juce::Font getScaledFont (float height, bool bold = false);

    // The rounded dark panel + left-justified title band every section panel
    // and the decay curve display share. Extracted so PageStrip's
    // SectionPanel and DecayCurveDisplay draw byte-identical chrome instead
    // of each reimplementing it (was duplicated inline in
    // PluginEditor::SectionPanel::paint before step 3B). Returns the
    // interior area below the title band, in case the caller wants it.
    static juce::Rectangle<float> drawPanelChrome (juce::Graphics&, juce::Rectangle<int> bounds,
                                                    const juce::String& title);

    // The title band height drawPanelChrome() reserves, exposed so resized()
    // can lay out interior content at the same offset paint() draws it at
    // without duplicating the "18 px or a sixth of the height" rule.
    static int panelTitleHeight (int panelHeight) noexcept;
};
