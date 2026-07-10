cmake_minimum_required(VERSION 3.24)

project(NeuracoustDW
    VERSION 0.1.0
    DESCRIPTION "Neuracoust DAW — new UI on the proven engine"
    LANGUAGES C CXX)

if(APPLE)
    enable_language(OBJCXX)
elseif(WIN32)
    enable_language(RC)
endif()

# The SwiftUI app needs a generator that speaks Swift. Ninja also qualifies, but
# Xcode is what ships with the toolchain here. Under Unix Makefiles the engine and
# all its tools/tests still build — only the app target is skipped.
set(NEURACOUST_BUILD_SWIFT_APP OFF)
if(APPLE AND (CMAKE_GENERATOR STREQUAL "Xcode" OR CMAKE_GENERATOR MATCHES "Ninja"))
    enable_language(Swift)
    set(NEURACOUST_BUILD_SWIFT_APP ON)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(MSVC)
    add_compile_options(/utf-8)
endif()

if(NOT DEFINED NEURACOUST_DAW_VERSION)
    string(TIMESTAMP NEURACOUST_DAW_VERSION_HOUR "%y%m%d.%H")
    string(TIMESTAMP NEURACOUST_DAW_VERSION_MINUTE "%M")
    math(EXPR NEURACOUST_DAW_VERSION_MINUTE_BUCKET "${NEURACOUST_DAW_VERSION_MINUTE} / 10 * 10")
    if(NEURACOUST_DAW_VERSION_MINUTE_BUCKET LESS 10)
        set(NEURACOUST_DAW_VERSION_MINUTE_BUCKET_TEXT "0${NEURACOUST_DAW_VERSION_MINUTE_BUCKET}")
    else()
        set(NEURACOUST_DAW_VERSION_MINUTE_BUCKET_TEXT "${NEURACOUST_DAW_VERSION_MINUTE_BUCKET}")
    endif()
    set(NEURACOUST_DAW_VERSION "${NEURACOUST_DAW_VERSION_HOUR}${NEURACOUST_DAW_VERSION_MINUTE_BUCKET_TEXT}")
endif()
set(NEURACOUST_DAW_VERSION "${NEURACOUST_DAW_VERSION}" CACHE STRING "Neuracoust date build version")
add_compile_definitions(NEURACOUST_DAW_VERSION="${NEURACOUST_DAW_VERSION}")

# The VST3 SDK is a large external checkout with its own git metadata. Reference the
# sibling DAW project's copy in place rather than duplicating ~half a million files.
set(NEURACOUST_VST3_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../DAW/third_party/vst3sdk"
    CACHE PATH "Path to Steinberg VST3 SDK")
set(NEURACOUST_LISTEN_SOURCE_DIR "/Volumes/Program Dev/Neuracoust Listen" CACHE PATH "Path to Neuracoust Listen source for bundling the browser relay")
option(NEURACOUST_DAW_ENABLE_NATIVE_WEBRTC "Enable DAW-side native WebRTC sender integration when libwebrtc is available" OFF)
set(NEURACOUST_WEBRTC_ROOT "" CACHE PATH "Path to WebRTC source root containing api/peer_connection_interface.h")
set(NEURACOUST_WEBRTC_BUILD_DIR "" CACHE PATH "Path to WebRTC build directory containing obj/libwebrtc.a or equivalent artifacts")
set(NEURACOUST_HAS_VST3_SDK OFF)
set(NEURACOUST_OPUS_INCLUDE_DIR "" CACHE PATH "Path to opus headers")
set(NEURACOUST_OPUS_LIBRARY "" CACHE FILEPATH "Path to libopus")
if(EXISTS "${NEURACOUST_VST3_SDK_DIR}/pluginterfaces/base/ipluginbase.h")
    set(NEURACOUST_HAS_VST3_SDK ON)
endif()

