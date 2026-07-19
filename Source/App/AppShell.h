/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    AppShell — the real application content (WP10 C5), replacing the throwaway
    WP6 shell. Owns the backend (store -> fileManager -> engine -> oscManager),
    the persistent HeaderBar, the six-tab XoaTabbedComponent, and the StatusBar;
    drives a single 25 Hz refresh timer and the single decoder-rebuild callback
    (fanned out to the header status and the analysis/decoder surfaces). Preserves
    the WP9 headless entry points: `--osc` (control-replay) and a project path on
    the command line; adds `--gui-smoke` (the xvfb CI GUI gate).

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

#include "../Parameters/XoaValueTreeState.h"
#include "../Parameters/XoaFileManager.h"
#include "../Audio/AudioEngine.h"
#include "../Network/OSCManager.h"
#include "../GUI/ColorScheme.h"
#include "../GUI/StatusBar.h"
#include "../GUI/XoaTabbedComponent.h"
#include "../GUI/Tabs/TabPage.h"
#include "../GUI/Analysis/RvReAnalysisService.h"
#include "HeaderBar.h"

namespace xoa::ui
{

class AppShell : public juce::Component,
                 private juce::Timer,
                 private ColorScheme::Manager::Listener
{
public:
    explicit AppShell (const juce::String& commandLine = {});
    ~AppShell() override;

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    void timerCallback() override;
    void colorSchemeChanged() override;

    void applyStartupCommandLine (const juce::String& commandLine);
    void applyLoadedProject (const juce::File& folderOrManifest);
    void refreshAllTabs();
    TabPage* visibleTab();

    void beginGuiSmoke();
    void stepGuiSmoke();

    // Backend — declaration order is the teardown order (OSC first down).
    XoaValueTreeState   store;
    xoa::XoaFileManager fileManager { store };
    xoa::AudioEngine    engine { store };
    xoa::OSCManager     oscManager { store, engine };
    RvReAnalysisService analysisService;
    InputSelectionModel inputSelection;

    AppContext context { store, fileManager, engine, oscManager, inputSelection, &analysisService, {}, {} };

    HeaderBar          header { context };
    XoaTabbedComponent tabs;
    StatusBar          statusBar;

    std::vector<TabPage*> tabPages;   // non-owning; the TabbedComponent owns them

    // GUI smoke (xvfb CI): -1 inactive, else the current step; a sub-tick counter
    // paces the steps off the 25 Hz timer.
    int smokeStep = -1;
    int smokeTick = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AppShell)
};

} // namespace xoa::ui
