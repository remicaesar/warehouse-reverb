/*
    Headless VST3 host check.

    auval validates the AU, but nothing validated the VST3 -- which is the format Ableton Live and
    FL Studio actually load. pluginval cannot run in this environment (it is a GUI app and cannot
    initialise a window), so this does the equivalent job with JUCE's own VST3 hosting layer:
    scan the real .vst3 bundle, instantiate it as a host would, negotiate buses, run audio through
    it, and check the output.

    This loads the SHIPPED BINARY, not our source, so it exercises the VST3 wrapper end to end.

    Usage:  WarehouseVst3Check [path-to-Warehouse.vst3]
*/

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>

namespace
{
    int failures = 0;

    void check (bool ok, const juce::String& name, const juce::String& detail)
    {
        std::printf ("[%s] %-42s %s\n", ok ? "PASS" : "FAIL",
                     name.toRawUTF8(), detail.toRawUTF8());
        if (! ok)
            ++failures;
    }

    float peakOf (const juce::AudioBuffer<float>& b)
    {
        float p = 0.0f;
        for (int c = 0; c < b.getNumChannels(); ++c)
            p = juce::jmax (p, b.getMagnitude (c, 0, b.getNumSamples()));
        return p;
    }

    bool allFinite (const juce::AudioBuffer<float>& b)
    {
        for (int c = 0; c < b.getNumChannels(); ++c)
            for (int n = 0; n < b.getNumSamples(); ++n)
                if (! std::isfinite (b.getSample (c, n)))
                    return false;
        return true;
    }
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    const juce::String path = argc > 1
        ? juce::String (argv[1])
        : juce::File::getSpecialLocation (juce::File::userHomeDirectory)
              .getChildFile ("Library/Audio/Plug-Ins/VST3/Warehouse.vst3").getFullPathName();

    std::printf ("VST3 host check\n---------------\nbundle: %s\n\n", path.toRawUTF8());

    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> found;

    format.findAllTypesForFile (found, path);

    check (found.size() > 0, "bundle scans and reports a plugin",
           juce::String (found.size()) + " description(s)");

    if (found.isEmpty())
    {
        std::printf ("\nVST3 CHECK FAILED (%d failures)\n", failures + 1);
        return 1;
    }

    const auto& desc = *found[0];
    std::printf ("       name=%s  manufacturer=%s  category=%s  isInstrument=%d  uid=%s\n\n",
                 desc.name.toRawUTF8(), desc.manufacturerName.toRawUTF8(),
                 desc.category.toRawUTF8(), (int) desc.isInstrument,
                 desc.createIdentifierString().toRawUTF8());

    // The rename check: this is what would have caught a half-done rename -- PRODUCT_NAME/
    // PLUGIN_NAME in CMakeLists.txt is "Warehouse" now, so the shipped bundle must report exactly
    // that to a hosting DAW, not a leftover "Reverb". equalsIgnoreCase rather than contains: a
    // substring match would still pass on a stale "ReverbWarehouse" half-rename.
    check (desc.name.equalsIgnoreCase ("Warehouse"), "plugin reports its name as Warehouse", desc.name);
    check (! desc.isInstrument, "declares itself an effect, not a synth", "isInstrument=false");
    // Category is the VST3 taxonomy tag (Fx|Reverb), a genre classification independent of the
    // product name -- this plugin is still an algorithmic reverb, so this stays "Reverb".
    check (desc.category.containsIgnoreCase ("Reverb"),
           "VST3 category includes Reverb", desc.category.isEmpty() ? "(empty)" : desc.category);

    constexpr double sampleRate = 48000.0;
    constexpr int    blockSize  = 512;

    juce::String error;
    auto instance = format.createInstanceFromDescription (desc, sampleRate, blockSize, error);

    check (instance != nullptr, "instantiates as a host would",
           instance != nullptr ? "ok" : error);

    if (instance == nullptr)
    {
        std::printf ("\nVST3 CHECK FAILED (%d failures)\n", failures);
        return 1;
    }

    const auto numParams = instance->getParameters().size();
    check (numParams >= 13, "exposes its parameters to the host",
           juce::String (numParams) + " parameters");

    // List them: a host shows every one of these in its automation menu, so an
    // unexpected extra parameter is a user-visible surprise worth knowing about.
    for (auto* p : instance->getParameters())
        std::printf ("         param %2d  %-24s = %-10s %s\n",
                     p->getParameterIndex(),
                     p->getName (24).toRawUTF8(),
                     p->getText (p->getValue(), 12).toRawUTF8(),
                     p->isAutomatable() ? "" : "(not automatable)");
    std::printf ("\n");

    // Count-agnostic on purpose: the factory preset set is expected to grow, and a hardcoded
    // count here would fail for a reason unrelated to the VST3 wrapper. What matters is that
    // the wrapper exposes them and that every one carries a name.
    bool allNamed = instance->getNumPrograms() > 0;

