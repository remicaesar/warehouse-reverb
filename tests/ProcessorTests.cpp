/*
    Console test harness for ReverbAudioProcessor -- the layer ReverbEngineTests deliberately cannot
    reach: state save/restore, buses negotiation, programs, and the block plumbing between the host
    and the engine.

    Same house style as ReverbEngineTests.cpp: one PASS/FAIL line per check carrying the measured
    value, non-zero exit code if anything fails.
*/

#include "Parameters.h"
#include "PluginEditor.h"
#include "PluginProcessor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace
{

int failureCount = 0;

void check (const char* name, bool passed, const std::string& measured)
{
    if (! passed)
        ++failureCount;

    std::printf ("[%s] %-38s  %s\n", passed ? "PASS" : "FAIL", name, measured.c_str());
    std::fflush (stdout);
}

std::string fmt (const char* pattern, double a, double b = 0.0, double c = 0.0)
{
    char buffer[256];
    std::snprintf (buffer, sizeof (buffer), pattern, a, b, c);
    return std::string (buffer);
}

//==================================================================================================
// The parameter IDs are the processor's contract with the editor and with saved host sessions, so
// they are spelled out here rather than read back from the layout: a silent rename is exactly the
// regression this file exists to catch.
//==================================================================================================
struct ParameterProbe
{
    const char* id;
    float       value;      // distinct, non-default, and exactly representable as a float
};

const ParameterProbe probes[] =
{
    { "mix",       12.5f },
    { "predelay",  37.5f },
    { "size",     162.5f },
    { "decay",      7.25f },
    // NOT 3500: step 1a repurposed `damping` as the Decay EQ high crossover with default 3500,
    // and a probe equal to the default makes `movedFromDefault` below vacuous. See
    // testProbesDifferFromDefaults, which now names any probe that collides.
    { "damping", 4800.0f },
    { "lowcut",   312.5f },
    { "bandwidth", 7500.0f },
    { "diffusion", 43.75f },
    { "moddepth",  81.25f },
    { "modrate",    2.75f },
    { "width",    137.5f },
    { "freeze",     1.0f },
    { "output",    -6.5f }
};

constexpr int numProbes = static_cast<int> (std::size (probes));

std::vector<float> readAll (const ReverbAudioProcessor& processor)
{
    std::vector<float> values;
    values.reserve (static_cast<size_t> (numProbes));

    for (const auto& probe : probes)
    {
        const auto* raw = processor.apvts.getRawParameterValue (probe.id);
        values.push_back (raw != nullptr ? raw->load() : std::numeric_limits<float>::quiet_NaN());
    }

    return values;
}

/** Sets every parameter to its probe value, the way a host or the editor would. */
void writeProbeValues (ReverbAudioProcessor& processor)
{
    for (const auto& probe : probes)
    {
        auto* parameter = processor.apvts.getParameter (probe.id);

        if (parameter != nullptr)
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (probe.value));
    }
}

/** Counts parameters whose stored value differs. Compared bit for bit: a state round trip that
    lands one ULP away has still lost information the host wrote.
*/
int countDifferences (const std::vector<float>& a, const std::vector<float>& b)
{
    int differences = 0;

    for (size_t i = 0; i < a.size() && i < b.size(); ++i)
        if (std::memcmp (&a[i], &b[i], sizeof (float)) != 0)
            ++differences;

    return differences;
}

//==================================================================================================
/** Sets one parameter by ID, the way a host or the editor would. */
void setParameterTo (ReverbAudioProcessor& processor, const char* id, float value)
{
    if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (processor.apvts.getParameter (id)))
        ranged->setValueNotifyingHost (ranged->convertTo0to1 (value));
}

/** Fills a buffer with uniform noise on every channel. */
void fillNoise (juce::AudioBuffer<float>& buffer, std::mt19937& rng, float amplitude)
{
    std::uniform_real_distribution<float> dist (-amplitude, amplitude);

    for (int c = 0; c < buffer.getNumChannels(); ++c)
        for (int n = 0; n < buffer.getNumSamples(); ++n)
            buffer.setSample (c, n, dist (rng));
}

bool allFinite (const juce::AudioBuffer<float>& buffer)
{
    for (int c = 0; c < buffer.getNumChannels(); ++c)
        for (int n = 0; n < buffer.getNumSamples(); ++n)
            if (! std::isfinite (buffer.getSample (c, n)))
                return false;

    return true;
}

