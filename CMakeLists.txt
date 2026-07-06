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

# spatcore-audio compiles Metal .mm (Objective-C++) backends on Apple and
# enables OBJCXX from its own add_subdirectory scope. CMake only reliably
# populates the OBJCXX compile rules (CMAKE_OBJCXX_COMPILE_OBJECT) when the
# language is enabled at the top level, so do it here — otherwise macOS
# configure fails at generate with "Missing variable: CMAKE_OBJCXX_COMPILE_OBJECT".
if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "macOS deployment target")
endif()

# ---------------------------------------------------------------------------
# JUCE (submodule) — must exist before spatcore per its consumer contract
# ---------------------------------------------------------------------------
add_subdirectory(ThirdParty/JUCE "${CMAKE_BINARY_DIR}/juce" EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# juce_simpleweb (vendored module) — required by spatcore-control's MCP
# transport. Same two consumer-side fixups as spatcore's reference wiring:
#  1. its .cpps were authored for Projucer and include "JuceHeader.h" — give
#     the module a minimal stub on its interface include path;
#  2. its module header declares libssl/libcrypto/z link libs unconditionally,
#     but spatcore builds it with SIMPLEWEB_SECURE_SUPPORTED=0 (no TLS), so
#     strip them to avoid an OpenSSL SDK requirement.
# ---------------------------------------------------------------------------
juce_add_module("${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/juce_simpleweb")
target_include_directories(juce_simpleweb INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/JuceHeaderStub")
get_target_property(_sw_libs juce_simpleweb INTERFACE_LINK_LIBRARIES)
if(_sw_libs)
    list(REMOVE_ITEM _sw_libs libssl libcrypto z)
    set_target_properties(juce_simpleweb PROPERTIES INTERFACE_LINK_LIBRARIES "${_sw_libs}")
endif()

# ---------------------------------------------------------------------------
# roli_blocks_basics (vendored module) — Lightpad support in spatcore-controllers
# ---------------------------------------------------------------------------
juce_add_module("${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/roli_blocks_basics")

# ---------------------------------------------------------------------------
# hidapi (submodule) — spatcore-controllers needs the HEADERS only; the
# platform implementation is built here (static) and linked into the app.
# ---------------------------------------------------------------------------
set(BUILD_SHARED_LIBS OFF)
set(HIDAPI_BUILD_HIDTEST OFF)
set(HIDAPI_WITH_LIBUSB OFF)   # Linux: hidraw backend only (matches WFS-DIY)
# hidapi declares cmake_minimum_required(3.1.3...) — CMake 4.x refuses
# minimums below 3.5 without this floor (no effect on CMake < 4).
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
add_subdirectory(ThirdParty/hidapi "${CMAKE_BINARY_DIR}/hidapi" EXCLUDE_FROM_ALL)
unset(CMAKE_POLICY_VERSION_MINIMUM)
set(SPATCORE_HIDAPI_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/hidapi")

# ---------------------------------------------------------------------------
# spatcore (submodule) — spatcore-audio / spatcore-control / spatcore-controllers
# ---------------------------------------------------------------------------
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
