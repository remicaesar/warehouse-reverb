#include "PageStrip.h"
#include "ReverbLookAndFeel.h"

#include "../Parameters.h"

namespace
{
    struct KnobSpec
    {
        juce::String parameterID;
        juce::String caption;
    };

    std::vector<std::unique_ptr<KnobComponent>> makeKnobs (juce::AudioProcessorValueTreeState& apvts,
                                                            std::initializer_list<KnobSpec> specs)
    {
        std::vector<std::unique_ptr<KnobComponent>> result;
        result.reserve (specs.size());

        for (auto& spec : specs)
            result.push_back (std::make_unique<KnobComponent> (apvts, spec.parameterID, spec.caption));

        return result;
    }

    // Moved verbatim from PluginEditor.cpp (pre-3B) -- see its own comments there for the
    // reasoning; PageStrip's SectionPanel is the same panel/knob-grid machinery PLAN-R2 section
    // 8 says to keep and reuse per page.
    constexpr int knobVerticalOverhead = 40;
    constexpr int minKnobCellWidth  = 60;
    constexpr int minKnobCellHeight = 70;

    int chooseKnobColumns (int numItems, int width, int height)
    {
        auto best = 1;
        auto bestScore = -1;

        for (auto columns = 1; columns <= numItems; ++columns)
        {
            auto rows = (numItems + columns - 1) / columns;
            auto cellWidth  = width / columns;
            auto cellHeight = height / rows;

            if (cellWidth < minKnobCellWidth || cellHeight < minKnobCellHeight)
                continue;

            auto score = juce::jmin (cellWidth, cellHeight - knobVerticalOverhead);

            if (score > bestScore)
            {
                bestScore = score;
                best = columns;
            }
        }

        if (bestScore >= 0)
            return best;

        for (auto columns = 1; columns <= numItems; ++columns)
        {
            auto rows = (numItems + columns - 1) / columns;
            auto cellWidth  = width / columns;
            auto cellHeight = height / rows;
            auto score = juce::jmin (cellWidth, cellHeight - knobVerticalOverhead);

            if (score > bestScore)
            {
                bestScore = score;
                best = columns;
            }
        }

        return best;
    }

    // ComboBoxAttachment does not populate the combo's items itself [verified,
    // AudioProcessorValueTreeState::ComboBoxAttachment's constructor just forwards to
    // ComboBoxParameterAttachment, juce_AudioProcessorValueTreeState.cpp:500-505] -- read the
    // choices from the live AudioParameterChoice instead of duplicating Parameters.cpp's private
    // choice tables here, so the two can never drift apart.
    void populateChoiceCombo (juce::AudioProcessorValueTreeState& apvts, const juce::String& parameterID,
                              juce::ComboBox& combo)
    {
        if (auto* choiceParam = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter (parameterID)))
            combo.addItemList (choiceParam->choices, 1);   // ComboBox item IDs are 1-based
    }
}

//==============================================================================
// SectionPanel
//==============================================================================
PageStrip::SectionPanel::SectionPanel (juce::String title, std::vector<std::unique_ptr<KnobComponent>> knobsIn)
    : panelTitle (std::move (title)), knobs (std::move (knobsIn))
{
    for (auto& knob : knobs)
        addAndMakeVisible (*knob);
}

KnobComponent* PageStrip::SectionPanel::findKnob (const juce::String& parameterID) const noexcept
{
    for (auto& knob : knobs)
        if (knob->getParameterID() == parameterID)
            return knob.get();

    return nullptr;
}

void PageStrip::SectionPanel::setExtraComponents (std::vector<juce::Component*> extras)
{
    extraComponents = std::move (extras);

    for (auto* extra : extraComponents)
        if (extra != nullptr)
            addAndMakeVisible (*extra);

    resized();
}

void PageStrip::SectionPanel::paint (juce::Graphics& g)
{
    ReverbLookAndFeel::drawPanelChrome (g, getLocalBounds(), panelTitle);
}