double channelRms (const juce::AudioBuffer<float>& buffer, int channel)
{
    double sum = 0.0;

    for (int n = 0; n < buffer.getNumSamples(); ++n)
    {
        const double v = buffer.getSample (channel, n);
        sum += v * v;
    }

    return buffer.getNumSamples() > 0
         ? std::sqrt (sum / static_cast<double> (buffer.getNumSamples()))
         : 0.0;
}

/** Normalised correlation between two channels; 1.0 means they are the same signal. */
double channelCorrelation (const juce::AudioBuffer<float>& buffer)
{
    double sumAB = 0.0, sumAA = 0.0, sumBB = 0.0;

    for (int n = 0; n < buffer.getNumSamples(); ++n)
    {
        const double a = buffer.getSample (0, n);
        const double b = buffer.getSample (1, n);

        sumAB += a * b;
        sumAA += a * a;
        sumBB += b * b;
    }

    const double denom = std::sqrt (sumAA * sumBB);

    return denom > 0.0 ? sumAB / denom : 0.0;
}

//==================================================================================================
// a. Every parameter survives getStateInformation -> a fresh processor -> setStateInformation.
//==================================================================================================
/** d3. Every preset's stored value actually reaches the parameter.

    Added by the CTO after the preset author red-proofed its own table and found that NOTHING in
    either binary could see a wrong preset VALUE. Check `d` only requires that consecutive presets
    differ from each other, which is an adjacent-inequality test, not a value test. Demonstrated by
    mutation at the time:
      - deleting the single most load-bearing value in the flagship preset (`Stabs — Short & Bright`
        `wetlowcut` 250 -> 20) left all 17 checks green;
      - setting its low crossover to 5000, which is OUTSIDE that parameter's 60..800 range, also
        left all 17 green -- because NormalisableRange::convertTo0to1 clamps silently, so a
        typo'd zero in the table is invisible to the build, the tests AND the host.

    This asserts the round trip instead: apply each preset, read every parameter back, and require
    it to equal what the table asked for. It is what makes the preset table safe to refactor.
*/
void testPresetValuesLand()
{
    ReverbAudioProcessor processor;

    const int numPresets = processor.getNumPrograms();

    int    mismatches   = 0;
    int    checked      = 0;
    double worstError   = 0.0;
    std::string worstWhere;

    for (int preset = 0; preset < numPresets; ++preset)
    {
        processor.setCurrentProgram (preset);

        for (const auto* id : reverb::param::allIds)
        {
            auto* parameter = processor.apvts.getParameter (id);

            if (parameter == nullptr)
                continue;

            // What the TABLE asked for, before any clamping...
            const float wanted = reverb::param::presetTableValue (preset, id);

            if (! std::isfinite (wanted))
            {
                ++mismatches;                  // every id must be covered by the accessor
                continue;
            }

            // ...against what the parameter actually holds. These differ when the table literal is
            // outside the parameter's declared range, because convertTo0to1 clamps silently.
            const float stored = parameter->convertFrom0to1 (parameter->getValue());

            // Tolerance is the normalise round trip only: the write path is
            // setValueNotifyingHost (convertTo0to1 (x)), so a correct literal comes back within a
            // float's worth of itself scaled by the range. Compare relative to the range width so
            // one tolerance works for a 0..100 % control and a 1000..20000 Hz one alike.
            const auto& range = parameter->getNormalisableRange();
            const double span = std::max (1.0e-6, static_cast<double> (range.end - range.start));
            const double error = std::abs (static_cast<double> (stored - wanted)) / span;

            ++checked;

            if (error > worstError)
            {
                worstError = error;
                worstWhere = std::string (" worst: preset ") + std::to_string (preset) + " " + id
                           + " table=" + std::to_string (wanted) + " stored=" + std::to_string (stored);
            }

            if (error > 1.0e-4)
                ++mismatches;
        }
    }

    check ("d3. every preset value reaches its parameter",
           mismatches == 0 && checked == numPresets * reverb::param::numParameters,
           fmt ("presets=%.0f  values=%.0f", numPresets, checked)
           + fmt ("  mismatches=%.0f", mismatches)
           + fmt ("  worst relative error=%.2e", worstError)
           + worstWhere);

    // presetDescription's bounds behaviour, which nothing else exercises.
    const bool descriptionsOk = reverb::param::presetDescription (-1).isEmpty()
                             && reverb::param::presetDescription (numPresets).isEmpty()
                             && reverb::param::presetDescription (0).isNotEmpty();

    // The em dash is why presetName/presetDescription decode UTF-8 explicitly. A decoded em dash is
    // ONE character; the mangled form is three ("\u00e2\u20ac\u0094"). Counting characters is the
    // cheap discriminator and cannot pass on the broken version.
    int mangled = 0;
    int withDash = 0;

    for (int i = 0; i < numPresets; ++i)
    {
        const auto name = reverb::param::presetName (i);

        if (name.containsChar (juce::juce_wchar (0x2014)))     // a real em dash
            ++withDash;

        if (name.containsChar (juce::juce_wchar (0x00e2)))     // the first byte of a mangled one
            ++mangled;
    }

    check ("d4. preset text decodes as UTF-8",
           descriptionsOk && mangled == 0 && withDash >= numPresets - 1,
           fmt ("descriptions in bounds=%.0f", static_cast<double> (descriptionsOk))
           + fmt ("  names with a real em dash=%.0f/%.0f", withDash, numPresets)
           + fmt ("  mangled=%.0f", mangled));
}

