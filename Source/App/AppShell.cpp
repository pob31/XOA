/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    AppShell implementation — see AppShell.h.

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#include "AppShell.h"

#include "../GUI/Tabs/SystemConfigTab.h"
#include "../GUI/Tabs/NetworkTab.h"
#include "DSP/AmbiDecoderDesigner.h"
#include "Localization/LocalizationManager.h"

namespace ids = xoa::ids;

namespace xoa::ui
{

AppShell::AppShell (const juce::String& commandLine)
{
    // Cross-cutting actions the tabs invoke (set before creating any tab).
    context.loadProject    = [this] (const juce::File& f) { applyLoadedProject (f); };
    context.refreshAllTabs = [this] { refreshAllTabs(); };

    // Decoder-rebuild callback: one engine slot, fanned to the header status
    // (rebuild-in-flight, read live) and a transient decoder summary.
    engine.onDecoderRebuilt = [this] (const xoa::decoder::DesignResult& r)
    {
        juce::String s;
        s << "decoder: order " << r.designOrder;
        if (r.conditionWarning)
            s << "  ·  κ=" << juce::String (r.conditionNumber, 1);
        if (! r.warnings.isEmpty())
            s << "  ·  " << r.warnings.joinIntoString ("; ");
        statusBar.showTemporaryMessage (s, 6000);
    };

    addAndMakeVisible (header);
    addAndMakeVisible (tabs);
    addAndMakeVisible (statusBar);

    const auto tabBg = ColorScheme::get().chromeBackground;

    auto addRealTab = [this, tabBg] (const char* nameKey, TabPage* page)
    {
        tabs.addTab (LOC (nameKey), tabBg, page, true);
        tabPages.push_back (page);
    };
    auto addPlaceholder = [this, tabBg] (const char* nameKey, Surface surface)
    {
        auto* page = new PlaceholderTab (context, surface, LOC (nameKey) + juce::String (" — WP10"));
        tabs.addTab (LOC (nameKey), tabBg, page, true);
        tabPages.push_back (page);
    };

    addRealTab ("tabs.systemConfig", new SystemConfigTab (context));
    addRealTab ("tabs.network",      new NetworkTab (context));
    addPlaceholder ("tabs.inputs",          Surface::inputs);
    addPlaceholder ("tabs.speakersDecoder", Surface::speakersDecoder);
    addPlaceholder ("tabs.monitoring",      Surface::monitoring);
    addPlaceholder ("tabs.map",             Surface::map);

    ColorScheme::Manager::getInstance().addListener (this);

    engine.openAudioDevice();
    applyStartupCommandLine (commandLine);
    oscManager.start();

    setSize (1280, 900);
    startTimerHz (25);
}

AppShell::~AppShell()
{
    ColorScheme::Manager::getInstance().removeListener (this);
    stopTimer();
    oscManager.stop();
    engine.closeAudioDevice();
    tabs.clearTabs();   // delete tab pages before the backend they reference
}

//==============================================================================
void AppShell::applyLoadedProject (const juce::File& folderOrManifest)
{
    fileManager.loadProject (folderOrManifest);
    engine.flushDecoderRebuild();
    refreshAllTabs();
}

void AppShell::refreshAllTabs()
{
    for (auto* t : tabPages)
        t->refresh();
    resized();
}

// commandLine may carry a project path and/or the tokens "--osc" (force the OSC
// receiver up, for the control-replay harness) and "--gui-smoke" (cycle the tabs
// and quit, for the xvfb CI GUI gate). Anything else is ignored.
void AppShell::applyStartupCommandLine (const juce::String& commandLine)
{
    juce::StringArray tokens = juce::StringArray::fromTokens (commandLine, true);
    tokens.removeEmptyStrings();

    bool forceOsc = false, guiSmoke = false;
    for (const auto& raw : tokens)
    {
        const juce::String tok = raw.unquoted();
        if (tok == "--osc")             { forceOsc = true;  continue; }
        if (tok == "--gui-smoke")       { guiSmoke = true;  continue; }
        const juce::File f (tok);
        if (f.exists())
            applyLoadedProject (f);
    }

    if (forceOsc)
        store.setParameterWithoutUndo (ids::oscEnabled, true);
    if (guiSmoke)
        beginGuiSmoke();
}

TabPage* AppShell::visibleTab()
{
    const int idx = tabs.getCurrentTabIndex();
    if (idx >= 0 && idx < (int) tabPages.size())
        return tabPages[(size_t) idx];
    return nullptr;
}

//==============================================================================
void AppShell::timerCallback()
{
    header.refresh();
    if (auto* t = visibleTab())
        t->refresh();

    if (smokeStep >= 0 && ++smokeTick >= 8)   // ~320 ms per step
    {
        smokeTick = 0;
        stepGuiSmoke();
    }
}

void AppShell::colorSchemeChanged()
{
    repaint();
}

//==============================================================================
void AppShell::beginGuiSmoke()
{
    smokeStep = 0;
    smokeTick = 0;
}

void AppShell::stepGuiSmoke()
{
    const int numTabs = tabs.getNumTabs();
    if (smokeStep < numTabs)
    {
        tabs.setCurrentTabIndex (smokeStep);
    }
    else if (smokeStep == numTabs)
    {
        ColorScheme::Manager::getInstance().setTheme (ColorScheme::Theme::OLEDBlack);
    }
    else if (smokeStep == numTabs + 1)
    {
        ColorScheme::Manager::getInstance().setTheme (ColorScheme::Theme::Default);
    }
    else
    {
        if (auto* app = juce::JUCEApplicationBase::getInstance())
            app->systemRequestedQuit();
        smokeStep = -1;
        return;
    }
    ++smokeStep;
}

//==============================================================================
void AppShell::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));
}

void AppShell::resized()
{
    XoaLookAndFeel::uiScale = juce::jmax (0.5f, (float) getHeight() / 1080.0f);

    auto area = getLocalBounds();
    const float sc = XoaLookAndFeel::uiScale;

    header.setBounds (area.removeFromTop (juce::roundToInt (120.0f * sc)));
    statusBar.setBounds (area.removeFromBottom (juce::roundToInt (30.0f * sc)));
    tabs.setBounds (area);
}

} // namespace xoa::ui
