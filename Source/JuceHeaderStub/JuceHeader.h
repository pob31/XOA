#pragma once

// Stub JuceHeader.h for CMake builds that compile the vendored juce_simpleweb
// module (same trick as spatcore/tests/standalone/JuceHeaderStub and
// WFS-DIY's Plugin/Source/JuceHeaderStub).
//
// juce_simpleweb's .cpp files were authored for Projucer, which generates a
// global JuceHeader.h on the include path. CMake builds don't have one, so
// this minimal forwarder is placed on the include path of the juce_simpleweb
// module target by the root CMakeLists.txt.

#include <juce_core/juce_core.h>
#include <juce_cryptography/juce_cryptography.h>
