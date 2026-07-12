/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    ParamBindings — the reusable store<->widget attachment layer (WP10 C4).

    A BindingSet owns a tab's attachments: it seeds each widget from the store,
    writes user gestures back through the store (opening a named undo transaction
    on the parameter's domain and tagging the write OriginTag::UI via the kit
    slider/dial base), and reflects external writes (OSC / project load / undo)
    into the widget via a store parameter listener. Ranges come from
    xoa::constraints::findBounds; labels/steps/enum items from the UI descriptor.

    Channel model: pass a fixed 0-based channel, -1 for a global parameter, or
    BindingSet::kCurrentChannel for a per-channel widget that follows the set's
    current channel. setChannel() retargets every current-channel attachment (the
    Inputs / Speakers detail editors).

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "Parameters/XoaValueTreeState.h"
#include "Parameters/XoaConstraints.h"
#include "Localization/LocalizationManager.h"
#include "UiParameterDescriptors.h"

namespace xoa::ui
{

/** Map a descriptor domain to the store's undo-domain enum. */
inline XoaValueTreeState::UndoDomain toStoreDomain (Domain d)
{
    switch (d)
    {
        case Domain::inputs:   return XoaValueTreeState::inputsDomain;
        case Domain::speakers: return XoaValueTreeState::speakersDomain;
        case Domain::decoder:  return XoaValueTreeState::decoderDomain;
        case Domain::config:   break;
    }
    return XoaValueTreeState::configDomain;
}

/** RAII gesture scope for custom canvases (Map drag, layout Apply): switches the
    active undo domain and opens a named transaction for the duration of a drag. */
struct ScopedParamGesture
{
    ScopedParamGesture (XoaValueTreeState& s, XoaValueTreeState::UndoDomain d, const juce::String& name)
        : domain (s, d)
    {
        s.beginUndoTransaction (name);
    }
    XoaValueTreeState::ScopedDomain domain;
};

//==============================================================================
class BindingSet
{
public:
    /** Channel sentinel: a per-channel widget that follows setChannel(). */
    static constexpr int kCurrentChannel = -2;

    explicit BindingSet (XoaValueTreeState& s) : store (s) {}

    ~BindingSet()
    {
        for (const auto& reg : registered)
            store.removeParameterListeners (reg.first, reg.second);
    }

    BindingSet (const BindingSet&) = delete;
    BindingSet& operator= (const BindingSet&) = delete;

    /** Retarget every current-channel attachment (0-based). */
    void setChannel (int newChannel)
    {
        if (newChannel == currentChannel)
            return;

        for (const auto& att : channelAttachments)
            store.removeParameterListeners (att.id, currentChannel);

        currentChannel = newChannel;

        for (const auto& att : channelAttachments)
        {
            auto seed = att.seed;
            store.addParameterListener (att.id, [seed] (const juce::var&) { seed(); }, currentChannel);
            registered.emplace_back (att.id, currentChannel);
            seed();
        }
    }
    int getChannel() const noexcept { return currentChannel; }

    //==========================================================================
    // Store-backed attachments
    //==========================================================================

    void bindSlider (juce::Slider& slider, const juce::Identifier& id, int channel = -1)
    {
        const auto* d = findDescriptor (id);
        applyRangeAndStep (slider, id, d);

        auto applying = std::make_shared<bool>(false);
        const auto dom = d ? toStoreDomain (d->domain) : XoaValueTreeState::configDomain;
        const juce::String txName = d ? LOC (d->labelKey) : id.toString();

        slider.onDragStart = [this, dom, txName] { openGesture (dom, txName); };
        slider.onValueChange = [this, &slider, id, channel, applying]
        {
            if (*applying) return;
            writeNumber (id, slider.getValue(), effChannel (channel));
        };

        auto seed = [this, &slider, id, channel, applying]
        {
            const juce::ScopedValueSetter<bool> guard (*applying, true);
            slider.setValue (store.getFloatParameter (id, effChannel (channel)), juce::dontSendNotification);
        };
        attach (id, channel, std::move (seed));
    }

    template <typename KitDial>
    void bindDial (KitDial& dial, const juce::Identifier& id, int channel = -1)
    {
        const auto* d = findDescriptor (id);
        if (const auto* b = xoa::constraints::findBounds (id))
            dial.setRange ((float) b->min, (float) b->max);
        if (d != nullptr)
            dial.setTTSInfo (LOC (d->labelKey), unitText (d));

        auto applying = std::make_shared<bool>(false);
        const auto dom = d ? toStoreDomain (d->domain) : XoaValueTreeState::configDomain;
        const juce::String txName = d ? LOC (d->labelKey) : id.toString();

        dial.onGestureStart = [this, dom, txName] { openGesture (dom, txName); };
        dial.onValueChanged = [this, id, channel, applying] (float v)
        {
            if (*applying) return;
            writeNumber (id, (double) v, effChannel (channel));
        };

        auto seed = [this, &dial, id, channel, applying]
        {
            const juce::ScopedValueSetter<bool> guard (*applying, true);
            dial.setValue (store.getFloatParameter (id, effChannel (channel)));
        };
        attach (id, channel, std::move (seed));
    }