//==================================================================================================
/** j. The playhead read, and the two atomics the GUI polls.

    Added by the CTO after step 5A, which verified this wiring with a THROWAWAY probe because it is
    structurally unreachable from the engine test target: HouseTests links into WarehouseTests, which
    has no juce_audio_processors, so `getPlayHead()` cannot be exercised there at all. This target
    does link it. A throwaway that proved something load-bearing should not stay a throwaway.

    What this guards, which nothing else does: that the processor actually reads the host tempo and
    resolves a musical division into milliseconds, that it falls back to the typed value when the
    host reports no tempo, and that the two values the GUI displays (duck reduction, effective
    pre-delay) are actually populated rather than left at their initialisers.
*/
void testPlayheadAndGuiAtomics()
{
    // Reports a fixed tempo, the way a host does while playing.
    struct StubPlayHead final : public juce::AudioPlayHead
    {
        explicit StubPlayHead (double bpmIn, bool reportTempoIn)
            : bpm (bpmIn), reportTempo (reportTempoIn) {}

        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo info;
            if (reportTempo)
                info.setBpm (bpm);
            info.setIsPlaying (true);
            info.setTimeInSamples (0);
            return info;
        }

        double bpm;
        bool   reportTempo;
    };

    constexpr double sr = 48000.0;
    constexpr int    bs = 512;
    constexpr float  typedMs = 40.0f;

    auto runOneBlock = [&] (juce::AudioPlayHead* head, bool sync, int division) -> float
    {
        ReverbAudioProcessor p;
        p.setPlayConfigDetails (2, 2, sr, bs);
        p.prepareToPlay (sr, bs);
        p.setPlayHead (head);

        setParameterTo (p, reverb::param::idPreDelay,     typedMs);
        setParameterTo (p, reverb::param::idPreDelaySync, sync ? 1.0f : 0.0f);
        setParameterTo (p, reverb::param::idPreDelayDiv,  (float) division);

        juce::AudioBuffer<float> buf (2, bs);
        buf.clear();
        juce::MidiBuffer midi;
        p.processBlock (buf, midi);

        const float ms = p.getEffectivePreDelayMs();
        p.setPlayHead (nullptr);       // never leave the processor pointing at a dead stub
        return ms;
    };

    StubPlayHead playing124 { 124.0, true };
    StubPlayHead noTempo    { 124.0, false };

    // 124 bpm: a 1/8 note is 60000/124/2 = 241.935 ms. Division index 5 is 1/8 (PLAN-R2 D6).
    const float eighth   = runOneBlock (&playing124, true,  5);
    const float freeRun  = runOneBlock (&playing124, false, 5);
    const float noTempoMs = runOneBlock (&noTempo,   true,  5);
    const float noHeadMs  = runOneBlock (nullptr,    true,  5);

    const float expectedEighth = 60000.0f / 124.0f / 2.0f;

    // The intermittent-host branch: a host that reports tempo only while playing. The resolved
    // pre-delay must HOLD the last known tempo rather than collapsing to the typed value, because
    // collapsing would audibly move the reverb the moment the transport stopped. This is the only
    // place that branch meets a real getPlayHead(), and it is the branch that protects the sound.
    float heldMs = 0.0f;
    {
        StubPlayHead intermittent { 124.0, true };

        ReverbAudioProcessor p;
        p.setPlayConfigDetails (2, 2, sr, bs);
        p.prepareToPlay (sr, bs);
        p.setPlayHead (&intermittent);

        setParameterTo (p, reverb::param::idPreDelay,     typedMs);
        setParameterTo (p, reverb::param::idPreDelaySync, 1.0f);
        setParameterTo (p, reverb::param::idPreDelayDiv,  5.0f);

        juce::AudioBuffer<float> buf (2, bs);
        juce::MidiBuffer midi;

        buf.clear();
        p.processBlock (buf, midi);          // tempo reported: resolves to 241.935 ms

        intermittent.reportTempo = false;    // transport stops; host stops reporting tempo
        buf.clear();
        p.processBlock (buf, midi);

        heldMs = p.getEffectivePreDelayMs();
        p.setPlayHead (nullptr);
    }

    check ("j. playhead drives synced pre-delay",
           std::abs (eighth - expectedEighth) < 1.0f
           && std::abs (freeRun  - typedMs) < 0.01f
           && std::abs (noTempoMs - typedMs) < 0.01f
           && std::abs (noHeadMs  - typedMs) < 0.01f
           && std::abs (heldMs - expectedEighth) < 1.0f,
           fmt ("1/8 @124bpm=%.3f (want %.3f)", eighth, expectedEighth)
           + fmt ("  sync off=%.3f", freeRun)
           + fmt ("  no tempo=%.3f  no playhead=%.3f", noTempoMs, noHeadMs)
           + fmt ("  transport stopped, holds=%.3f", heldMs));

    // The duck atomic must actually move, and settle back to exactly 0 dB.
    ReverbAudioProcessor p;
    p.setPlayConfigDetails (2, 2, sr, bs);
    p.prepareToPlay (sr, bs);

    const float idle = p.getDuckReductionDb();

    setParameterTo (p, reverb::param::idDuckAmount, 100.0f);
    setParameterTo (p, reverb::param::idDuckThresh,  -40.0f);
    setParameterTo (p, reverb::param::idMix,         100.0f);

    juce::AudioBuffer<float> buf (2, bs);
    juce::MidiBuffer midi;
    std::mt19937 rng (5150);

    for (int b = 0; b < 60; ++b)
    {
        fillNoise (buf, rng, 0.9f);
        p.processBlock (buf, midi);
    }

    const float ducked = p.getDuckReductionDb();

    setParameterTo (p, reverb::param::idDuckAmount, 0.0f);

    for (int b = 0; b < 200; ++b)
    {
        buf.clear();
        p.processBlock (buf, midi);
    }

    const float released = p.getDuckReductionDb();

    check ("j2. duck reduction atomic is live",
           std::abs (idle) < 0.01f && ducked < -6.0f && std::abs (released) < 0.01f,
           fmt ("idle=%.3f dB", idle) + fmt ("  driven=%.3f dB", ducked)
           + fmt ("  released=%.3f dB", released));
}

