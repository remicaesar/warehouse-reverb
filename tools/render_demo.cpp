/*
    Offline renderer: writes WAV files so the reverb can be JUDGED BY EAR rather than by test
    output. Synthesises its own house-tempo source material, runs it through the real plugin
    processor, and writes one file per setting.

    No audio device, no DAW, no window server -- so it works in a headless/agent session, and the
    user can drag the results into anything.

    Usage:  WarehouseRender [output-directory]     (default: ./renders)
*/

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include "Parameters.h"
#include "PluginProcessor.h"

namespace
{
    constexpr double sampleRate = 48000.0;
    constexpr int    blockSize  = 512;
    constexpr double bpm        = 124.0;          // house
    constexpr double beatSecs   = 60.0 / bpm;

    struct Note { double startBeats, freqHz, lengthBeats, gain; };

    /** A short percussive body: pitch drop + fast exponential decay. Kick-ish / tom-ish. */
    void addDrum (juce::AudioBuffer<float>& b, double atSec, double f0, double f1,
                  double decaySec, float gain)
    {
        const int start = (int) (atSec * sampleRate);
        const int len   = (int) (decaySec * 4.0 * sampleRate);
        double phase = 0.0;

        for (int n = 0; n < len; ++n)
        {
            const int i = start + n;
            if (i < 0 || i >= b.getNumSamples()) break;

            const double t   = (double) n / sampleRate;
            const double env = std::exp (-t / decaySec);
            const double f   = f1 + (f0 - f1) * std::exp (-t / (decaySec * 0.35));

            phase += 2.0 * juce::MathConstants<double>::pi * f / sampleRate;

            const float s = (float) (std::sin (phase) * env) * gain;
            b.addSample (0, i, s);
            b.addSample (1, i, s);
        }
    }

    /** Filtered noise burst: clap / hat / snare-ish transient, the thing early reflections show up on. */
    void addNoise (juce::AudioBuffer<float>& b, double atSec, double decaySec, float gain,
                   juce::Random& rng, float lowpass)
    {
        const int start = (int) (atSec * sampleRate);
        const int len   = (int) (decaySec * 5.0 * sampleRate);
        float z = 0.0f;

        for (int n = 0; n < len; ++n)
        {
            const int i = start + n;
            if (i < 0 || i >= b.getNumSamples()) break;

            const double t   = (double) n / sampleRate;
            const float  env = (float) std::exp (-t / decaySec);

            const float white = rng.nextFloat() * 2.0f - 1.0f;
            z += lowpass * (white - z);

            b.addSample (0, i, z * env * gain);
            b.addSample (1, i, z * env * gain * 0.92f);   // slight stereo offset
        }
    }

    /** Sustained-ish chord stab: the classic house Rhodes/organ chord that reverb is judged on. */
    void addChord (juce::AudioBuffer<float>& b, double atSec, const std::vector<double>& freqs,
                   double lengthSec, float gain)
    {
        const int start = (int) (atSec * sampleRate);
        const int len   = (int) (lengthSec * sampleRate);

        for (int n = 0; n < len; ++n)
        {
            const int i = start + n;
            if (i < 0 || i >= b.getNumSamples()) break;

            const double t = (double) n / sampleRate;
            // Fast attack, gentle decay, short release -- a stab, not a pad.
            const double env = std::min (1.0, t / 0.004) * std::exp (-t / (lengthSec * 0.45));

            double sum = 0.0;
            for (size_t k = 0; k < freqs.size(); ++k)
            {
                const double f = freqs[k];
                // A little odd harmonic so it is not a pure sine wall.
                sum += std::sin (2.0 * juce::MathConstants<double>::pi * f * t)
                     + 0.22 * std::sin (2.0 * juce::MathConstants<double>::pi * f * 3.0 * t);
            }

            const float s = (float) (sum / (double) (freqs.size() * 2) * env) * gain;
            b.addSample (0, i, s);
            b.addSample (1, i, s);
        }
    }

    /** Two bars of house at 124 bpm, then two bars of silence so the TAIL is audible on its own. */
    juce::AudioBuffer<float> makeSource()
    {
        const double barSecs   = beatSecs * 4.0;
        const double totalSecs = barSecs * 2.0 + 6.0;      // 6 s of tail room

        juce::AudioBuffer<float> b (2, (int) (totalSecs * sampleRate));
        b.clear();

        juce::Random rng (20260827);

        // Am7 stab, off-beat, twice per bar -- where the Decay EQ is most audible.
        const std::vector<double> am7 { 220.0, 261.63, 329.63, 392.00 };

        for (int bar = 0; bar < 2; ++bar)
        {
            const double t0 = bar * barSecs;

            for (int beat = 0; beat < 4; ++beat)
                addDrum (b, t0 + beat * beatSecs, 150.0, 48.0, 0.11, 0.85f);       // four-to-the-floor

            addNoise (b, t0 + beatSecs * 1.0, 0.035, 0.30f, rng, 0.55f);           // hats on the offs
            addNoise (b, t0 + beatSecs * 1.5, 0.018, 0.22f, rng, 0.75f);
            addNoise (b, t0 + beatSecs * 3.0, 0.035, 0.30f, rng, 0.55f);
            addNoise (b, t0 + beatSecs * 3.5, 0.018, 0.22f, rng, 0.75f);
            addNoise (b, t0 + beatSecs * 2.0, 0.090, 0.40f, rng, 0.35f);           // clap on 3

            addChord (b, t0 + beatSecs * 0.5, am7, beatSecs * 0.9, 0.55f);
            addChord (b, t0 + beatSecs * 2.5, am7, beatSecs * 1.4, 0.55f);
        }

        return b;
    }

