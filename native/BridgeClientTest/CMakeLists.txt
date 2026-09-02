cmake_minimum_required(VERSION 3.22)

project(BridgeClientTest VERSION 0.1.0)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../JUCE JUCE)

# Drives the REAL BridgeClient class (native/BeatShoreBridge/Source/
# BridgeClient.h) exactly as BeatShoreBridgeAudioProcessor uses it -- not a
# reimplementation, the same header, unmodified. Proves the plugin's actual
# IPC client code connects to a running BeatShoreDesktop and completes a
# real ANALYSIS_REQUEST, without needing a full VST3 host / DAW GUI to do
# it. BeatShoreTestClient (native/BeatShoreTestClient) proved the wire
# protocol and desktop side; this proves the plugin side's own class.
juce_add_console_app(BridgeClientTest PRODUCT_NAME "BridgeClientTest")

target_sources(BridgeClientTest PRIVATE Source/main.cpp)

target_compile_definitions(BridgeClientTest
    PRIVATE
        JucePlugin_VersionString="0.1.0-integrationtest"
)

target_link_libraries(BridgeClientTest
    PRIVATE
        juce::juce_core
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

# Compiles the REAL PluginProcessor.cpp/PluginEditor.cpp (unmodified) into a
# console harness that stress-tests the double-buffered capture swap under
# real concurrent processBlock() calls -- see Source/stress_main.cpp. Needs
# the same JucePlugin_* macros juce_add_plugin would normally generate for
# the two symbols this code actually references (checked via grep -- just
# these two); it does not link juce_audio_plugin_client, so none of the
# VST3/AU wrapper-specific macros are needed.
juce_add_console_app(BridgeStressTest PRODUCT_NAME "BridgeStressTest")

target_sources(BridgeStressTest
    PRIVATE
        Source/stress_main.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../BeatShoreBridge/Source/PluginProcessor.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../BeatShoreBridge/Source/PluginEditor.cpp
        # juce_dsp (MixChain.h) and libebur128 (MasterMeter.h) -- both
        # added to PluginProcessor.cpp/.h after this target was first
        # written; without these two lines this target fails to
        # link (undefined ebur128_* symbols) or even compile
        # (missing juce_dsp headers). Same deps StateRoundTripTest
        # below needs, for the same reason.
        ${CMAKE_CURRENT_SOURCE_DIR}/../BeatShoreBridge/Source/third_party/libebur128/ebur128.c
)

target_include_directories(BridgeStressTest
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../BeatShoreBridge/Source/third_party/libebur128/queue
)

set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/../BeatShoreBridge/Source/third_party/libebur128/ebur128.c PROPERTIES
    COMPILE_DEFINITIONS "_USE_MATH_DEFINES"
)

target_compile_definitions(BridgeStressTest
    PRIVATE
        JucePlugin_Name="BeatShore Bridge"
        JucePlugin_VersionString="0.1.0-stresstest"
)

