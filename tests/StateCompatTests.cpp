/*
    State-compatibility tests -- PLAN-R2 7.0 (S1-S3).

    Same house style as ProcessorTests.cpp: one PASS/FAIL line per check carrying the measured
    value, own failureCount, no dependency on tests/TestHarness.h (that header is for the DSP-level
    ReverbEngine/FDNTank suites; this file works one layer up, against ReverbAudioProcessor and its
    APVTS, so it keeps ProcessorTests.cpp's independent local check()/fmt() rather than sharing the
    frozen one).

    Uses the real v0.1 blob captured at step 0a (tests/fixtures/v01-state.bin / v01-state.h) -- see
    PLAN-R2 3.3 for the rule under test: on load, any parameter the incoming document does not
    carry is reset to its OWN default, never left at whatever a previously-loaded preset set it to
    (the C14 defect this closes).
*/

#include "Parameters.h"
#include "PluginProcessor.h"

#include "fixtures/v01-state.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
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
// The real v0.1 blob, read from disk. tools/capture_state.cpp and every other CTO-owned tool in
// this codebase resolves "tests/fixtures" the same way -- relative to the process's current
// working directory, which the build/run convention here is the project root.
//==================================================================================================
juce::MemoryBlock readV01Blob()
{
    const auto file = juce::File::getCurrentWorkingDirectory().getChildFile ("tests/fixtures/v01-state.bin");

    juce::MemoryBlock block;

    if (file.existsAsFile())
        file.loadFileAsData (block);

    return block;
}

//==================================================================================================
// A float normalise round-trip (C13: APVTS stores denormalised values, but getting there and back
// through convertTo0to1/convertFrom0to1 and an XML text round-trip loses a little precision -- the
// fixture itself records this: damping is 3199.999756, not 3200). This tolerance is generous
// enough to absorb that and tight enough that a genuinely broken loader (wrong parameter, stuck at
// a compile default, or a value from a different parameter's range) cannot pass by accident.
//==================================================================================================
bool nearlyEqual (float a, float b, float tolerance = 0.01f)
{
    return std::isfinite (a) && std::isfinite (b) && std::abs (a - b) <= tolerance;
}

bool isLegacyId (const char* id)
{
    for (const auto& p : v01Fixture::params)
        if (std::strcmp (p.id, id) == 0)
            return true;

    return false;
}

float legacyExpectedValue (const char* id)
{
    for (const auto& p : v01Fixture::params)
        if (std::strcmp (p.id, id) == 0)
            return p.value;

    return std::numeric_limits<float>::quiet_NaN();
}

/** The parameter's own default, denormalised -- what resetParametersMissingFrom() (PluginProcessor
    .cpp) is supposed to write a missing parameter back to. */
float defaultValueOf (const juce::AudioProcessorValueTreeState& apvts, const char* id)
{
    auto* parameter = apvts.getParameter (id);
    return parameter != nullptr ? parameter->convertFrom0to1 (parameter->getDefaultValue())
                                : std::numeric_limits<float>::quiet_NaN();
}

int stateVersionOf (const ReverbAudioProcessor& processor)
{
    return static_cast<int> (processor.apvts.state.getProperty (juce::Identifier ("stateVersion"), -1));
}

//==================================================================================================
// S2's probe table: one non-default, exactly-representable value per parameter, covering all 29
// IDs (PLAN-R2 3.1/3.2). Choice/bool parameters are probed with their raw (index / 0-or-1) value,
// same convention ProcessorTests.cpp's `probes` table uses for freeze.
//==================================================================================================
struct Probe { const char* id; float value; };