    bool writeWav (const juce::File& file, const juce::AudioBuffer<float>& buffer)
    {
        file.deleteFile();

        juce::WavAudioFormat wav;
        auto stream = std::unique_ptr<juce::FileOutputStream> (file.createOutputStream());

        if (stream == nullptr)
            return false;

        // JUCE 9 deprecated the raw-pointer createWriterFor in favour of the options builder.
        std::unique_ptr<juce::OutputStream> out (stream.release());

        const auto options = juce::AudioFormatWriterOptions{}
                                 .withSampleRate (sampleRate)
                                 .withNumChannels (2)
                                 .withBitsPerSample (24);

        auto writer = wav.createWriterFor (out, options);

        if (writer == nullptr)
            return false;

        return writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
    }

    /** Reports a fixed tempo, so the tempo-synced pre-delay has something to sync to.
        Without a playhead the plugin correctly falls back to the typed millisecond value, which
        would make a "synced" render indistinguishable from an unsynced one -- i.e. the leg would
        look like it worked while proving nothing.
    */
    class StubPlayHead final : public juce::AudioPlayHead
    {
    public:
        explicit StubPlayHead (double bpmIn) : bpm (bpmIn) {}

        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo info;
            info.setBpm (bpm);
            info.setIsPlaying (true);
            info.setTimeInSamples (samplesElapsed);
            info.setPpqPosition ((double) samplesElapsed / sampleRate * bpm / 60.0);
            return info;
        }

        void advance (int numSamples) noexcept { samplesElapsed += numSamples; }