add_library(neuracoust_daw_core
    src/ai/AiAssistant.cpp
    src/core/AppIdentity.cpp
    src/core/DawState.cpp
    src/core/Localization.cpp
    src/audio/AudioDeviceModel.cpp
    src/audio/AsioRuntimeAdapter.cpp
    src/audio/ListenRoom.cpp
    src/audio/NativeWebRtcSender.cpp
    src/audio/RealtimeAudioEngine.cpp
    src/audio/RemoteDspAsyncStream.cpp
    src/audio/RemoteDspServerClient.cpp
    src/audio/RemoteDspPluginCatalog.cpp
    src/audio/MonitorOutputRouting.cpp
    src/audio/NeuracoustDspEngine.cpp
    src/audio/MetronomeClick.cpp
    src/audio/MixMath.cpp
    src/audio/MixerGraph.cpp
    src/audio/MixerProcessorChain.cpp
    src/audio/ProjectAudioRenderer.cpp
    src/audio/MasterInsertProcessor.cpp
    src/audio/MidiInputRecorder.cpp
    src/audio/RecordingTake.cpp
    src/audio/AudioInputRecorder.cpp
    src/audio/WavFile.cpp
    src/audio/MonitorDspProcessor.cpp
    src/audio/OfflineBounce.cpp
    src/license/LicenseAgentClient.cpp
    src/nuclust/NuclustDspManager.cpp
    src/plugins/MonitorDspModules.cpp
    src/plugins/InsertDspPolicy.cpp
    src/plugins/Vst3SdkAdapter.cpp
    src/plugins/Vst3ModuleRuntime.cpp
    src/plugins/Vst3RealtimeBridge.cpp
    src/plugins/PluginScanner.cpp
    src/plugins/Vst3HostFoundation.cpp
    src/project/AudioImportAnalysis.cpp
    src/project/EditOperations.cpp
    src/project/ProjectMediaPool.cpp
    src/project/StemMagicBridge.cpp
    src/project/TimelineExport.cpp
    src/project/ProjectDocument.cpp
    src/project/ProjectHistory.cpp
    src/ui/common/DawUiModel.cpp
    src/bridge/NeuracoustEngineBridge.cpp)

target_include_directories(neuracoust_daw_core PUBLIC src)
if(NOT NEURACOUST_OPUS_INCLUDE_DIR AND EXISTS "${NEURACOUST_LISTEN_SOURCE_DIR}/build/_deps/opus_lib-src/include/opus.h")
    set(NEURACOUST_OPUS_INCLUDE_DIR "${NEURACOUST_LISTEN_SOURCE_DIR}/build/_deps/opus_lib-src/include")
endif()
if(NOT NEURACOUST_OPUS_LIBRARY AND EXISTS "${NEURACOUST_LISTEN_SOURCE_DIR}/build/_deps/opus_lib-build/libopus.a")
    set(NEURACOUST_OPUS_LIBRARY "${NEURACOUST_LISTEN_SOURCE_DIR}/build/_deps/opus_lib-build/libopus.a")
endif()
if(NOT NEURACOUST_OPUS_INCLUDE_DIR AND EXISTS "${NEURACOUST_LISTEN_SOURCE_DIR}/build-release-arm64/_deps/opus_lib-src/include/opus.h")
    set(NEURACOUST_OPUS_INCLUDE_DIR "${NEURACOUST_LISTEN_SOURCE_DIR}/build-release-arm64/_deps/opus_lib-src/include")
endif()
if(NOT NEURACOUST_OPUS_LIBRARY AND EXISTS "${NEURACOUST_LISTEN_SOURCE_DIR}/build-release-arm64/_deps/opus_lib-build/libopus.a")
    set(NEURACOUST_OPUS_LIBRARY "${NEURACOUST_LISTEN_SOURCE_DIR}/build-release-arm64/_deps/opus_lib-build/libopus.a")
