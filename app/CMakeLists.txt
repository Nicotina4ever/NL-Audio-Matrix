cmake_minimum_required(VERSION 3.22)

project(NLMatrix VERSION 0.4.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Local JUCE 9.0.4 tree, used read-only. No modifications to JUCE/.
set(JUCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../JUCE" CACHE PATH "Path to the JUCE source tree")
if(NOT EXISTS "${JUCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "JUCE not found at ${JUCE_DIR}. Set -DJUCE_DIR=<path>.")
endif()

set(JUCE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(JUCE_BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
add_subdirectory("${JUCE_DIR}" "${CMAKE_BINARY_DIR}/JUCE")

function(nlmatrix_console_app target)
    juce_add_console_app(${target}
        PRODUCT_NAME "${target}"
        COMPANY_NAME "Nicola Lanci"
        VERSION "${PROJECT_VERSION}"
        NEEDS_WEB_BROWSER FALSE
        NEEDS_CURL FALSE)

    target_sources(${target} PRIVATE ${ARGN})

    target_compile_definitions(${target} PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_ASIO=1
        JUCE_WASAPI=1)

    target_link_libraries(${target} PRIVATE
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_basics
        juce::juce_core
        juce::juce_events
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
endfunction()

nlmatrix_console_app(NLMatrixProbe  src/main.cpp)
nlmatrix_console_app(NLMatrixLive   src/live_main.cpp)
# The isolated Windows 11 VM has an older VC++ runtime; use a self-contained
# build for driver/audio tests so the test app does not depend on the guest CRT.
set_property(TARGET NLMatrixLive PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
nlmatrix_console_app(NLMatrixBridge src/bridge_main.cpp src/wasapi_loopback.cpp)
nlmatrix_console_app(NLMatrixRouter src/router_main.cpp src/wasapi_loopback.cpp src/process_loopback.cpp src/nl_driver_client.cpp)

target_include_directories(NLMatrixRouter PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../driver)
set_property(TARGET NLMatrixRouter PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")

# Legacy engine comparison build: the pre-refactor router sources (old paced
# writer, fixed 30 ms ring target), kept only to produce reproducible
# before/after continuity evidence on the same driver ABI.  Not part of the
# product artefact set.
nlmatrix_console_app(NLMatrixRouterLegacy
    legacy/router_legacy_main.cpp
    src/wasapi_loopback.cpp
    src/process_loopback.cpp
    legacy/nl_driver_client.cpp)
target_include_directories(NLMatrixRouterLegacy BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/legacy
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(NLMatrixRouterLegacy PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../driver)
set_property(TARGET NLMatrixRouterLegacy PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
if(WIN32)
    target_link_libraries(NLMatrixRouterLegacy PRIVATE ole32 propsys mmdevapi)
endif()

nlmatrix_console_app(NLMatrixDriverAudioTest src/driver_audio_test.cpp)
set_property(TARGET NLMatrixDriverAudioTest PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")

add_executable(NLMatrixEndpointVolume src/endpoint_volume.cpp)
target_link_libraries(NLMatrixEndpointVolume PRIVATE ole32 propsys)
set_property(TARGET NLMatrixEndpointVolume PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")

add_executable(NLMatrixLoopbackTest src/loopback_test.cpp src/wasapi_loopback.cpp)
target_link_libraries(NLMatrixLoopbackTest PRIVATE ole32 propsys)
set_property(TARGET NLMatrixLoopbackTest PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")

add_executable(NLMatrixDriverTest src/driver_test.cpp src/nl_driver_client.cpp)
target_include_directories(NLMatrixDriverTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../driver)
target_compile_features(NLMatrixDriverTest PRIVATE cxx_std_20)

juce_add_gui_app(NLMatrixUI
    PRODUCT_NAME "NLMatrixUI"
    COMPANY_NAME "Nicola Lanci"
    BUNDLE_ID "com.nicolalanci.nlaudiomatrix"
    VERSION "0.4.0"
    NEEDS_WEB_BROWSER FALSE
    NEEDS_CURL FALSE
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/../assets/nlmatrix_icon_big.png"
    ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/../assets/nlmatrix_icon_small.png")

target_sources(NLMatrixUI PRIVATE src/ui_main.cpp)

target_compile_definitions(NLMatrixUI PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_ASIO=1
    JUCE_WASAPI=1)

set_property(TARGET NLMatrixUI PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
target_compile_definitions(NLMatrixUI PRIVATE JUCE_USER_DEFINED_RC_FILE="${CMAKE_CURRENT_SOURCE_DIR}/src/NLMatrixUI.rc")
set_target_properties(NLMatrixUI PROPERTIES
    JUCE_COMPANY_NAME "Nicola Lanci"
    JUCE_VERSION "0.4.0"
    JUCE_BUILD_VERSION "0.4.0")
target_link_libraries(NLMatrixUI PRIVATE
    juce::juce_gui_basics
    juce::juce_gui_extra
    juce::juce_audio_devices
    juce::juce_audio_basics
    juce::juce_core
    juce::juce_events
    juce::juce_data_structures
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

if(WIN32)
    target_link_libraries(NLMatrixBridge PRIVATE ole32 propsys)
    target_link_libraries(NLMatrixRouter PRIVATE ole32 propsys mmdevapi)
    target_link_libraries(NLMatrixUI PRIVATE ole32 propsys mmdevapi)
endif()