    void bindToggle (juce::Button& button, const juce::Identifier& id, int channel = -1)
    {
        button.setClickingTogglesState (true);
        auto applying = std::make_shared<bool>(false);

        button.onClick = [this, &button, id, channel, applying]
        {
            if (*applying) return;
            store.setParameter (id, button.getToggleState(), effChannel (channel));
        };

        auto seed = [this, &button, id, channel, applying]
        {
            const juce::ScopedValueSetter<bool> guard (*applying, true);
            button.setToggleState ((bool) store.getParameter (id, effChannel (channel)), juce::dontSendNotification);
        };
        attach (id, channel, std::move (seed));
    }

    void bindCombo (juce::ComboBox& combo, const juce::Identifier& id, int channel = -1)
    {
        const auto* d = findDescriptor (id);
        const int minVal = comboMin (id);
        populateCombo (combo, d);

        auto applying = std::make_shared<bool>(false);
        combo.onChange = [this, &combo, id, channel, minVal, applying]
        {
            if (*applying) return;
            const int sel = combo.getSelectedId();
            if (sel > 0)
                store.setParameter (id, minVal + sel - 1, effChannel (channel));
        };

        auto seed = [this, &combo, id, channel, minVal, applying]
        {
            const juce::ScopedValueSetter<bool> guard (*applying, true);
            combo.setSelectedId (store.getIntParameter (id, effChannel (channel)) - minVal + 1,
                                 juce::dontSendNotification);
        };
        attach (id, channel, std::move (seed));
    }

    /** Numeric or string text field, chosen by whether the id has numeric bounds. */
    void bindText (juce::TextEditor& editor, const juce::Identifier& id, int channel = -1)
    {
        const auto* b = xoa::constraints::findBounds (id);
        auto applying = std::make_shared<bool>(false);

        auto commit = [this, &editor, id, channel, b, applying]
        {
            if (*applying) return;
            if (b != nullptr)
                writeNumber (id, editor.getText().getDoubleValue(), effChannel (channel));
            else
                store.setParameter (id, editor.getText(), effChannel (channel));
        };
        editor.onReturnKey = commit;
        editor.onFocusLost = commit;

        auto seed = [this, &editor, id, channel, b, applying]
        {
            const juce::ScopedValueSetter<bool> guard (*applying, true);
            if (b != nullptr)
                editor.setText (b->isInt ? juce::String (store.getIntParameter (id, effChannel (channel)))
                                         : juce::String (store.getFloatParameter (id, effChannel (channel)), 3),
                                juce::dontSendNotification);
            else
                editor.setText (store.getStringParameter (id, effChannel (channel)), juce::dontSendNotification);
        };
        attach (id, channel, std::move (seed));
    }

    /** Speaker-EQ band slider (two-index) at the current channel. The store
        listener on eqId fires band-blind for the speaker, so the seed idempotently
        re-reads its own band. Always a current-channel attachment. */
    void bindEqBand (juce::Slider& slider, const juce::Identifier& eqId, int band)
    {
        const auto* d = findDescriptor (eqId);
        applyRangeAndStep (slider, eqId, d);
        auto applying = std::make_shared<bool>(false);
        const juce::String txName = d ? LOC (d->labelKey) : eqId.toString();

        slider.onDragStart = [this, txName] { openGesture (XoaValueTreeState::speakersDomain, txName); };
        slider.onValueChange = [this, &slider, eqId, band, applying]
        {
            if (*applying) return;
            store.setEqBandParameter (currentChannel, band, eqId, slider.getValue());
        };

        auto seed = [this, &slider, eqId, band, applying]
        {
            const juce::ScopedValueSetter<bool> guard (*applying, true);
            slider.setValue ((double) store.getEqBandParameter (currentChannel, band, eqId),
                             juce::dontSendNotification);
        };
        attach (eqId, kCurrentChannel, std::move (seed));
    }

    void bindEqBandCombo (juce::ComboBox& combo, const juce::Identifier& eqId, int band)
    {
        const auto* d = findDescriptor (eqId);
        const int minVal = comboMin (eqId);
        populateCombo (combo, d);

        auto applying = std::make_shared<bool>(false);
        combo.onChange = [this, &combo, eqId, band, minVal, applying]
        {
            if (*applying) return;
            const int sel = combo.getSelectedId();
            if (sel > 0)
                store.setEqBandParameter (currentChannel, band, eqId, minVal + sel - 1);
        };

        auto seed = [this, &combo, eqId, band, minVal, applying]
        {
            const juce::ScopedValueSetter<bool> guard (*applying, true);
            combo.setSelectedId ((int) store.getEqBandParameter (currentChannel, band, eqId) - minVal + 1,
                                 juce::dontSendNotification);
        };
        attach (eqId, kCurrentChannel, std::move (seed));
    }

