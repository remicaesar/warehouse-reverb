#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_gui_basics/juce_gui_basics.h>

// A two-channel vertical peak meter. Passive: it holds the last level it was
// given and repaints on request. The caller (the editor's 30 Hz timer) is
// responsible for polling the processor and pushing new levels in; this
// component does not own a Timer.
class LevelMeter : public juce::Component
{
public:
    LevelMeter();

    // linearPeak is a linear (not dB) peak value, channel 0 or 1.
    void setLevel (int channel, float linearPeak) noexcept;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    static constexpr float minDb = -60.0f;
    static constexpr float maxDb = 0.0f;

    std::array<float, 2> levelsDb { minDb, minDb };

    // Maps a dB value (clamped to [minDb, maxDb]) to a 0..1 fill proportion.
    static float dbToProportion (float db) noexcept;

    void drawChannelBar (juce::Graphics&, juce::Rectangle<float> area, float db) const;
    void drawScale (juce::Graphics&, juce::Rectangle<float> area) const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LevelMeter)
};
