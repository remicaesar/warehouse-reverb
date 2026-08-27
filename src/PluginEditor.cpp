#include "PluginEditor.h"
#include "Parameters.h"

//==============================================================================
// DuckMeter
//==============================================================================
void ReverbAudioProcessorEditor::DuckMeter::setReduction (float dB) noexcept
{
    reductionDb = juce::jlimit (minDb, maxDb, dB);
}

float ReverbAudioProcessorEditor::DuckMeter::dbToProportion (float db) noexcept
{
    return juce::jlimit (0.0f, 1.0f, (db - minDb) / (maxDb - minDb));
}

void ReverbAudioProcessorEditor::DuckMeter::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();

    g.setColour (ReverbLookAndFeel::colourPanel);
    g.fillRoundedRectangle (bounds, 6.0f);
    g.setColour (ReverbLookAndFeel::colourPanelBorder);
    g.drawRoundedRectangle (bounds.reduced (0.5f), 6.0f, 1.0f);

    auto label = bounds.removeFromTop (juce::jmin (14.0f, bounds.getHeight() * 0.3f));
    g.setColour (ReverbLookAndFeel::colourTextDim);
    g.setFont (ReverbLookAndFeel::getScaledFont (juce::jmin (10.0f, label.getHeight() * 0.8f), true));
    g.drawText ("GR", label, juce::Justification::centred, false);

    // Numeric readout, always drawn -- including "0.0" at rest -- so an idle meter (whose bar
    // below is entirely masked, since there is nothing to show) still reads as a live "currently
    // reading zero" indicator rather than as a component that never got wired up.
    auto readout = bounds.removeFromBottom (juce::jmin (14.0f, bounds.getHeight() * 0.22f));
    g.setColour (reductionDb < -0.05f ? ReverbLookAndFeel::colourText : ReverbLookAndFeel::colourTextDim);
    g.setFont (ReverbLookAndFeel::getScaledFont (juce::jmin (11.0f, readout.getHeight() * 0.85f), true));
    g.drawText (juce::String (reductionDb, 1), readout, juce::Justification::centred, false);

    bounds.reduce (2.0f, 1.0f);

    auto scaleArea = bounds.removeFromLeft (juce::jmin (16.0f, bounds.getWidth() * 0.4f));
    drawScale (g, scaleArea);

    drawBar (g, bounds);
}

void ReverbAudioProcessorEditor::DuckMeter::drawScale (juce::Graphics& g, juce::Rectangle<float> area) const
{
    static const float ticks[] = { 0.0f, -10.0f, -20.0f, -30.0f, -40.0f };

    g.setFont (ReverbLookAndFeel::getScaledFont (8.5f));
    g.setColour (ReverbLookAndFeel::colourTextDim);

    for (auto db : ticks)
    {
        auto centreY = area.getBottom() - dbToProportion (db) * area.getHeight();
        // Clamped inside the scale area, same reasoning as LevelMeter::drawScale: the extreme
        // ticks (0 dB at the top, -40 dB at the bottom) must not draw half off the edge.
        auto y = juce::jlimit (area.getY(), area.getBottom() - 11.0f, centreY - 5.5f);
        g.drawText (juce::String ((int) db), juce::Rectangle<float> (area.getX(), y, area.getWidth(), 11.0f),
                    juce::Justification::centredLeft, false);
    }
}

void ReverbAudioProcessorEditor::DuckMeter::drawBar (juce::Graphics& g, juce::Rectangle<float> area) const
{
    if (area.getWidth() <= 0.0f || area.getHeight() <= 0.0f)
        return;

    auto yForDb = [&] (float db)
    {
        return area.getBottom() - dbToProportion (db) * area.getHeight();
    };

    auto zone = [&] (float dbLow, float dbHigh, juce::Colour colour)
    {
        auto yLow  = yForDb (dbLow);
        auto yHigh = yForDb (dbHigh);
        g.setColour (colour);
        g.fillRect (juce::Rectangle<float> (area.getX(), yHigh, area.getWidth(), yLow - yHigh));
    };

    // Full-scale zones, drawn top (0 dB) to bottom (-40 dB) so deeper reduction reads hotter --
    // the ducker's measured range at duckamt 100 (about -32 dB Gentle .. -39 dB Pump) sits mostly
    // in the red zone, which is the point: heavy ducking should look heavy.
    zone (-10.0f, 0.0f,   ReverbLookAndFeel::colourMeterGreen);
    zone (-25.0f, -10.0f, ReverbLookAndFeel::colourMeterAmber);
    zone (minDb,  -25.0f, ReverbLookAndFeel::colourMeterRed);

    // Mask off everything below the current reduction depth so only the top-down span from 0 dB
    // to the live reading reads as lit -- mirrors LevelMeter::drawChannelBar's masking, just
    // inverted (this fills from the top down as reduction grows, not from the bottom up as level
    // grows). Idle (0 dB) masks the whole bar; the numeric readout above and the border/ticks
    // below are what keep that reading as "live at zero" rather than "not wired up".
    auto litBottom = yForDb (reductionDb);
    g.setColour (ReverbLookAndFeel::colourPanel);
    g.fillRect (juce::Rectangle<float> (area.getX(), litBottom, area.getWidth(), area.getBottom() - litBottom));

    g.setColour (ReverbLookAndFeel::colourPanelBorder);
    g.drawRect (area, 1.0f);
}

