#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "gui/DecayCurveDisplay.h"
#include "gui/LevelMeter.h"
#include "gui/PageStrip.h"
#include "gui/ReverbLookAndFeel.h"

class ReverbAudioProcessorEditor : public juce::AudioProcessorEditor,
                                   private juce::Timer
{
public:
    explicit ReverbAudioProcessorEditor (ReverbAudioProcessor&);
    ~ReverbAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // The approved sizes (ruling A2, PLAN-R2 section 8): 700x480 minimum, 900x620 default,
    // 1800x1240 maximum. WarehouseSnapshot derives its render sizes from these via the constrainer,
    // so changing them here is the only place that needs touching to change what every snapshot
    // renders at.
    //
    // PUBLIC deliberately: PluginProcessor carries its own default-size constants and the two must
    // agree, so tests/ProcessorTests.cpp asserts against these rather than against literals. That
    // is the only formulation that catches a divergence -- reading the editor's constructed size
    // cannot, because the editor restores its size FROM the processor and the two therefore agree
    // by construction. The divergence has already happened once (processor still said 760x520
    // after the editor moved to 900x620), and because WarehouseSnapshot derives its render size from
    // the editor's actual bounds, every GUI snapshot was silently verified at the old dimensions.
    static constexpr int defaultWidth  = 900;
    static constexpr int defaultHeight = 620;
    static constexpr int minWidth      = 700;
    static constexpr int minHeight     = 480;
    static constexpr int maxWidth      = 1800;
    static constexpr int maxHeight     = 1240;

private:
    void timerCallback() override;

    // Refreshes presetDescriptionLabel from reverb::param::presetDescription() for whatever
    // program getCurrentProgram() currently reports. Called both from presetBox's onChange and
    // from the constructor (getCurrentProgram() reflects state restored from a saved session,
    // which sets the combo's selected ID directly rather than firing onChange).
    void updatePresetDescription();

    // The duck gain-reduction meter (PLAN-R2 section 8's mockup, top right; live as of step 6 --
    // ReverbAudioProcessor::getDuckReductionDb() mirrors the ducker's GR off the audio thread once
    // per block). Polled from the editor's 30 Hz timer, same as LevelMeter.
    class DuckMeter : public juce::Component
    {
    public:
        void paint (juce::Graphics&) override;

        // dB is negative for a reduction (getDuckReductionDb()'s own convention), clamped to this
        // meter's -40..0 dB span, which comfortably covers the ducker's measured range (duckamt
        // 100 reaches about -32 dB Gentle .. -39 dB Pump).
        void setReduction (float dB) noexcept;

    private:
        static constexpr float minDb = -40.0f;
        static constexpr float maxDb = 0.0f;

        static float dbToProportion (float db) noexcept;
        void drawScale (juce::Graphics&, juce::Rectangle<float> area) const;
        void drawBar (juce::Graphics&, juce::Rectangle<float> area) const;

        float reductionDb = 0.0f;
    };

    ReverbAudioProcessor& audioProcessor;

    ReverbLookAndFeel lookAndFeel;

    // Makes every child SettableTooltipClient's setTooltip() (KnobComponent's sliders, the SYNC
    // toggle, the Decay Curve's own tooltip) actually show something on hover. One instance only,
    // parented to this editor rather than the desktop, per juce::TooltipWindow's own documented
    // recommendation for plugin editors (scales with the editor / the host's DPI setting instead
    // of opening a native window a host might refuse).
    juce::TooltipWindow tooltipWindow { this };

    juce::Label titleLabel;
    juce::ComboBox presetBox;

    // Plain-language caption for whichever preset is selected, secondary to presetBox (dimmer,
    // smaller -- see ReverbLookAndFeel's default Label colour). Empty text renders as nothing,
    // which is deliberately how the "no preset selected" state (out-of-range program, e.g. -1)
    // is handled: presetDescription() already returns "" for that case, so there is nothing
    // extra to special-case here.
    juce::Label presetDescriptionLabel;

    DecayCurveDisplay decayCurveDisplay;
    DuckMeter duckMeter;

    PageStrip pageStrip;

    LevelMeter levelMeter;

    juce::ComponentBoundsConstrainer constrainer;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReverbAudioProcessorEditor)
};
