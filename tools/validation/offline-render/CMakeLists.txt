# XOA offline-render harness - added by the root CMakeLists (in-tree, unlike the
# Projucer-era WFS-DIY harness which self-configured JUCE). One configure builds
# it alongside the app; the ctest smoke runs it headless on all three CI OSes.
#
# Baselines gate on the pinned Release flags, so this MUST call
# spatcore_apply_compile_flags (as the app and tests do) to stay bit-exact.

juce_add_console_app(xoa-offline-render PRODUCT_NAME "xoa-offline-render")

target_sources(xoa-offline-render PRIVATE
    main.cpp
    ../../../Source/DSP/ConvexHull.cpp)

# The app DSP headers (Source/DSP/Ambi*, Source/Audio/TestSceneGenerator.h) are
# header-only and pulled in by main.cpp; DecoderMatrixBuilder.h transitively
# includes the parameter store, so compile its TU in too.
target_include_directories(xoa-offline-render PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../Source")

target_link_libraries(xoa-offline-render PRIVATE
    spatcore-audio
    spatcore-control)

target_compile_definitions(xoa-offline-render PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    XOA_VERSION_STRING="${PROJECT_VERSION}")

spatcore_apply_compile_flags(xoa-offline-render)

# CI smoke: build + run headless (no --check; per-machine baselines stay local).
add_test(NAME xoa-offline-render-smoke
         COMMAND xoa-offline-render --scenario all --blocks 40)
