/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    MapTab — the scene view (WP10 C9, D32): a top-down XY plan and a read-only
    side X-Z elevation strip, plus a latching "3D" toggle that swaps the panels
    for an orbitable OpenGL scene (Map3DView, lazily created on first toggle so
    the default 2-D path stays GL-free). Sources (input positions) and the
    listener sweet-spot (D18) are draggable in the plan; speakers are read-only
    reference dots. The shared current input is highlighted and movable with the
    arrow keys / PageUp-PageDown (InputNudger) in both the 2-D and 3-D views;
    mouse editing in 3-D is selection-only.

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
#include "../Binding/InputNudger.h"
#include "../Map3D/Map3DView.h"
#include "../Widgets/RigProjection.h"
#include "Localization/LocalizationManager.h"
#include "XoaConstants.h"

namespace xoa::ui
{

class MapTab : public TabPage,
               private InputSelectionModel::Listener
{
public:
    explicit MapTab (AppContext& ctx) : TabPage (ctx, Surface::map)
    {
        setWantsKeyboardFocus (true);   // arrow-key / PageUp-PageDown nudging
        hintLabel.setJustificationType (juce::Justification::centredLeft);
        hintLabel.setText (LOC ("map.hint"), juce::dontSendNotification);
        addAndMakeVisible (hintLabel);

        view3dButton.setButtonText (LOC ("map.view3d"));
        view3dButton.setClickingTogglesState (true);
        view3dButton.onClick = [this] { set3DActive (view3dButton.getToggleState()); };
        addAndMakeVisible (view3dButton);

        context.inputSelection.addListener (this);
        verifyRegistryCoverage();
    }

    ~MapTab() override
    {
        context.inputSelection.removeListener (this);
    }

    void refresh() override
    {
        TabPage::refresh();
        if (is3DActive())
        {
            map3D->updateScene (context.inputSelection.get());
            return;
        }
        if (dragTarget == DragTarget::none)
            refit();
        repaint();
    }

    void resized() override
    {
        const float sc = XoaLookAndFeel::uiScale;
        auto px = [sc] (int v) { return juce::roundToInt ((float) v * sc); };
        auto area = getLocalBounds().reduced (px (10));
        auto hintRow = area.removeFromTop (px (20));
        view3dButton.setBounds (hintRow.removeFromRight (px (56)));
        hintRow.removeFromRight (px (8));
        hintLabel.setBounds (hintRow);
        area.removeFromTop (px (4));
        if (map3D != nullptr)
            map3D->setBounds (area);   // the 3-D view swaps in for plan + side
        sideArea = area.removeFromRight (px (160));
        area.removeFromRight (px (8));
        planArea = area;
        refit();
    }

    void paint (juce::Graphics& g) override
    {
        const auto& col = ColorScheme::get();
        g.fillAll (col.background);

        if (is3DActive())
            return;   // the GL child covers the content area

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

        // Sources (draggable input positions). The shared current input gets a
        // highlight ring (plan + side) — that's the one the arrow keys move.
        const int selected = context.inputSelection.get();
        for (int i = 0; i < context.store.getNumInputs(); ++i)
        {
            const double x = inp (i, ids::inputPositionX), y = inp (i, ids::inputPositionY), z = inp (i, ids::inputPositionZ);
            const auto p = plan.planToScreen (x, y);
            g.setColour (XoaColorUtilities::getInputColor (i + 1));
            g.fillEllipse (p.x - 5.0f, p.y - 5.0f, 10.0f, 10.0f);
            if (i == selected)
            {
                g.setColour (col.textPrimary);
                g.drawEllipse (p.x - 8.0f, p.y - 8.0f, 16.0f, 16.0f, 1.5f);
            }
            g.setColour (col.textPrimary);
            g.setFont (juce::FontOptions (11.0f));
            g.drawText (juce::String (i + 1), (int) p.x - 10, (int) p.y - 22, 20, 14, juce::Justification::centred);
            const auto sp = sideScreen (x, z);
            g.setColour (XoaColorUtilities::getInputColor (i + 1));
            g.fillEllipse (sp.x - 3.0f, sp.y - 3.0f, 6.0f, 6.0f);
            if (i == selected)
            {
                g.setColour (col.textPrimary);
                g.drawEllipse (sp.x - 5.5f, sp.y - 5.5f, 11.0f, 11.0f, 1.2f);
            }
        }
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (nudger.handleKey (key, context.inputSelection.get()))
        {
            if (is3DActive())
                map3D->updateScene (context.inputSelection.get());
            repaint();
            return true;
        }
        return false;
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        grabKeyboardFocus();
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
            context.inputSelection.set (best);
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

    void currentInputChanged (int newIndex) override
    {
        if (is3DActive())
            map3D->updateScene (newIndex);
        repaint();
    }

    bool is3DActive() const { return map3D != nullptr && map3D->isVisible(); }

    void set3DActive (bool on)
    {
        if (on && map3D == nullptr)   // lazy: the GL context only ever exists if 3D is used
        {
            map3D = std::make_unique<Map3DView> (context.store);
            map3D->onSourceClicked = [this] (int index) { context.inputSelection.set (index); };
            addAndMakeVisible (*map3D);
        }
        if (map3D != nullptr)
            map3D->setVisible (on);
        hintLabel.setText (LOC (on ? "map.hint3d" : "map.hint"), juce::dontSendNotification);
        resized();
        if (on)
            map3D->updateScene (context.inputSelection.get());
        grabKeyboardFocus();
        repaint();
    }

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
    juce::TextButton view3dButton;
    std::unique_ptr<Map3DView> map3D;
    juce::Rectangle<int> planArea, sideArea;
    RigProjection plan;

    DragTarget dragTarget = DragTarget::none;
    int dragIndex = -1;
    std::unique_ptr<ScopedParamGesture> gesture;
    InputNudger nudger { context.store };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MapTab)
};

} // namespace xoa::ui