endif()
function(neuracoust_opus_library_matches_target output_var library_path)
    if(NOT library_path OR NOT EXISTS "${library_path}")
        set(${output_var} FALSE PARENT_SCOPE)
        return()
    endif()
    if(NOT APPLE)
        set(${output_var} TRUE PARENT_SCOPE)
        return()
    endif()
    set(required_arches "${CMAKE_OSX_ARCHITECTURES}")
    if(NOT required_arches)
        set(required_arches "${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    execute_process(
        COMMAND /usr/bin/lipo -info "${library_path}"
        OUTPUT_VARIABLE library_info
        ERROR_VARIABLE library_info
        RESULT_VARIABLE library_info_result
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT library_info_result EQUAL 0)
        set(${output_var} FALSE PARENT_SCOPE)
        return()
    endif()
    foreach(required_arch IN LISTS required_arches)
        if(required_arch AND NOT library_info MATCHES "(^|[^A-Za-z0-9_])${required_arch}([^A-Za-z0-9_]|$)")
            set(${output_var} FALSE PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${output_var} TRUE PARENT_SCOPE)
endfunction()
if(NEURACOUST_OPUS_INCLUDE_DIR AND NEURACOUST_OPUS_LIBRARY)
    neuracoust_opus_library_matches_target(NEURACOUST_OPUS_LIBRARY_OK "${NEURACOUST_OPUS_LIBRARY}")
    if(NEURACOUST_OPUS_LIBRARY_OK)
        target_compile_definitions(neuracoust_daw_core PUBLIC NEURACOUST_DAW_HAS_OPUS=1)
        target_include_directories(neuracoust_daw_core SYSTEM PUBLIC "${NEURACOUST_OPUS_INCLUDE_DIR}")
        target_link_libraries(neuracoust_daw_core PUBLIC "${NEURACOUST_OPUS_LIBRARY}")
    else()
        message(WARNING "Skipping libopus because it does not contain the target architecture: ${NEURACOUST_OPUS_LIBRARY}")
        set(NEURACOUST_OPUS_LIBRARY "")
    endif()
endif()
if(NEURACOUST_DAW_ENABLE_NATIVE_WEBRTC)
    if(NEURACOUST_WEBRTC_ROOT
            AND EXISTS "${NEURACOUST_WEBRTC_ROOT}/api/peer_connection_interface.h"
            AND NEURACOUST_WEBRTC_BUILD_DIR
            AND EXISTS "${NEURACOUST_WEBRTC_BUILD_DIR}/obj/libwebrtc.a")
        target_compile_definitions(neuracoust_daw_core PUBLIC NEURACOUST_DAW_HAS_NATIVE_WEBRTC=1)
        target_compile_definitions(neuracoust_daw_core PUBLIC
            WEBRTC_ENABLE_PROTOBUF=1
            WEBRTC_STRICT_FIELD_TRIALS=0
            WEBRTC_HAVE_SCTP
            WEBRTC_ARCH_ARM64
            WEBRTC_HAS_NEON
            WEBRTC_POSIX
            WEBRTC_MAC
            RTC_ENABLE_VP9
            RTC_DAV1D_IN_INTERNAL_DECODER_FACTORY
            ABSL_ALLOCATOR_NOTHROW=1
            CHROMIUM)
        target_include_directories(neuracoust_daw_core SYSTEM PUBLIC
            "${NEURACOUST_WEBRTC_ROOT}"
            "${NEURACOUST_WEBRTC_BUILD_DIR}/gen"
            "${NEURACOUST_WEBRTC_ROOT}/third_party/abseil-cpp"
            "${NEURACOUST_WEBRTC_ROOT}/third_party/perfetto/include"
            "${NEURACOUST_WEBRTC_BUILD_DIR}/gen/third_party/perfetto/build_config"
            "${NEURACOUST_WEBRTC_BUILD_DIR}/gen/third_party/perfetto"
            "${NEURACOUST_WEBRTC_ROOT}/third_party/libyuv/include")
        target_link_libraries(neuracoust_daw_core PUBLIC "${NEURACOUST_WEBRTC_BUILD_DIR}/obj/libwebrtc.a")
    else()
        message(WARNING "NEURACOUST_DAW_ENABLE_NATIVE_WEBRTC is ON but WebRTC headers and obj/libwebrtc.a were not both found; building native WebRTC stub only.")
    endif()