const Probe stateCompatProbes[] =
{
    { reverb::param::idMix,        12.5f },
    { reverb::param::idPreDelay,   37.5f },
    { reverb::param::idSize,      162.5f },
    { reverb::param::idDecay,       7.25f },
    { reverb::param::idDamping,   4800.0f },   // D1a high crossover, within 1000..12000
    { reverb::param::idLowCut,     312.5f },   // D1a low crossover, within 60..800
    { reverb::param::idBandwidth, 7500.0f },
    { reverb::param::idDiffusion,  43.75f },
    { reverb::param::idModDepth,   81.25f },
    { reverb::param::idModRate,     2.75f },
    { reverb::param::idWidth,     137.5f },
    { reverb::param::idFreeze,       1.0f },
    { reverb::param::idOutput,      -6.5f },

    { reverb::param::idDecayLow,    2.3f },
    { reverb::param::idDecayHigh,   1.8f },
    { reverb::param::idErMix,      55.0f },
    { reverb::param::idErSize,    150.0f },
    { reverb::param::idErSpread,   30.0f },
    { reverb::param::idDensity,    72.0f },

    { reverb::param::idDuckAmount,    65.0f },
    { reverb::param::idDuckThresh,   -18.0f },
    { reverb::param::idDuckRelease,  400.0f },
    { reverb::param::idDuckChar,       1.0f },   // Pump
    { reverb::param::idPreDelaySync,   1.0f },   // true
    { reverb::param::idPreDelayDiv,    8.0f },   // "1/4"
    { reverb::param::idBassMono,      90.0f },
    { reverb::param::idWetLowCut,     45.0f },
    { reverb::param::idWetHighCut, 12000.0f },
    { reverb::param::idWetTilt,        3.5f },
};

static_assert (std::size (stateCompatProbes) == static_cast<size_t> (reverb::param::numParameters),
              "stateCompatProbes must cover exactly numParameters IDs");

void writeProbes (ReverbAudioProcessor& processor)
{
    for (const auto& probe : stateCompatProbes)
        if (auto* parameter = processor.apvts.getParameter (probe.id))
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (probe.value));
}

std::vector<float> readAllProbes (const ReverbAudioProcessor& processor)
{
    std::vector<float> values;
    values.reserve (std::size (stateCompatProbes));

    for (const auto& probe : stateCompatProbes)
    {
        auto* raw = processor.apvts.getRawParameterValue (probe.id);
        values.push_back (raw != nullptr ? raw->load() : std::numeric_limits<float>::quiet_NaN());
    }

    return values;
}

//==================================================================================================
// S1. The real v0.1 blob loads with all 13 values exact and all 16 new parameters at their
// defaults. Deliberately a FRESH instance -- PLAN-R2 7.0 says this is the one S1 "can pass without"
// exercising the reused-instance (C14) defect; S3 below is the test built specifically for that.
//==================================================================================================
void testS1()
{
    ReverbAudioProcessor processor;

    const auto blob = readV01Blob();

    if (blob.getSize() == 0)
    {
        check ("S1. v0.1 blob: 13 exact, 16 at default", false,
               "could not read tests/fixtures/v01-state.bin, cwd="
               + juce::File::getCurrentWorkingDirectory().getFullPathName().toStdString());
        return;
    }

    processor.setStateInformation (blob.getData(), static_cast<int> (blob.getSize()));

    int legacyMismatches = 0;
    int newNotDefault    = 0;

    for (const char* id : reverb::param::allIds)
    {
        auto* raw = processor.apvts.getRawParameterValue (id);

        if (raw == nullptr)
        {
            ++legacyMismatches;
            ++newNotDefault;
            continue;
        }

        const float loaded = raw->load();

        if (isLegacyId (id))
        {
            if (! nearlyEqual (loaded, legacyExpectedValue (id)))
                ++legacyMismatches;
        }
        else if (! nearlyEqual (loaded, defaultValueOf (processor.apvts, id)))
        {
            ++newNotDefault;
        }
    }

    check ("S1. v0.1 blob: 13 exact, 16 at default",
           legacyMismatches == 0 && newNotDefault == 0,
           fmt ("legacy mismatches=%.0f/13  new params not at default=%.0f/16",
                static_cast<double> (legacyMismatches), static_cast<double> (newNotDefault)));
}

