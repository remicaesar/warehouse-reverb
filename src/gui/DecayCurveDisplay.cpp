#include "DecayCurveDisplay.h"
#include "ReverbLookAndFeel.h"

#include "../Parameters.h"

#include <cmath>

namespace
{
    // Only ever fed from bands returned by decaycurve::sanitise(), so this reads what the
    // curve and the handles both already agree on -- never the raw, unclamped atomics.
    juce::String frequencyReadout (float hz)
    {
        return hz >= 1000.0f ? juce::String (hz / 1000.0f, 2) + " kHz" : juce::String (hz, 0) + " Hz";
    }

    juce::String multiplierReadout (float mult)
    {
        return juce::String (mult, 2) + "x";
    }

    //==========================================================================================
    // The auto-ranged y axis walks a standard 1-2-5 x 10^n gridline ladder. Represented as
    // (exponent, mantissa index) rather than a bare float so stepping along it is exact -- no
    // log10/pow round-trip to accumulate error over several steps, which matters here because
    // paintGrid() walks from axisMinSeconds to axisMaxSeconds one step at a time.
    struct NiceValue { int exponent = 0; int mantissaIndex = 0; };

    constexpr std::array<float, 3> niceMantissas { 1.0f, 2.0f, 5.0f };

    float niceValueOf (NiceValue n) noexcept
    {
        return niceMantissas[(size_t) n.mantissaIndex] * std::pow (10.0f, (float) n.exponent);
    }

    // Largest nice value <= v.
    NiceValue niceFloorOf (float v) noexcept
    {
        v = juce::jmax (v, 1.0e-6f);
        const auto exponent = (int) std::floor (std::log10 (v));
        const auto scale    = std::pow (10.0f, (float) exponent);
        const auto ratio    = v / scale;

        int bestIndex = 0;

        for (int i = 0; i < (int) niceMantissas.size(); ++i)
            if (niceMantissas[(size_t) i] <= ratio + 1.0e-4f)
                bestIndex = i;

        return { exponent, bestIndex };
    }

    NiceValue stepNiceUp (NiceValue n) noexcept
    {
        if (n.mantissaIndex + 1 < (int) niceMantissas.size())
            return { n.exponent, n.mantissaIndex + 1 };

        return { n.exponent + 1, 0 };
    }

    // Smallest nice value >= v.
    NiceValue niceCeilOf (float v) noexcept
    {
        const auto floorResult = niceFloorOf (v);

        if (niceValueOf (floorResult) >= v - 1.0e-4f * v)
            return floorResult;

        return stepNiceUp (floorResult);
    }

    // A readable time label for a nice-ladder value -- always a clean integer, since the ladder
    // only ever lands on 1/2/5 x 10^n.
    juce::String formatSeconds (float seconds)
    {
        return seconds < 1.0f ? juce::String (juce::roundToInt (seconds * 1000.0f)) + "ms"
                              : juce::String (juce::roundToInt (seconds)) + "s";
    }
}