    private:
        double bpm;
        juce::int64 samplesElapsed { 0 };
    };

    void setParam (ReverbAudioProcessor& p, const char* id, float value)
    {
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (p.apvts.getParameter (id)))
            ranged->setValueNotifyingHost (ranged->convertTo0to1 (value));
    }

    /** Renders the source through a freshly-configured processor. `configure` sets the parameters. */
    juce::AudioBuffer<float> render (const juce::AudioBuffer<float>& source,
                                     const std::function<void (ReverbAudioProcessor&)>& configure,
                                     bool withTempo = false)
    {
        ReverbAudioProcessor processor;
        processor.setPlayConfigDetails (2, 2, sampleRate, blockSize);
        processor.prepareToPlay (sampleRate, blockSize);

        StubPlayHead playHead (bpm);

        if (withTempo)
            processor.setPlayHead (&playHead);

        configure (processor);

        juce::AudioBuffer<float> out (2, source.getNumSamples());
        out.makeCopyOf (source);

        juce::MidiBuffer midi;

        for (int pos = 0; pos < out.getNumSamples(); pos += blockSize)
        {
            const int n = std::min (blockSize, out.getNumSamples() - pos);

            float* chans[2] = { out.getWritePointer (0, pos), out.getWritePointer (1, pos) };
            juce::AudioBuffer<float> slice (chans, 2, n);

            processor.processBlock (slice, midi);
            playHead.advance (n);
        }

        // The processor must not outlive the playhead it points at.
        processor.setPlayHead (nullptr);

        return out;
    }

    float peakOf (const juce::AudioBuffer<float>& b)
    {
        float p = 0.0f;
        for (int c = 0; c < b.getNumChannels(); ++c)
            p = juce::jmax (p, b.getMagnitude (c, 0, b.getNumSamples()));
        return p;
    }
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    const auto cwd    = juce::File::getCurrentWorkingDirectory();
    const auto outDir = argc > 1 ? cwd.getChildFile (juce::String (argv[1])) : cwd.getChildFile ("renders");
    outDir.createDirectory();

    const auto source = makeSource();

    std::printf ("Warehouse offline renderer\n--------------------------\n");
    std::printf ("source: 2 bars of house at %.0f bpm + 6 s of tail room, %.1f s total\n",
                 bpm, (double) source.getNumSamples() / sampleRate);
    std::printf ("output: %s\n\n", outDir.getFullPathName().toRawUTF8());

    int failures = 0;

    auto emit = [&] (const juce::String& name, const juce::AudioBuffer<float>& buffer)
    {
        const auto file = outDir.getChildFile (name + ".wav");

        if (writeWav (file, buffer))
            std::printf ("  %-34s peak %5.3f  %s\n", name.toRawUTF8(), peakOf (buffer),
                         file.getFileName().toRawUTF8());
        else
        {
            std::printf ("  [FAIL] could not write %s\n", file.getFullPathName().toRawUTF8());
            ++failures;
        }
    };

    emit ("00-dry-source", source);

    // --- every factory preset, so the voicing can be judged as a set ---
    std::printf ("\nfactory presets:\n");
    {
        ReverbAudioProcessor probe;
        const int numPresets = probe.getNumPrograms();

        for (int i = 0; i < numPresets; ++i)
        {
            const auto presetName = probe.getProgramName (i);

            // Preset names carry em dashes ("Drums — Tight Room"), and replaceCharacter(' ', '-')
            // would leave them in the filename verbatim. Writes fine on APFS but is awkward from a
            // shell, so reduce to [a-z0-9-] and collapse runs of dashes.
            juce::String slug;

            for (auto c : presetName.toLowerCase())
                slug << (juce::CharacterFunctions::isLetterOrDigit (c) ? juce::String::charToString (c)
                                                                       : juce::String ("-"));

            while (slug.contains ("--"))
                slug = slug.replace ("--", "-");

            emit (juce::String::formatted ("10-preset-%02d-", i) + slug.trimCharactersAtEnd ("-"),
                  render (source, [i] (ReverbAudioProcessor& p) { p.setCurrentProgram (i); }));
        }
    }

    // --- the flagship feature, as an A/B a listener can actually hear ---
    // Same everything except the Decay EQ. If these two do not sound obviously different,
    // the Decay Rate EQ is not working.
    std::printf ("\ndecay EQ A/B (the flagship feature):\n");

    auto decayAb = [&] (const char* label, float lowMult, float highMult)
    {
        emit (juce::String ("20-decay-eq-") + label,
              render (source, [lowMult, highMult] (ReverbAudioProcessor& p)
              {
                  setParam (p, reverb::param::idMix,       45.0f);
                  setParam (p, reverb::param::idDecay,      4.0f);
                  setParam (p, reverb::param::idSize,     120.0f);
                  setParam (p, reverb::param::idPreDelay,  25.0f);
                  setParam (p, reverb::param::idDecayLow,  lowMult);
                  setParam (p, reverb::param::idDecayHigh, highMult);
              }));
    };

    decayAb ("flat",        1.0f, 1.0f);
    decayAb ("dark-tail",   2.5f, 0.15f);   // bass lingers, treble dies -- a real room
    decayAb ("bright-tail", 0.2f, 2.5f);    // the opposite; should sound obviously wrong/thin

    // --- ducking: THE house reverb move. Kick on every beat, so the pumping is obvious. ---
    std::printf ("\nducking (the most-used dance-music reverb trick):\n");

    auto duckLeg = [&] (const char* label, float amount, int character)
    {
        emit (juce::String ("30-duck-") + label,
              render (source, [amount, character] (ReverbAudioProcessor& p)
              {
                  setParam (p, reverb::param::idMix,          60.0f);
                  setParam (p, reverb::param::idDecay,         3.2f);
                  setParam (p, reverb::param::idSize,        130.0f);
                  setParam (p, reverb::param::idPreDelay,     20.0f);
                  setParam (p, reverb::param::idDuckAmount,   amount);
                  setParam (p, reverb::param::idDuckThresh,  -30.0f);
                  setParam (p, reverb::param::idDuckRelease, 220.0f);
                  setParam (p, reverb::param::idDuckChar,     (float) character);
              }));
    };

    duckLeg ("00-off",      0.0f,   0);
    duckLeg ("01-gentle", 100.0f,   0);
    duckLeg ("02-pump",   100.0f,   1);

    // --- tempo-synced pre-delay, against the same setting unsynced ---
    std::printf ("\ntempo-synced pre-delay (%.0f bpm; 1/8 = %.1f ms):\n",
                 bpm, 60000.0 / bpm / 2.0);

    auto syncLeg = [&] (const char* label, bool synced, int division, float freeMs)
    {
        emit (juce::String ("40-sync-") + label,
              render (source, [synced, division, freeMs] (ReverbAudioProcessor& p)
              {
                  setParam (p, reverb::param::idMix,      50.0f);
                  setParam (p, reverb::param::idDecay,     2.6f);
                  setParam (p, reverb::param::idPreDelay, freeMs);
                  setParam (p, reverb::param::idPreDelaySync, synced ? 1.0f : 0.0f);
                  setParam (p, reverb::param::idPreDelayDiv,  (float) division);
              }, true));
    };

    syncLeg ("00-off-10ms",  false, 5, 10.0f);
    syncLeg ("01-eighth",    true,  5, 10.0f);   // 1/8
    syncLeg ("02-sixteenth", true,  2, 10.0f);   // 1/16

    std::printf ("\n%s (%d failure%s)\n",
                 failures == 0 ? "RENDERS WRITTEN" : "RENDER FAILURES",
                 failures, failures == 1 ? "" : "s");

    return failures == 0 ? 0 : 1;
}
