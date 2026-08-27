#pragma once

/*
    Suite registry.

    Each test translation unit exposes ONE entry point returning its failure count. A single
    main() per target (EngineTestMain.cpp / ProcessorTestMain.cpp) calls them and sums.

    Why: two test .cpp files each carrying their own main() cannot link into one target, and the
    plan adds one test file per builder assignment so that parallel assignments never share a
    file. This header and the two mains are CTO-owned precisely because a file listing every
    suite is the one place all those assignments would otherwise collide.

    Contract for a new suite:
      - define `int runXxxTests();` in its own .cpp,
      - return the failure COUNT (0 = all passed) -- do NOT return 1, the runner sums,
      - print its own header/footer; the runner prints the grand total,
      - declare it below, and ask the CTO to add it to the CMake source list.
*/

// --- WarehouseTests target (DSP engine; links only juce_dsp + juce_audio_basics) ---
int runEngineTests();
int runDecayTests();              // step 2A
int runEarlyReflectionsTests();   // step 2B
int runDensityTests();            // step 3A
int runHouseTests();              // step 5A
int runWetPathTests();            // step 5B

// --- WarehouseProcessorTests target (full processor + editor) ---
int runProcessorTests();
int runStateCompatTests();
