cmake_minimum_required(VERSION 3.22)

# JUCE 9 hard-errors ("A C compiler is required...") unless C is in LANGUAGES,
# because several of its modules contain C sources.
project(Warehouse VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# FORCE, for the same reason CMAKE_OSX_ARCHITECTURES below carries it, and this one was
# missed. project() has already created CMAKE_OSX_DEPLOYMENT_TARGET as a cache entry by
# the time this line runs, so a set(... CACHE ...) WITHOUT FORCE is silently a no-op: the
# entry stays empty, no -mmacosx-version-min reaches the compiler, and every bundle is
# stamped with the build machine's SDK version instead of 11.0. Measured before the fix on
# a macOS 26.5 SDK: `otool -l` reported `minos 26.0` on both slices of a universal build,
# i.e. a plugin that refuses to load on every macOS this project claims to support. The
# defect is invisible in the build log and in the source, which is why ci.yml asserts the
# resulting minos rather than trusting this line.
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version" FORCE)

option(UNIVERSAL_BINARY "Build a fat arm64 + x86_64 binary" OFF)

# Whether JUCE's post-build step copies the built bundles into the user's plugin
# folders. ON is right for local development -- it is what makes a rebuild
# immediately visible in Logic. It is WRONG for two cases, and both bit us:
#
#   * CI, where on Windows the destination is %CommonProgramFiles%\VST3 and needs
#     elevation the job should not depend on;
#   * any build that deliberately mutates the plugin (a red-proof, a bisect),
#     because the copy step lands the mutated bundle on top of the user's LIVE
#     install. That happened: a red-proof that temporarily reverted PRODUCT_NAME
#     to "Reverb" overwrote ~/Library/Audio/Plug-Ins/VST3/Reverb.vst3 with a
#     mutant. Nothing was lost, but only because a byte-identical reference build
#     still existed to restore from.
#
# So: mutation and packaging builds pass -DWAREHOUSE_INSTALL_AFTER_BUILD=OFF and cannot
# touch anything installed.
# NAME: deliberately not "COPY_PLUGIN_AFTER_BUILD". JUCE defines its own global
# cache option JUCE_COPY_PLUGIN_AFTER_BUILD, and the two sat 13 lines apart in
# CMakeCache.txt. That cost us a second incident: an agent read
# JUCE_COPY_PLUGIN_AFTER_BUILD:BOOL=OFF, concluded installing was already
# disabled, skipped reconfiguring, and its build promptly overwrote the live
# installed bundles. The per-target keyword below is what actually governs the
# copy, and JUCE's global does not control it. So this option carries a prefix
# that cannot be mistaken for JUCE's.
option(WAREHOUSE_INSTALL_AFTER_BUILD "Copy built plugin bundles into the user's plug-in folders" ON)

# Both branches FORCE the cache entry. Without the else(), running
# `build.sh --universal` once in a build dir left CMAKE_OSX_ARCHITECTURES cached
# as fat forever, so every later plain build in that dir silently stayed fat.
if(UNIVERSAL_BINARY)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
else()
    set(CMAKE_OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}" CACHE STRING "" FORCE)
endif()

add_subdirectory(libs/JUCE)

# ==================================================================================================
# Plugin
# ==================================================================================================

juce_add_plugin(Warehouse
    COMPANY_NAME                "Emir Sezer"
    BUNDLE_ID                   "com.emirsezer.warehouse"
    PLUGIN_MANUFACTURER_CODE    Emsz
    PLUGIN_CODE                 Whs1
    FORMATS                     AU VST3 Standalone
    PRODUCT_NAME                "Warehouse"
    PLUGIN_NAME                 "Warehouse"
    DESCRIPTION                 "House-focused algorithmic reverb"
    IS_SYNTH                    FALSE
    NEEDS_MIDI_INPUT            FALSE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    # Without this, JUCE emits a resourceUsage dict into the AU's Info.plist
    # declaring network.client and read-write access to every file on disk
    # (juce_PlistOptions.cpp:342-353). This reverb touches neither, so declaring
    # it sandbox-safe is both accurate and strictly less permissive.
    AU_SANDBOX_SAFE             TRUE
    # Puts the plugin in the host's Reverb category rather than generic Fx.
    VST3_CATEGORIES             "Fx" "Reverb"
    COPY_PLUGIN_AFTER_BUILD     ${WAREHOUSE_INSTALL_AFTER_BUILD}
    NEEDS_WEB_BROWSER           FALSE
    NEEDS_CURL                  FALSE)

# The DSP sources are shared with the test target, so keep them in one list.
set(WAREHOUSE_DSP_SOURCES
    src/dsp/Primes.cpp
    src/dsp/DelayLineFrac.cpp
    src/dsp/Filters.cpp
    src/dsp/SchroederAllpass.cpp
    src/dsp/Diffuser.cpp
    # Round 2 (PLAN-R2). Some of these are header-only classes whose .cpp is an include-only
    # translation unit, matching the existing SchroederAllpass.cpp pattern, so that every DSP
    # class has a stable slot in this list and adding one never needs a CMake edit mid-phase.
    src/dsp/DecayCurve.cpp
    src/dsp/Shelf.cpp
    src/dsp/AttenuationFilter.cpp
    src/dsp/EarlyReflections.cpp
    src/dsp/Ducker.cpp
    src/dsp/TempoSync.cpp
    src/dsp/WetChain.cpp
    src/dsp/PitchShifter.cpp
    src/dsp/Saturator.cpp
    src/dsp/Voicing.cpp
    src/dsp/FDNTank.cpp
    src/dsp/ReverbEngine.cpp)

# Processor + editor + GUI + DSP: shared with the processor-level test and snapshot targets.
set(WAREHOUSE_PLUGIN_SOURCES
    src/Parameters.cpp
    src/PluginProcessor.cpp
    src/PluginEditor.cpp
    src/gui/ReverbLookAndFeel.cpp
    src/gui/KnobComponent.cpp
    src/gui/LevelMeter.cpp
    src/gui/DecayCurveDisplay.cpp
    src/gui/PageStrip.cpp
    ${WAREHOUSE_DSP_SOURCES})

target_sources(Warehouse PRIVATE ${WAREHOUSE_PLUGIN_SOURCES})

target_include_directories(Warehouse PRIVATE src)

target_compile_definitions(Warehouse
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(Warehouse
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_dsp
        juce::juce_gui_basics
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# --------------------------------------------------------------------------------------------------
# Ad-hoc codesign the AU and VST3 bundles.
#
# Logic Pro on Apple Silicon refuses to load an AU that carries no signature at all, so sign every
# bundle format in place right after JUCE's own copy step has run.
#
# No --deep: these bundles contain no nested code (just Contents/MacOS/<binary> and
# Contents/Resources/moduleinfo.json), and --deep is deprecated for signing.
#
# The build log carries three codesign warnings ("code has no resources but signature indicates
# they must be present", "a sealed resource is missing or invalid"). They come from JUCE's OWN
# signing passes, which run before this step -- JUCE signs the VST3, then writes moduleinfo.json,
# which invalidates that signature, then re-signs. Verified after a clean build: both installed
# bundles report "valid on disk" and "satisfies its Designated Requirement" under
# `codesign --verify --strict`, with moduleinfo.json sealed. The warnings are noise, not damage.
#
# GUARDED ON APPLE, NOT ON if(TARGET ...). The target test is true on Windows too -- VST3 and
# Standalone targets exist there -- and it is the wrong question: what this block needs is not "does
# the target exist" but "is it a macOS bundle". $<TARGET_BUNDLE_DIR:...> is only valid for bundle
# targets, so on Windows the guard passed and CMake failed at GENERATE time, before a single file
# compiled:
#
#     CMake Error at CMakeLists.txt:154 (add_custom_command):
#       Error evaluating generator expression:
#         $<TARGET_BUNDLE_DIR:Warehouse_VST3>
#       TARGET_BUNDLE_DIR is allowed only for Bundle targets.
#     CMake Generate step failed.  Build files cannot be regenerated correctly.
#
# That is the whole of the Windows CI failure, and it means the Windows build had never once
# succeeded. codesign is a macOS tool and this block is macOS-only by nature, so APPLE is both the
# correct guard and the one that states the intent.
# --------------------------------------------------------------------------------------------------

if(APPLE)
    foreach(format AU VST3)
        if(TARGET Warehouse_${format})
            add_custom_command(TARGET Warehouse_${format} POST_BUILD
                COMMAND codesign --force --sign - "$<TARGET_BUNDLE_DIR:Warehouse_${format}>"
                COMMENT "Ad-hoc signing Warehouse ${format}"
                VERBATIM)
        endif()
    endforeach()
endif()

# ==================================================================================================
# Tests
#
# Deliberately links only juce_dsp / juce_audio_basics: the DSP must stay free of any GUI dependency
# so this target keeps building even when the editor does not.
# ==================================================================================================

juce_add_console_app(WarehouseTests
    PRODUCT_NAME      "WarehouseTests"
    NEEDS_WEB_BROWSER FALSE
    NEEDS_CURL        FALSE)

# One main() per target lives in the *TestMain.cpp runner; every other test file exposes a
# runXxxTests() entry point instead (see tests/TestSuites.h). This is what lets each builder
# assignment add its own test file without two assignments ever sharing one.
target_sources(WarehouseTests
    PRIVATE
        tests/EngineTestMain.cpp
        tests/ReverbEngineTests.cpp
        tests/DecayTests.cpp
        tests/EarlyReflectionsTests.cpp
        tests/DensityTests.cpp
        tests/HouseTests.cpp
        tests/WetPathTests.cpp
        ${WAREHOUSE_DSP_SOURCES})

target_include_directories(WarehouseTests PRIVATE src)

target_compile_definitions(WarehouseTests
    PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_STANDALONE_APPLICATION=1)

# LTO deliberately matches the plugin target. Without it this binary measures a configuration
# we do not ship: cross-translation-unit calls to small DSP methods cannot be inlined, so the CPU
# probe over-reports, and it over-reports MORE as the DSP is split into more classes. Step 1b
# added ten classes' worth of call boundaries and the figure moved ~4% on wiring that is provably
# inert -- which is a measurement artefact, not a cost the plugin pays.
target_link_libraries(WarehouseTests
    PRIVATE
        juce::juce_dsp
        juce::juce_audio_basics
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# ==================================================================================================
# Processor-level tests and the headless GUI snapshot tool.
#
# These compile the full processor + editor + GUI + DSP directly, so they can reach the layers
# WarehouseTests deliberately cannot: state round-trip, buses negotiation, programs, and the editor.
# They do NOT link the `Warehouse` plugin target -- its JUCE modules are PRIVATE and its interface
# defines JUCE_STANDALONE_APPLICATION differently from juce_add_console_app, which collides.
# JucePlugin_Name is normally injected by juce_add_plugin, so it is supplied here by hand.
#
# WarehouseSnapshot renders the editor offscreen via Component::createComponentSnapshot: no display,
# no window server, no Screen Recording permission needed. It is the only way to actually inspect
# the GUI in this environment.
# ==================================================================================================

foreach(app WarehouseProcessorTests WarehouseSnapshot WarehouseVst3Check WarehouseCaptureState WarehouseRender)
    juce_add_console_app(${app}
        PRODUCT_NAME      "${app}"
        NEEDS_WEB_BROWSER FALSE
        NEEDS_CURL        FALSE)

    target_include_directories(${app} PRIVATE src)

    target_compile_definitions(${app}
        PRIVATE
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0
            JUCE_VST3_CAN_REPLACE_VST2=0
            JucePlugin_Name="Warehouse")

    target_link_libraries(${app}
        PRIVATE
            juce::juce_audio_utils
            juce::juce_audio_processors
            juce::juce_dsp
            juce::juce_gui_basics
            juce::juce_gui_extra
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)
endforeach()

target_sources(WarehouseProcessorTests PRIVATE
        tests/ProcessorTestMain.cpp
        tests/ProcessorTests.cpp
        tests/StateCompatTests.cpp
        ${WAREHOUSE_PLUGIN_SOURCES})
target_sources(WarehouseSnapshot       PRIVATE tools/snapshot_main.cpp    ${WAREHOUSE_PLUGIN_SOURCES})

# Captures a real pre-upgrade state blob as a test fixture. Must be run BEFORE the parameter
# layout changes; after that a genuine v0.1 document can no longer be produced.
target_sources(WarehouseCaptureState   PRIVATE tools/capture_state.cpp    ${WAREHOUSE_PLUGIN_SOURCES})

# Offline WAV renderer, so the reverb can be judged BY EAR rather than by test output. Synthesises
# its own house-tempo source material and renders every preset plus a Decay-EQ A/B. Needs
# juce_audio_formats for the WAV writer; nothing else links it.
target_sources(WarehouseRender         PRIVATE tools/render_demo.cpp     ${WAREHOUSE_PLUGIN_SOURCES})
target_link_libraries(WarehouseRender  PRIVATE juce::juce_audio_formats)

# WarehouseVst3Check HOSTS the shipped .vst3 binary rather than compiling our sources, so it needs
# JUCE's VST3 hosting layer and must NOT link the plugin sources (that would test the wrong thing).
target_sources(WarehouseVst3Check PRIVATE tools/vst3_host_check.cpp)
target_compile_definitions(WarehouseVst3Check PRIVATE JUCE_PLUGINHOST_VST3=1)