//==============================================================================
// ReverbAudioProcessorEditor
//==============================================================================
ReverbAudioProcessorEditor::ReverbAudioProcessorEditor (ReverbAudioProcessor& p)
    : juce::AudioProcessorEditor (p),
      audioProcessor (p),
      decayCurveDisplay (p.apvts),
      pageStrip (p.apvts)
{
    setLookAndFeel (&lookAndFeel);

    titleLabel.setText ("WAREHOUSE", juce::dontSendNotification);
    titleLabel.setJustificationType (juce::Justification::centredLeft);
    titleLabel.setColour (juce::Label::textColourId, ReverbLookAndFeel::colourText);
    titleLabel.setFont (ReverbLookAndFeel::getScaledFont (22.0f, true));
    addAndMakeVisible (titleLabel);

    presetBox.setTextWhenNothingSelected ("Preset");
    presetBox.addItemList (ReverbAudioProcessor::getPresetNames(), 1); // ComboBox item IDs are 1-based
    // Reflect whatever program is already loaded (e.g. reopening a saved
    // project) instead of always starting on the "Preset" placeholder.
    // getCurrentProgram() is 0-based; ComboBox IDs are 1-based.
    presetBox.setSelectedId (audioProcessor.getCurrentProgram() + 1, juce::dontSendNotification);
    presetBox.onChange = [this]
    {
        // applyPreset() takes a 0-based index; ComboBox IDs start at 1
        // (set via the addItemList offset above), so translate here.
        audioProcessor.applyPreset (presetBox.getSelectedId() - 1);
        updatePresetDescription();
    };
    addAndMakeVisible (presetBox);

    // Secondary to presetBox -- see the member comment in PluginEditor.h. Not interested in
    // clicks: it is a caption, not a control, and should not steal them from whatever sits
    // behind it if the window is ever resized small enough for rows to touch.
    presetDescriptionLabel.setJustificationType (juce::Justification::centredRight);
    presetDescriptionLabel.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (presetDescriptionLabel);
    updatePresetDescription();

    addAndMakeVisible (decayCurveDisplay);
    addAndMakeVisible (duckMeter);
    addAndMakeVisible (pageStrip);
    addAndMakeVisible (levelMeter);

    setResizable (true, true);
    // Approved sizes, ruling A2 (PLAN-R2 section 8) -- WarehouseSnapshot derives its render sizes
    // from this constrainer, so these two calls are the single point of truth for what every
    // snapshot, and every host resize gesture, actually allows.
    constrainer.setFixedAspectRatio ((double) defaultWidth / (double) defaultHeight);
    constrainer.setSizeLimits (minWidth, minHeight, maxWidth, maxHeight);
    setConstrainer (&constrainer);

    setSize (audioProcessor.getSavedEditorWidth(), audioProcessor.getSavedEditorHeight());

    startTimerHz (30);
}

ReverbAudioProcessorEditor::~ReverbAudioProcessorEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void ReverbAudioProcessorEditor::updatePresetDescription()
{
    presetDescriptionLabel.setText (reverb::param::presetDescription (audioProcessor.getCurrentProgram()),
                                     juce::dontSendNotification);
}

void ReverbAudioProcessorEditor::timerCallback()
{
    levelMeter.setLevel (0, audioProcessor.getOutputLevel (0));
    levelMeter.setLevel (1, audioProcessor.getOutputLevel (1));
    levelMeter.repaint();

    duckMeter.setReduction (audioProcessor.getDuckReductionDb());
    duckMeter.repaint();

    pageStrip.setEffectivePreDelayMs (audioProcessor.getEffectivePreDelayMs());
}

void ReverbAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (ReverbLookAndFeel::colourBackground);
}

void ReverbAudioProcessorEditor::resized()
{
    auto scale = (float) getWidth() / (float) defaultWidth;

    auto bounds = getLocalBounds();

    // The header now carries two rows: the title/algo/preset controls (unchanged height, scales
    // with the window exactly as before) and a slim caption row underneath for the selected
    // preset's description. The caption row floors at 14px instead of scaling all the way down
    // with everything else, so it stays legible at the 700x480 minimum instead of shrinking below
    // readable size.
    auto controlRowHeight = juce::roundToInt (56.0f * scale);
    auto descRowHeight    = juce::jmax (14, juce::roundToInt (18.0f * scale));
    auto descRowGap       = juce::roundToInt (2.0f * scale);

    auto header = bounds.removeFromTop (controlRowHeight + descRowGap + descRowHeight);
    header.reduce (juce::roundToInt (12.0f * scale), 0);

    auto controlRow = header.removeFromTop (controlRowHeight);
    header.removeFromTop (descRowGap);
    auto descRow = header;

    // Hoisted into a local before sizing against it: removeFromRight()
    // mutates `controlRow` as a side effect of evaluating its own argument, so
    // reading controlRow.getWidth() a second time in the same expression read
    // the already-shrunk width (compiler-dependent under C++14, and always
    // the shrunk value under C++17's left-to-right argument sequencing).
    auto presetArea = controlRow.removeFromRight (juce::jmin (220, controlRow.getWidth()));
    presetBox.setBounds (presetArea.withSizeKeepingCentre (juce::jmin (200, presetArea.getWidth()),
                                                            juce::roundToInt (26.0f * scale)));

    // The algo mode combo that used to sit between the title and the preset box was removed from
    // the public parameter surface for v1.0 (see the "Phase 3's four IDs" comment in
    // Parameters.h). Rather than leave its gap as dead space, the title label now claims the
    // whole remainder of controlRow -- it stays left-justified (Justification::centredLeft) at
    // the same position it always occupied, just with more room to its right.
    titleLabel.setFont (ReverbLookAndFeel::getScaledFont (22.0f * scale, true));
    titleLabel.setBounds (controlRow);

    // Fixed size, not scaled by `scale` -- the same choice getComboBoxFont() makes for the preset
    // name this captions, so the description stays visibly smaller and dimmer than the preset
    // name at every window size instead of closing the gap as the window grows. Right-justified
    // so it reads as tied to presetBox above it, growing left as the line gets longer rather than
    // overlapping the title on the left.
    presetDescriptionLabel.setFont (ReverbLookAndFeel::getScaledFont (11.0f));
    presetDescriptionLabel.setBounds (descRow);

    bounds.reduce (juce::roundToInt (12.0f * scale), juce::roundToInt (12.0f * scale));

    auto gap = juce::roundToInt (8.0f * scale);

    // One shared column width for both meters, computed from the full content width before
    // either row is split off, so the duck GR meter (top) and the output level meter (bottom)
    // form a single continuous column down the right edge instead of two independently-sized
    // boxes that happen to be near each other -- the duck meter previously used a narrower,
    // unrelated formula and read as an orphaned box disconnected from the meter below it.
    auto meterColumnWidth = juce::jmax (56, bounds.getWidth() / 9);

    // Top: the decay curve -- the flagship display (PLAN-R2 section 8) -- with the duck GR
    // meter beside it, matching the mockup's top-right placement.
    auto curveRow = bounds.removeFromTop (juce::roundToInt ((float) bounds.getHeight() * 0.42f));
    auto duckArea = curveRow.removeFromRight (meterColumnWidth);
    duckMeter.setBounds (duckArea);
    curveRow.removeFromRight (gap);
    decayCurveDisplay.setBounds (curveRow);

    bounds.removeFromTop (gap);

    // Bottom: the paged knob strip, with the output level meter beside it -- same proportions
    // the old 2x2 grid used for its meter column.
    auto meterArea = bounds.removeFromRight (meterColumnWidth);
    levelMeter.setBounds (meterArea);
    bounds.removeFromRight (gap);
    pageStrip.setBounds (bounds);

    audioProcessor.saveEditorSize (getWidth(), getHeight());
}
