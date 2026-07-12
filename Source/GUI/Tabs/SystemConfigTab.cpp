/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    SystemConfigTab implementation — see SystemConfigTab.h.

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#include "SystemConfigTab.h"

#include "Audio/AudioEngine.h"
#include "Parameters/XoaFileManager.h"
#include "Localization/LocalizationManager.h"
#include "XoaConstants.h"

namespace ids = xoa::ids;

namespace xoa::ui
{

SystemConfigTab::SystemConfigTab (AppContext& ctx) : TabPage (ctx, Surface::systemConfig)
{
    addAndMakeVisible (showGroup);
    addAndMakeVisible (playbackGroup);
    addAndMakeVisible (deviceGroup);
    addAndMakeVisible (appearanceGroup);
    showGroup.setText (LOC ("systemConfig.show"));
    playbackGroup.setText (LOC ("systemConfig.playback"));
    deviceGroup.setText (LOC ("systemConfig.device"));
    appearanceGroup.setText (LOC ("systemConfig.appearance"));

    // Show + project
    showNameLabel.setText (LOC ("param.showName"), juce::dontSendNotification);
    showNameLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (showNameLabel);
    addAndMakeVisible (showNameEditor);
    bindings.bindText (showNameEditor, ids::showName);

    loadButton.setButtonText (LOC ("systemConfig.loadProject"));
    saveButton.setButtonText (LOC ("systemConfig.saveProject"));
    importButton.setButtonText (LOC ("systemConfig.importWfs"));
    addAndMakeVisible (loadButton);
    addAndMakeVisible (saveButton);
    addAndMakeVisible (importButton);
    loadButton.onClick   = [this] { loadProjectDialog(); };
    saveButton.onClick   = [this] { saveProjectDialog(); };
    importButton.onClick = [this] { importWfsDialog(); };

    // Playback interpretation
    contentOrderLabel.setText (LOC ("param.playbackContentOrder"), juce::dontSendNotification);
    contentOrderLabel.setJustificationType (juce::Justification::centredRight);
    conventionLabel.setText (LOC ("param.playbackConvention"), juce::dontSendNotification);
    conventionLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (contentOrderLabel);
    addAndMakeVisible (conventionLabel);
    addAndMakeVisible (contentOrderCombo);
    addAndMakeVisible (conventionCombo);
    bindings.bindCombo (contentOrderCombo, ids::playbackContentOrder);
    bindings.bindCombo (conventionCombo, ids::playbackConvention);

    // Audio device (audioDeviceState is persisted by the engine's device manager)
    deviceSelector = std::make_unique<juce::AudioDeviceSelectorComponent> (
        context.engine.getDeviceManager(),
        0, xoa::kMaxInputs, 0, xoa::kMaxSpeakers,
        false, false, false, false);
    addAndMakeVisible (*deviceSelector);

    // Appearance: theme + language
    themeLabel.setText (LOC ("systemConfig.theme"), juce::dontSendNotification);
    themeLabel.setJustificationType (juce::Justification::centredRight);
    languageLabel.setText (LOC ("systemConfig.language"), juce::dontSendNotification);
    languageLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (themeLabel);
    addAndMakeVisible (languageLabel);
    addAndMakeVisible (themeCombo);
    addAndMakeVisible (languageCombo);

    themeCombo.addItem (LOC ("systemConfig.themeDark"),  1);
    themeCombo.addItem (LOC ("systemConfig.themeOled"),  2);
    themeCombo.addItem (LOC ("systemConfig.themeLight"), 3);
    themeCombo.setSelectedId (ColorScheme::getThemeIndex() + 1, juce::dontSendNotification);
    themeCombo.onChange = [this]
    {
        ColorScheme::Manager::getInstance().setTheme (themeCombo.getSelectedId() - 1);
    };

    for (const auto& loc : LocalizationManager::getInstance().getAvailableLanguages())
        languageCombo.addItem (loc, languageCombo.getNumItems() + 1);
    languageCombo.onChange = [this]
    {
        LocalizationManager::getInstance().loadLanguage (languageCombo.getText());
        // Labels are read at construction; a restart applies the change fully.
    };

    ColorScheme::Manager::getInstance().addListener (this);

    verifyRegistryCoverage();
}

SystemConfigTab::~SystemConfigTab()
{
    ColorScheme::Manager::getInstance().removeListener (this);
}

void SystemConfigTab::colorSchemeChanged()
{
    themeCombo.setSelectedId (ColorScheme::getThemeIndex() + 1, juce::dontSendNotification);
    repaint();
}

void SystemConfigTab::loadProjectDialog()
{
    fileChooser = std::make_unique<juce::FileChooser> (LOC ("systemConfig.loadProject"),
                                                       juce::File(), "*");
    fileChooser->launchAsync (juce::FileBrowserComponent::openMode
                                  | juce::FileBrowserComponent::canSelectDirectories,
                              [this] (const juce::FileChooser& fc)
    {
        const auto f = fc.getResult();
        if (f != juce::File() && context.loadProject)
            context.loadProject (f);
    });
}

void SystemConfigTab::saveProjectDialog()
{
    fileChooser = std::make_unique<juce::FileChooser> (LOC ("systemConfig.saveProject"),
                                                       juce::File(), "*");
    fileChooser->launchAsync (juce::FileBrowserComponent::saveMode
                                  | juce::FileBrowserComponent::canSelectDirectories,
                              [this] (const juce::FileChooser& fc)
    {
        const auto f = fc.getResult();
        if (f != juce::File())
            context.fileManager.createProject (f, context.store.getStringParameter (ids::showName));
    });
}

void SystemConfigTab::importWfsDialog()
{
    fileChooser = std::make_unique<juce::FileChooser> (LOC ("systemConfig.importWfs"),
                                                       juce::File(), "*.xml");
    fileChooser->launchAsync (juce::FileBrowserComponent::openMode
                                  | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc)
    {
        const auto f = fc.getResult();
        if (f == juce::File())
            return;
        context.fileManager.importWfsSpeakerLayout (f);
        context.engine.flushDecoderRebuild();
        if (context.refreshAllTabs)
            context.refreshAllTabs();
    });
}

