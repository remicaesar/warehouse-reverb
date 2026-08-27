#pragma once

/*
    The paged knob layout (PLAN-R2 section 8): 33 parameters do not fit one panel grid, so this
    replaces the old fixed 2x2 grid with four tabbed pages -- SPACE / TONE / MOVE / MIX -- each
    reusing the same SectionPanel + chooseKnobColumns machinery the old grid used (moved here
    verbatim from PluginEditor.cpp, since PageStrip is now the thing with pages to fill).

    `damping`, `lowcut`, `decaylow` and `decayhigh` are NOT here -- they left the knob pages
    entirely and became the decay curve's own handles (DecayCurveDisplay). `shimmermode`,
    `shimmeramt`, `drive` and `algomode` are not here either -- they were removed from the public
    parameter surface for v1.0 (see the "Phase 3's four IDs" comment in Parameters.h).
*/

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "KnobComponent.h"

#include <memory>
#include <vector>

class PageStrip : public juce::Component
{
public:
    explicit PageStrip (juce::AudioProcessorValueTreeState& apvts);
    ~PageStrip() override;

    void resized() override;

    // Pushed from the editor's 30 Hz timer, mirroring how LevelMeter/DuckMeter are driven -- this
    // is the value ReverbAudioProcessor::getEffectivePreDelayMs() reports is ACTUALLY in force
    // (the synced division's length when Sync is on and a tempo is known, the free ms value
    // otherwise), so the SPACE page's readout can never disagree with what the engine is doing,
    // regardless of why (PLAN-R2 D6's honesty requirement -- the Pre-Delay knob alone cannot show
    // this, since it only ever reflects the free ms parameter).
    void setEffectivePreDelayMs (float ms) noexcept;

private:
    enum class Page { space, tone, move, mix };

    // A titled rounded panel that lays out a row of knobs, plus any number of non-owned extra
    // components in a row along the bottom (the FREEZE toggle in MOVE, the Sync toggle +
    // Division combo in SPACE, Duck Character in MIX). Moved verbatim
    // from the pre-3B PluginEditor::SectionPanel -- same knob-grid geometry, same panel chrome
    // (now ReverbLookAndFeel::drawPanelChrome, extracted so this and DecayCurveDisplay share
    // it byte-for-byte) -- generalised only to allow more than one extra component, which the
    // paged layout needs and the old 2x2 grid did not.
    class SectionPanel : public juce::Component
    {
    public:
        SectionPanel (juce::String title, std::vector<std::unique_ptr<KnobComponent>> knobsIn);

        void setExtraComponents (std::vector<juce::Component*> extras);

        // Looks a knob up by the APVTS parameter ID it was built with (KnobComponent::
        // getParameterID()) rather than by position, so a caller reaching in for one specific
        // knob (e.g. Pre-Delay, to dim it when Sync overrides it) does not silently break if this
        // panel's knob list is ever reordered. nullptr if no knob here is bound to that ID.
        KnobComponent* findKnob (const juce::String& parameterID) const noexcept;

        void paint (juce::Graphics&) override;
        void resized() override;

    private:
        juce::String panelTitle;
        std::vector<std::unique_ptr<KnobComponent>> knobs;
        std::vector<juce::Component*> extraComponents;
    };

    void setPage (Page);
    void updateTabColours();

    juce::TextButton spaceTab { "SPACE" }, toneTab { "TONE" }, moveTab { "MOVE" }, mixTab { "MIX" };

    SectionPanel spacePanel, tonePanel, movePanel, mixPanel;

    // SPACE extras: Pre-Delay Sync + Division, plus the honest "what's actually in force" readout
    // (PLAN-R2 D6).
    juce::ToggleButton preDelaySyncButton;
    juce::ComboBox preDelayDivBox;
    juce::Label effectivePreDelayLabel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> preDelaySyncAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> preDelayDivAttachment;

    // Read-only listener on idPreDelaySync (never written -- preDelaySyncAttachment above owns
    // the write path), so the Pre-Delay knob itself can be dimmed via Slider::setEnabled() the
    // moment Sync takes over, the same way a disabled slider dims anywhere else in this look and
    // feel (ReverbLookAndFeel::drawRotarySlider already skips the filled arc when !isEnabled()) --
    // rather than leaving a knob showing a free-ms number Sync is no longer reading.
    juce::ParameterAttachment preDelaySyncListener;
    KnobComponent* preDelayKnob = nullptr;
    bool preDelaySyncOn = false;
    float lastEffectivePreDelayMs = 10.0f;

    void updateEffectivePreDelayLabel();

    // MOVE extra: Freeze (moved here from the old OUTPUT panel). The Shimmer mode combo that used
    // to sit alongside it was removed along with the rest of the public parameter surface's phase-3
    // controls for v1.0 (see the "Phase 3's four IDs" comment in Parameters.h).
    juce::ToggleButton freezeButton { "FREEZE" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> freezeAttachment;

    // MIX extra: Duck Character.
    juce::ComboBox duckCharBox;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> duckCharAttachment;

    Page currentPage = Page::space;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PageStrip)
};