//==================================================================================================
/** Guards the probe table itself.

    Every probe value must differ from its parameter's default, otherwise testStateRoundTrip's
    `movedFromDefault` guard silently stops proving anything for that parameter. This fired for
    real: a round-1 probe of 3500 for `damping` became exactly the default when step 1a repurposed
    that ID, and the only symptom was "moved off default=12/13" with no indication of which one.
    This check names it.
*/
void testProbesDifferFromDefaults()
{
    ReverbAudioProcessor fresh;

    std::string colliding;
    int collisions = 0;

    for (const auto& probe : probes)
    {
        auto* parameter = fresh.apvts.getParameter (probe.id);

        if (parameter == nullptr)
        {
            if (! colliding.empty()) colliding += " ";
            colliding += std::string (probe.id) + "(missing)";
            ++collisions;
            continue;
        }

        const float defaultValue = parameter->convertFrom0to1 (parameter->getDefaultValue());

        if (std::abs (defaultValue - probe.value) < 1.0e-4f)
        {
            char buffer[64];
            std::snprintf (buffer, sizeof (buffer), "(=%.2f)", (double) defaultValue);

            if (! colliding.empty()) colliding += " ";
            colliding += std::string (probe.id) + buffer;
            ++collisions;
        }
    }

    check ("a0. every probe differs from its default",
           collisions == 0,
           collisions == 0 ? fmt ("probes=%.0f all non-default", static_cast<double> (numProbes))
                           : std::string ("colliding: ") + colliding);
}

