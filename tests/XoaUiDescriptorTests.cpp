/*
    XoaUiDescriptorTests.cpp - WP10 C4.

    The GUI parameter-completeness gate, headless (no GUI/widgets linked). It
    proves the UI descriptor table is a faithful, exhaustive mirror of the schema:

      - descriptor set == the id universe (constraints bounds ids UNION osc
        bindings ids UNION the explicit bool/string extras), in BOTH directions
        and with no duplicate descriptor;
      - every combo descriptor lists exactly (max - min + 1) enum-label keys, and
        non-combo descriptors carry none;
      - each descriptor's unit/label keys are non-empty where required;
      - every TabParameterRegistry row resolves to a descriptor (registry subset
        of descriptors).

    The complementary direction - every non-system descriptor reachable from at
    least one registered surface, i.e. "every v1 parameter reachable from the GUI"
    (DEVPLAN WP10 exit) - is enabled at C9 once all six tabs have registered their
    rows. Until then the registry is grown one chunk at a time.
*/

#include <juce_core/juce_core.h>

#include <set>
#include <string>

#include "XoaTestFramework.h"

#include "GUI/Binding/UiParameterDescriptors.h"
#include "GUI/Binding/TabParameterRegistry.h"
#include "Parameters/XoaConstraints.h"
#include "Network/OSCMessageRouter.h"

namespace
{
using StrSet = std::set<juce::String>;

// bool/string parameters that carry no numeric bounds and no OSC binding, yet are
// real v1 parameters a surface must expose (or reach via a system surface).
const char* const kExtras[] = {
    "showName", "playbackFilePath",
    "oscEnabled", "oscTcpEnabled", "oscAcceptAnyHost", "oscFeedbackEnabled",
    "oscMeterEnabled", "oscSendAddress", "audioDeviceState"
};

StrSet buildUniverse()
{
    StrSet u;
    for (const auto& b : xoa::constraints::allBounds())
        u.insert (b.first.toString());
    for (const auto& b : xoa::osc::bindings())
        u.insert (b.id.toString());
    for (const char* e : kExtras)
        u.insert (juce::String (e));
    return u;
}
} // namespace

void runXoaUiDescriptorTests()
{
    const StrSet universe = buildUniverse();

    // Descriptor set (and duplicate-descriptor guard).
    StrSet descriptors;
    for (const auto& d : xoa::ui::allDescriptors())
    {
        const bool inserted = descriptors.insert (d.id.toString()).second;
        CHECK (inserted); // no duplicate descriptor for an id
    }

    // Both directions: descriptors == universe.
    for (const auto& id : universe)
    {
        if (descriptors.count (id) == 0)
            std::fprintf (stderr, "FAIL descriptors: no UI descriptor for '%s'\n", id.toRawUTF8());
        CHECK (descriptors.count (id) == 1);
    }
    for (const auto& id : descriptors)
    {
        if (universe.count (id) == 0)
            std::fprintf (stderr, "FAIL descriptors: '%s' has a descriptor but is not a schema parameter\n",
                          id.toRawUTF8());
        CHECK (universe.count (id) == 1);
    }

    // Per-descriptor structural checks.
    for (const auto& d : xoa::ui::allDescriptors())
    {
        CHECK (d.labelKey != nullptr && d.labelKey[0] != '\0');

        if (d.kind == xoa::ui::Kind::combo)
        {
            const auto* b = xoa::constraints::findBounds (d.id);
            CHECK (b != nullptr);          // combos are backed by int bounds
            if (b != nullptr)
            {
                const int span = (int) (b->max - b->min) + 1;
                if ((int) d.enumKeys.size() != span)
                    std::fprintf (stderr, "FAIL descriptors: combo '%s' has %d enum keys, span is %d\n",
                                  d.id.toString().toRawUTF8(), (int) d.enumKeys.size(), span);
                CHECK ((int) d.enumKeys.size() == span);
            }
        }
        else
        {
            CHECK (d.enumKeys.empty()); // only combos carry enum labels
        }
    }

    // Registry rows all resolve to descriptors (registry subset of descriptors).
    for (const auto& r : xoa::ui::registryRows())
    {
        if (xoa::ui::findDescriptor (r.id) == nullptr)
            std::fprintf (stderr, "FAIL registry: row id '%s' has no descriptor\n",
                          r.id.toString().toRawUTF8());
        CHECK (xoa::ui::findDescriptor (r.id) != nullptr);
    }
}
