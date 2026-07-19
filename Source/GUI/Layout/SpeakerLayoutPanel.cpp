/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    SpeakerLayoutPanel implementation — see SpeakerLayoutPanel.h.

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#include "SpeakerLayoutPanel.h"

#include "../ColorScheme.h"
#include "../XoaLookAndFeel.h"
#include "../Binding/ParamBindings.h"
#include "../../XoaConstants.h"
#include "../../Parameters/XoaParameterDefaults.h"
#include "Localization/LocalizationManager.h"

namespace ids = xoa::ids;

namespace xoa::ui
{

SpeakerLayoutPanel::SpeakerLayoutPanel (XoaValueTreeState& storeToApply) : store (storeToApply)
{
    presetLabel.setText (LOC ("layout.preset"), juce::dontSendNotification);
    presetLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (presetLabel);
    presetCombo.addItem (LOC ("layout.ring"), 1);
    presetCombo.addItem (LOC ("layout.line"), 2);
    presetCombo.addItem (LOC ("layout.dome"), 3);
    presetCombo.addItem (LOC ("layout.grid"), 4);
    presetCombo.setSelectedId (1, juce::dontSendNotification);
    presetCombo.onChange = [this] { updateFieldVisibility(); repaint(); };
    addAndMakeVisible (presetCombo);

    auto setupField = [this] (juce::Label& lab, juce::Slider& sl, const char* key,
                              double lo, double hi, double step, double val)
    {
        lab.setText (LOC (key), juce::dontSendNotification);
        lab.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (lab);
        sl.setRange (lo, hi, step);
        sl.setValue (val, juce::dontSendNotification);
        sl.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 64, 20);
        sl.onValueChange = [this] { repaint(); };
        addAndMakeVisible (sl);
    };
    auto setupKitField = [this] (XoaStandardSlider& sl, const char* key,
                                 float lo, float hi, float val)
    {
        sl.setRange (lo, hi);
        sl.setValue (val);
        sl.setLabel (LOC (key));
        sl.setInlineMode (true);
        sl.setTrackColours (ColorScheme::get().sliderTrackBg, ColorScheme::accents::spatial);
        sl.onValueChanged = [this] (float) { repaint(); };
        addAndMakeVisible (sl);
    };

    setupField (countLabel, countSlider, "layout.count", 1.0, (double) xoa::kMaxSpeakers, 1.0, 24.0);
    setupField (ringsLabel, ringsSlider, "layout.rings", 1.0, 12.0, 1.0, 3.0);
    setupKitField (radiusSlider,  "layout.radius",  0.5f, 50.0f, (float) xoa::defaults::kDefaultRigRadius);
    setupKitField (heightSlider,  "layout.height", -10.0f, 10.0f, 0.0f);
    setupKitField (spacingSlider, "layout.spacing", 0.2f, 10.0f, 1.0f);

    applyButton.setButtonText (LOC ("common.apply"));
    applyButton.onClick = [this] { apply(); };
    addAndMakeVisible (applyButton);

    updateFieldVisibility();
}

void SpeakerLayoutPanel::updateFieldVisibility()
{
    const auto preset = (Preset) presetCombo.getSelectedItemIndex();
    const bool isRing = preset == Preset::ring;
    const bool isLine = preset == Preset::line;
    const bool isDome = preset == Preset::dome;
    const bool isGrid = preset == Preset::grid;

    radiusSlider.setVisible (isRing || isDome);
    heightSlider.setVisible (isRing || isLine || isGrid);
    ringsLabel.setVisible (isDome);              ringsSlider.setVisible (isDome);
    spacingSlider.setVisible (isGrid || isLine);
    countLabel.setVisible (! isGrid);            countSlider.setVisible (! isGrid);
    resized();
}

