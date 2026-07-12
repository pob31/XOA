# XOA unit tests — added unconditionally by the root CMakeLists (enable_testing).
# Dependency-free (no gtest): plain CHECK macro, exit code 0 = pass. Clones the
# spatcore/tests pattern; spatcore's own suite stays off (SPATCORE_STANDALONE_TESTS
# is a spatcore-repo concern, not a consumer one).

juce_add_console_app(xoa-tests PRODUCT_NAME "xoa-tests")

target_sources(xoa-tests PRIVATE
    XoaTests.cpp
    XoaParameterTests.cpp
    XoaShTests.cpp
    XoaRotationTests.cpp
    XoaDecoderTests.cpp
    XoaFilePlayerTests.cpp
    XoaBusTests.cpp
    XoaSceneTests.cpp
    XoaEngineTests.cpp
    XoaVbapTests.cpp
    XoaCompTests.cpp
    XoaTestSignalTests.cpp
    ../Source/Parameters/XoaValueTreeState.cpp
    ../Source/Parameters/XoaFileManager.cpp
    ../Source/Audio/FilePlayer.cpp
    ../Source/Audio/AudioEngine.cpp
    ../Source/DSP/ConvexHull.cpp)

# XoaConstants.h and, later, the Source/DSP/Ambi* headers under test.
target_include_directories(xoa-tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../Source")

# The JUCE modules arrive transitively through the libs' PUBLIC link
# interface (their sources compile into this target, JUCE-module style).
# juce_audio_utils brings juce_audio_devices (AudioTransportSource) for
# FilePlayer; harmless headless (no device is opened by the tests).
target_link_libraries(xoa-tests PRIVATE
    spatcore-audio
    spatcore-control
    juce::juce_audio_utils)

target_compile_definitions(xoa-tests PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    XOA_VERSION_STRING="${PROJECT_VERSION}"
    XOA_TESTS_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

# Test TUs compile under the same transcribed app flags as the app itself
# (the optimization/FP flags spatcore's bit-exactness gates were baselined
# with) — offline-render baselines from WP6 on depend on this.
spatcore_apply_compile_flags(xoa-tests)

add_test(NAME xoa-tests COMMAND xoa-tests)