//==============================================================================
DecayCurveDisplay::DecayCurveDisplay (juce::AudioProcessorValueTreeState& apvts)
    : decayValue     (apvts.getRawParameterValue (reverb::param::idDecay)),
      lowXoverValue  (apvts.getRawParameterValue (reverb::param::idLowCut)),
      highXoverValue (apvts.getRawParameterValue (reverb::param::idDamping)),
      lowMultValue   (apvts.getRawParameterValue (reverb::param::idDecayLow)),
      highMultValue  (apvts.getRawParameterValue (reverb::param::idDecayHigh)),
      // decayAttachment is never written -- only its callback is used, so this display
      // repaints when the SPACE page's Decay knob moves even though the curve has no handle
      // of its own for it. The other four are read AND written (dragged) below.
      decayAttachment     (*apvts.getParameter (reverb::param::idDecay),
                           [this] (float) { repaint(); }),
      lowXoverAttachment  (*apvts.getParameter (reverb::param::idLowCut),
                           [this] (float) { repaint(); }),
      highXoverAttachment (*apvts.getParameter (reverb::param::idDamping),
                           [this] (float) { repaint(); }),
      lowMultAttachment   (*apvts.getParameter (reverb::param::idDecayLow),
                           [this] (float) { repaint(); }),
      highMultAttachment  (*apvts.getParameter (reverb::param::idDecayHigh),
                           [this] (float) { repaint(); })
{
    setInterceptsMouseClicks (true, false);

    // The Low/High Decay handles set how fast each band's tail dies away over time. That is
    // distinct from TONE's Bandwidth (the tail's initial timbre) and Wet High Cut (the wet
    // signal's static tone) -- the three most likely controls to be conflated with each other, so
    // this names the other two by where to find them (PLAN-R2 step 6).
    setTooltip ("Drag the handles to set the Decay EQ: the two vertical lines are the crossovers, "
                "dragging inside a flat region sets how fast that band's tail dies away over time. "
                "This does not change the tail's starting timbre (TONE's Bandwidth) or the wet "
                "signal's static tone (TONE's Wet High Cut).");

    decayAttachment.sendInitialUpdate();
    lowXoverAttachment.sendInitialUpdate();
    highXoverAttachment.sendInitialUpdate();
    lowMultAttachment.sendInitialUpdate();
    highMultAttachment.sendInitialUpdate();
}

DecayCurveDisplay::~DecayCurveDisplay() = default;

//==============================================================================
float DecayCurveDisplay::xForHz (float hz) const noexcept
{
    const auto clamped = juce::jlimit (freqMinHz, freqMaxHz, hz);
    const auto logMin  = std::log10 (freqMinHz);
    const auto logMax  = std::log10 (freqMaxHz);
    const auto prop    = (std::log10 (clamped) - logMin) / (logMax - logMin);

    return plotArea.getX() + prop * plotArea.getWidth();
}

float DecayCurveDisplay::hzForX (float x) const noexcept
{
    if (plotArea.getWidth() <= 0.0f)
        return freqMinHz;

    const auto prop   = juce::jlimit (0.0f, 1.0f, (x - plotArea.getX()) / plotArea.getWidth());
    const auto logMin = std::log10 (freqMinHz);
    const auto logMax = std::log10 (freqMaxHz);

    return std::pow (10.0f, logMin + prop * (logMax - logMin));
}

float DecayCurveDisplay::yForSeconds (float seconds) const noexcept
{
    // Falls back to the absolute bounds before the first updateAxisRange() call has run (it
    // always has, by the time paint()'s helpers reach here, but mouseDown()/mouseDrag() call
    // this too, and defending against construction-order surprises is cheap).
    const auto axisMin = axisMinSeconds > 0.0f ? axisMinSeconds : t60AbsoluteMinSeconds;
    const auto axisMax = axisMaxSeconds > 0.0f ? axisMaxSeconds : t60AbsoluteMaxSeconds;

    const auto clamped = juce::jlimit (axisMin, axisMax, seconds);
    const auto logMin   = std::log10 (axisMin);
    const auto logMax   = std::log10 (axisMax);
    const auto prop     = (std::log10 (clamped) - logMin) / (logMax - logMin);

    // Longer decay reads higher on screen, mirroring how a boost reads higher on an EQ curve.
    return plotArea.getBottom() - prop * plotArea.getHeight();
}

float DecayCurveDisplay::secondsForY (float y) const noexcept
{
    const auto axisMin = axisMinSeconds > 0.0f ? axisMinSeconds : t60AbsoluteMinSeconds;
    const auto axisMax = axisMaxSeconds > 0.0f ? axisMaxSeconds : t60AbsoluteMaxSeconds;

    if (plotArea.getHeight() <= 0.0f)
        return axisMin;

    const auto prop   = juce::jlimit (0.0f, 1.0f, (plotArea.getBottom() - y) / plotArea.getHeight());
    const auto logMin = std::log10 (axisMin);
    const auto logMax = std::log10 (axisMax);

    return std::pow (10.0f, logMin + prop * (logMax - logMin));
}