void PageStrip::SectionPanel::resized()
{
    auto bounds = getLocalBounds();
    bounds.removeFromTop (ReverbLookAndFeel::panelTitleHeight (bounds.getHeight()));
    bounds.reduce (8, 6);

    // The extra components (SYNC+DIVISION in SPACE, SHIMMER+FREEZE in MOVE, DUCK CHARACTER in
    // MIX) share one full-width strip at the bottom, split evenly, rather than being squeezed
    // into the knob grid as extra, narrower cells.
    if (! extraComponents.empty())
    {
        auto extraHeight = juce::jmax (28, bounds.getHeight() / 5);
        auto extraArea = bounds.removeFromBottom (extraHeight);
        auto count = (int) extraComponents.size();
        auto cellWidth = extraArea.getWidth() / count;

        for (int i = 0; i < count; ++i)
        {
            // Last cell absorbs the integer-division remainder rather than leaving a sliver
            // unused on the right.
            auto width = (i == count - 1) ? extraArea.getWidth() : cellWidth;
            extraComponents[(size_t) i]->setBounds (extraArea.removeFromLeft (width).reduced (4, 3));
        }

        bounds.removeFromBottom (4);
    }

    auto numKnobs = (int) knobs.size();

    if (numKnobs == 0)
        return;

    auto columns = chooseKnobColumns (numKnobs, bounds.getWidth(), bounds.getHeight());
    auto rows = (numKnobs + columns - 1) / columns;

    auto cellWidth  = bounds.getWidth() / columns;
    auto cellHeight = bounds.getHeight() / rows;

    for (int i = 0; i < numKnobs; ++i)
    {
        auto column = i % columns;
        auto row = i / columns;
        knobs[(size_t) i]->setBounds (bounds.getX() + column * cellWidth,
                                       bounds.getY() + row * cellHeight,
                                       cellWidth, cellHeight);
    }
}

