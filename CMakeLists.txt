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

# The VST3 SDK is a large external checkout with its own git metadata (~40 MB). Prefer a
# local copy under this project's third_party/ (kept out of git via .gitignore) so the build
# is self-contained; fall back to the sibling DAW project's copy if the local one is absent.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vst3sdk/CMakeLists.txt")
    set(_NEURACOUST_VST3_DEFAULT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vst3sdk")
else()
    set(_NEURACOUST_VST3_DEFAULT "${CMAKE_CURRENT_SOURCE_DIR}/../DAW/third_party/vst3sdk")
endif()
set(NEURACOUST_VST3_SDK_DIR "${_NEURACOUST_VST3_DEFAULT}"
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
    src/core/Base64.cpp
    src/core/DawState.cpp
    src/core/Localization.cpp
    src/control/MackieHuiProtocol.cpp
    src/control/ControlSurfaceMidi.cpp
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
    src/audio/LoudnessMeter.cpp
    src/audio/MetronomeClick.cpp
    src/audio/TestSignalGenerator.cpp
    src/audio/TimePitchProcessor.cpp
    src/audio/PitchEditor.cpp
    src/audio/VocalAlign.cpp
    src/audio/MixMath.cpp
    src/audio/MixerGraph.cpp
    src/audio/MixerProcessorChain.cpp
    src/audio/ConsoleChannelProcessor.cpp
    src/audio/ProjectAudioRenderer.cpp
    src/audio/MasterInsertProcessor.cpp
    src/audio/AsyncInsertChainPreparer.cpp
    src/audio/MidiInputRecorder.cpp
    src/audio/RecordingTake.cpp
    src/audio/AudioInputRecorder.cpp
    src/audio/WavFile.cpp
    src/audio/MonitorDspProcessor.cpp
    src/audio/ParametricEq.cpp
    src/audio/SweepMeasurement.cpp
    src/audio/MonitorCorrection.cpp
    src/audio/OutputChainModeling.cpp
    src/audio/MonitorFirEq.cpp
    src/audio/SpeakerProfiles.generated.cpp
    src/audio/HeadphoneProfiles.generated.cpp
    src/audio/AudioInterfaceCatalog.generated.cpp
    src/audio/AudioInterfaceProfiles.generated.cpp
    src/audio/MonitorSpecCurves.cpp
    src/audio/AudioInterfaceModeler.cpp
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
    src/project/AudioImport.cpp
    src/project/AafImport.cpp
    src/plugins/AraHost.cpp
    src/ui/common/DawUiModel.cpp
    src/bridge/NeuracoustEngineBridge.cpp)

# libAAF (GPL v2+) — reads AAF sessions from other DAWs/NLEs. Vendored rather than referenced in
# the sibling Ardour tree so this project does not gain another external-folder dependency.
# NOTE: linking this makes the built DAW GPL. That is fine here (personal, non-distributed build);
# if this ever ships commercially, this dependency has to go.
file(GLOB NEURACOUST_LIBAAF_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libaaf/*.c")
if(NEURACOUST_LIBAAF_SOURCES)
    add_library(neuracoust_libaaf STATIC ${NEURACOUST_LIBAAF_SOURCES})
    target_include_directories(neuracoust_libaaf SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libaaf")
    # Third-party C: its warnings are not ours to fix.
    target_compile_options(neuracoust_libaaf PRIVATE -w)
    set(NEURACOUST_HAS_LIBAAF ON)
else()
    set(NEURACOUST_HAS_LIBAAF OFF)
endif()

# ARA SDK (Celemony, ARA license — permissive, unlike libAAF). Header-only for what we need: the
# C API (ARA_API/ARAInterface.h) plus the VST3 binding (ARA_API/ARAVST3.h) that reaches an ARA
# plug-in's factory through its VST3 factory.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ARA_SDK/ARA_API/ARAInterface.h")
    set(NEURACOUST_HAS_ARA_SDK ON)
else()
    set(NEURACOUST_HAS_ARA_SDK OFF)
endif()

target_include_directories(neuracoust_daw_core PUBLIC src)
if(NEURACOUST_HAS_ARA_SDK)
    target_include_directories(neuracoust_daw_core SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ARA_SDK")
    target_compile_definitions(neuracoust_daw_core PUBLIC NEURACOUST_HAS_ARA_SDK=1)
    message(STATUS "ARA SDK found — ARA plug-in hosting is compiled in.")
else()
    message(STATUS "ARA SDK not found at third_party/ARA_SDK — ARA hosting disabled.")
endif()
if(NEURACOUST_HAS_LIBAAF)
    target_link_libraries(neuracoust_daw_core PUBLIC neuracoust_libaaf)
    target_compile_definitions(neuracoust_daw_core PUBLIC NEURACOUST_HAS_LIBAAF=1)
endif()
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
    target_sources(neuracoust_daw_core PRIVATE src/audio/CoreAudioBackend.mm src/audio/CoreAudioRealtimeEngine.mm src/audio/MacAudioInputRecorder.mm src/audio/MacMidiInputRecorder.mm src/control/MacControlSurfaceMidi.mm)
    target_compile_definitions(neuracoust_daw_core PUBLIC NEURACOUST_DAW_MAC=1)
    target_link_libraries(neuracoust_daw_core
        PUBLIC
            "-framework Foundation"
            "-framework CoreFoundation"
            "-framework CoreAudio"
            "-framework CoreMIDI"
            "-framework AudioToolbox"
            "-framework Accelerate"
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
            neuracoust_daw_core
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
    add_executable(NeuracoustVocalConverter MACOSX_BUNDLE
        src/app/swift/VocalConverterApp.swift
        resources/icons/NeuracoustDAW.icns)

    set_source_files_properties(resources/icons/NeuracoustDAW.icns
        PROPERTIES MACOSX_PACKAGE_LOCATION Resources)

    target_link_libraries(NeuracoustVocalConverter PRIVATE
        "-framework AppKit"
        "-framework AVFoundation")
    target_compile_options(NeuracoustVocalConverter PRIVATE "-parse-as-library")

    set_target_properties(NeuracoustVocalConverter PROPERTIES
        OUTPUT_NAME "Neuracoust Vocal Converter"
        MACOSX_BUNDLE TRUE
        XCODE_ATTRIBUTE_SWIFT_VERSION "5.0"
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.neuracoust.vocal-converter"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.neuracoust.vocal-converter"
        MACOSX_BUNDLE_BUNDLE_NAME "Neuracoust Vocal Converter"
        MACOSX_BUNDLE_DISPLAY_NAME "Neuracoust Vocal Converter"
        MACOSX_BUNDLE_ICON_FILE "NeuracoustDAW.icns"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1.0"
        MACOSX_BUNDLE_BUNDLE_VERSION "${NEURACOUST_DAW_VERSION}"
        MACOSX_BUNDLE_COPYRIGHT "(C) 2026 Neuracoust"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/Info.plist.in")

    add_custom_command(TARGET NeuracoustVocalConverter POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/NeuracoustDAW.icns"
            "$<TARGET_BUNDLE_CONTENT_DIR:NeuracoustVocalConverter>/Resources/NeuracoustDAW.icns"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/vocal_converter_setup.sh"
            "$<TARGET_BUNDLE_CONTENT_DIR:NeuracoustVocalConverter>/Resources/vocal_converter_setup.sh"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/vocal_converter_pipeline.py"
            "$<TARGET_BUNDLE_CONTENT_DIR:NeuracoustVocalConverter>/Resources/vocal_converter_pipeline.py"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/vocal_converter_train.py"
            "$<TARGET_BUNDLE_CONTENT_DIR:NeuracoustVocalConverter>/Resources/vocal_converter_train.py"
        COMMAND ${CMAKE_COMMAND} -E rm -f
            "$<TARGET_BUNDLE_CONTENT_DIR:NeuracoustVocalConverter>/MacOS/main.d")

    add_executable(NeuracoustDAW MACOSX_BUNDLE
        src/app/swift/NeuracoustApp.swift
        src/app/swift/Diagnostics.swift
        src/app/swift/Theme.swift
        src/app/swift/TransportBar.swift
        src/app/swift/MonitorControls.swift
        src/app/swift/MonitorDock.swift
        src/app/swift/SpeakerComparisonView.swift
        src/app/swift/ListenRoom.swift
        src/app/swift/ListenInviteQR.swift
        src/app/swift/AiAssistant.swift
        src/app/swift/MonitorEqView.swift
        src/app/swift/ChatPanel.swift
        src/app/swift/MixerControls.swift
        src/app/swift/MixerView.swift
        src/app/swift/ConsoleModulesView.swift
        src/app/swift/TrackInspector.swift
        src/app/swift/PianoRoll.swift
        src/app/swift/VirtualKeyboard.swift
        src/app/swift/AraEditor.swift
        src/app/swift/PitchEditorPanel.swift
        src/app/swift/PluginEditor.swift
        src/app/swift/PluginBrowser.swift
        src/app/swift/MidiLibrary.swift
        src/app/swift/MidiLibraryBrowser.swift
        src/app/swift/TimelineView.swift
        src/app/swift/GlobalTracksBar.swift
        src/app/swift/FadeCrossfadeEditor.swift
        src/app/swift/MonitorStationShell.swift
        src/app/swift/SpectrumAnalyzer.swift
        src/app/swift/MetalSpectrumView.swift
        src/app/swift/MetalGridView.swift
        src/app/swift/AnalyzerWindow.swift
        src/app/swift/GlobalKeypadCapture.swift
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
        MACOSX_BUNDLE_BUNDLE_VERSION "${NEURACOUST_DAW_VERSION}"
        # Without this the app shipped CMake's stub Info.plist (no NSMicrophoneUsageDescription),
        # so the moment it asked for microphone/input access macOS hard-crashed it (TCC privacy
        # violation). The template carries the usage strings and localizations.
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/Info.plist.in")

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
        # MACOSX_PACKAGE_LOCATION does not reliably stage the icns under the Ninja
        # generator, so copy it in explicitly — the app icon and the project-folder
        # icon (NSWorkspace.setIcon) both read it from Resources at runtime.
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/NeuracoustDAW.icns"
            "$<TARGET_BUNDLE_CONTENT_DIR:NeuracoustDAW>/Resources/NeuracoustDAW.icns"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/speaker_model_dataset.json"
            "$<TARGET_BUNDLE_CONTENT_DIR:NeuracoustDAW>/Resources/speaker_model_dataset.json"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/headphone_model_meta.json"
            "$<TARGET_BUNDLE_CONTENT_DIR:NeuracoustDAW>/Resources/headphone_model_meta.json"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/audio_interface_catalog.json"
            "$<TARGET_BUNDLE_CONTENT_DIR:NeuracoustDAW>/Resources/audio_interface_catalog.json"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/power_amp_catalog.json"
            "$<TARGET_BUNDLE_CONTENT_DIR:NeuracoustDAW>/Resources/power_amp_catalog.json"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/speaker_cable_catalog.json"
            "$<TARGET_BUNDLE_CONTENT_DIR:NeuracoustDAW>/Resources/speaker_cable_catalog.json"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/power_cable_catalog.json"
            "$<TARGET_BUNDLE_CONTENT_DIR:NeuracoustDAW>/Resources/power_cable_catalog.json"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/connector_catalog.json"
            "$<TARGET_BUNDLE_CONTENT_DIR:NeuracoustDAW>/Resources/connector_catalog.json"
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

    # Re-sign the finished bundle with a proper code identity (codesign, not the linker's
    # auto-signature). The linker-signed flag makes macOS treat the app as a loose binary, so
    # the microphone privacy prompt never appears and input capture (BlackHole monitoring,
    # recording) is silently denied.
    #
    # Prefer a STABLE self-signed dev certificate over ad-hoc. Ad-hoc's designated requirement is
    # the cdhash, which changes every build — so every rebuild is a "new app" to TCC and the user
    # must re-grant Accessibility (global keypad) / Input Monitoring each time. A self-signed cert
    # gives `identifier "com.neuracoust.daw" and certificate leaf = H"…"`, stable across rebuilds,
    # so the grant persists. Create the cert once (see docs/dev-signing-cert.md) named below; if it
    # is not present on this machine we fall back to ad-hoc "-".
    set(NEURACOUST_DEV_SIGN_CERT_NAME "Neuracoust Dev Signing")
    # A certificate record can remain in Keychain after its private key has disappeared.
    # `find-certificate` still reports that orphan, but codesign then waits/fails forever.
    # `find-identity -p codesigning` lists only identities that currently have a usable key.
    #
    # Deliberately WITHOUT -v: that flag lists only identities the system considers *valid*, i.e.
    # trusted, and a self-signed dev cert is reported CSSMERR_TP_NOT_TRUSTED unless it is explicitly
    # trusted (which needs an admin password). codesign signs with it regardless — trust only affects
    # *verification*. With -v the cert was silently skipped and every build fell back to ad-hoc, which
    # is what kept resetting the Accessibility grant (global keypad) on each rebuild.
    execute_process(
        COMMAND security find-identity -p codesigning
        OUTPUT_VARIABLE _nc_identity_dump ERROR_QUIET)
    string(REGEX MATCH "([0-9A-F]+) \"${NEURACOUST_DEV_SIGN_CERT_NAME}\""
           _nc_cert_match "${_nc_identity_dump}")
    # Capture the hash NOW: the MATCHALL below is another regex op and resets CMAKE_MATCH_1, so
    # reading it after that would always be empty — which silently forced ad-hoc signing (and the
    # per-rebuild TCC/Accessibility resets this whole block exists to prevent).
    set(_nc_cert_hash "${CMAKE_MATCH_1}")
    # More than one identity of the same name is a TCC hazard: which one a build signs with can
    # switch between rebuilds (the first match here, but keychain order is not guaranteed), and a
    # changed signing leaf makes macOS treat the app as new — silently resetting mic/feed and
    # accessibility grants. Surface it so the duplicate can be removed (keep exactly one).
    string(REGEX MATCHALL "\"${NEURACOUST_DEV_SIGN_CERT_NAME}\"" _nc_all_matches "${_nc_identity_dump}")
    list(LENGTH _nc_all_matches _nc_match_count)
    if(_nc_match_count GREATER 1)
        message(WARNING "Found ${_nc_match_count} code-signing identities named '${NEURACOUST_DEV_SIGN_CERT_NAME}'. Rebuilds may switch between them, resetting TCC (microphone/reference-feed, Accessibility) grants. Delete the extras so exactly one remains — Keychain Access → login → My Certificates, or `security delete-identity -Z <sha1>`.")
    endif()
    if(_nc_cert_hash)
        set(NEURACOUST_CODESIGN_IDENTITY "${_nc_cert_hash}")
        message(STATUS "Code-signing app with stable dev cert '${NEURACOUST_DEV_SIGN_CERT_NAME}' (${_nc_cert_hash}) — TCC grants persist across rebuilds.")
    else()
        set(NEURACOUST_CODESIGN_IDENTITY "-")
        message(STATUS "Dev signing cert '${NEURACOUST_DEV_SIGN_CERT_NAME}' not found — ad-hoc signing (TCC grants reset each rebuild). See docs/dev-signing-cert.md.")
    endif()
    # Runs LAST (after the editor hosts are copied in) so the whole bundle is covered.
    add_custom_command(TARGET NeuracoustDAW POST_BUILD
        COMMAND codesign --force --deep --sign "${NEURACOUST_CODESIGN_IDENTITY}" "$<TARGET_BUNDLE_DIR:NeuracoustDAW>"
        COMMENT "Code-signing Neuracoust DAW.app (identity: ${NEURACOUST_CODESIGN_IDENTITY})")
    add_custom_command(TARGET NeuracoustVocalConverter POST_BUILD
        COMMAND codesign --force --deep --sign "${NEURACOUST_CODESIGN_IDENTITY}" "$<TARGET_BUNDLE_DIR:NeuracoustVocalConverter>"
        COMMENT "Code-signing Neuracoust Vocal Converter.app (identity: ${NEURACOUST_CODESIGN_IDENTITY})")
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

# Bundle the out-of-process VST3 worker into the app (defaultVst3ProcessWorkerPath looks for
# it next to the app executable). Without this, out-of-process insert hosting fails — which
# both silenced third-party inserts and cut the editor's live-audio observer (so analyzers
# like Waves F6 / CurvesEQ showed nothing). Mirrors the editor-host bundling above.
if(NEURACOUST_BUILD_SWIFT_APP AND TARGET NeuracoustDAW)
    add_dependencies(NeuracoustDAW neuracoust_vst3_process_worker)
    add_custom_command(TARGET NeuracoustDAW POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:neuracoust_vst3_process_worker>"
            "$<TARGET_BUNDLE_CONTENT_DIR:NeuracoustDAW>/MacOS/Neuracoust VST3 Process Worker")
endif()

# The MIDI library indexer is spawned by the app to classify the collection — bundle it (no libtorch).
if(NEURACOUST_BUILD_SWIFT_APP AND TARGET NeuracoustDAW)
    add_dependencies(NeuracoustDAW neuracoust_midi_indexer)
    add_custom_command(TARGET NeuracoustDAW POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:neuracoust_midi_indexer>"
            "$<TARGET_BUNDLE_CONTENT_DIR:NeuracoustDAW>/MacOS/neuracoust_midi_indexer")
    # Polyphonic audio→MIDI helper (basic-pitch, Python). Bundled as a Resource; runs via system python3.
    add_custom_command(TARGET NeuracoustDAW POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/tools/neuracoust_basic_pitch.py"
            "$<TARGET_BUNDLE_CONTENT_DIR:NeuracoustDAW>/Resources/neuracoust_basic_pitch.py")
    # Apple-Silicon stem path: native Demucs on Metal/MPS. The development runtime is resolved from
    # Stem Magic; unlike the LibTorch helper this script refuses to silently fall back to CPU.
    add_custom_command(TARGET NeuracoustDAW POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/tools/neuracoust_stem_mps.py"
            "$<TARGET_BUNDLE_CONTENT_DIR:NeuracoustDAW>/Resources/neuracoust_stem_mps.py")
endif()

add_executable(neuracoust_vst3_isolated_process_smoke tools/vst3_isolated_process_smoke.cpp)
target_link_libraries(neuracoust_vst3_isolated_process_smoke PRIVATE neuracoust_daw_core)

add_executable(neuracoust_vst3_realtime_bridge_smoke tools/vst3_realtime_bridge_smoke.cpp)
target_link_libraries(neuracoust_vst3_realtime_bridge_smoke PRIVATE neuracoust_daw_core)

add_executable(neuracoust_meter_plugin_fixture_host tools/meter_plugin_fixture_host.cpp)
target_link_libraries(neuracoust_meter_plugin_fixture_host PRIVATE neuracoust_daw_core)

# Times the console strip and the linear-phase monitor FIR against the realtime budget. Wall-clock,
# so it is a hand-run tool rather than a ctest.
add_executable(neuracoust_dsp_load_bench tools/dsp_load_bench.cpp)
target_link_libraries(neuracoust_dsp_load_bench PRIVATE neuracoust_daw_core)

add_executable(neuracoust_vst3_realtime_process_probe tools/vst3_realtime_process_probe.cpp)
target_link_libraries(neuracoust_vst3_realtime_process_probe PRIVATE neuracoust_daw_core)

add_executable(neuracoust_vst3_engine_insert_probe tools/vst3_engine_insert_probe.cpp)
target_link_libraries(neuracoust_vst3_engine_insert_probe PRIVATE neuracoust_daw_core)

add_executable(neuracoust_vst3_midi_instrument_smoke tools/vst3_midi_instrument_smoke.cpp)
target_link_libraries(neuracoust_vst3_midi_instrument_smoke PRIVATE neuracoust_daw_core)

# ARA document-controller probe. A Cocoa harness (NSApplication run loop) used to validate the ARA
# bring-up against a real plug-in without risking the DAW — Melodyne blocks when driven from a bare
# console process. Development only; never bundled into the app.
if(APPLE AND NEURACOUST_HAS_ARA_SDK)
    add_executable(neuracoust_ara_document_probe tools/ara_document_probe.mm)
    target_link_libraries(neuracoust_ara_document_probe PRIVATE neuracoust_daw_core "-framework Cocoa")
    # The same story one level up: drives the engine BRIDGE end to end (import → ARA → commit →
    # save → reload) against a real plug-in. Dev-only, never bundled, never in ctest — it needs a
    # licensed ARA plug-in installed.
    add_executable(neuracoust_ara_bridge_probe tools/ara_bridge_probe.mm)
    target_link_libraries(neuracoust_ara_bridge_probe PRIVATE neuracoust_daw_core "-framework Cocoa")
endif()

add_executable(neuracoust_remote_core_server tools/neuracoust_remote_core_server.cpp)
target_link_libraries(neuracoust_remote_core_server PRIVATE neuracoust_daw_core)

add_executable(neuracoust_daw_render tools/neuracoust_daw_render.cpp)
target_link_libraries(neuracoust_daw_render PRIVATE neuracoust_daw_core)

# MIDI library indexer: parses + classifies a folder tree of .mid files (drum vs melodic, instrument,
# genre, mood) into a TSV the app's MIDI library loads. Bundled into the app so it can be spawned.
add_executable(neuracoust_midi_indexer tools/neuracoust_midi_indexer.cpp)
target_link_libraries(neuracoust_midi_indexer PRIVATE neuracoust_daw_core)
set_property(TARGET neuracoust_midi_indexer PROPERTY CXX_STANDARD 17)

add_executable(neuracoust_audio_measure_harness tools/audio_measure_harness.cpp)
target_link_libraries(neuracoust_audio_measure_harness PRIVATE neuracoust_daw_core)
add_test(NAME audio_measure_harness COMMAND neuracoust_audio_measure_harness)

add_executable(neuracoust_bridge_transport_smoke tools/bridge_transport_smoke.c)
target_link_libraries(neuracoust_bridge_transport_smoke PRIVATE neuracoust_daw_core)

add_executable(neuracoust_external_dsp_crash_test tests/ExternalDspCrashTest.cpp)
target_link_libraries(neuracoust_external_dsp_crash_test PRIVATE neuracoust_daw_core)

add_executable(neuracoust_speaker_catalog_probe tests/SpeakerCatalogProbe.cpp)
target_link_libraries(neuracoust_speaker_catalog_probe PRIVATE neuracoust_daw_core)

add_executable(neuracoust_vst3_initial_parameters_smoke tools/vst3_initial_parameters_smoke.cpp)
target_link_libraries(neuracoust_vst3_initial_parameters_smoke PRIVATE neuracoust_daw_core)

add_executable(neuracoust_listen_share_host_smoke tools/listen_share_host_smoke.cpp)
target_link_libraries(neuracoust_listen_share_host_smoke PRIVATE neuracoust_daw_core)

add_executable(neuracoust_project_io_smoke tools/project_io_smoke.cpp)
target_link_libraries(neuracoust_project_io_smoke PRIVATE neuracoust_daw_core)
if(APPLE)
    # writeTestToneWavFile is only referenced by the fixture builder, so keep the
    # whole archive rather than let the linker drop it.
    target_link_options(neuracoust_project_io_smoke PRIVATE
        "LINKER:-force_load,$<TARGET_FILE:neuracoust_daw_core>")
endif()

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

# --- Stem separation helper (LibTorch / htdemucs), bundled as a SUBPROCESS -----------------------
# Deliberately kept OUT of neuracoust_daw_core: only this one helper links LibTorch, so the ~300 MB
# dependency AND any inference OOM/crash stay out of the main DAW process (spawned like the VST3
# editor host). Guarded on libtorch being present so a checkout without it still builds everything.
set(NEURACOUST_LIBTORCH_DIR "" CACHE PATH "Path to a libtorch distribution for stem separation")
if(NOT NEURACOUST_LIBTORCH_DIR)
    foreach(candidate
            "${CMAKE_SOURCE_DIR}/../Neuracoust Stem Magic/libtorch"
            "${CMAKE_SOURCE_DIR}/third_party/libtorch")
        if(EXISTS "${candidate}/share/cmake/Torch/TorchConfig.cmake")
            set(NEURACOUST_LIBTORCH_DIR "${candidate}")
            break()
        endif()
    endforeach()
endif()
if(NEURACOUST_LIBTORCH_DIR AND EXISTS "${NEURACOUST_LIBTORCH_DIR}/share/cmake/Torch/TorchConfig.cmake")
    list(APPEND CMAKE_PREFIX_PATH "${NEURACOUST_LIBTORCH_DIR}")
    find_package(Torch QUIET)
endif()
if(Torch_FOUND)
    add_executable(neuracoust_stem_separator tools/neuracoust_stem_separator.cpp)
    neuracoust_link_core(neuracoust_stem_separator)
    target_link_libraries(neuracoust_stem_separator PRIVATE ${TORCH_LIBRARIES})
    target_compile_options(neuracoust_stem_separator PRIVATE ${TORCH_CXX_FLAGS})
    set_property(TARGET neuracoust_stem_separator PROPERTY CXX_STANDARD 17)
    if(APPLE)
        # ADD @executable_path/../Frameworks to the build RPATH (CMake keeps the auto libtorch/lib
        # path too), so the SAME binary runs both from build/ and once copied into the app bundle.
        set_property(TARGET neuracoust_stem_separator APPEND PROPERTY
            BUILD_RPATH "@executable_path/../Frameworks")
    endif()
    # CREPE neural pitch detector — same libtorch, its own subprocess helper.
    add_executable(neuracoust_pitch_detector tools/neuracoust_pitch_detector.cpp)
    neuracoust_link_core(neuracoust_pitch_detector)
    target_link_libraries(neuracoust_pitch_detector PRIVATE ${TORCH_LIBRARIES})
    target_compile_options(neuracoust_pitch_detector PRIVATE ${TORCH_CXX_FLAGS})
    set_property(TARGET neuracoust_pitch_detector PROPERTY CXX_STANDARD 17)
    if(APPLE)
        set_property(TARGET neuracoust_pitch_detector APPEND PROPERTY BUILD_RPATH "@executable_path/../Frameworks")
    endif()
    # Neural noise-floor denoiser — same libtorch, its own subprocess helper.
    add_executable(neuracoust_denoiser tools/neuracoust_denoiser.cpp)
    neuracoust_link_core(neuracoust_denoiser)
    target_include_directories(neuracoust_denoiser PRIVATE tools)
    target_link_libraries(neuracoust_denoiser PRIVATE ${TORCH_LIBRARIES})
    target_compile_options(neuracoust_denoiser PRIVATE ${TORCH_CXX_FLAGS})
    set_property(TARGET neuracoust_denoiser PROPERTY CXX_STANDARD 17)
    if(APPLE)
        set_property(TARGET neuracoust_denoiser APPEND PROPERTY BUILD_RPATH "@executable_path/../Frameworks")
    endif()

    # The htdemucs + CREPE models ship next to the libtorch distribution (in the Stem Magic folder).
    get_filename_component(NEURACOUST_STEM_MODEL_DIR "${NEURACOUST_LIBTORCH_DIR}" DIRECTORY)
    set(NEURACOUST_STEM_MODEL "${NEURACOUST_STEM_MODEL_DIR}/demucs.pt")
    set(NEURACOUST_CREPE_MODEL "${NEURACOUST_STEM_MODEL_DIR}/crepe_full.pt")
    set(NEURACOUST_CREPE_TINY_MODEL "${NEURACOUST_STEM_MODEL_DIR}/crepe_tiny.pt")
    set(NEURACOUST_DENOISER_MODEL "${NEURACOUST_STEM_MODEL_DIR}/denoiser.pt")
    message(STATUS "Stem separation + CREPE + denoiser ENABLED (libtorch: ${NEURACOUST_LIBTORCH_DIR})")

    # Bundle the helper + the torch dylibs it needs + the model into DW.app (only under a Swift-capable
    # generator, where NeuracoustDAW exists). The helper is spawned as a subprocess at runtime.
    if(TARGET NeuracoustDAW)
        set(_torch_lib "${NEURACOUST_LIBTORCH_DIR}/lib")
        add_custom_command(TARGET NeuracoustDAW POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:neuracoust_stem_separator>"
                "$<TARGET_BUNDLE_CONTENT_DIR:NeuracoustDAW>/MacOS/neuracoust_stem_separator"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:neuracoust_pitch_detector>"
                "$<TARGET_BUNDLE_CONTENT_DIR:NeuracoustDAW>/MacOS/neuracoust_pitch_detector"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:neuracoust_denoiser>"
                "$<TARGET_BUNDLE_CONTENT_DIR:NeuracoustDAW>/MacOS/neuracoust_denoiser"
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_BUNDLE_CONTENT_DIR:NeuracoustDAW>/Frameworks"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_torch_lib}/libtorch.dylib" "${_torch_lib}/libtorch_cpu.dylib"
                "${_torch_lib}/libc10.dylib" "${_torch_lib}/libomp.dylib"
                "${_torch_lib}/libtorch_global_deps.dylib"
                "$<TARGET_BUNDLE_CONTENT_DIR:NeuracoustDAW>/Frameworks/"
            COMMENT "Bundling stem separator + libtorch dylibs")
        if(EXISTS "${NEURACOUST_STEM_MODEL}")
            add_custom_command(TARGET NeuracoustDAW POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${NEURACOUST_STEM_MODEL}"
                    "$<TARGET_BUNDLE_CONTENT_DIR:NeuracoustDAW>/Resources/demucs.pt"
                COMMENT "Bundling demucs.pt model")
        else()
            message(WARNING "demucs.pt not found at ${NEURACOUST_STEM_MODEL}; stem separation will fail at runtime until it is bundled")
        endif()
        # Optional 6-source model (guitar/piano too). Bundled only if present; the UI hides the 6-part
        # option and the helper falls back to demucs.pt otherwise.
        if(EXISTS "${NEURACOUST_STEM_MODEL_DIR}/htdemucs_6s.pt")
            add_custom_command(TARGET NeuracoustDAW POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${NEURACOUST_STEM_MODEL_DIR}/htdemucs_6s.pt"
                    "$<TARGET_BUNDLE_CONTENT_DIR:NeuracoustDAW>/Resources/htdemucs_6s.pt"
                COMMENT "Bundling htdemucs_6s.pt model (6-part separation)")
        else()
            message(STATUS "htdemucs_6s.pt not found — 6-part separation unavailable until it is exported (tools/export_demucs_6s_torchscript.py)")
        endif()
        # Optional drum-split model (kick/snare/toms/cymbals, or +hihat). Same TorchScript contract; the
        # helper is run with --kind drum so the sources are named as drums. UI hides the option otherwise.
        if(EXISTS "${NEURACOUST_STEM_MODEL_DIR}/drumsep.pt")
            add_custom_command(TARGET NeuracoustDAW POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${NEURACOUST_STEM_MODEL_DIR}/drumsep.pt"
                    "$<TARGET_BUNDLE_CONTENT_DIR:NeuracoustDAW>/Resources/drumsep.pt"
                COMMENT "Bundling drumsep.pt model (drum split: kick/snare/toms/cymbals)")
        else()
            message(STATUS "drumsep.pt not found — drum split unavailable until it is exported (tools/export_demucs_checkpoint_torchscript.py <drumsep.th> …/drumsep.pt)")
        endif()
        # Optional orchestra-family model (strings/brass/woodwinds/… — experimental). Run with
        # --kind orchestra. No such model ships with Demucs; the option appears only if one is bundled.
        if(EXISTS "${NEURACOUST_STEM_MODEL_DIR}/orchestra.pt")
            add_custom_command(TARGET NeuracoustDAW POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${NEURACOUST_STEM_MODEL_DIR}/orchestra.pt"
                    "$<TARGET_BUNDLE_CONTENT_DIR:NeuracoustDAW>/Resources/orchestra.pt"
                COMMENT "Bundling orchestra.pt model (experimental family split)")
        else()
            message(STATUS "orchestra.pt not found — orchestra family split unavailable (needs a trained checkpoint; tools/export_demucs_checkpoint_torchscript.py)")
        endif()
        if(EXISTS "${NEURACOUST_CREPE_MODEL}")
            add_custom_command(TARGET NeuracoustDAW POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${NEURACOUST_CREPE_MODEL}"
                    "$<TARGET_BUNDLE_CONTENT_DIR:NeuracoustDAW>/Resources/crepe_full.pt"
                COMMENT "Bundling crepe_full.pt model")
        else()
            message(STATUS "crepe_full.pt not found — CREPE pitch detection unavailable until it is exported (tools/export_crepe_torchscript.py)")
        endif()
        if(EXISTS "${NEURACOUST_CREPE_TINY_MODEL}")
            add_custom_command(TARGET NeuracoustDAW POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${NEURACOUST_CREPE_TINY_MODEL}"
                    "$<TARGET_BUNDLE_CONTENT_DIR:NeuracoustDAW>/Resources/crepe_tiny.pt"
                COMMENT "Bundling crepe_tiny.pt model (fast detection option)")
        else()
            message(STATUS "crepe_tiny.pt not found — the fast CREPE option will fall back to the full model")
        endif()
        if(EXISTS "${NEURACOUST_DENOISER_MODEL}")
            add_custom_command(TARGET NeuracoustDAW POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${NEURACOUST_DENOISER_MODEL}"
                    "$<TARGET_BUNDLE_CONTENT_DIR:NeuracoustDAW>/Resources/denoiser.pt"
                COMMENT "Bundling denoiser.pt model")
        else()
            message(STATUS "denoiser.pt not found — 노이즈 제거 unavailable until it is exported (tools/export_denoiser_torchscript.py)")
        endif()
    endif()
else()
    message(STATUS "Stem separation disabled (no libtorch found)")
endif()

# The optional stem helpers and LibTorch frameworks are appended after the main app's
# original signing step. Re-sign once more after every bundle copy so deep validation
# covers those late-added binaries and dylibs as well.
if(TARGET NeuracoustDAW AND DEFINED NEURACOUST_CODESIGN_IDENTITY)
    add_custom_command(TARGET NeuracoustDAW POST_BUILD
        COMMAND codesign --force --deep --sign "${NEURACOUST_CODESIGN_IDENTITY}" "$<TARGET_BUNDLE_DIR:NeuracoustDAW>"
        COMMENT "Final code-sign after all Neuracoust DAW bundle resources")
endif()

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

add_executable(neuracoust_spectrum_fft_tests tests/SpectrumFftSmokeTest.cpp)
neuracoust_link_core(neuracoust_spectrum_fft_tests)
neuracoust_enable_release_asserts(neuracoust_spectrum_fft_tests)
add_test(NAME spectrum_fft_smoke COMMAND neuracoust_spectrum_fft_tests)

add_executable(neuracoust_loudness_meter_tests tests/LoudnessMeterSmokeTest.cpp)
neuracoust_link_core(neuracoust_loudness_meter_tests)
neuracoust_enable_release_asserts(neuracoust_loudness_meter_tests)
add_test(NAME loudness_meter_smoke COMMAND neuracoust_loudness_meter_tests)

add_executable(neuracoust_test_signal_generator_tests tests/TestSignalGeneratorSmokeTest.cpp)
neuracoust_link_core(neuracoust_test_signal_generator_tests)
neuracoust_enable_release_asserts(neuracoust_test_signal_generator_tests)
add_test(NAME test_signal_generator_smoke COMMAND neuracoust_test_signal_generator_tests)

add_executable(neuracoust_time_pitch_tests tests/TimePitchSmokeTest.cpp)
neuracoust_link_core(neuracoust_time_pitch_tests)
neuracoust_enable_release_asserts(neuracoust_time_pitch_tests)
add_test(NAME time_pitch_smoke COMMAND neuracoust_time_pitch_tests)

add_executable(neuracoust_pitch_editor_tests tests/PitchEditorSmokeTest.cpp)
neuracoust_link_core(neuracoust_pitch_editor_tests)
neuracoust_enable_release_asserts(neuracoust_pitch_editor_tests)
add_test(NAME pitch_editor_smoke COMMAND neuracoust_pitch_editor_tests)

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
add_test(NAME vst3_initial_parameters_smoke COMMAND neuracoust_vst3_initial_parameters_smoke)
add_test(NAME listen_share_host_smoke COMMAND neuracoust_listen_share_host_smoke)

# Document plumbing only — no audio device, so this one runs anywhere.
add_test(NAME project_io_smoke COMMAND neuracoust_project_io_smoke)

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

add_executable(neuracoust_spectrum_pipeline_probe tests/SpectrumPipelineProbe.cpp)
target_link_libraries(neuracoust_spectrum_pipeline_probe PRIVATE neuracoust_daw_core)

add_executable(neuracoust_time_sig_bridge_smoke tests/TimeSignatureBridgeSmoke.cpp)
target_link_libraries(neuracoust_time_sig_bridge_smoke PRIVATE neuracoust_daw_core)
add_test(NAME time_sig_bridge_smoke COMMAND neuracoust_time_sig_bridge_smoke)

add_executable(neuracoust_plugin_automation_bridge_smoke tests/PluginAutomationBridgeSmoke.cpp)
target_link_libraries(neuracoust_plugin_automation_bridge_smoke PRIVATE neuracoust_daw_core)
add_test(NAME plugin_automation_bridge_smoke COMMAND neuracoust_plugin_automation_bridge_smoke)

add_executable(neuracoust_grid_quantum_bridge_smoke tests/GridQuantumBridgeSmoke.cpp)
target_link_libraries(neuracoust_grid_quantum_bridge_smoke PRIVATE neuracoust_daw_core)
add_test(NAME grid_quantum_bridge_smoke COMMAND neuracoust_grid_quantum_bridge_smoke)

add_executable(neuracoust_track_reorder_bridge_smoke tests/TrackReorderBridgeSmoke.cpp)
target_link_libraries(neuracoust_track_reorder_bridge_smoke PRIVATE neuracoust_daw_core)
add_test(NAME track_reorder_bridge_smoke COMMAND neuracoust_track_reorder_bridge_smoke)

add_executable(neuracoust_pan_law_smoke tests/PanLawSmoke.cpp)
target_link_libraries(neuracoust_pan_law_smoke PRIVATE neuracoust_daw_core)
add_test(NAME pan_law_smoke COMMAND neuracoust_pan_law_smoke)

add_executable(neuracoust_metronome_smoke tests/MetronomeSmoke.cpp)
target_link_libraries(neuracoust_metronome_smoke PRIVATE neuracoust_daw_core)
add_test(NAME metronome_smoke COMMAND neuracoust_metronome_smoke)

add_executable(neuracoust_parametric_eq_tests tests/ParametricEqTest.cpp)
neuracoust_link_core(neuracoust_parametric_eq_tests)
neuracoust_enable_release_asserts(neuracoust_parametric_eq_tests)
add_test(NAME parametric_eq COMMAND neuracoust_parametric_eq_tests)

add_executable(neuracoust_console_channel_tests
    tests/ConsoleChannelProcessorTest.cpp
    src/audio/ConsoleChannelProcessor.cpp)
target_include_directories(neuracoust_console_channel_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
neuracoust_enable_release_asserts(neuracoust_console_channel_tests)
add_test(NAME console_channel_4000e COMMAND neuracoust_console_channel_tests)

add_executable(neuracoust_sweep_measurement_tests tests/SweepMeasurementTest.cpp)
neuracoust_link_core(neuracoust_sweep_measurement_tests)
neuracoust_enable_release_asserts(neuracoust_sweep_measurement_tests)
add_test(NAME sweep_measurement COMMAND neuracoust_sweep_measurement_tests)

add_executable(neuracoust_monitor_correction_tests tests/MonitorCorrectionTest.cpp)
neuracoust_link_core(neuracoust_monitor_correction_tests)
neuracoust_enable_release_asserts(neuracoust_monitor_correction_tests)
add_test(NAME monitor_correction COMMAND neuracoust_monitor_correction_tests)

# Vocal alignment (MFCC-DTW) — pure DSP, always runs.
add_executable(neuracoust_vocal_align_tests tests/VocalAlignTest.cpp)
neuracoust_link_core(neuracoust_vocal_align_tests)
add_test(NAME vocal_align COMMAND neuracoust_vocal_align_tests)

# Denoiser band-split DSP (resampler + reconstruction) — pure, no libtorch, always runs.
add_executable(neuracoust_denoiser_dsp_tests tests/DenoiserDspTest.cpp)
target_include_directories(neuracoust_denoiser_dsp_tests PRIVATE tools)
set_property(TARGET neuracoust_denoiser_dsp_tests PROPERTY CXX_STANDARD 17)
add_test(NAME denoiser_dsp COMMAND neuracoust_denoiser_dsp_tests)

add_executable(neuracoust_output_chain_modeling_tests tests/OutputChainModelingTest.cpp)
neuracoust_link_core(neuracoust_output_chain_modeling_tests)
neuracoust_enable_release_asserts(neuracoust_output_chain_modeling_tests)
add_test(NAME output_chain_modeling COMMAND neuracoust_output_chain_modeling_tests)

add_executable(neuracoust_monitor_fir_eq_tests tests/MonitorFirEqTest.cpp)
neuracoust_link_core(neuracoust_monitor_fir_eq_tests)
neuracoust_enable_release_asserts(neuracoust_monitor_fir_eq_tests)
add_test(NAME monitor_fir_eq COMMAND neuracoust_monitor_fir_eq_tests)

add_executable(neuracoust_automation_mode_bridge_smoke tests/AutomationModeBridgeSmoke.cpp)
target_link_libraries(neuracoust_automation_mode_bridge_smoke PRIVATE neuracoust_daw_core)
add_test(NAME automation_mode_bridge_smoke COMMAND neuracoust_automation_mode_bridge_smoke)

add_executable(neuracoust_mackie_hui_tests tests/MackieHuiProtocolTest.cpp)
target_link_libraries(neuracoust_mackie_hui_tests PRIVATE neuracoust_daw_core)
add_test(NAME mackie_hui_protocol COMMAND neuracoust_mackie_hui_tests)
