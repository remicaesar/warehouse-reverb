#pragma once

/*
    The flagship display (PLAN-R2 section 8): T60 vs frequency, log-x, drawn by calling
    reverb::decaycurve::t60At() directly -- the same function the DSP evaluates its filter
    targets from (D1a's "one T60(f) curve" argument, C2). This header is the only place in the
    GUI that includes dsp/DecayCurve.h, and it is read-only here.

    Every t60At() call here names the line length it is drawing -- the reference for the curve, the
    shortest and longest for the two band brackets -- and what comes back is the COMPLETE feedback
    path for that line: attenuation filter plus the fixed DC blocker (see DecayCurve.h's
    dbPerSampleAt). The blocker costs a fixed dB per pass and so takes a share of the decay that
    grows with the decay time; before it was folded in, the curve claimed a tail up to 12% longer
    than the tank delivers on this grid, and 92% longer at 20 Hz on a 30 s bass tail -- always in
    that one direction.

    Two crossover handles (damping = high crossover, lowcut = low crossover) drag horizontally
    along the log-frequency axis; dragging inside the flat low-band or high-band region drags
    the curve up/down, which is decaylow/decayhigh. All four write through
    juce::ParameterAttachment -- never apvts.setParameter() or the raw atomics directly.
*/

#include "../dsp/DecayCurve.h"
#include "ReverbLookAndFeel.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <atomic>
#include <vector>