void testStateRoundTrip()
{
    ReverbAudioProcessor source;
    writeProbeValues (source);

    const auto written = readAll (source);

    juce::MemoryBlock state;
    source.getStateInformation (state);

    ReverbAudioProcessor restored;
    const auto defaults = readAll (restored);

    restored.setStateInformation (state.getData(), static_cast<int> (state.getSize()));
    const auto reloaded = readAll (restored);

    // A round trip that restored nothing would look identical to one that restored everything if
    // the probe values happened to be the defaults, so assert the values moved in the first place.
    const int movedFromDefault = countDifferences (defaults, written);
    const int mismatches       = countDifferences (written, reloaded);

    check ("a. state round-trip, 13 parameters",
           mismatches == 0 && movedFromDefault == numProbes && state.getSize() > 0,
           fmt ("restored exactly=%.0f/%.0f", static_cast<double> (numProbes - mismatches),
                static_cast<double> (numProbes))
           + fmt ("  moved off default=%.0f/%.0f", static_cast<double> (movedFromDefault),
                  static_cast<double> (numProbes))
           + fmt ("  state=%.0f bytes", static_cast<double> (state.getSize())));
}

//==================================================================================================
// b. Malformed state must be ignored, not trusted. Hosts do hand over garbage.
//
// Reported per input rather than in aggregate: "something moved" is not actionable, and the
// interesting failure mode here is one specific shape of bad document, not all of them.
//==================================================================================================
void testMalformedState()
{
    ReverbAudioProcessor processor;
    writeProbeValues (processor);

    juce::MemoryBlock valid;
    processor.getStateInformation (valid);

    const auto before = readAll (processor);

    std::mt19937 rng (0x9A11u);
    std::uniform_int_distribution<int> byteDist (0, 255);

    juce::MemoryBlock garbage (512);

    for (size_t i = 0; i < garbage.getSize(); ++i)
        static_cast<char*> (garbage.getData())[i] = static_cast<char> (byteDist (rng));

    // Well-formed XML with the wrong root tag.
    juce::XmlElement wrongRoot ("SOMETHING_ELSE");
    wrongRoot.setAttribute ("mix", 99.0);

    juce::MemoryBlock wrongRootBlock;
    juce::AudioProcessor::copyXmlToBinary (wrongRoot, wrongRootBlock);

    // The right root tag carrying a completely foreign schema.
    juce::XmlElement wrongSchema ("PARAMETERS");
    wrongSchema.createNewChildElement ("NOT_A_PARAM")->setAttribute ("value", "not a number");

    juce::MemoryBlock wrongSchemaBlock;
    juce::AudioProcessor::copyXmlToBinary (wrongSchema, wrongSchemaBlock);

    struct Case { const char* label; const void* data; int size; };

    const Case cases[] =
    {
        { "random bytes",   garbage.getData(),         static_cast<int> (garbage.getSize()) },
        { "truncated",      valid.getData(),           static_cast<int> (valid.getSize() / 2) },
        { "wrong root",     wrongRootBlock.getData(),  static_cast<int> (wrongRootBlock.getSize()) },
        { "wrong schema",   wrongSchemaBlock.getData(), static_cast<int> (wrongSchemaBlock.getSize()) },
        { "nullptr",        nullptr,                   128 },
        { "zero length",    valid.getData(),           0 }
    };

    int         damaged = 0;
    std::string detail;

    for (const auto& c : cases)
    {
        // Each case starts from known-good state, so one that does damage cannot be mistaken for
        // several - and so a later case is still testing what it says it is.
        processor.setStateInformation (valid.getData(), static_cast<int> (valid.getSize()));

        processor.setStateInformation (c.data, c.size);

        const int changed = countDifferences (before, readAll (processor));

        if (changed != 0)
        {
            ++damaged;
            detail += std::string (" ") + c.label + "=" + std::to_string (changed) + "-changed";
        }
        else
        {
            detail += std::string (" ") + c.label + "=ok";
        }
    }

    check ("b. malformed state is ignored",
           damaged == 0,
           fmt ("inputs that damaged state=%.0f ", static_cast<double> (damaged)) + detail);
}