    for (int i = 0; i < instance->getNumPrograms(); ++i)
        allNamed = allNamed && instance->getProgramName (i).isNotEmpty();

    check (instance->getNumPrograms() >= 9 && allNamed, "exposes its factory presets, all named",
           juce::String (instance->getNumPrograms()) + " programs");

    // --- stereo render -----------------------------------------------------------------------
    instance->setPlayConfigDetails (2, 2, sampleRate, blockSize);
    instance->prepareToPlay (sampleRate, blockSize);

    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    juce::Random rng (1234);

    bool finite = true;
    float worstPeak = 0.0f;

    // 2 seconds of noise, then 2 seconds of silence to hear the tail.
    const int noiseBlocks = (int) (2.0 * sampleRate / blockSize);
    const int tailBlocks  = noiseBlocks;
    float tailEnergy = 0.0f;

    for (int b = 0; b < noiseBlocks + tailBlocks; ++b)
    {
        const bool feeding = b < noiseBlocks;

        for (int c = 0; c < 2; ++c)
            for (int n = 0; n < blockSize; ++n)
                buffer.setSample (c, n, feeding ? (rng.nextFloat() * 2.0f - 1.0f) * 0.5f : 0.0f);

        instance->processBlock (buffer, midi);

        finite    = finite && allFinite (buffer);
        worstPeak = juce::jmax (worstPeak, peakOf (buffer));

        if (! feeding && b < noiseBlocks + 20)
            tailEnergy = juce::jmax (tailEnergy, peakOf (buffer));
    }

    check (finite, "stereo render stays finite (4 s)", "finite=1");
    check (worstPeak < 4.0f, "stereo render does not blow up",
           "worst peak=" + juce::String (worstPeak, 3));
    check (tailEnergy > 1.0e-4f, "produces an audible reverb tail after input stops",
           "tail peak=" + juce::String (tailEnergy, 5));

    // --- mono-in / stereo-out, the common Logic/Live case ------------------------------------
    // A layout can only be changed while the processor is INACTIVE. JUCE's VST3 host wrapper
    // refuses (and jasserts) otherwise -- canApplyBusesLayout carries
    // "someone tried to change the layout while the AudioProcessor is running, call
    // releaseResources first!" (juce_VST3PluginFormatImpl.h:2591-2601). Without this release the
    // query returns false even for a layout the plugin genuinely supports, which is exactly the
    // false negative this check produced on its first run.
    instance->releaseResources();

    const bool monoToStereo = instance->setBusesLayout (
        juce::AudioProcessor::BusesLayout {
            { juce::AudioChannelSet::mono() },
            { juce::AudioChannelSet::stereo() } });

    check (monoToStereo, "accepts mono-in / stereo-out", monoToStereo ? "supported" : "REJECTED");

    if (monoToStereo)
    {
        instance->prepareToPlay (sampleRate, blockSize);

        juce::AudioBuffer<float> mono (2, blockSize);
        bool monoFinite = true;
        float monoPeak = 0.0f;

        for (int b = 0; b < 200; ++b)
        {
            mono.clear();
            for (int n = 0; n < blockSize; ++n)
                mono.setSample (0, n, (rng.nextFloat() * 2.0f - 1.0f) * 0.5f);

            instance->processBlock (mono, midi);
            monoFinite = monoFinite && allFinite (mono);
            monoPeak = juce::jmax (monoPeak, peakOf (mono));
        }

        check (monoFinite && monoPeak > 1.0e-3f, "mono-in / stereo-out renders",
               "peak=" + juce::String (monoPeak, 3) + " finite=" + juce::String ((int) monoFinite));
    }

    // --- host-side state round-trip through the VST3 wrapper ---------------------------------
    if (auto* mixParam = instance->getParameters()[0])
    {
        const float original = mixParam->getValue();
        mixParam->setValue (original < 0.5f ? 0.9f : 0.1f);
        const float changed = mixParam->getValue();

        juce::MemoryBlock state;
        instance->getStateInformation (state);

        mixParam->setValue (original);
        instance->setStateInformation (state.getData(), (int) state.getSize());

        check (std::abs (mixParam->getValue() - changed) < 1.0e-4f,
               "state round-trip through the VST3 wrapper",
               "restored=" + juce::String (mixParam->getValue(), 4)
                   + " expected=" + juce::String (changed, 4));
        check (state.getSize() > 0, "state block is non-empty",
               juce::String ((int) state.getSize()) + " bytes");
    }

    instance->releaseResources();
    instance.reset();

    std::printf ("\n%s (%d failures)\n",
                 failures == 0 ? "VST3 CHECK PASSED" : "VST3 CHECK FAILED", failures);
    return failures == 0 ? 0 : 1;
}