target_link_libraries(BridgeStressTest
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

# Deterministic tests for the overlapped-I/O hardening in BridgeClient.h /
# OverlappedPipeIO.h -- drives the REAL BridgeClient class against a mock
# server this test fully controls, so "the desktop never responds" is a
# guarantee, not a hope. See Source/io_hardening_test.cpp. Doesn't need a
# real BeatShoreDesktop.exe running (and must not have one running,
# actually -- both would fight over the same pipe name).
juce_add_console_app(BridgeIOHardeningTest PRODUCT_NAME "BridgeIOHardeningTest")

target_sources(BridgeIOHardeningTest PRIVATE Source/io_hardening_test.cpp)

target_compile_definitions(BridgeIOHardeningTest
    PRIVATE
        JucePlugin_VersionString="0.1.0-iohardeningtest"
)

target_link_libraries(BridgeIOHardeningTest
    PRIVATE
        juce::juce_core
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

# Controllable stand-in for node.exe (Source/mock_node_process.cpp) -- lets
# NodeEngineTest reproduce "Node hangs before its first line of output" and
# "Node goes silent mid-stream" on demand, which a real Node script can't do
# deterministically. No JUCE dependency needed; deliberately not a
# juce_add_console_app so it stays a plain, fast-to-launch process.
add_executable(MockNodeProcess Source/mock_node_process.cpp)

# Deterministic tests for the desktop<->Node pipe's overlapped-I/O
# conversion (NodeEngine.h) -- drives the REAL NodeEngine class against
# MockNodeProcess.exe instead of a real node.exe. See
# Source/node_engine_test.cpp.
juce_add_console_app(NodeEngineTest PRODUCT_NAME "NodeEngineTest")

target_sources(NodeEngineTest PRIVATE Source/node_engine_test.cpp)

target_compile_definitions(NodeEngineTest
    PRIVATE
        JucePlugin_VersionString="0.1.0-nodeenginetest"
        MOCK_NODE_EXE_PATH="$<TARGET_FILE:MockNodeProcess>"
)

add_dependencies(NodeEngineTest MockNodeProcess)

target_link_libraries(NodeEngineTest
    PRIVATE
        juce::juce_core
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

# Deterministic tests for BridgeClient.h's sendHeartbeat() requiring a real,
# matched HEARTBEAT_ACK rather than accepting any line as an
# acknowledgement -- see Source/heartbeat_ack_test.cpp.
juce_add_console_app(HeartbeatAckTest PRODUCT_NAME "HeartbeatAckTest")

target_sources(HeartbeatAckTest PRIVATE Source/heartbeat_ack_test.cpp)

target_compile_definitions(HeartbeatAckTest
    PRIVATE
        JucePlugin_VersionString="0.1.0-heartbeatacktest"
)

target_link_libraries(HeartbeatAckTest
    PRIVATE
        juce::juce_core
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

# Unit tests for AnalysisScheduler.h's pure data structures (JobQueue,
# JobRegistry, SessionOutbox, SessionRegistry) -- no pipes, no JUCE
# dependency needed since these headers only use the standard library and
# Win32 event handles.
add_executable(SchedulerTest Source/scheduler_test.cpp)

# Real load-boundary test against a REAL, already-running
# BeatShoreDesktop.exe -- exercises kMaxConcurrentSessions and
# kMaxGlobalQueueDepth with genuine HELLO-completed connections and real
# shared-memory audio. Deliberately NOT built on BridgeClient.h (see
# Source/load_boundary_test.cpp's own header comment for why its
# requestInFlight guard makes it unsuitable for this specific test) --
# a thin, direct client instead, so no JUCE dependency needed here
# either.
add_executable(LoadBoundaryTest Source/load_boundary_test.cpp)
target_compile_features(LoadBoundaryTest PRIVATE cxx_std_17)

# Integration tests for BeatShoreDesktop's multi-session architecture --
# drives TWO real BridgeClient instances against a REAL, already-running
# BeatShoreDesktop.exe (like BridgeClientTest, not a mock -- needs a real
# node engine doing real transcribePolyphonic work to prove "cancel a
# genuinely running job" for real). See Source/multi_session_test.cpp.
juce_add_console_app(MultiSessionTest PRODUCT_NAME "MultiSessionTest")

target_sources(MultiSessionTest PRIVATE Source/multi_session_test.cpp)

target_compile_definitions(MultiSessionTest
    PRIVATE
        JucePlugin_VersionString="0.1.0-multisessiontest"
)

target_link_libraries(MultiSessionTest
    PRIVATE
        juce::juce_core
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

# Direct, no-protocol verification of MixChain.h (native/BeatShoreBridge/
# Source/MixChain.h) -- the real EQ/Compressor/Limiter header
# PluginProcessor.cpp drives in processBlock(), included here unmodified.
# Every other test in this file proves something over the IPC protocol;
# this one can't use that approach at all since Mix is pure audio-domain
# DSP with no request/response round trip -- see Source/mixchain_test.cpp's
# own header comment for the four checks it runs.
juce_add_console_app(MixChainTest PRODUCT_NAME "MixChainTest")

target_sources(MixChainTest PRIVATE Source/mixchain_test.cpp)

target_compile_definitions(MixChainTest
    PRIVATE
        JucePlugin_VersionString="0.1.0-mixchaintest"
)

target_link_libraries(MixChainTest
    PRIVATE
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

# Direct, no-protocol verification of MasterMeter.h (native/BeatShoreBridge/
# Source/MasterMeter.h) -- the real libebur128-backed EBU R128 loudness/
# true-peak meter PluginProcessor.cpp feeds in processBlock(), included
# here unmodified. Same reasoning as MixChainTest above: Master has no
# protocol round trip either, so this is the only way to prove the
# measurement is real. Builds the vendored libebur128 C source directly
# (same file BeatShoreBridge itself compiles) rather than depending on
# that other CMake project's build output.
juce_add_console_app(MasterMeterTest PRODUCT_NAME "MasterMeterTest")

target_sources(MasterMeterTest
    PRIVATE
        Source/mastermeter_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../BeatShoreBridge/Source/third_party/libebur128/ebur128.c
)

target_include_directories(MasterMeterTest
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../BeatShoreBridge/Source/third_party/libebur128/queue
)

# Same MSVC M_PI fix as native/BeatShoreBridge/CMakeLists.txt -- see that
# file's own comment for why (ebur128.c's own top-of-file note).
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/../BeatShoreBridge/Source/third_party/libebur128/ebur128.c PROPERTIES
    COMPILE_DEFINITIONS "_USE_MATH_DEFINES"
)

target_compile_definitions(MasterMeterTest
    PRIVATE
        JucePlugin_VersionString="0.1.0-mastermetertest"
)

target_link_libraries(MasterMeterTest
    PRIVATE
        juce::juce_core
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

# Direct proof that getStateInformation()/setStateInformation() actually
# round-trip real plugin state onto a SECOND, freshly-constructed
# processor instance -- exactly the path a host uses for an offline
# render or a project reload, and exactly the path the old placeholder
# implementation silently failed (see Source/state_roundtrip_test.cpp's
# own header comment for how this bug was actually found: a live REAPER
# render kept reverting to unprocessed audio despite the Mix page
# visibly showing real values). Compiles the REAL PluginProcessor.cpp/
# PluginEditor.cpp, same pattern as BridgeStressTest below -- needs the
# same juce_dsp/libebur128 dependencies BeatShoreBridge itself now needs,
# since PluginProcessor.cpp includes MixChain.h and MasterMeter.h.
juce_add_console_app(StateRoundTripTest PRODUCT_NAME "StateRoundTripTest")

target_sources(StateRoundTripTest
    PRIVATE
        Source/state_roundtrip_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../BeatShoreBridge/Source/PluginProcessor.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../BeatShoreBridge/Source/PluginEditor.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../BeatShoreBridge/Source/third_party/libebur128/ebur128.c
)

target_include_directories(StateRoundTripTest
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../BeatShoreBridge/Source/third_party/libebur128/queue
)

set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/../BeatShoreBridge/Source/third_party/libebur128/ebur128.c PROPERTIES
    COMPILE_DEFINITIONS "_USE_MATH_DEFINES"
)

target_compile_definitions(StateRoundTripTest
    PRIVATE
        JucePlugin_Name="BeatShore Bridge"
        JucePlugin_VersionString="0.1.0-stateroundtriptest"
)

target_link_libraries(StateRoundTripTest
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)