//==================================================================================================
// c. Editor size round-trip, and the fallback when the stored value is absent or out of range.
//==================================================================================================
void testEditorSizeRoundTrip()
{
    ReverbAudioProcessor source;
    source.saveEditorSize (1000, 700);

    juce::MemoryBlock state;
    source.getStateInformation (state);

    ReverbAudioProcessor restored;
    const int freshWidth  = restored.getSavedEditorWidth();
    const int freshHeight = restored.getSavedEditorHeight();

    restored.setStateInformation (state.getData(), static_cast<int> (state.getSize()));

    const int roundTrippedWidth  = restored.getSavedEditorWidth();
    const int roundTrippedHeight = restored.getSavedEditorHeight();

    // A nonsense value written straight onto the tree, as a stale or hand-edited session would.
    restored.apvts.state.setProperty (juce::Identifier ("editorWidth"),  17, nullptr);
    restored.apvts.state.setProperty (juce::Identifier ("editorHeight"), 99999, nullptr);

    const int clampedWidth  = restored.getSavedEditorWidth();
    const int clampedHeight = restored.getSavedEditorHeight();

    // Asserted against the EDITOR's own constants rather than literals. The processor and the
    // editor each carry a default size and they must agree; hardcoding either number here means a
    // future resize updates one, leaves the other stale, and this test then certifies the wrong
    // size. That already happened once: the processor still said 760x520 after the editor moved to
    // 900x620, and because WarehouseSnapshot derives its render size from the editor's actual bounds,
    // every GUI snapshot was being verified at the old dimensions. Referencing the editor's
    // constants makes a divergence between the two FAIL here instead of hiding.
    constexpr int expectedWidth  = ReverbAudioProcessorEditor::defaultWidth;
    constexpr int expectedHeight = ReverbAudioProcessorEditor::defaultHeight;

    check ("c. editor size round-trip",
           roundTrippedWidth == 1000 && roundTrippedHeight == 700
           && freshWidth == expectedWidth && freshHeight == expectedHeight
           && clampedWidth == expectedWidth && clampedHeight == expectedHeight,
           fmt ("restored=%.0fx%.0f", roundTrippedWidth, roundTrippedHeight)
           + fmt ("  absent=%.0fx%.0f", freshWidth, freshHeight)
           + fmt ("  out-of-range=%.0fx%.0f", clampedWidth, clampedHeight)
           + fmt ("  (editor default %.0fx%.0f)", expectedWidth, expectedHeight));
}

//==================================================================================================
// d. Programs are the preset table, and the selected one has to survive a session.
//==================================================================================================
void testPrograms()
{
    ReverbAudioProcessor processor;

    const auto names = ReverbAudioProcessor::getPresetNames();
    const int  count = processor.getNumPrograms();

    int indexMismatches = 0;
    int nameMismatches  = 0;
    int inertPresets    = 0;

    auto previous = readAll (processor);

    for (int i = 0; i < count; ++i)
    {
        processor.setCurrentProgram (i);

        if (processor.getCurrentProgram() != i)
            ++indexMismatches;

        if (processor.getProgramName (i) != names[i])
            ++nameMismatches;

        // Every preset must actually move the parameters; a table wired to the wrong index, or an
        // applyPreset that silently returned early, would leave them where they were.
        const auto current = readAll (processor);

        if (i > 0 && countDifferences (previous, current) == 0)
            ++inertPresets;

        previous = current;
    }

    check ("d. programs select and name correctly",
           count == names.size() && indexMismatches == 0 && nameMismatches == 0
           && inertPresets == 0,
           fmt ("presets=%.0f index mismatches=%.0f name mismatches=%.0f",
                static_cast<double> (count), static_cast<double> (indexMismatches),
                static_cast<double> (nameMismatches))
           + fmt ("  inert presets=%.0f", static_cast<double> (inertPresets)));

    // The program index is saved on the state tree, so a reopened session shows the preset's name
    // in the host's own menu rather than falling back to the first entry.
    const int chosen = 5;   // whatever preset 5 is; the index is what matters here
    processor.setCurrentProgram (chosen);

    juce::MemoryBlock state;
    processor.getStateInformation (state);

    ReverbAudioProcessor restored;
    const int beforeRestore = restored.getCurrentProgram();
    restored.setStateInformation (state.getData(), static_cast<int> (state.getSize()));

    check ("d2. program index survives a save",
           restored.getCurrentProgram() == chosen && beforeRestore == 0,
           fmt ("saved=%.0f fresh=%.0f restored=%.0f",
                static_cast<double> (chosen), static_cast<double> (beforeRestore),
                static_cast<double> (restored.getCurrentProgram())));
}

