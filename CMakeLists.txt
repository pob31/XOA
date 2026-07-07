# XOA — tenth-order Ambisonics spatial audio processor.
#
# CMake-native consumer of spatcore (the shared real-time core extracted from
# WFS-DIY). The consumer-provides-dependencies contract and the reference
# wiring this file follows are documented in spatcore/CMakeLists.txt and
# spatcore/tests/standalone/CMakeLists.txt.
#
# Configure/build (Windows example, from the repo root):
#   cmake -S . -B build
#   cmake --build build --config Release
# Binary: build/XOA_artefacts/<config>/

cmake_minimum_required(VERSION 3.22)

project(XOA VERSION 0.1.0 LANGUAGES C CXX)

# spatcore's CMake-native consumer helpers — OBJCXX enable, juce_simpleweb and
# hidapi wiring. Centralized in the submodule (spatcore/cmake/SpatcoreConsumer.cmake)
# so these fiddly, drift-prone, platform-specific bits live in ONE place across
# XOA / Tight-WFS / spatcore's own example, and a fix arrives on a pin bump.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/spatcore/cmake")
include(SpatcoreConsumer)

# Enable OBJCXX at this top level on Apple (spatcore's Metal backends AND JUCE's
# module sources are .mm and compile into this app's own targets). Must run
# before add_subdirectory(JUCE).
spatcore_enable_consumer_languages()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "macOS deployment target")
endif()

# ---------------------------------------------------------------------------
# Consumer-provides dependencies (see spatcore/README.md). The helpers apply
# the required fixups: juce_simpleweb gets the JuceHeader stub + OpenSSL strip;
# hidapi is built static (hidraw on Linux) and its header dir exported to
# spatcore. JUCE and roli_blocks_basics need no fixups.
# ---------------------------------------------------------------------------
add_subdirectory(ThirdParty/JUCE "${CMAKE_BINARY_DIR}/juce" EXCLUDE_FROM_ALL)
spatcore_add_juce_simpleweb("${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/juce_simpleweb")
juce_add_module("${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/roli_blocks_basics")
spatcore_add_hidapi("${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/hidapi")

add_subdirectory(spatcore "${CMAKE_BINARY_DIR}/spatcore")

# ---------------------------------------------------------------------------
# The XOA application
# ---------------------------------------------------------------------------
juce_add_gui_app(XOA
    PRODUCT_NAME                    "XOA"
    VERSION                         "${PROJECT_VERSION}"
    COMPANY_NAME                    "Pix et Bel"
    COMPANY_EMAIL                   "contact@pixetbel.org"
    COMPANY_WEBSITE                 "wfs-diy.net"
    BUNDLE_ID                       "org.pixetbel.xoa"
    MICROPHONE_PERMISSION_ENABLED   TRUE
    MICROPHONE_PERMISSION_TEXT      "XOA needs audio input access for Ambisonics processing.")

target_sources(XOA PRIVATE
    Source/Main.cpp
    Source/MainComponent.cpp)

target_include_directories(XOA PRIVATE Source)

target_compile_definitions(XOA PRIVATE
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:XOA,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:XOA,JUCE_VERSION>"
    $<$<PLATFORM_ID:Linux>:JUCE_JACK=1>)

target_link_libraries(XOA PRIVATE
    spatcore-audio
    spatcore-control
    spatcore-controllers
    hidapi::hidapi
    juce::juce_audio_utils
    juce::juce_gui_extra
    juce::juce_osc)

# Same optimization / floating-point flags the spatcore bit-exactness gates
# were baselined with (spatcore's DSP is header-only from the consumer's
# point of view, so the app's flags are the flags it compiles under).
spatcore_apply_compile_flags(XOA)

# ---------------------------------------------------------------------------
# Tests — the xoa-tests console app, registered with ctest.
# (After the JUCE/spatcore subdirectories: tests/ needs their targets.)
# ---------------------------------------------------------------------------
enable_testing()
add_subdirectory(tests)