//==============================================================================
// PageStrip
//==============================================================================
PageStrip::PageStrip (juce::AudioProcessorValueTreeState& apvts)
    : spacePanel ("SPACE", makeKnobs (apvts, { { reverb::param::idSize,      "Size" },
                                                { reverb::param::idDecay,     "Decay" },
                                                { reverb::param::idPreDelay,  "Pre-Delay" },
                                                { reverb::param::idErMix,     "Early / Late" },
                                                { reverb::param::idErSize,    "ER Size" },
                                                { reverb::param::idErSpread,  "ER Spread" },
                                                { reverb::param::idDensity,   "Density" } })),
      tonePanel ("TONE", makeKnobs (apvts, { { reverb::param::idBandwidth,   "Bandwidth" },
                                              { reverb::param::idDiffusion,  "Diffusion" },
                                              { reverb::param::idWetLowCut,  "Wet Low Cut" },
                                              { reverb::param::idWetHighCut, "Wet High Cut" },
                                              { reverb::param::idWetTilt,    "Wet Tilt" } })),
      movePanel ("MOVE", makeKnobs (apvts, { { reverb::param::idModDepth,   "Mod Depth" },
                                              { reverb::param::idModRate,    "Mod Rate" } })),
      mixPanel ("MIX", makeKnobs (apvts, { { reverb::param::idMix,          "Mix" },
                                            { reverb::param::idWidth,        "Width" },
                                            { reverb::param::idOutput,       "Output" },
                                            { reverb::param::idBassMono,     "Bass Mono" },
                                            { reverb::param::idDuckAmount,   "Duck Amount" },
                                            { reverb::param::idDuckThresh,   "Duck Threshold" },
                                            { reverb::param::idDuckRelease,  "Duck Release" } })),
      // Read-only: never writes idPreDelaySync (preDelaySyncAttachment, set up below, owns that),
      // only reacts to it. spacePanel above is already fully constructed by the time this runs
      // (declaration order), but the callback itself only ever fires after sendInitialUpdate() is
      // called explicitly in the constructor body below, by which point preDelayKnob has been
      // resolved -- see the "SPACE extras" block.
      preDelaySyncListener (*apvts.getParameter (reverb::param::idPreDelaySync),
                            [this] (float value)
                            {
                                preDelaySyncOn = value > 0.5f;

                                if (preDelayKnob != nullptr)
                                    preDelayKnob->getSlider().setEnabled (! preDelaySyncOn);

                                updateEffectivePreDelayLabel();
                            })
{
    for (auto* tab : { &spaceTab, &toneTab, &moveTab, &mixTab })
        addAndMakeVisible (*tab);

    spaceTab.onClick = [this] { setPage (Page::space); };
    toneTab.onClick  = [this] { setPage (Page::tone); };
    moveTab.onClick  = [this] { setPage (Page::move); };
    mixTab.onClick   = [this] { setPage (Page::mix); };

    addAndMakeVisible (spacePanel);
    addAndMakeVisible (tonePanel);
    addAndMakeVisible (movePanel);
    addAndMakeVisible (mixPanel);

    // ---- SPACE extras: Pre-Delay Sync + Division (PLAN-R2 D6) ----
    preDelaySyncButton.setButtonText ("SYNC");
    preDelaySyncButton.setTooltip ("When on, pre-delay follows the host tempo and the Division "
                                    "below instead of the Pre-Delay knob, clamped to 500 ms. Falls "
                                    "back to the knob's free ms value if the host reports no tempo. "
                                    "The readout to the right always shows which one is actually "
                                    "in force.");
    preDelaySyncAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        apvts, reverb::param::idPreDelaySync, preDelaySyncButton);

    populateChoiceCombo (apvts, reverb::param::idPreDelayDiv, preDelayDivBox);
    preDelayDivAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
        apvts, reverb::param::idPreDelayDiv, preDelayDivBox);

    // The Pre-Delay knob shows the free ms parameter and nothing else -- it cannot know whether
    // Sync is currently overriding it. This readout is the one place that always reflects
    // ReverbAudioProcessor::getEffectivePreDelayMs(), i.e. what the engine is actually using, so
    // the knob's own number is never mistaken for live when it is not (D6's honesty requirement).
    effectivePreDelayLabel.setJustificationType (juce::Justification::centred);
    effectivePreDelayLabel.setColour (juce::Label::textColourId, ReverbLookAndFeel::colourTextDim);
    effectivePreDelayLabel.setFont (ReverbLookAndFeel::getScaledFont (11.0f));
    effectivePreDelayLabel.setMinimumHorizontalScale (0.5f);
    effectivePreDelayLabel.setTooltip ("The pre-delay the engine is actually using right now.");

    spacePanel.setExtraComponents ({ &preDelaySyncButton, &preDelayDivBox, &effectivePreDelayLabel });

    // Resolved by parameter ID (see SectionPanel::findKnob), not position, so the Pre-Delay knob
    // can be dimmed the instant Sync takes over -- must be set before preDelaySyncListener's first
    // callback (sendInitialUpdate(), below) can fire.
    preDelayKnob = spacePanel.findKnob (reverb::param::idPreDelay);
    preDelaySyncListener.sendInitialUpdate();

    // ---- MOVE extra: Freeze (moved here from the old OUTPUT panel) ----
    freezeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        apvts, reverb::param::idFreeze, freezeButton);

    movePanel.setExtraComponents ({ &freezeButton });

    // ---- MIX extra: Duck Character ----
    populateChoiceCombo (apvts, reverb::param::idDuckChar, duckCharBox);
    duckCharAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
        apvts, reverb::param::idDuckChar, duckCharBox);

    mixPanel.setExtraComponents ({ &duckCharBox });

    // ---- TONE tooltips: three controls touch high frequencies and each does exactly one thing
    // (measured, not just phrased differently) -- Bandwidth sets the tail's initial timbre,
    // High Decay (the Decay Curve's own handle, not a knob here) sets how fast the top dies,
    // Wet High Cut sets the wet tone. Conflating these is the most likely support question this
    // plugin generates, so each tooltip below names the other two by where to find them.
    if (auto* bandwidthKnob = tonePanel.findKnob (reverb::param::idBandwidth))
        bandwidthKnob->getSlider().setTooltip (
            "Sets the tail's initial timbre: a fixed low-pass at time zero. For how fast the top "
            "of the tail decays over time, see High Decay on the Decay Curve; for the wet "
            "signal's overall tone, see Wet High Cut.");

    if (auto* wetHighCutKnob = tonePanel.findKnob (reverb::param::idWetHighCut))
        wetHighCutKnob->getSlider().setTooltip (
            "A static low-pass on the wet signal's tone. Does not change decay time -- see High "
            "Decay on the Decay Curve for that, or Bandwidth for the tail's initial timbre.");

    // Wet Tilt reads as the FULL top-to-bottom tilt, not per side: +12 dB means +6 dB of high
    // shelf boost and -6 dB of low shelf cut, pivoting at 1 kHz (the geometric mean of the two
    // Decay-EQ crossover defaults, so both EQs hinge in the same place).
    if (auto* wetTiltKnob = tonePanel.findKnob (reverb::param::idWetTilt))
        wetTiltKnob->getSlider().setTooltip (
            "Full top-to-bottom tilt, pivoting at 1 kHz. +12 dB means +6 dB of high shelf boost "
            "and -6 dB of low shelf cut, not +/-12 dB on each side; -12 dB is the mirror image.");

    setPage (Page::space);
}