//==================================================================================================
// e. Bus layouts. 1->2 matters: a stereo reverb on a mono track is the commonest way a reverb is
//    used, and without it Logic instantiates 1->1 and the tail collapses to mono.
//==================================================================================================
void testBusLayouts()
{
    ReverbAudioProcessor processor;

    auto layout = [] (const juce::AudioChannelSet& in, const juce::AudioChannelSet& out)
    {
        juce::AudioProcessor::BusesLayout l;
        l.inputBuses.add (in);
        l.outputBuses.add (out);
        return l;
    };

    const auto mono     = juce::AudioChannelSet::mono();
    const auto stereo   = juce::AudioChannelSet::stereo();
    const auto disabled = juce::AudioChannelSet::disabled();
    const auto surround = juce::AudioChannelSet::create5point1();

    struct Case { const char* label; juce::AudioProcessor::BusesLayout layout; bool expected; };

    const Case cases[] =
    {
        { "1->1", layout (mono,     mono),     true  },
        { "2->2", layout (stereo,   stereo),   true  },
        { "1->2", layout (mono,     stereo),   true  },
        { "2->1", layout (stereo,   mono),     false },
        { "0->2", layout (disabled, stereo),   false },
        { "2->0", layout (stereo,   disabled), false },
        { "6->6", layout (surround, surround), false }
    };

    int wrong = 0;
    std::string detail;

    for (const auto& c : cases)
    {
        const bool supported = processor.checkBusesLayoutSupported (c.layout);

        if (supported != c.expected)
            ++wrong;

        detail += std::string (" ") + c.label + (supported ? "=yes" : "=no");
    }

    check ("e. bus layouts", wrong == 0, fmt ("wrong verdicts=%.0f ", static_cast<double> (wrong)) + detail);
}

//==================================================================================================
// e2. Mono in / stereo out must produce a genuinely stereo tail, not a duplicated mono one.
//==================================================================================================
void testMonoToStereoProducesStereo()
{
    ReverbAudioProcessor processor;

    juce::AudioProcessor::BusesLayout monoToStereo;
    monoToStereo.inputBuses.add (juce::AudioChannelSet::mono());
    monoToStereo.outputBuses.add (juce::AudioChannelSet::stereo());

    const bool accepted = processor.setBusesLayout (monoToStereo);

    // Fully wet, so the verdict is about the tail and not about the dry signal, which is common to
    // both channels by construction and would hold the correlation near 1 at any ordinary mix.
    for (const auto& setting : { std::make_pair ("mix", 100.0f), std::make_pair ("width", 100.0f) })
        if (auto* parameter = processor.apvts.getParameter (setting.first))
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (setting.second));

    processor.prepareToPlay (48000.0, 512);

    juce::AudioBuffer<float> buffer (2, 512);
    juce::MidiBuffer midi;

    std::mt19937 rng (0x57E2E0u);

    bool   finite = true;
    double rightRms = 0.0;
    double correlation = 1.0;

    for (int b = 0; b < 200; ++b)
    {
        // Mono source: the host only guarantees the first channel; whatever is in the second is
        // the plug-in's business.
        std::uniform_real_distribution<float> dist (-0.5f, 0.5f);

        for (int n = 0; n < buffer.getNumSamples(); ++n)
        {
            buffer.setSample (0, n, dist (rng));
            buffer.setSample (1, n, 0.0f);
        }

        processor.processBlock (buffer, midi);

        finite = finite && allFinite (buffer);
        rightRms = channelRms (buffer, 1);
        correlation = channelCorrelation (buffer);
    }

    // Without the mono-to-stereo duplication the engine runs one channel and the processor clears
    // the second, so the right channel is silent: rightRms catches that. With it, the two diffuser
    // chains and the tank's two output taps are genuinely decorrelated, so a correlation anywhere
    // near 1 would mean the wet signal had been folded back down to mono.
    check ("e2. mono in -> stereo out is stereo",
           accepted && finite && rightRms > 1.0e-3 && correlation < 0.5,
           fmt ("layout accepted=%.0f right rms=%.5f L/R correlation=%.4f",
                accepted ? 1.0 : 0.0, rightRms, correlation));
}

//==================================================================================================
// f. A block larger than the one promised to prepareToPlay must still come out finite: the engine
//    chunks it internally, and the processor is what hands it over.
//==================================================================================================
void testOversizedBlock()
{
    ReverbAudioProcessor processor;
    processor.prepareToPlay (48000.0, 64);

    juce::AudioBuffer<float> buffer (2, 2048);
    juce::MidiBuffer midi;

    std::mt19937 rng (0x81165u);

    bool   finite = true;
    double rms = 0.0;

    for (int b = 0; b < 16; ++b)
    {
        fillNoise (buffer, rng, 0.5f);
        processor.processBlock (buffer, midi);

        finite = finite && allFinite (buffer);
        rms = channelRms (buffer, 0);
    }

    check ("f. oversized block through processor",
           finite && rms > 1.0e-3,
           fmt ("prepared=64 processed=2048 rms=%.5f finite=%.0f", rms, finite ? 1.0 : 0.0));
}