endif()
if(NEURACOUST_HAS_VST3_SDK)
    target_sources(neuracoust_daw_core PRIVATE
        "${NEURACOUST_VST3_SDK_DIR}/pluginterfaces/base/funknown.cpp"
        "${NEURACOUST_VST3_SDK_DIR}/pluginterfaces/base/coreiids.cpp"
        "${NEURACOUST_VST3_SDK_DIR}/public.sdk/source/common/commoniids.cpp"
        "${NEURACOUST_VST3_SDK_DIR}/public.sdk/source/vst/vstinitiids.cpp")
    target_compile_definitions(neuracoust_daw_core PUBLIC NEURACOUST_HAS_VST3_SDK=1)
    target_include_directories(neuracoust_daw_core SYSTEM PUBLIC "${NEURACOUST_VST3_SDK_DIR}")
else()
    message(WARNING "VST3 SDK not found at ${NEURACOUST_VST3_SDK_DIR} — plugin hosting will be stubbed out.")
endif()

if(APPLE)
    target_sources(neuracoust_daw_core PRIVATE src/audio/CoreAudioBackend.mm src/audio/CoreAudioRealtimeEngine.mm src/audio/MacAudioInputRecorder.mm src/audio/MacMidiInputRecorder.mm)
    target_compile_definitions(neuracoust_daw_core PUBLIC NEURACOUST_DAW_MAC=1)
    target_link_libraries(neuracoust_daw_core
        PUBLIC
            "-framework Foundation"
            "-framework CoreFoundation"
            "-framework CoreAudio"
            "-framework CoreMIDI"
            "-framework AudioToolbox"
            "-framework ApplicationServices")
elseif(WIN32)
    target_sources(neuracoust_daw_core PRIVATE src/audio/WindowsAudioBackend.cpp)
    target_compile_definitions(neuracoust_daw_core PUBLIC NEURACOUST_DAW_WINDOWS=1)
    target_link_libraries(neuracoust_daw_core PUBLIC ole32 uuid winmm advapi32 avrt ws2_32)
else()
    target_sources(neuracoust_daw_core PRIVATE src/audio/WindowsAudioBackend.cpp)
    target_compile_definitions(neuracoust_daw_core PUBLIC NEURACOUST_DAW_LINUX=1)
    target_link_libraries(neuracoust_daw_core PUBLIC pthread rt)
endif()