//==============================================================================
reverb::decaycurve::Bands DecayCurveDisplay::currentBands() const noexcept
{
    reverb::decaycurve::Bands bands;
    bands.lowMult     = lowMultValue->load();
    bands.highMult    = highMultValue->load();
    bands.xoverLowHz  = lowXoverValue->load();
    bands.xoverHighHz = highXoverValue->load();
    return bands;
}

float DecayCurveDisplay::currentBaseT60() const noexcept
{
    return juce::jmax (0.01f, decayValue->load());
}

void DecayCurveDisplay::updateAxisRange (const reverb::decaycurve::Bands& bands, float baseT60) const
{
    // A coarse fit -- pixel resolution is not needed to find a min/max, so this stays cheap and
    // independent of plotArea's (possibly much larger) width. Samples the reference curve AND
    // both confidence-band brackets, so the band drawn in paintConfidenceBand() is never clipped
    // by an axis fitted only to the reference line.
    constexpr int fitSteps = 48;
    const auto refLen   = reverb::decaycurve::referenceLengthSeconds;
    const auto shortLen = reverb::decaycurve::shortestLineSeconds;
    const auto longLen  = reverb::decaycurve::longestLineSeconds;

    const auto logMin = std::log10 (freqMinHz);
    const auto logMax = std::log10 (freqMaxHz);

    float rawMin = t60AbsoluteMaxSeconds;
    float rawMax = t60AbsoluteMinSeconds;

    for (int i = 0; i <= fitSteps; ++i)
    {
        const auto prop = (float) i / (float) fitSteps;
        const auto hz   = std::pow (10.0f, logMin + prop * (logMax - logMin));

        for (auto lineLengthSeconds : { refLen, shortLen, longLen })
        {
            const auto lineLengthSamples = static_cast<float> (static_cast<double> (lineLengthSeconds)
                                                                * displaySampleRate);
            auto t60 = reverb::decaycurve::t60At (hz, baseT60, bands, displaySampleRate, lineLengthSamples);
            t60 = juce::jlimit (t60AbsoluteMinSeconds, t60AbsoluteMaxSeconds, t60);

            rawMin = juce::jmin (rawMin, t60);
            rawMax = juce::jmax (rawMax, t60);
        }
    }

    // Pad outward in log space so the curve does not touch the plot's own edge, then enforce the
    // zoom floor (a flat curve should still look flat, not blow up into meaningless full-height
    // rounding noise), then snap both ends outward to the nearest gridline.
    const auto padFactor = std::pow (10.0f, axisPaddingDecades);
    auto paddedMin = juce::jmax (t60AbsoluteMinSeconds, rawMin / padFactor);
    auto paddedMax = juce::jmin (t60AbsoluteMaxSeconds, rawMax * padFactor);

    if (paddedMax < paddedMin * minAxisZoomRatio)
    {
        const auto centre    = std::sqrt (paddedMin * paddedMax);
        const auto halfRatio = std::sqrt (minAxisZoomRatio);
        paddedMin = juce::jmax (t60AbsoluteMinSeconds, centre / halfRatio);
        paddedMax = juce::jmin (t60AbsoluteMaxSeconds, centre * halfRatio);
    }

    const auto fittedMin = niceValueOf (niceFloorOf (paddedMin));
    const auto fittedMax = niceValueOf (niceCeilOf (paddedMax));

    // Hysteresis: keep the current window unless the fit no longer fits inside it, or the fit
    // would use less than axisRezoomFillThreshold of it. Without this the axis would re-snap
    // every frame while dragging, and the curve would appear to sit still while the axis slid
    // underneath it -- the same "looks like it isn't moving" failure this feature exists to fix,
    // just moved from the data onto the axis instead.
    const bool uninitialised = axisMinSeconds <= 0.0f || axisMaxSeconds <= 0.0f;
    const bool outsideWindow = ! uninitialised
                              && (fittedMin < axisMinSeconds * 0.999f || fittedMax > axisMaxSeconds * 1.001f);

    bool tooLoose = false;

    if (! uninitialised && ! outsideWindow)
    {
        const auto windowDecades  = std::log10 (axisMaxSeconds) - std::log10 (axisMinSeconds);
        const auto fittedDecades  = std::log10 (fittedMax) - std::log10 (fittedMin);
        tooLoose = windowDecades > 0.0f && (fittedDecades / windowDecades) < axisRezoomFillThreshold;
    }

    if (uninitialised || outsideWindow || tooLoose)
    {
        axisMinSeconds = fittedMin;
        axisMaxSeconds = fittedMax;
    }
}

