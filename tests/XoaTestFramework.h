#pragma once

/*
    XoaTestFramework.h - shared plumbing for the xoa-tests console app.
    Dependency-free CHECK pattern (no gtest); exit 0 = pass. See
    tools/reference/README.md for the golden-data conventions.
*/

#include <juce_core/juce_core.h>

#include <cstdio>

inline int failures = 0;

#define CHECK(expr)                                                          \
    do                                                                       \
    {                                                                        \
        if (!(expr))                                                         \
        {                                                                    \
            std::fprintf (stderr, "FAIL %s:%d: %s\n",                        \
                          __FILE__, __LINE__, #expr);                        \
            ++failures;                                                      \
        }                                                                    \
    } while (false)

/** Unique temp directory, recursively deleted on scope exit.
    (juce::TemporaryFile is for atomic single-file replacement — wrong tool
    for a project-folder sandbox.) */
struct ScopedTempDir
{
    ScopedTempDir()
    {
        dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                  .getChildFile ("xoa-tests-" + juce::String::toHexString (
                                     juce::Random::getSystemRandom().nextInt64()));
        dir.createDirectory();
    }

    ~ScopedTempDir() { dir.deleteRecursively(); }

    juce::File dir;
};

/** Drop the XML declaration and header comment lines (the "Created:" line
    varies per write), so save->load->save byte comparisons see only payload. */
inline juce::String stripXmlHeader (const juce::String& fileText)
{
    juce::StringArray lines;
    lines.addLines (fileText);
    while (! lines.isEmpty()
           && (lines[0].startsWith ("<?xml") || lines[0].startsWith ("<!--")
               || lines[0].trim().isEmpty()))
        lines.remove (0);
    return lines.joinIntoString ("\n");
}