# ---------------------------------------------------------------------------
# Out-of-process plugin editor hosts. The realtime engine spawns these; they are
# kept verbatim from the DAW project. The new UI has no window target yet, so
# nothing is copied into an app bundle at this stage.
# ---------------------------------------------------------------------------
if(APPLE)
    add_executable(NeuracoustVst3EditorHost src/app/macos/Vst3EditorHostMain.mm)
    add_executable(NeuracoustAuEditorHost src/app/macos/AuEditorHostMain.mm)

    target_link_libraries(NeuracoustVst3EditorHost
        PRIVATE
            neuracoust_daw_core
            "-framework Cocoa")

    target_link_libraries(NeuracoustAuEditorHost
        PRIVATE
            "-framework Cocoa"
            "-framework AudioToolbox"
            "-framework CoreAudio")

    set_target_properties(NeuracoustVst3EditorHost PROPERTIES
        OUTPUT_NAME "Neuracoust VST3 Editor Host")

    set_target_properties(NeuracoustAuEditorHost PROPERTIES
        OUTPUT_NAME "Neuracoust AU Editor Host")

    add_executable(NeuracoustDspManager MACOSX_BUNDLE
        src/app/macos/NuclustDspManagerMain.mm
        resources/icons/NeuracoustDAW.icns)

    set_source_files_properties(resources/icons/NeuracoustDAW.icns
        PROPERTIES MACOSX_PACKAGE_LOCATION Resources)

    target_link_libraries(NeuracoustDspManager
        PRIVATE
            neuracoust_daw_core
            "-framework Cocoa")

    set_target_properties(NeuracoustDspManager PROPERTIES
        OUTPUT_NAME "Neuracoust DSP Manager"
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.neuracoust.dsp-manager"
        MACOSX_BUNDLE_BUNDLE_NAME "Neuracoust DSP Manager"
        MACOSX_BUNDLE_DISPLAY_NAME "Neuracoust DSP Manager"
        MACOSX_BUNDLE_ICON_FILE "NeuracoustDAW.icns"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1.0"
        MACOSX_BUNDLE_BUNDLE_VERSION "${NEURACOUST_DAW_VERSION}"
        MACOSX_BUNDLE_COPYRIGHT "(C) 2026 Neuracoust"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/Info.plist.in")
endif()

# ---------------------------------------------------------------------------
# The new SwiftUI app. Swift reaches the C++ engine through the pure-C facade in
# src/bridge, imported via a bridging header. Heavy views (timeline, piano roll,
# meters, VST3 editor windows) will be AppKit/Metal wrapped in NSViewRepresentable.
# ---------------------------------------------------------------------------
if(NEURACOUST_BUILD_SWIFT_APP)
    add_executable(NeuracoustDAW MACOSX_BUNDLE
        src/app/swift/NeuracoustApp.swift
        src/app/swift/Theme.swift
        src/app/swift/TransportBar.swift
        src/app/swift/MonitorControls.swift
        src/app/swift/MonitorDock.swift
        src/app/swift/ListenRoom.swift
        src/app/swift/ChatPanel.swift
        src/app/swift/MixerControls.swift
        src/app/swift/MixerView.swift
        src/app/swift/PluginBrowser.swift
        src/app/swift/EngineController.swift
        resources/icons/NeuracoustDAW.icns)

    set_source_files_properties(resources/icons/NeuracoustDAW.icns
        PROPERTIES MACOSX_PACKAGE_LOCATION Resources)

    target_link_libraries(NeuracoustDAW PRIVATE neuracoust_daw_core)

    # This source tree lives under a path containing a space, so the bridging header
    # and include dir must reach the compiler as single quoted arguments. Passing
    # them as separate list items lets CMake quote each one; a "SHELL:-import-objc-header
    # <path>" string would be split at the space instead.
    if(CMAKE_GENERATOR STREQUAL "Xcode")
        set_target_properties(NeuracoustDAW PROPERTIES
            XCODE_ATTRIBUTE_SWIFT_OBJC_BRIDGING_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/src/app/swift/Bridging.h"
            XCODE_ATTRIBUTE_HEADER_SEARCH_PATHS "\"${CMAKE_CURRENT_SOURCE_DIR}/src\"")
    else()
        target_compile_options(NeuracoustDAW PRIVATE
            "-import-objc-header" "${CMAKE_CURRENT_SOURCE_DIR}/src/app/swift/Bridging.h"
            "-Xcc" "-I${CMAKE_CURRENT_SOURCE_DIR}/src")
    endif()

    set_target_properties(NeuracoustDAW PROPERTIES
        OUTPUT_NAME "Neuracoust DAW"
        MACOSX_BUNDLE TRUE
        XCODE_ATTRIBUTE_SWIFT_VERSION "5.0"
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.neuracoust.daw"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.neuracoust.daw"
        MACOSX_BUNDLE_BUNDLE_NAME "Neuracoust DAW"
        MACOSX_BUNDLE_DISPLAY_NAME "Neuracoust DAW"
        MACOSX_BUNDLE_ICON_FILE "NeuracoustDAW.icns"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1.0"
        MACOSX_BUNDLE_BUNDLE_VERSION "${NEURACOUST_DAW_VERSION}")

    # The engine spawns these; keep them next to the app the way it expects.
    # The swift driver also drops a main.d depfile beside the executable — evict it
    # so nothing but real binaries ships inside MacOS/.
    add_dependencies(NeuracoustDAW NeuracoustVst3EditorHost NeuracoustAuEditorHost)
    add_custom_command(TARGET NeuracoustDAW POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:NeuracoustVst3EditorHost>"
            "$<TARGET_BUNDLE_CONTENT_DIR:NeuracoustDAW>/MacOS/Neuracoust VST3 Editor Host"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:NeuracoustAuEditorHost>"
            "$<TARGET_BUNDLE_CONTENT_DIR:NeuracoustDAW>/MacOS/Neuracoust AU Editor Host"
        COMMAND ${CMAKE_COMMAND} -E rm -f
            "$<TARGET_BUNDLE_CONTENT_DIR:NeuracoustDAW>/MacOS/main.d")

    # Listen Room streams through a Python relay that ships with the Neuracoust
    # Listen project. Bundle it so the app can launch the relay without that
    # source tree being present at runtime.
    if(EXISTS "${NEURACOUST_LISTEN_SOURCE_DIR}/scripts/start-relay.sh")
        add_custom_command(TARGET NeuracoustDAW POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${NEURACOUST_LISTEN_SOURCE_DIR}/scripts/start-relay.sh"
                "$<TARGET_BUNDLE_CONTENT_DIR:NeuracoustDAW>/Resources/start-relay.sh"
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_BUNDLE_CONTENT_DIR:NeuracoustDAW>/relay"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${NEURACOUST_LISTEN_SOURCE_DIR}/relay/listen_relay.py"
                "${NEURACOUST_LISTEN_SOURCE_DIR}/relay/listen_version.py"
                "${NEURACOUST_LISTEN_SOURCE_DIR}/relay/network_info.py"
                "${NEURACOUST_LISTEN_SOURCE_DIR}/relay/opus_transcode.py"
                "${NEURACOUST_LISTEN_SOURCE_DIR}/relay/protocol.py"
                "${NEURACOUST_LISTEN_SOURCE_DIR}/relay/qr_util.py"
                "${NEURACOUST_LISTEN_SOURCE_DIR}/relay/requirements.txt"
                "${NEURACOUST_LISTEN_SOURCE_DIR}/relay/run.sh"
                "$<TARGET_BUNDLE_CONTENT_DIR:NeuracoustDAW>/relay"
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_BUNDLE_CONTENT_DIR:NeuracoustDAW>/web"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${NEURACOUST_LISTEN_SOURCE_DIR}/web/index.html"
                "${NEURACOUST_LISTEN_SOURCE_DIR}/web/logo.png"
                "${NEURACOUST_LISTEN_SOURCE_DIR}/web/pcm-player-processor.js"
                "$<TARGET_BUNDLE_CONTENT_DIR:NeuracoustDAW>/web"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${NEURACOUST_LISTEN_SOURCE_DIR}/ListenVersion.txt"
                "$<TARGET_BUNDLE_CONTENT_DIR:NeuracoustDAW>/ListenVersion.txt")
    else()
        message(STATUS "Listen relay assets not found at ${NEURACOUST_LISTEN_SOURCE_DIR} — Listen Room will report a missing relay at runtime.")
    endif()
else()
    message(STATUS "Swift app target skipped: generator '${CMAKE_GENERATOR}' does not support Swift. Use -G Xcode.")
endif()

# ---------------------------------------------------------------------------
# Engine-side tools and probes
# ---------------------------------------------------------------------------
add_executable(nuclust_mock_plugin_client tools/nuclust_mock_plugin_client.cpp)
target_link_libraries(nuclust_mock_plugin_client PRIVATE neuracoust_daw_core)

add_executable(neuracoust_vst3_host_audit tools/vst3_host_audit.cpp)
target_link_libraries(neuracoust_vst3_host_audit PRIVATE neuracoust_daw_core)

add_executable(neuracoust_vst3_process_worker tools/vst3_process_worker.cpp)
target_link_libraries(neuracoust_vst3_process_worker PRIVATE neuracoust_daw_core)

add_executable(neuracoust_vst3_isolated_process_smoke tools/vst3_isolated_process_smoke.cpp)
target_link_libraries(neuracoust_vst3_isolated_process_smoke PRIVATE neuracoust_daw_core)

add_executable(neuracoust_vst3_realtime_bridge_smoke tools/vst3_realtime_bridge_smoke.cpp)
target_link_libraries(neuracoust_vst3_realtime_bridge_smoke PRIVATE neuracoust_daw_core)

add_executable(neuracoust_vst3_realtime_process_probe tools/vst3_realtime_process_probe.cpp)
target_link_libraries(neuracoust_vst3_realtime_process_probe PRIVATE neuracoust_daw_core)

add_executable(neuracoust_vst3_engine_insert_probe tools/vst3_engine_insert_probe.cpp)
target_link_libraries(neuracoust_vst3_engine_insert_probe PRIVATE neuracoust_daw_core)

add_executable(neuracoust_vst3_midi_instrument_smoke tools/vst3_midi_instrument_smoke.cpp)
target_link_libraries(neuracoust_vst3_midi_instrument_smoke PRIVATE neuracoust_daw_core)

add_executable(neuracoust_remote_core_server tools/neuracoust_remote_core_server.cpp)
target_link_libraries(neuracoust_remote_core_server PRIVATE neuracoust_daw_core)

add_executable(neuracoust_daw_render tools/neuracoust_daw_render.cpp)
target_link_libraries(neuracoust_daw_render PRIVATE neuracoust_daw_core)

add_executable(neuracoust_bridge_transport_smoke tools/bridge_transport_smoke.c)
target_link_libraries(neuracoust_bridge_transport_smoke PRIVATE neuracoust_daw_core)

if(APPLE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/neuracoust_video_render.mm")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/video_render_fixture.mm")
        add_executable(neuracoust_video_render_fixture tools/video_render_fixture.mm)
        target_link_libraries(neuracoust_video_render_fixture PRIVATE
            neuracoust_daw_core
            "-framework Foundation"
            "-framework AVFoundation"
            "-framework CoreMedia"
            "-framework CoreVideo")
    endif()

    add_executable(neuracoust_video_render tools/neuracoust_video_render.mm)
    target_link_libraries(neuracoust_video_render PRIVATE
        neuracoust_daw_core
        "-framework Foundation"
        "-framework AVFoundation"
        "-framework CoreMedia")
endif()

add_executable(neuracoust_video_render_ffmpeg tools/neuracoust_video_render_ffmpeg.cpp)
target_link_libraries(neuracoust_video_render_ffmpeg PRIVATE neuracoust_daw_core)
if(APPLE)
    target_link_options(neuracoust_video_render_ffmpeg PRIVATE
        "LINKER:-force_load,$<TARGET_FILE:neuracoust_daw_core>")
endif()

add_executable(neuracoust_native_webrtc_smoke tools/neuracoust_native_webrtc_smoke.cpp)
target_link_libraries(neuracoust_native_webrtc_smoke PRIVATE neuracoust_daw_core)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()

function(neuracoust_enable_release_asserts target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /UNDEBUG)
    else()
        target_compile_options(${target} PRIVATE -UNDEBUG)
    endif()
endfunction()

function(neuracoust_link_core target)
    target_link_libraries(${target} PRIVATE neuracoust_daw_core)
    if(APPLE)
        target_link_options(${target} PRIVATE
            "LINKER:-force_load,$<TARGET_FILE:neuracoust_daw_core>")
    endif()
endfunction()

add_executable(neuracoust_interchange_roundtrip_fixture tools/interchange_roundtrip_fixture.cpp)
neuracoust_link_core(neuracoust_interchange_roundtrip_fixture)

add_executable(neuracoust_daw_core_tests tests/CoreSmokeTest.cpp)
neuracoust_link_core(neuracoust_daw_core_tests)
neuracoust_enable_release_asserts(neuracoust_daw_core_tests)
add_test(NAME core_smoke COMMAND neuracoust_daw_core_tests)

add_executable(neuracoust_daw_ai_assistant_tests tests/AiAssistantSmokeTest.cpp)
neuracoust_link_core(neuracoust_daw_ai_assistant_tests)
neuracoust_enable_release_asserts(neuracoust_daw_ai_assistant_tests)
add_test(NAME ai_assistant_smoke COMMAND neuracoust_daw_ai_assistant_tests)

add_executable(neuracoust_daw_localization_tests tests/LocalizationSmokeTest.cpp)
neuracoust_link_core(neuracoust_daw_localization_tests)
neuracoust_enable_release_asserts(neuracoust_daw_localization_tests)
add_test(NAME localization_smoke COMMAND neuracoust_daw_localization_tests)

add_executable(neuracoust_daw_desktop_wav_workflow_tests tests/DesktopWavWorkflowSmokeTest.cpp)
neuracoust_link_core(neuracoust_daw_desktop_wav_workflow_tests)
neuracoust_enable_release_asserts(neuracoust_daw_desktop_wav_workflow_tests)
add_test(NAME desktop_wav_workflow_smoke COMMAND neuracoust_daw_desktop_wav_workflow_tests)

add_executable(neuracoust_daw_remote_dsp_tests tests/RemoteDspServerClientTest.cpp)
neuracoust_link_core(neuracoust_daw_remote_dsp_tests)
neuracoust_enable_release_asserts(neuracoust_daw_remote_dsp_tests)
add_test(NAME remote_dsp_server_client COMMAND neuracoust_daw_remote_dsp_tests)

add_executable(neuracoust_project_history_tests tests/ProjectHistoryTest.cpp)
neuracoust_link_core(neuracoust_project_history_tests)
neuracoust_enable_release_asserts(neuracoust_project_history_tests)
add_test(NAME project_history COMMAND neuracoust_project_history_tests)

add_executable(neuracoust_insert_dsp_policy_tests tests/InsertDspPolicyTest.cpp)
neuracoust_link_core(neuracoust_insert_dsp_policy_tests)
neuracoust_enable_release_asserts(neuracoust_insert_dsp_policy_tests)
add_test(NAME insert_dsp_policy COMMAND neuracoust_insert_dsp_policy_tests)

add_executable(neuracoust_dsp_manager_tests tests/NuclustDspManagerTest.cpp)
neuracoust_link_core(neuracoust_dsp_manager_tests)
neuracoust_enable_release_asserts(neuracoust_dsp_manager_tests)
add_test(NAME neuracoust_dsp_manager COMMAND neuracoust_dsp_manager_tests)

if(APPLE OR WIN32)
    add_executable(neuracoust_daw_audio_tests tests/AudioEngineSmokeTest.cpp)
    neuracoust_link_core(neuracoust_daw_audio_tests)
    neuracoust_enable_release_asserts(neuracoust_daw_audio_tests)
    add_test(NAME audio_engine_smoke COMMAND neuracoust_daw_audio_tests)
endif()

add_test(NAME vst3_host_audit_smoke
    COMMAND neuracoust_vst3_host_audit --path /definitely/not/a/plugin.vst3 --no-process)
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME waves_compat_audit_summary_smoke
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/waves_compat_audit.py" --self-test-summary)
endif()
# Exercises the exact C facade the SwiftUI app calls. Exits 0 with a SKIP line when
# no audio device is available, so it stays green on machines without one.
add_test(NAME bridge_transport_smoke COMMAND neuracoust_bridge_transport_smoke)

add_test(NAME vst3_process_worker_smoke COMMAND neuracoust_vst3_process_worker --self-test)
add_test(NAME vst3_isolated_process_smoke COMMAND neuracoust_vst3_isolated_process_smoke)
add_test(NAME remote_core_server_self_test COMMAND neuracoust_remote_core_server --self-test)
add_test(NAME native_webrtc_smoke COMMAND neuracoust_native_webrtc_smoke)

# Editor-host self-tests run against the built executables directly (no app bundle yet).
if(APPLE)
    add_test(NAME vst3_editor_parameter_polling_smoke
        COMMAND "$<TARGET_FILE:NeuracoustVst3EditorHost>" --self-test-parameter-polling)
    add_test(NAME vst3_editor_waves_view_size_smoke
        COMMAND "$<TARGET_FILE:NeuracoustVst3EditorHost>" --self-test-waves-view-size)
    add_test(NAME vst3_editor_synthetic_audio_policy_smoke
        COMMAND "$<TARGET_FILE:NeuracoustVst3EditorHost>" --self-test-synthetic-audio-policy)
    add_test(NAME vst3_editor_host_smoke
        COMMAND
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/smoke-vst3-editor-host.command"
            "$<TARGET_FILE:NeuracoustVst3EditorHost>")
endif()