void DecayCurveDisplay::sampleCurve (std::vector<juce::Point<float>>& points,
                                     const reverb::decaycurve::Bands& bands, float baseT60,
                                     float lineLengthSeconds) const
{
    const auto lineLengthSamples = static_cast<float> (static_cast<double> (lineLengthSeconds)
                                                        * displaySampleRate);
    const auto steps = juce::jmax (2, static_cast<int> (plotArea.getWidth()));

    points.resize (static_cast<size_t> (steps) + 1);

    for (int i = 0; i <= steps; ++i)
    {
        const auto x   = plotArea.getX() + static_cast<float> (i) / static_cast<float> (steps)
                                          * plotArea.getWidth();
        const auto hz  = hzForX (x);
        const auto t60 = reverb::decaycurve::t60At (hz, baseT60, bands, displaySampleRate,
                                                    lineLengthSamples);

        points[static_cast<size_t> (i)] = { x, yForSeconds (t60) };
    }
}

//==============================================================================
void DecayCurveDisplay::paintGrid (juce::Graphics& g) const
{
    static const std::array<float, 5> freqTicks { 20.0f, 100.0f, 1000.0f, 10000.0f, 20000.0f };
    static const std::array<const char*, 5> freqLabels { "20", "100", "1k", "10k", "20k" };

    g.setColour (ReverbLookAndFeel::colourPanelBorder);

    for (auto hz : freqTicks)
        g.drawVerticalLine (juce::roundToInt (xForHz (hz)), plotArea.getY(), plotArea.getBottom());

    // The y axis is auto-ranged (updateAxisRange(), called from paint() before this), so its
    // gridlines are walked from the current [axisMinSeconds, axisMaxSeconds] rather than read
    // from a fixed table. Both bounds are themselves nice-ladder values (updateAxisRange() only
    // ever stores a niceFloorOf()/niceCeilOf() result), so this walk always starts and ends
    // exactly on a labelled line. 20 entries comfortably covers even the widest reachable window
    // (the full 5 ms .. 200 s absolute range is under 5 decades, i.e. under 15 ticks).
    std::array<float, 20> secTicks {};
    int numSecTicks = 0;

    if (axisMinSeconds > 0.0f && axisMaxSeconds > axisMinSeconds)
    {
        auto current = niceFloorOf (axisMinSeconds);

        while (numSecTicks < (int) secTicks.size())
        {
            const auto value = niceValueOf (current);
            secTicks[(size_t) numSecTicks++] = value;

            if (value >= axisMaxSeconds * 0.999f)
                break;

            current = stepNiceUp (current);
        }
    }

    for (int i = 0; i < numSecTicks; ++i)
        g.drawHorizontalLine (juce::roundToInt (yForSeconds (secTicks[(size_t) i])),
                              plotArea.getX(), plotArea.getRight());

    g.setFont (axisFont);
    g.setColour (ReverbLookAndFeel::colourTextDim);

    constexpr float freqLabelWidth = 28.0f;

    for (size_t i = 0; i < freqTicks.size(); ++i)
    {
        const auto x = xForHz (freqTicks[i]);
        g.drawText (freqLabels[i],
                    juce::Rectangle<float> (x - freqLabelWidth * 0.5f, plotArea.getBottom() + 2.0f,
                                            freqLabelWidth, 12.0f),
                    juce::Justification::centred, false);
    }

    constexpr float secLabelWidth = 36.0f;

    for (int i = 0; i < numSecTicks; ++i)
    {
        const auto y = yForSeconds (secTicks[(size_t) i]);
        g.drawText (formatSeconds (secTicks[(size_t) i]),
                    juce::Rectangle<float> (plotArea.getX() - secLabelWidth - 4.0f, y - 6.0f,
                                            secLabelWidth, 12.0f),
                    juce::Justification::centredRight, false);
    }
}

