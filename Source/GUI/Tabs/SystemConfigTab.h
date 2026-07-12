/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    SystemConfigTab — show/project, audio device, playback interpretation, and
    appearance (theme / language) settings (WP10 C5).

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_utils/juce_audio_utils.h>   // AudioDeviceSelectorComponent

#include <memory>

#include "TabPage.h"

namespace xoa::ui
{

class SystemConfigTab : public TabPage,
                        private ColorScheme::Manager::Listener
{
public:
    explicit SystemConfigTab (AppContext& ctx);
    ~SystemConfigTab() override;

    void resized() override;

private:
    void colorSchemeChanged() override;
    void loadProjectDialog();
    void saveProjectDialog();
    void importWfsDialog();

    juce::GroupComponent showGroup, playbackGroup, deviceGroup, appearanceGroup;

    juce::Label      showNameLabel;
    juce::TextEditor showNameEditor;
    juce::TextButton loadButton, saveButton, importButton;

    juce::Label    contentOrderLabel, conventionLabel;
    juce::ComboBox contentOrderCombo, conventionCombo;

    juce::Label    themeLabel, languageLabel;
    juce::ComboBox themeCombo, languageCombo;

    std::unique_ptr<juce::AudioDeviceSelectorComponent> deviceSelector;
    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SystemConfigTab)
};

} // namespace xoa::ui
