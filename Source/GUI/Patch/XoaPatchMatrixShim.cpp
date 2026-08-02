#include "XoaPatchMatrixShim.h"

#include "../ColorScheme.h"
#include "../ColorUtilities.h"
#include "../XoaLookAndFeel.h"
#include "Accessibility/TTSManager.h"
#include "Localization/LocalizationManager.h"
#include "XoaConstants.h"

namespace xoa::ui
{

namespace
{
    /** The input owning a flattened stem-channel row, and the row's offset
        within that input's span. Returns {-1, 0} past the last input. */
    std::pair<int, int> inputForStemRow (const XoaValueTreeState& store, int row)
    {
        int offset = 0;
        const int numInputs = store.getNumInputs();
        for (int i = 0; i < numInputs; ++i)
        {
            const int span = store.getInputChannelCount (i);
            if (row < offset + span)
                return { i, row - offset };
            offset += span;
        }
        return { -1, 0 };
    }
} // namespace

spatcore::ui::patch::PatchMatrixConfig
XoaPatchMatrix::makeConfig (XoaValueTreeState& store, bool isInputPatch)
{
    using namespace spatcore::ui::patch;

    PatchMatrixConfig config;

    //--------------------------------------------------------------------------
    // Trees + schema. Property names are shared verbatim with the component's
    // defaults except the per-direction active-channel ids.
    config.patchTree    = isInputPatch ? store.getInputPatchTree() : store.getOutputPatchTree();
    config.channelsTree = isInputPatch ? store.getInputsSection() : store.getSpeakersSection();

    config.ids.patchData = ids::patchData;
    config.ids.rows = ids::rows;
    config.ids.cols = ids::cols;
    config.ids.activeHardwareChannels = isInputPatch ? ids::activeHardwareInputs
                                                     : ids::activeHardwareOutputs;
    // No binaural tree: XOA's binaural monitoring is post-v1 (D2).

    config.maxHardwareChannels = xoa::kMaxHardwareChannels;

    //--------------------------------------------------------------------------
    // Host queries. Input rows are FLATTENED stem channels (D43): a group
    // input owns several consecutive rows, labelled and coloured by the
    // owning input so it reads as one block in the matrix.
    config.numChannelsProvider = [&store, isInputPatch]
    {
        return isInputPatch ? store.getTotalStemChannels() : store.getNumSpeakers();
    };

    config.recomputeColumns = [&store] { store.recomputePatchCols(); };

    config.channelNameProvider = [&store, isInputPatch] (int row) -> juce::String
    {
        if (! isInputPatch)
        {
            return row >= 0 && row < store.getNumSpeakers()
                       ? store.getStringParameter (ids::speakerName, row)
                       : juce::String();
        }

        const auto [input, channelInGroup] = inputForStemRow (store, row);
        if (input < 0)
            return {};

        const juce::String name = store.getStringParameter (ids::inputName, input);
        if (store.getInputChannelCount (input) <= 1)
            return name;
        return name + " · ACN " + juce::String (channelInGroup);
    };

    config.rowColourProvider = [&store, isInputPatch] (int row) -> juce::Colour
    {
        if (isInputPatch)
        {
            const int input = inputForStemRow (store, row).first;
            return input >= 0 ? XoaColorUtilities::getInputColor (input + 1)
                              : juce::Colours::grey;
        }
        // Speakers carry no per-channel colour in XOA's schema (unlike
        // WFS-DIY's array colours); one accent reads calmer than 256 hues.
        return ColorScheme::accents::spatial;
    };

    config.contrastingTextProvider = [] (juce::Colour background)
    {
        return XoaColorUtilities::getContrastingTextColor (background);
    };

    //--------------------------------------------------------------------------
    // Presentation — read at paint/layout time so live theme, language and
    // scale changes show up without rebuilding the matrix.
    config.paletteProvider = []
    {
        const auto& scheme = ColorScheme::get();
        return PatchMatrixPalette { scheme.background,
                                    scheme.backgroundAlt,
                                    scheme.surfaceCard,
                                    scheme.chromeDivider,
                                    scheme.textPrimary,
                                    scheme.textSecondary,
                                    scheme.textDisabled };
    };

    config.translate = [] (const char* key) { return LOC (key); };
    config.uiScaleProvider = [] { return XoaLookAndFeel::uiScale; };

    //--------------------------------------------------------------------------
    // Accessibility.
    config.announce = [] (const juce::String& text)
    {
        TTSManager::getInstance().announceImmediate (
            text, juce::AccessibilityHandler::AnnouncementPriority::medium);
    };

    config.announceDebounced = [] (const juce::String& text)
    {
        TTSManager::getInstance().announceDebounced (text);
    };

    config.cancelDebouncedAnnouncement = []
    {
        TTSManager::getInstance().cancelDebouncedAnnouncement();
    };

    return config;
}

} // namespace xoa::ui