void SystemConfigTab::resized()
{
    const float sc = XoaLookAndFeel::uiScale;
    auto px = [sc] (int v) { return juce::roundToInt ((float) v * sc); };
    auto area = getLocalBounds().reduced (px (12));

    // Left column: show/project + playback + appearance. Right column: device.
    auto right = area.removeFromRight (juce::jmax (px (280), area.getWidth() / 2));
    right.removeFromLeft (px (8));
    deviceGroup.setBounds (right);
    if (deviceSelector != nullptr)
        deviceSelector->setBounds (right.reduced (px (10), px (22)));

    auto& col = area;
    const int rowH = px (26);
    const int labelW = px (110);

    auto labelled = [&] (juce::Rectangle<int>& c, juce::Label& lab, juce::Component& ed, int edW)
    {
        auto r = c.removeFromTop (rowH);
        lab.setBounds (r.removeFromLeft (labelW));
        r.removeFromLeft (px (4));
        ed.setBounds (r.removeFromLeft (edW).reduced (0, px (2)));
        c.removeFromTop (px (4));
    };

    {
        auto g = col.removeFromTop (px (130));
        showGroup.setBounds (g);
        auto inner = g.reduced (px (12), px (20));
        labelled (inner, showNameLabel, showNameEditor, px (220));
        auto btns = inner.removeFromTop (rowH);
        loadButton.setBounds (btns.removeFromLeft (px (100))); btns.removeFromLeft (px (6));
        saveButton.setBounds (btns.removeFromLeft (px (100))); btns.removeFromLeft (px (6));
        importButton.setBounds (btns.removeFromLeft (px (130)));
    }
    col.removeFromTop (px (8));

    {
        auto g = col.removeFromTop (px (100));
        playbackGroup.setBounds (g);
        auto inner = g.reduced (px (12), px (20));
        labelled (inner, contentOrderLabel, contentOrderCombo, px (140));
        labelled (inner, conventionLabel, conventionCombo, px (140));
    }
    col.removeFromTop (px (8));

    {
        auto g = col.removeFromTop (px (100));
        appearanceGroup.setBounds (g);
        auto inner = g.reduced (px (12), px (20));
        labelled (inner, themeLabel, themeCombo, px (140));
        labelled (inner, languageLabel, languageCombo, px (140));
    }
}

} // namespace xoa::ui