PageStrip::~PageStrip() = default;

void PageStrip::setEffectivePreDelayMs (float ms) noexcept
{
    lastEffectivePreDelayMs = ms;
    updateEffectivePreDelayLabel();
}

void PageStrip::updateEffectivePreDelayLabel()
{
    // "Synced" / "Free" names which parameter is actually driving the number that follows, rather
    // than making the reader infer it from the SYNC toggle alone -- the toggle can be on while the
    // engine is still using the free ms value (no host tempo), and this always names the one that
    // is really in force (ReverbAudioProcessor::getEffectivePreDelayMs()'s own contract).
    auto text = juce::String (preDelaySyncOn ? "Synced: " : "Free: ")
              + juce::String (lastEffectivePreDelayMs, 1) + " ms";

    effectivePreDelayLabel.setText (text, juce::dontSendNotification);
}

void PageStrip::setPage (Page page)
{
    currentPage = page;

    spacePanel.setVisible (page == Page::space);
    tonePanel.setVisible  (page == Page::tone);
    movePanel.setVisible  (page == Page::move);
    mixPanel.setVisible   (page == Page::mix);

    updateTabColours();
}

void PageStrip::updateTabColours()
{
    auto style = [this] (juce::TextButton& button, Page page)
    {
        auto selected = currentPage == page;
        button.setColour (juce::TextButton::buttonColourId,
                          selected ? ReverbLookAndFeel::colourAccent : ReverbLookAndFeel::colourPanel);
        button.setColour (juce::TextButton::textColourOffId,
                          selected ? ReverbLookAndFeel::colourBackground : ReverbLookAndFeel::colourTextDim);
    };

    style (spaceTab, Page::space);
    style (toneTab, Page::tone);
    style (moveTab, Page::move);
    style (mixTab, Page::mix);
}

void PageStrip::resized()
{
    auto bounds = getLocalBounds();

    auto tabRow = bounds.removeFromTop (juce::jmax (24, bounds.getHeight() / 14));
    auto tabWidth = tabRow.getWidth() / 4;

    spaceTab.setBounds (tabRow.removeFromLeft (tabWidth).reduced (2));
    toneTab.setBounds  (tabRow.removeFromLeft (tabWidth).reduced (2));
    moveTab.setBounds  (tabRow.removeFromLeft (tabWidth).reduced (2));
    mixTab.setBounds   (tabRow.reduced (2));

    bounds.removeFromTop (4);

    spacePanel.setBounds (bounds);
    tonePanel.setBounds (bounds);
    movePanel.setBounds (bounds);
    mixPanel.setBounds (bounds);
}