void DecayCurveDisplay::paintConfidenceBand (juce::Graphics& g, const reverb::decaycurve::Bands& bands,
                                             float baseT60) const
{
    // First honesty constraint (PLAN-R2 section 8): t60At() is exact at the reference length
    // only. These two brackets are the shortest and longest line the tank runs, so the band is
    // the actual range the sixteen lines cover inside a deep shelf transition rather than an
    // assertion that the reference curve is the whole truth.
    const auto shortLen = reverb::decaycurve::shortestLineSeconds;
    const auto longLen  = reverb::decaycurve::longestLineSeconds;

    sampleCurve (shortLengthPoints, bands, baseT60, shortLen);
    sampleCurve (longLengthPoints, bands, baseT60, longLen);

    confidenceBandPath.clear();

    if (shortLengthPoints.empty() || longLengthPoints.empty())
        return;

    const auto n = shortLengthPoints.size();

    confidenceBandPath.startNewSubPath (shortLengthPoints[0].x,
                                        juce::jmin (shortLengthPoints[0].y, longLengthPoints[0].y));

    for (size_t i = 1; i < n; ++i)
        confidenceBandPath.lineTo (shortLengthPoints[i].x,
                                   juce::jmin (shortLengthPoints[i].y, longLengthPoints[i].y));

    for (size_t i = n; i-- > 0;)
        confidenceBandPath.lineTo (shortLengthPoints[i].x,
                                   juce::jmax (shortLengthPoints[i].y, longLengthPoints[i].y));

    confidenceBandPath.closeSubPath();

    g.setColour (ReverbLookAndFeel::colourAccent.withAlpha (0.12f));
    g.fillPath (confidenceBandPath);
}

void DecayCurveDisplay::paintCurve (juce::Graphics& g, const reverb::decaycurve::Bands& bands,
                                    float baseT60) const
{
    sampleCurve (referencePoints, bands, baseT60, reverb::decaycurve::referenceLengthSeconds);

    referenceStrokePath.clear();

    if (referencePoints.empty())
        return;

    referenceStrokePath.startNewSubPath (referencePoints.front());

    for (size_t i = 1; i < referencePoints.size(); ++i)
        referenceStrokePath.lineTo (referencePoints[i]);

    constexpr float strokeWidth = 2.2f;
    const juce::PathStrokeType stroke (strokeWidth, juce::PathStrokeType::curved,
                                       juce::PathStrokeType::rounded);

    // Second honesty constraint: below decayHighFloorWarning the achieved HF decay floors out
    // on the tank's own circulation time, which t60At() does not model at all. Rather than draw
    // a solid line implying a precision the DSP does not have there, the HF portion (above the
    // high crossover) switches to a dashed stroke and paintCaption() explains why.
    if (bands.highMult >= decayHighFloorWarning)
    {
        g.setColour (ReverbLookAndFeel::colourAccent);
        g.strokePath (referenceStrokePath, stroke);
        return;
    }

    const auto hfBoundaryX = xForHz (bands.xoverHighHz);

    {
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (juce::Rectangle<float> (plotArea.getX(), plotArea.getY(),
                                                     hfBoundaryX - plotArea.getX(),
                                                     plotArea.getHeight()).getSmallestIntegerContainer());
        g.setColour (ReverbLookAndFeel::colourAccent);
        g.strokePath (referenceStrokePath, stroke);
    }

    {
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (juce::Rectangle<float> (hfBoundaryX, plotArea.getY(),
                                                     plotArea.getRight() - hfBoundaryX,
                                                     plotArea.getHeight()).getSmallestIntegerContainer());

        juce::Path dashed;
        const float dashLengths[] { 5.0f, 4.0f };
        stroke.createDashedStroke (dashed, referenceStrokePath, dashLengths, 2);

        g.setColour (ReverbLookAndFeel::colourAccent);
        g.fillPath (dashed);
    }
}

