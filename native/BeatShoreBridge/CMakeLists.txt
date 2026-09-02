cmake_minimum_required(VERSION 3.22)

project(BeatShoreBridge VERSION 0.1.0)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../JUCE JUCE)

# This is the thin bridge described in the implementation plan's Section 2:
# audio/MIDI passthrough plus read-only host context (sample rate, block
# size, tempo, time signature, transport, playhead), plus a real IPC client
# (Source/BridgeClient.h) that connects to the BeatShore desktop process
# over the named-pipe protocol in native/protocol/PROTOCOL.md and can run a
# tempo analysis on captured audio. See BridgeClient.h for the connection
# state machine and STATUS.md for what's still not wired up (only the
# "tempo" analysis kind has a UI trigger so far).
juce_add_plugin(BeatShoreBridge
    COMPANY_NAME "BeatShore"
    PLUGIN_MANUFACTURER_CODE Bsts
    PLUGIN_CODE Brdg
    FORMATS VST3
    PRODUCT_NAME "BeatShore Bridge"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT TRUE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_CATEGORIES "Fx"
    COPY_PLUGIN_AFTER_BUILD FALSE
)

target_sources(BeatShoreBridge
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
        Source/BridgeClient.h
        Source/BridgeTypes.h
        Source/MixChain.h
        Source/MasterMeter.h
        # libebur128 (MIT, vendored unmodified in Source/third_party/
        # libebur128 -- see that directory's VENDORED.md): real EBU R128
        # loudness/true-peak measurement for the Master page, wrapped by
        # MasterMeter.h above. A plain C library, not JUCE/C++.
        Source/third_party/libebur128/ebur128.c
        Source/third_party/libebur128/ebur128.h
)

# ebur128.c does `#include <sys/queue.h>` (angle brackets -- a real system
# header on BSD/Linux, not available on MSVC) -- libebur128 ships its own
# vendored replacement at third_party/libebur128/queue/sys/queue.h; this is
# what puts that directory on the include path so the angle-bracket include
# resolves to the vendored copy instead of failing to find a system one.
target_include_directories(BeatShoreBridge
    PRIVATE
        Source/third_party/libebur128/queue
)

# ebur128.c's own top-of-file comment says it outright: "You may have to
# define _USE_MATH_DEFINES if you use MSVC" -- MSVC's <math.h> doesn't
# define M_PI unless this is set before it's included, unlike glibc's.
# Scoped to just this one source file (via CMake source-file properties,
# not a project-wide define) so it stays true that nothing in the vendored
# file itself was edited -- see third_party/libebur128/VENDORED.md.
set_source_files_properties(Source/third_party/libebur128/ebur128.c PROPERTIES
    COMPILE_DEFINITIONS "_USE_MATH_DEFINES"
)

target_compile_definitions(BeatShoreBridge
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
)

# Without this, MSVC compiles narrow string/char literals using the active
# Windows code page instead of UTF-8, silently mangling any non-ASCII
# character in a source file's string literals (an em-dash rendered as
# "a-box-box" in a real REAPER session is exactly this bug -- caught by
# manual testing, not by anything automated, since it's a text-rendering
# issue no headless test observes). juce::String's narrow-char constructor
# assumes UTF-8 input, so this flag is what makes that assumption actually
# hold at the compiler level.
if (MSVC)
    target_compile_options(BeatShoreBridge PRIVATE /utf-8)
endif()

target_link_libraries(BeatShoreBridge
    PRIVATE
        juce::juce_audio_utils
        # juce_dsp: real-time EQ/Compressor/Limiter (native/BeatShoreBridge/
        # Source/MixChain.h) run in processBlock() itself, processing the
        # live audio actually flowing through this plugin in the host --
        # not a separate analysis request to the desktop broker like every
        # other feature so far. Already vendored in native/JUCE, no new
        # dependency to fetch.
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)
