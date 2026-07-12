/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    MapTab — the 2-D scene view (WP10 C9, D32, time-boxed): a top-down XY plan and
    a read-only side X-Z elevation strip. Sources (input positions) and the
    listener sweet-spot (D18) are draggable in the plan; speakers are read-only
    reference dots. Editing is time/level-plane only here — Z stays as set in the
    Inputs tab. No 3-D, no zoom gestures.

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <memory>

#include "TabPage.h"
#include "../ColorScheme.h"
#include "../ColorUtilities.h"
#include "../Widgets/RigProjection.h"
#include "Localization/LocalizationManager.h"
#include "XoaConstants.h"

namespace xoa::ui
{

class MapTab : public TabPage
{
public:
    explicit MapTab (AppContext& ctx) : TabPage (ctx, Surface::map)
    {
        hintLabel.setJustificationType (juce::Justification::centredLeft);
        hintLabel.setText (LOC ("map.hint"), juce::dontSendNotification);
        addAndMakeVisible (hintLabel);
        verifyRegistryCoverage();
    }

    void refresh() override
    {
        TabPage::refresh();
        if (dragTarget == DragTarget::none)
            refit();
        repaint();
    }

    void resized() override
    {
        const float sc = XoaLookAndFeel::uiScale;
        auto px = [sc] (int v) { return juce::roundToInt ((float) v * sc); };
        auto area = getLocalBounds().reduced (px (10));
        hintLabel.setBounds (area.removeFromTop (px (20)));
        area.removeFromTop (px (4));
        sideArea = area.removeFromRight (px (160));
        area.removeFromRight (px (8));
        planArea = area;
        refit();
    }

    void paint (juce::Graphics& g) override
    {
        const auto& col = ColorScheme::get();
        g.fillAll (col.background);

        drawPanel (g, planArea, LOC ("map.plan"));
        drawPanel (g, sideArea, LOC ("map.side"));

        // Grid + origin in the plan.
        drawGrid (g, planArea);

        // Speakers (read-only).
        g.setColour (col.textSecondary);
        for (int s = 0; s < context.store.getNumSpeakers(); ++s)
        {
            const auto p = plan.planToScreen (spk (s, ids::speakerPositionX), spk (s, ids::speakerPositionY));
            g.drawRect (p.x - 3.0f, p.y - 3.0f, 6.0f, 6.0f, 1.0f);
            const auto sp = sideScreen (spk (s, ids::speakerPositionX), spk (s, ids::speakerPositionZ));
            g.fillRect (sp.x - 1.5f, sp.y - 1.5f, 3.0f, 3.0f);
        }

        // Listener sweet-spot (draggable).
        {
            const auto p = plan.planToScreen (cfg (ids::listenerX), cfg (ids::listenerY));
            g.setColour (col.accentGreen);
            g.drawLine (p.x - 7, p.y, p.x + 7, p.y, 2.0f);
            g.drawLine (p.x, p.y - 7, p.x, p.y + 7, 2.0f);
        }

        // Sources (draggable input positions).
        for (int i = 0; i < context.store.getNumInputs(); ++i)
        {
            const double x = inp (i, ids::inputPositionX), y = inp (i, ids::inputPositionY), z = inp (i, ids::inputPositionZ);
            const auto p = plan.planToScreen (x, y);
            g.setColour (XoaColorUtilities::getInputColor (i + 1));
            g.fillEllipse (p.x - 5.0f, p.y - 5.0f, 10.0f, 10.0f);
            g.setColour (col.textPrimary);
            g.setFont (juce::FontOptions (11.0f));
            g.drawText (juce::String (i + 1), (int) p.x - 10, (int) p.y - 22, 20, 14, juce::Justification::centred);
            const auto sp = sideScreen (x, z);
            g.setColour (XoaColorUtilities::getInputColor (i + 1));
            g.fillEllipse (sp.x - 3.0f, sp.y - 3.0f, 6.0f, 6.0f);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! planArea.contains (e.getPosition()))
            return;

        // Nearest source within grab radius?
        int best = -1; float bestD = 12.0f;
        for (int i = 0; i < context.store.getNumInputs(); ++i)
        {
            const auto p = plan.planToScreen (inp (i, ids::inputPositionX), inp (i, ids::inputPositionY));
            const float d = p.getDistanceFrom (e.position);
            if (d < bestD) { bestD = d; best = i; }
        }
        if (best >= 0)
        {
            dragTarget = DragTarget::source; dragIndex = best;
            gesture = std::make_unique<ScopedParamGesture> (context.store, XoaValueTreeState::inputsDomain, LOC ("map.moveSource"));
            return;
        }
        // Listener?
        const auto lp = plan.planToScreen (cfg (ids::listenerX), cfg (ids::listenerY));
        if (lp.getDistanceFrom (e.position) < 12.0f)
        {
            dragTarget = DragTarget::listener;
            gesture = std::make_unique<ScopedParamGesture> (context.store, XoaValueTreeState::configDomain, LOC ("map.moveListener"));
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragTarget == DragTarget::none)
            return;
        const auto w = plan.screenToPlan (e.position);
        const double x = juce::jlimit (-100.0, 100.0, w.x);
        const double y = juce::jlimit (-100.0, 100.0, w.y);
        if (dragTarget == DragTarget::source)
        {
            context.store.setParameter (ids::inputPositionX, x, dragIndex);
            context.store.setParameter (ids::inputPositionY, y, dragIndex);
        }
        else
        {
            context.store.setParameter (ids::listenerX, x);
            context.store.setParameter (ids::listenerY, y);
        }
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        dragTarget = DragTarget::none;
        dragIndex = -1;
        gesture.reset();
    }

private:
    enum class DragTarget { none, source, listener };