void DecayCurveDisplay::paintHandles (juce::Graphics& g, const reverb::decaycurve::Bands& bands) const
{
    auto drawHandle = [&] (float hz)
    {
        const auto x = xForHz (hz);

        g.setColour (ReverbLookAndFeel::colourText.withAlpha (0.35f));
        g.drawLine (x, plotArea.getY(), x, plotArea.getBottom(), 1.2f);

        constexpr float gripHalfWidth = 4.5f;
        constexpr float gripHeight    = 7.0f;

        juce::Path grip;
        grip.addTriangle (x - gripHalfWidth, plotArea.getBottom(),
                          x + gripHalfWidth, plotArea.getBottom(),
                          x, plotArea.getBottom() - gripHeight);

        g.setColour (dragTarget == DragTarget::lowCrossover || dragTarget == DragTarget::highCrossover
                         ? ReverbLookAndFeel::colourAccent.brighter (0.2f)
                         : ReverbLookAndFeel::colourAccent);
        g.fillPath (grip);
    };

    drawHandle (bands.xoverLowHz);
    drawHandle (bands.xoverHighHz);
}

void DecayCurveDisplay::paintCaption (juce::Graphics& g, const reverb::decaycurve::Bands& bands,
                                      float baseT60) const
{
    juce::ignoreUnused (baseT60);

    juce::String text;

    if (bands.highMult < decayHighFloorWarning)
    {
        text = "HF decay approximate below 0.15x -- achieved decay floors out below the tank's "
               "own circulation time";
    }
    else
    {
        text = "Low X-over " + frequencyReadout (bands.xoverLowHz)
             + "   Low Decay " + multiplierReadout (bands.lowMult)
             + "   High X-over " + frequencyReadout (bands.xoverHighHz)
             + "   High Decay " + multiplierReadout (bands.highMult);
    }

    g.setColour (ReverbLookAndFeel::colourTextDim);
    g.setFont (captionFont);

    auto captionArea = getLocalBounds().removeFromBottom (14);
    g.drawFittedText (text, captionArea, juce::Justification::centred, 1);
}

//==============================================================================
void DecayCurveDisplay::paint (juce::Graphics& g)
{
    ReverbLookAndFeel::drawPanelChrome (g, getLocalBounds(), "DECAY CURVE");

    if (plotArea.getWidth() <= 0.0f || plotArea.getHeight() <= 0.0f)
        return;

    // Sanitised once here and threaded through every helper below, so the curve, the handle
    // positions and the caption readout can never disagree with each other -- the DSP's own
    // xoverHigh >= 4 * xoverLow clamp (D1 trap 2) is enforced the same way for all three.
    const auto bands   = reverb::decaycurve::sanitise (currentBands());
    const auto baseT60 = currentBaseT60();

    updateAxisRange (bands, baseT60);

    paintGrid (g);
    paintConfidenceBand (g, bands, baseT60);
    paintCurve (g, bands, baseT60);
    paintHandles (g, bands);
    paintCaption (g, bands, baseT60);
}