//==================================================================================================
// S2. A v0.2 blob (i.e. one written by THIS build) round-trips all 33 parameters, and stateVersion
// stays 2.
//==================================================================================================
void testS2()
{
    ReverbAudioProcessor source;
    writeProbes (source);

    const auto written = readAllProbes (source);

    juce::MemoryBlock state;
    source.getStateInformation (state);

    ReverbAudioProcessor restored;
    restored.setStateInformation (state.getData(), static_cast<int> (state.getSize()));

    const auto reloaded = readAllProbes (restored);

    int mismatches = 0;

    for (size_t i = 0; i < written.size() && i < reloaded.size(); ++i)
        if (! nearlyEqual (written[i], reloaded[i]))
            ++mismatches;

    const int version = stateVersionOf (restored);

    check ("S2. v0.2 blob round-trips 29 params, stateVersion=2",
           mismatches == 0 && version == reverb::param::currentStateVersion,
           fmt ("mismatches=%.0f/%.0f", static_cast<double> (mismatches),
                static_cast<double> (std::size (stateCompatProbes)))
           + fmt ("  stateVersion=%.0f (want %.0f)", static_cast<double> (version),
                static_cast<double> (reverb::param::currentStateVersion)));
}

//==================================================================================================
// S3. The reused-processor-instance case (C14). Move several new (post-v0.1) parameters away from
// their defaults -- the way a previously-loaded preset would have left them -- THEN load the real
// v0.1 blob into that SAME instance, and assert every one of them lands back at its own default
// rather than staying wherever it was. This is the case S1 cannot exercise, because a freshly
// constructed processor's untouched parameters already sit at their compile-time default with or
// without the reset logic under test.
//==================================================================================================
void testS3()
{
    ReverbAudioProcessor processor;

    struct Move { const char* id; float value; };

    // A few from each remaining phase (1 and 2 -- phase 3's four IDs no longer exist, see the
    // "Phase 3's four IDs" comment in Parameters.h).
    const Move moves[] =
    {
        { reverb::param::idDecayLow,     3.6f },
        { reverb::param::idDensity,     88.0f },
        { reverb::param::idDuckAmount,  70.0f },
        { reverb::param::idBassMono,   310.0f },
        { reverb::param::idPreDelaySync, 1.0f },
        { reverb::param::idWetTilt,    -11.0f },
    };

    for (const auto& m : moves)
        if (auto* parameter = processor.apvts.getParameter (m.id))
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (m.value));

    // Confirm the setup itself worked, so a later failure cannot be mistaken for "never moved".
    int failedToMove = 0;

    for (const auto& m : moves)
    {
        auto* raw = processor.apvts.getRawParameterValue (m.id);

        if (raw == nullptr || ! nearlyEqual (raw->load(), m.value))
            ++failedToMove;
    }

    const auto blob = readV01Blob();

    if (blob.getSize() == 0)
    {
        check ("S3. reused instance: new params reset on old load", false,
               "could not read tests/fixtures/v01-state.bin");
        return;
    }

    processor.setStateInformation (blob.getData(), static_cast<int> (blob.getSize()));

    int stillMoved = 0;

    for (const auto& m : moves)
    {
        auto* raw = processor.apvts.getRawParameterValue (m.id);

        if (raw == nullptr || ! nearlyEqual (raw->load(), defaultValueOf (processor.apvts, m.id)))
            ++stillMoved;
    }

    check ("S3. reused instance: new params reset on old load",
           failedToMove == 0 && stillMoved == 0,
           fmt ("setup moves that failed to land=%.0f/%.0f", static_cast<double> (failedToMove),
                static_cast<double> (std::size (moves)))
           + fmt ("  still non-default after load=%.0f/%.0f", static_cast<double> (stillMoved),
                static_cast<double> (std::size (moves))));
}

} // namespace

//==================================================================================================

#include "TestSuites.h"

// Entry point for the suite runner (tests/TestSuites.h). Returns the failure COUNT, not 0/1, so
// the runner can sum across suites.
int runStateCompatTests()
{
    failureCount = 0;

    std::printf ("StateCompat tests\n");
    std::printf ("------------------\n");

    testS1();
    testS2();
    testS3();

    std::printf ("------------------\n");
    std::printf ("%s (%d failure%s)\n",
                 failureCount == 0 ? "ALL CHECKS PASSED" : "FAILURES",
                 failureCount,
                 failureCount == 1 ? "" : "s");

    return failureCount;
}