    double inp (int ch, const juce::Identifier& id) const { return context.store.getFloatParameter (id, ch); }
    double spk (int ch, const juce::Identifier& id) const { return context.store.getFloatParameter (id, ch); }
    double cfg (const juce::Identifier& id) const { return context.store.getFloatParameter (id); }

    juce::Point<float> sideScreen (double x, double z) const
    {
        RigProjection side; side.bounds = sideArea.toFloat().reduced (8.0f); side.extent = plan.extent;
        return side.sideToScreen (x, z);
    }

    void refit()
    {
        plan.bounds = planArea.toFloat().reduced (8.0f);
        double maxAbs = 1.0;
        for (int s = 0; s < context.store.getNumSpeakers(); ++s)
        {
            maxAbs = juce::jmax (maxAbs, std::abs (spk (s, ids::speakerPositionX)));
            maxAbs = juce::jmax (maxAbs, std::abs (spk (s, ids::speakerPositionY)));
            maxAbs = juce::jmax (maxAbs, std::abs (spk (s, ids::speakerPositionZ)));
        }
        for (int i = 0; i < context.store.getNumInputs(); ++i)
        {
            maxAbs = juce::jmax (maxAbs, std::abs (inp (i, ids::inputPositionX)));
            maxAbs = juce::jmax (maxAbs, std::abs (inp (i, ids::inputPositionY)));
        }
        plan.extent = maxAbs * 1.2;
    }

    void drawPanel (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& title) const
    {
        const auto& col = ColorScheme::get();
        g.setColour (col.surfaceCard); g.fillRect (r);
        g.setColour (col.chromeDivider); g.drawRect (r, 1);
        g.setColour (col.textSecondary); g.setFont (juce::FontOptions (12.0f));
        g.drawText (title, r.getX() + 4, r.getY() + 2, r.getWidth() - 8, 16, juce::Justification::topLeft);
    }

    void drawGrid (juce::Graphics& g, juce::Rectangle<int> r) const
    {
        const auto& col = ColorScheme::get();
        g.setColour (col.chromeDivider.withAlpha (0.5f));
        const auto o = plan.planToScreen (0.0, 0.0);
        g.drawLine ((float) r.getX(), o.y, (float) r.getRight(), o.y, 1.0f);
        g.drawLine (o.x, (float) r.getY(), o.x, (float) r.getBottom(), 1.0f);
        // 1 m rings.
        for (double ring = 1.0; ring <= plan.extent; ring += 1.0)
        {
            const float rad = (float) ring * plan.scale();
            g.drawEllipse (o.x - rad, o.y - rad, rad * 2.0f, rad * 2.0f, 0.5f);
        }
    }

    juce::Label hintLabel;
    juce::Rectangle<int> planArea, sideArea;
    RigProjection plan;

    DragTarget dragTarget = DragTarget::none;
    int dragIndex = -1;
    std::unique_ptr<ScopedParamGesture> gesture;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MapTab)
};

} // namespace xoa::ui
