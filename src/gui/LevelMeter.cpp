#include "LevelMeter.h"
#include "ReverbLookAndFeel.h"

LevelMeter::LevelMeter()
{
    setInterceptsMouseClicks (false, false);
}

void LevelMeter::setLevel (int channel, float linearPeak) noexcept
{
    if (channel < 0 || channel > 1)
        return;

    // Decibels::gainToDecibels is NaN-safe (a NaN gain fails the "> 0"
    // check internally and falls through to the floor value), so no
    // separate isfinite guard is needed here.
    levelsDb[(size_t) channel] = juce::jlimit (minDb, maxDb,
        juce::Decibels::gainToDecibels (linearPeak, minDb));
}

float LevelMeter::dbToProportion (float db) noexcept
{
    return juce::jlimit (0.0f, 1.0f, (db - minDb) / (maxDb - minDb));
}

void LevelMeter::paint (juce::Graphics& g)
{
    // Small vertical margin so bars and scale ticks read as sitting inside
    // the meter rather than flush against its physical top/bottom edge.
    auto bounds = getLocalBounds().toFloat().reduced (0.0f, 2.0f);
    // Reserve up to 22 px for the "-60"-style tick labels; only compress
    // below that if the whole meter is too narrow to afford it, rather than
    // always taking a fixed proportion (which starved the labels down to a
    // couple of pixels - and truncated digits - once the meter itself was
    // only ~60 px wide, as it is near the constrainer minimum).
    auto scaleArea = bounds.removeFromLeft (juce::jmin (22.0f, bounds.getWidth() * 0.5f));
    drawScale (g, scaleArea);

    auto gap = 3.0f;
    auto barWidth = (bounds.getWidth() - gap) * 0.5f;
    auto leftBar  = bounds.removeFromLeft (barWidth);
    bounds.removeFromLeft (gap);
    auto rightBar = bounds;

    drawChannelBar (g, leftBar, levelsDb[0]);
    drawChannelBar (g, rightBar, levelsDb[1]);
}

void LevelMeter::resized()
{
    // Layout is computed directly in paint() from the current bounds, so
    // nothing to precompute here.
}

void LevelMeter::drawChannelBar (juce::Graphics& g, juce::Rectangle<float> area, float db) const
{
    if (area.getWidth() <= 0.0f || area.getHeight() <= 0.0f)
        return;

    auto zone = [&] (float dbLow, float dbHigh, juce::Colour colour)
    {
        auto yLow  = area.getBottom() - dbToProportion (dbLow)  * area.getHeight();
        auto yHigh = area.getBottom() - dbToProportion (dbHigh) * area.getHeight();
        g.setColour (colour);
        g.fillRect (juce::Rectangle<float> (area.getX(), yHigh, area.getWidth(), yLow - yHigh));
    };

    zone (minDb, -6.0f, ReverbLookAndFeel::colourMeterGreen);
    zone (-6.0f, -1.0f, ReverbLookAndFeel::colourMeterAmber);
    zone (-1.0f, maxDb, ReverbLookAndFeel::colourMeterRed);

    // Mask off the unlit portion above the current peak so only the bottom
    // of the bar, up to the level, reads as lit.
    auto litTop = area.getBottom() - dbToProportion (db) * area.getHeight();
    g.setColour (ReverbLookAndFeel::colourPanel);
    g.fillRect (juce::Rectangle<float> (area.getX(), area.getY(), area.getWidth(), litTop - area.getY()));

    g.setColour (ReverbLookAndFeel::colourPanelBorder);
    g.drawRect (area, 1.0f);
}

void LevelMeter::drawScale (juce::Graphics& g, juce::Rectangle<float> area) const
{
    static const float ticks[] = { 0.0f, -6.0f, -20.0f, -40.0f, -60.0f };

    g.setFont (ReverbLookAndFeel::getScaledFont (9.5f));
    g.setColour (ReverbLookAndFeel::colourTextDim);

    for (auto db : ticks)
    {
        auto centreY = area.getBottom() - dbToProportion (db) * area.getHeight();
        // Clamp the label's own rectangle inside the scale area so the
        // extreme ticks (0 dB at the top, -60 dB at the bottom) never draw
        // half off the edge and get clipped.
        auto y = juce::jlimit (area.getY(), area.getBottom() - 12.0f, centreY - 6.0f);
        g.drawText (juce::String ((int) db), juce::Rectangle<float> (area.getX(), y, area.getWidth(), 12.0f),
                    juce::Justification::centredLeft, false);
    }
}