    //==========================================================================
    // Engine-backed attachments (not in the store; polled on the tab tick,
    // skipped while a gesture is live so a drag isn't fought by the poll).
    //==========================================================================

    void bindEngineValue (juce::Slider& slider,
                          std::function<double()> get, std::function<void(double)> set)
    {
        slider.onValueChange = [&slider, set] { set (slider.getValue()); };
        engineRefreshers.push_back ([&slider, get]
        {
            if (! slider.isMouseButtonDown())
                slider.setValue (get(), juce::dontSendNotification);
        });
    }

    void bindEngineCombo (juce::ComboBox& combo, std::function<int()> get, std::function<void(int)> set)
    {
        combo.onChange = [&combo, set] { if (combo.getSelectedId() > 0) set (combo.getSelectedId() - 1); };
        engineRefreshers.push_back ([&combo, get]
        {
            combo.setSelectedId (get() + 1, juce::dontSendNotification);
        });
    }

    /** Call on the owning tab's timer tick to refresh engine-backed widgets. */
    void refreshEngineBindings()
    {
        for (auto& r : engineRefreshers)
            r();
    }

    //==========================================================================
    // Introspection (used by the runtime registry assertion)
    //==========================================================================

    juce::Array<juce::Identifier> boundIds() const
    {
        juce::Array<juce::Identifier> out;
        for (const auto& reg : registered)
            out.addIfNotAlreadyThere (reg.first);
        return out;
    }

private:
    struct ChannelAttachment
    {
        juce::Identifier      id;
        std::function<void()> seed;
    };

    int effChannel (int channel) const noexcept
    {
        return channel == kCurrentChannel ? currentChannel : channel;
    }

    // Register a listener at the effective channel, seed the widget now, and (for
    // current-channel widgets) remember the attachment so setChannel can retarget.
    void attach (const juce::Identifier& id, int channel, std::function<void()> seed)
    {
        const int eff = effChannel (channel);
        auto seedCopy = seed;
        store.addParameterListener (id, [seedCopy] (const juce::var&) { seedCopy(); }, eff);
        registered.emplace_back (id, eff);
        seed();

        if (channel == kCurrentChannel)
            channelAttachments.push_back ({ id, std::move (seed) });
    }

    void openGesture (XoaValueTreeState::UndoDomain dom, const juce::String& name)
    {
        store.setActiveDomain (dom);
        store.beginUndoTransaction (name);
    }

    void writeNumber (const juce::Identifier& id, double value, int channel)
    {
        const auto* b = xoa::constraints::findBounds (id);
        if (b != nullptr && b->isInt)
            store.setParameter (id, (int) juce::roundToInt (value), channel);
        else
            store.setParameter (id, value, channel);
    }

    void applyRangeAndStep (juce::Slider& slider, const juce::Identifier& id, const UiDescriptor* d)
    {
        if (const auto* b = xoa::constraints::findBounds (id))
        {
            const double step = (d != nullptr && d->step > 0.0) ? d->step : 0.0;
            slider.setRange (b->min, b->max, step);
            if (d != nullptr && d->logSkew && b->min > 0.0)
                slider.setSkewFactorFromMidPoint (std::sqrt (b->min * b->max));
        }
        if (d != nullptr)
            slider.setTextValueSuffix (unitSuffix (d));
    }

    static void populateCombo (juce::ComboBox& combo, const UiDescriptor* d)
    {
        combo.clear (juce::dontSendNotification);
        if (d != nullptr)
            for (int i = 0; i < (int) d->enumKeys.size(); ++i)
                combo.addItem (LOC (d->enumKeys[(size_t) i]), i + 1);
    }

    static juce::String unitText (const UiDescriptor* d)
    {
        return (d != nullptr && d->unit[0] != '\0') ? LOC (d->unit) : juce::String();
    }
    static juce::String unitSuffix (const UiDescriptor* d)
    {
        const juce::String u = unitText (d);
        return u.isEmpty() ? juce::String() : " " + u;
    }
    static int comboMin (const juce::Identifier& id)
    {
        if (const auto* b = xoa::constraints::findBounds (id))
            return (int) b->min;
        return 0;
    }

    XoaValueTreeState& store;
    int currentChannel = 0;

    std::vector<std::pair<juce::Identifier, int>> registered;   // for listener teardown
    std::vector<ChannelAttachment> channelAttachments;          // retarget on setChannel
    std::vector<std::function<void()>> engineRefreshers;        // polled each tab tick
};

} // namespace xoa::ui