std::vector<layout::SpeakerPos> SpeakerLayoutPanel::generate() const
{
    const auto preset = (Preset) presetCombo.getSelectedItemIndex();
    const int  count  = (int) countSlider.getValue();
    const double r    = radiusSlider.getValue();
    const double h    = heightSlider.getValue();

    switch (preset)
    {
        case Preset::line:
        {
            const double half = 0.5 * spacingSlider.getValue() * (double) juce::jmax (1, count - 1);
            return layout::line (count, { 0.0, half, h }, { 0.0, -half, h });   // across the front, left-to-right
        }
        case Preset::dome:
            return layout::dome ((int) ringsSlider.getValue(), count, r, 75.0, true);
        case Preset::grid:
        {
            const int side = juce::jmax (1, (int) std::round (std::sqrt ((double) count)));
            return layout::ceilingGrid (side, side, spacingSlider.getValue(), h);
        }
        case Preset::ring:
        default:
            return layout::ring (count, r, h, 0.0);
    }
}

void SpeakerLayoutPanel::apply()
{
    const auto pos = generate();
    if (pos.empty())
        return;

    const int n = juce::jlimit (1, xoa::kMaxSpeakers, (int) pos.size());
    ScopedParamGesture gesture (store, XoaValueTreeState::speakersDomain, LOC ("layout.applyUndo"));
    store.setParameter (ids::speakerCount, n);
    for (int i = 0; i < n; ++i)
    {
        store.setParameter (ids::speakerPositionX, pos[(size_t) i].x, i);
        store.setParameter (ids::speakerPositionY, pos[(size_t) i].y, i);
        store.setParameter (ids::speakerPositionZ, pos[(size_t) i].z, i);
        store.setParameter (ids::speakerName, xoa::defaults::getDefaultSpeakerName (i), i);
    }

    if (onApplied)
        onApplied();
}

void SpeakerLayoutPanel::paint (juce::Graphics& g)
{
    const auto& col = ColorScheme::get();
    g.setColour (col.background);
    g.fillRect (previewArea);
    g.setColour (col.chromeDivider);
    g.drawRect (previewArea, 1);

    const auto pos = generate();
    if (pos.empty() || previewArea.isEmpty())
        return;

    RigProjection proj;
    proj.bounds = previewArea.toFloat().reduced (8.0f);
    proj.fitTo (pos, (int) pos.size());

    // Origin cross.
    g.setColour (col.chromeDivider);
    const auto o = proj.planToScreen (0.0, 0.0);
    g.drawLine (o.x - 6, o.y, o.x + 6, o.y, 1.0f);
    g.drawLine (o.x, o.y - 6, o.x, o.y + 6, 1.0f);

    for (size_t i = 0; i < pos.size(); ++i)
    {
        const auto p = proj.planToScreen (pos[i].x, pos[i].y);
        // Height tints the marker: higher = warmer.
        const float t = juce::jlimit (0.0f, 1.0f, (float) (pos[i].z / juce::jmax (0.5, proj.extent)) * 0.5f + 0.5f);
        g.setColour (col.accentBlue.interpolatedWith (col.accentRed, t));
        g.fillEllipse (p.x - 3.0f, p.y - 3.0f, 6.0f, 6.0f);
    }
}

void SpeakerLayoutPanel::resized()
{
    const float sc = XoaLookAndFeel::uiScale;
    auto px = [sc] (int v) { return juce::roundToInt ((float) v * sc); };
    auto area = getLocalBounds().reduced (px (8));

    auto controls = area.removeFromLeft (px (240));
    area.removeFromLeft (px (8));
    previewArea = area;

    const int rowH = px (26);
    const int labelW = px (90);
    auto row = [&] (juce::Label& lab, juce::Component& c)
    {
        if (! c.isVisible()) return;
        auto r = controls.removeFromTop (rowH);
        lab.setBounds (r.removeFromLeft (labelW));
        r.removeFromLeft (px (4));
        c.setBounds (r.reduced (0, px (2)));
        controls.removeFromTop (px (4));
    };
    // Inline kit sliders carry their own label/value — full row width.
    auto kitRow = [&] (juce::Component& c)
    {
        if (! c.isVisible()) return;
        c.setBounds (controls.removeFromTop (rowH).reduced (0, px (2)));
        controls.removeFromTop (px (4));
    };
    row (presetLabel, presetCombo);
    row (countLabel, countSlider);
    kitRow (radiusSlider);
    kitRow (heightSlider);
    row (ringsLabel, ringsSlider);
    kitRow (spacingSlider);
    controls.removeFromTop (px (4));
    applyButton.setBounds (controls.removeFromTop (rowH).removeFromLeft (px (120)));
}

} // namespace xoa::ui