void DecayCurveDisplay::resized()
{
    auto bounds = getLocalBounds();
    bounds.removeFromTop (ReverbLookAndFeel::panelTitleHeight (bounds.getHeight()));

    // Reserve exactly what paintGrid()/paintCaption() draw into, so the plot area never
    // overlaps the axis labels or the caption line.
    constexpr int captionHeight  = 16;
    constexpr int freqLabelSpace = 14;
    constexpr int secLabelSpace  = 40;

    bounds.removeFromBottom (captionHeight);
    bounds.removeFromBottom (freqLabelSpace);
    bounds.removeFromLeft (secLabelSpace);
    bounds.reduce (10, 4);

    plotArea = bounds.toFloat();

    axisFont    = ReverbLookAndFeel::getScaledFont (9.5f);
    captionFont = ReverbLookAndFeel::getScaledFont (10.5f);
}

//==============================================================================
void DecayCurveDisplay::mouseDown (const juce::MouseEvent& e)
{
    dragTarget = DragTarget::none;

    if (! plotArea.contains (e.position))
        return;

    const auto bands = reverb::decaycurve::sanitise (currentBands());
    const auto lowX  = xForHz (bands.xoverLowHz);
    const auto highX = xForHz (bands.xoverHighHz);

    constexpr float handleGrabPx = 10.0f;

    if (std::abs (e.position.x - lowX) <= handleGrabPx)
        dragTarget = DragTarget::lowCrossover;
    else if (std::abs (e.position.x - highX) <= handleGrabPx)
        dragTarget = DragTarget::highCrossover;
    else if (e.position.x < lowX)
        dragTarget = DragTarget::lowBand;
    else if (e.position.x > highX)
        dragTarget = DragTarget::highBand;

    switch (dragTarget)
    {
        case DragTarget::lowCrossover:  lowXoverAttachment.beginGesture(); break;
        case DragTarget::highCrossover: highXoverAttachment.beginGesture(); break;
        case DragTarget::lowBand:       lowMultAttachment.beginGesture(); break;
        case DragTarget::highBand:      highMultAttachment.beginGesture(); break;
        case DragTarget::none: return;
    }

    mouseDrag (e);
}

void DecayCurveDisplay::mouseDrag (const juce::MouseEvent& e)
{
    switch (dragTarget)
    {
        case DragTarget::lowCrossover:
        {
            const auto hz = juce::jlimit (reverb::decaycurve::minXoverLowHz,
                                          reverb::decaycurve::maxXoverLowHz, hzForX (e.position.x));
            lowXoverAttachment.setValueAsPartOfGesture (hz);
            break;
        }
        case DragTarget::highCrossover:
        {
            const auto hz = juce::jlimit (reverb::decaycurve::minXoverHighHz,
                                          reverb::decaycurve::maxXoverHighHz, hzForX (e.position.x));
            highXoverAttachment.setValueAsPartOfGesture (hz);
            break;
        }
        case DragTarget::lowBand:
        {
            const auto mult = juce::jlimit (lowMultMin, lowMultMax,
                                            secondsForY (e.position.y) / currentBaseT60());
            lowMultAttachment.setValueAsPartOfGesture (mult);
            break;
        }
        case DragTarget::highBand:
        {
            const auto mult = juce::jlimit (highMultMin, highMultMax,
                                            secondsForY (e.position.y) / currentBaseT60());
            highMultAttachment.setValueAsPartOfGesture (mult);
            break;
        }
        case DragTarget::none: break;
    }
}

void DecayCurveDisplay::mouseUp (const juce::MouseEvent&)
{
    switch (dragTarget)
    {
        case DragTarget::lowCrossover:  lowXoverAttachment.endGesture(); break;
        case DragTarget::highCrossover: highXoverAttachment.endGesture(); break;
        case DragTarget::lowBand:       lowMultAttachment.endGesture(); break;
        case DragTarget::highBand:      highMultAttachment.endGesture(); break;
        case DragTarget::none: break;
    }

    dragTarget = DragTarget::none;
}
