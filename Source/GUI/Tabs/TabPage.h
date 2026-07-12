/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    TabPage — base class for the six application tabs (WP10 C5). Holds the shared
    AppContext (store / file manager / engine / OSC manager) and a per-tab
    BindingSet, exposes refresh() (called on the app timer for the visible tab)
    and a debug-only registry self-check.

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../XoaLookAndFeel.h"
#include "../Binding/ParamBindings.h"
#include "../Binding/TabParameterRegistry.h"
#include "../Binding/UiParameterDescriptors.h"

namespace xoa
{
class XoaFileManager;
class AudioEngine;
class OSCManager;
}

namespace xoa::ui
{

class RvReAnalysisService;

/** References the app-owned backend, handed to every tab, plus cross-cutting
    actions the AppShell wires (so a tab can trigger a project load / full-UI
    refresh without owning the other tabs). */
struct AppContext
{
    XoaValueTreeState&   store;
    xoa::XoaFileManager& fileManager;
    xoa::AudioEngine&    engine;
    xoa::OSCManager&     oscManager;
    RvReAnalysisService* analysis = nullptr;   // owned by the AppShell (FR-18)

    std::function<void (const juce::File&)> loadProject;   // load + rebuild + refresh all tabs
    std::function<void()>                   refreshAllTabs; // after an import / count change
};

class TabPage : public juce::Component
{
public:
    TabPage (AppContext& ctx, Surface tabSurface)
        : context (ctx), bindings (ctx.store), surface (tabSurface) {}

    ~TabPage() override = default;

    /** Called on the app's timer tick while this tab is visible. */
    virtual void refresh() { bindings.refreshEngineBindings(); }

    Surface getSurface() const noexcept { return surface; }

    /** Debug-only: every non-system parameter the registry places on this surface
        must be bound by a widget here. Runs in the xvfb smoke lane (Debug). */
    void verifyRegistryCoverage() const
    {
       #if JUCE_DEBUG
        const auto bound = bindings.boundIds();
        for (const auto& id : idsForSurface (surface))
        {
            const auto* d = findDescriptor (id);
            if (d != nullptr && d->kind == Kind::system)
                continue;   // reached via a dedicated surface, not a bound widget
            jassert (bound.contains (id));
        }
       #else
        juce::ignoreUnused (surface);
       #endif
    }

protected:
    AppContext&  context;
    BindingSet   bindings;
    Surface      surface;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TabPage)
};

/** A not-yet-built tab: shows a localized "coming soon" note. Replaced by the
    real tab in its chunk (Inputs C6, Speakers+Decoder C7, Monitoring/Map C9). */
class PlaceholderTab : public TabPage
{
public:
    PlaceholderTab (AppContext& ctx, Surface tabSurface, const juce::String& titleText)
        : TabPage (ctx, tabSurface)
    {
        title.setText (titleText, juce::dontSendNotification);
        title.setJustificationType (juce::Justification::centred);
        title.setFont (juce::FontOptions (20.0f * XoaLookAndFeel::uiScale));
        addAndMakeVisible (title);
    }

    void resized() override { title.setBounds (getLocalBounds()); }

private:
    juce::Label title;
};

} // namespace xoa::ui