//==================================================================================================
// g. Two instances must not share anything.
//==================================================================================================
void testTwoInstances()
{
    ReverbAudioProcessor first, second;

    first.prepareToPlay (48000.0, 512);
    second.prepareToPlay (48000.0, 512);

    // Different settings, so shared state would show up as identical output.
    first.setCurrentProgram (1);    // Small Room
    second.setCurrentProgram (5);   // any preset; only the index round-trip matters here

    juce::AudioBuffer<float> a (2, 512), b (2, 512);
    juce::MidiBuffer midi;

    std::mt19937 shared (0xB0A7u);

    bool   finite = true;
    double rmsA = 0.0, rmsB = 0.0;

    for (int block = 0; block < 200; ++block)
    {
        fillNoise (a, shared, 0.5f);
        b.makeCopyOf (a);

        first.processBlock (a, midi);
        second.processBlock (b, midi);

        finite = finite && allFinite (a) && allFinite (b);
        rmsA = channelRms (a, 0);
        rmsB = channelRms (b, 0);
    }

    const double separation = rmsA > 0.0 ? std::abs (rmsB - rmsA) / rmsA : 0.0;

    check ("g. two instances stay independent",
           finite && rmsA > 1.0e-3 && rmsB > 1.0e-3 && separation > 0.02,
           fmt ("rms A=%.5f B=%.5f separation=%.1f%%", rmsA, rmsB, separation * 100.0));
}

//==================================================================================================
// h. A sample-rate change mid-session re-prepares cleanly.
//==================================================================================================
void testSampleRateChange()
{
    ReverbAudioProcessor processor;
    juce::MidiBuffer midi;

    std::mt19937 rng (0x48C0u);

    bool   finite = true;
    double lastRms = 0.0;

    for (const double sampleRate : { 48000.0, 96000.0, 44100.0, 192000.0 })
    {
        processor.prepareToPlay (sampleRate, 256);

        juce::AudioBuffer<float> buffer (2, 256);

        for (int b = 0; b < 100; ++b)
        {
            fillNoise (buffer, rng, 0.5f);
            processor.processBlock (buffer, midi);

            finite = finite && allFinite (buffer);
        }

        lastRms = channelRms (buffer, 0);
    }

    check ("h. sample-rate change mid-session",
           finite && lastRms > 1.0e-3,
           fmt ("48k -> 96k -> 44.1k -> 192k, final rms=%.5f finite=%.0f",
                lastRms, finite ? 1.0 : 0.0));
}

//==================================================================================================
// i. The tail length reported to the host must account for Freeze, which is unbounded by design.
//==================================================================================================
void testTailLength()
{
    ReverbAudioProcessor processor;

    auto set = [&processor] (const char* id, float value)
    {
        if (auto* p = processor.apvts.getParameter (id))
            p->setValueNotifyingHost (p->convertTo0to1 (value));
    };

    set ("decay", 2.5f);
    set ("predelay", 100.0f);
    set ("freeze", 0.0f);

    const double open = processor.getTailLengthSeconds();

    set ("freeze", 1.0f);
    const double frozen = processor.getTailLengthSeconds();

    check ("i. tail length respects Freeze",
           std::abs (open - 2.6) < 0.01 && frozen >= 30.0,
           fmt ("open=%.3f s frozen=%.1f s", open, frozen));
}

} // namespace

//==================================================================================================

#include "TestSuites.h"

// Entry point for the suite runner (tests/TestSuites.h). Returns the failure COUNT, not 0/1,
// so the runner can sum across suites.
int runProcessorTests()
{
    // The processor owns an AudioProcessorValueTreeState and reports parameter changes to the host,
    // both of which expect a message thread to exist.
    const juce::ScopedJuceInitialiser_GUI juceInitialiser;

    std::printf ("ReverbAudioProcessor tests\n");
    std::printf ("--------------------------\n");

    testProbesDifferFromDefaults();
    testPlayheadAndGuiAtomics();
    testStateRoundTrip();
    testMalformedState();
    testEditorSizeRoundTrip();
    testPrograms();
    testPresetValuesLand();
    testBusLayouts();
    testMonoToStereoProducesStereo();
    testOversizedBlock();
    testTwoInstances();
    testSampleRateChange();
    testTailLength();

    std::printf ("--------------------------\n");
    std::printf ("%s (%d failure%s)\n",
                 failureCount == 0 ? "ALL CHECKS PASSED" : "FAILURES",
                 failureCount,
                 failureCount == 1 ? "" : "s");

    return failureCount;
}