class DecayCurveDisplay : public juce::Component,
                          public juce::SettableTooltipClient
{
public:
    explicit DecayCurveDisplay (juce::AudioProcessorValueTreeState& apvts);
    ~DecayCurveDisplay() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    enum class DragTarget { none, lowCrossover, highCrossover, lowBand, highBand };

    // x is fixed -- SS8 asks for "roughly 20 Hz .. 20 kHz" and there is no reason to move it.
    // y is auto-ranged (see updateAxisRange()): a fixed 5 ms .. 120 s axis makes every reachable
    // curve fit, but at the shipping defaults (a 2:1 ratio) that is one thirteenth of the panel
    // height -- numerically correct, but it reads as "this control does nothing". The axis
    // instead fits the currently plotted range each paint, so the same 2:1 ratio fills roughly
    // half the panel and the shape is legible at a glance, at every setting.
    static constexpr float freqMinHz = 20.0f;
    static constexpr float freqMaxHz = 20000.0f;

    // Absolute floor/ceiling the auto-ranged axis can never go outside -- the widest T60 the
    // sanitised parameter ranges can produce is 5 ms .. 120 s (decay 0.1..30 s x multiplier
    // 0.05..4.0), so nothing legitimate is ever clipped even though the axis normally sits well
    // inside this.
    static constexpr float t60AbsoluteMinSeconds = 0.005f;
    static constexpr float t60AbsoluteMaxSeconds = 200.0f;

    // How far the fitted [min, max] is padded outward (in decades, each side) before snapping to
    // the nearest 1-2-5 gridline, so the curve does not touch the plot's own top/bottom edge.
    static constexpr float axisPaddingDecades = 0.15f;

    // Floor on how tight the axis can zoom in, as a ratio -- otherwise a perfectly flat curve
    // (both multipliers at 1.0, i.e. min == max exactly) would snap to an arbitrarily narrow
    // window. A flat curve should still look flat, just not blow up into a meaningless ripple
    // from rounding noise at extreme zoom.
    static constexpr float minAxisZoomRatio = 2.0f;

    // Hysteresis: the fitted range must leave the CURRENT axis window (or shrink to less than
    // this fraction of it) before the axis re-snaps. Without this, dragging a handle would
    // recompute the fit every frame and the axis would slide continuously under the curve --
    // exactly the "looks like it isn't moving" failure this feature exists to fix, just moved
    // from the data to the axis.
    static constexpr float axisRezoomFillThreshold = 0.35f;

    // Mirrors Parameters.cpp's idDecayLow / idDecayHigh ranges (PLAN-R2 3.2) -- narrower than
    // decaycurve::minMultiplier for the low band, which is deliberately the wider of the two so
    // the DSP stays safe when driven directly (DecayCurve.h). Dragging here must clamp to what
    // the parameter itself will accept, or the handle would visually stall short of where the
    // pointer is once the parameter's own clamp (inside convertTo0to1) bites.
    static constexpr float lowMultMin  = 0.1f;
    static constexpr float lowMultMax  = 4.0f;
    static constexpr float highMultMin = reverb::decaycurve::minMultiplier;   // 0.05
    static constexpr float highMultMax = 4.0f;

    // Below this, the requested HF decay drops below the tank's own circulation time (PLAN-R2
    // section 8's second honesty constraint) -- t60At() does not model that floor at all, so
    // the curve would otherwise show a rate that is not what comes out. Handled by drawing the
    // HF portion of the curve dashed rather than solid, and by a caption, not by a number.
    static constexpr float decayHighFloorWarning = 0.15f;

    // No line-length constants live here. The reference curve is exact only for one line length
    // (DecayCurve.h's own documented caveat -- individual lines disagree inside a deep shelf
    // transition), and the confidence band brackets it with the SHORTEST and LONGEST line the
    // tank actually runs, read from decaycurve::shortestLineSeconds / longestLineSeconds. This
    // file used to restate that spread as its own "3.4:1 ratio" split geometrically around the
    // reference, on a comment claiming a table change could not invalidate it -- and then the
    // eight-line table became a sixteen-line one spanning 5.26:1, so the band drew 25.8..87.6 ms
    // against a real 18.03..94.91 ms and came out NARROWER than the disagreement it exists to
    // show. Reading the two endpoints straight from DecayCurve.h removes both the duplication
    // and the geometric-split approximation: the band now spans exactly the shipped table.

    // The curve is fs-invariant where it matters (the shelf gains in dB fall out of fs
    // algebraically -- see DecayCurve.h's dbPerSampleAt), so a fixed representative rate is
    // used for the corner-warping terms that do depend on it. The GUI has no live sample rate
    // to read (PluginProcessor exposes none, and adding one is outside this step's file set).
    static constexpr double displaySampleRate = 48000.0;

    float xForHz (float hz) const noexcept;
    float hzForX (float x) const noexcept;
    float yForSeconds (float seconds) const noexcept;
    float secondsForY (float y) const noexcept;

    reverb::decaycurve::Bands currentBands() const noexcept;
    float currentBaseT60() const noexcept;

    // Fits axisMinSeconds/axisMaxSeconds to whatever is about to be plotted (the reference curve
    // and the two confidence-band brackets, so the band is never clipped either), padded and
    // snapped to a 1-2-5 gridline, then only adopted if the current window no longer fits it or
    // has become mostly empty (see axisRezoomFillThreshold) -- called once per paint, before any
    // of the paint* helpers below, all of which read the result via yForSeconds()/secondsForY().
    void updateAxisRange (const reverb::decaycurve::Bands& bands, float baseT60) const;

    // Samples t60At() across the plot's frequency range at the given line length into `points`,
    // one entry per pixel column, y-clamped to the axis bounds. `points` is one of the three
    // persistent buffers below, reused frame to frame so a repaint triggered by a drag or by
    // automation does not reallocate; only resized() grows them.
    void sampleCurve (std::vector<juce::Point<float>>& points, const reverb::decaycurve::Bands& bands,
                      float baseT60, float lineLengthSeconds) const;

    void paintGrid (juce::Graphics&) const;
    void paintConfidenceBand (juce::Graphics&, const reverb::decaycurve::Bands&, float baseT60) const;
    void paintCurve (juce::Graphics&, const reverb::decaycurve::Bands&, float baseT60) const;
    void paintHandles (juce::Graphics&, const reverb::decaycurve::Bands&) const;
    void paintCaption (juce::Graphics&, const reverb::decaycurve::Bands&, float baseT60) const;

    std::atomic<float>* decayValue     = nullptr;
    std::atomic<float>* lowXoverValue  = nullptr;
    std::atomic<float>* highXoverValue = nullptr;
    std::atomic<float>* lowMultValue   = nullptr;
    std::atomic<float>* highMultValue  = nullptr;

    // Read-only: never driven, only listened to, so decay-knob moves on the SPACE page repaint
    // this display even though it has no handle of its own here.
    juce::ParameterAttachment decayAttachment;
    juce::ParameterAttachment lowXoverAttachment;
    juce::ParameterAttachment highXoverAttachment;
    juce::ParameterAttachment lowMultAttachment;
    juce::ParameterAttachment highMultAttachment;

    DragTarget dragTarget = DragTarget::none;
    juce::Rectangle<float> plotArea;

    // The auto-ranged y axis (see updateAxisRange()). `mutable` and initialised invalid so the
    // first paint() always adopts a fitted range rather than reading an arbitrary default one.
    mutable float axisMinSeconds = -1.0f;
    mutable float axisMaxSeconds = -1.0f;

    // Persistent scratch buffers (see sampleCurve()'s comment) -- the reference curve, and the
    // short/long brackets that fill the confidence band. `mutable`: rebuilt from paint(), which
    // is const, because they are a paint-time cache rather than logical state.
    mutable std::vector<juce::Point<float>> referencePoints, shortLengthPoints, longLengthPoints;
    mutable juce::Path referenceStrokePath, confidenceBandPath;

    // Default-initialised via getScaledFont() (FontOptions-based) rather than left to
    // juce::Font's implicit default constructor, which is deprecated.
    juce::Font axisFont { ReverbLookAndFeel::getScaledFont (9.5f) };
    juce::Font captionFont { ReverbLookAndFeel::getScaledFont (10.5f) };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DecayCurveDisplay)
};
