/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    EQDisplayComponent — interactive parametric EQ graph: log-frequency response
    curve summed across bands, draggable colour-coded band markers, crosshair
    drags (freq-only / gain-only), mouse-wheel Q, keyboard nudging.

    Ported from WFS-DIY (Source/gui/EQDisplayComponent.h); both projects are
    GPLv3. Kept config-driven (EQDisplayConfig) exactly as in WFS-DIY, where the
    same component serves the Output EQ and the Reverb pre/post EQs — XOA uses
    the speaker-EQ config today and the reverb configs arrive with the reverb
    work. Candidate for extraction into a shared spatcore UI module (with the
    palette/strings injected) so Tight-WFS can reuse it too.

    Differences from the WFS-DIY original:
      • The response math mirrors spatcore/dsp/OutputEQBiquadFilter.h formula
        for formula (instead of juce::dsp coefficients), so the curve is exactly
        what XOA's per-speaker filters do — including the separate shelf `slope`
        parameter (RBJ S), which WFS-DIY folds into Q.
      • Writes always delegate to onParameterChanged (the owning tab persists
        through the store's scoped-undo/array seam); the component never writes
        the ValueTree itself.

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <complex>
#include <map>
#include <vector>

#include "ColorScheme.h"
#include "Localization/LocalizationManager.h"
#include "Parameters/XoaParameterIDs.h"
#include "Parameters/XoaParameterDefaults.h"

//==============================================================================
/** Unified filter type for the display (both WFS-DIY shape encodings). */
enum class EQFilterType
{
    Off = 0,
    LowCut,      // high-pass with resonance
    LowShelf,
    PeakNotch,
    BandPass,
    AllPass,     // phase only, flat magnitude
    HighShelf,
    HighCut      // low-pass with resonance
};

//==============================================================================
/** Parameter-id/config seam so one component serves different EQs (WFS-DIY:
    Output EQ vs Reverb pre/post EQ — different ids, shape order and Q range). */
struct EQDisplayConfig
{
    juce::Identifier shapeId, frequencyId, gainId, qId;
    juce::Identifier slopeId;      // invalid ⇒ shelves use Q as S (WFS-DIY behaviour)

    float qMin = 0.1f, qMax = 10.0f;
    float slopeMin = 0.1f, slopeMax = 1.0f;
    bool hasBandPass = false;      // true ⇒ Output-EQ shape order, else Reverb order

    static EQDisplayConfig forSpeakerEQ()
    {
        EQDisplayConfig c;
        c.shapeId     = xoa::ids::eqShape;
        c.frequencyId = xoa::ids::eqFrequency;
        c.gainId      = xoa::ids::eqGain;
        c.qId         = xoa::ids::eqQ;
        c.slopeId     = xoa::ids::eqSlope;
        c.qMin  = (float) xoa::defaults::eqQMin;
        c.qMax  = (float) xoa::defaults::eqQMax;
        c.slopeMin = (float) xoa::defaults::eqSlopeMin;
        c.slopeMax = (float) xoa::defaults::eqSlopeMax;
        c.hasBandPass = true;
        return c;
    }
    // forReverbPreEQ() / forReverbPostEQ() arrive with XOA's reverb work — the
    // shape mapping below already understands the reverb (no-bandpass) order.
};

//==============================================================================
class EQDisplayComponent : public juce::Component,
                           private juce::ValueTree::Listener
{
public:
    //==========================================================================
    EQDisplayComponent (juce::ValueTree eqParentTree,
                        int numBandsIn,
                        const EQDisplayConfig& configIn)
        : eqTree (eqParentTree),
          numBands (numBandsIn),
          config (configIn)
    {
        eqTree.addListener (this);
        bandCoefficients.resize (static_cast<size_t> (numBands));
        updateAllCoefficients();

        setInterceptsMouseClicks (true, false);
        setWantsKeyboardFocus (true);
    }

    ~EQDisplayComponent() override
    {
        eqTree.removeListener (this);
    }

    //==========================================================================
    void setdBRange (float newMinDb, float newMaxDb)
    {
        mindB = newMinDb;
        maxdB = newMaxDb;
        repaint();
    }

    void setSampleRate (double newSampleRate)
    {
        if (newSampleRate > 0.0 && sampleRate != newSampleRate)
        {
            sampleRate = newSampleRate;
            updateAllCoefficients();
            repaint();
        }
    }

    void setEQEnabled (bool enabled)
    {
        if (eqEnabled != enabled)
        {
            eqEnabled = enabled;
            repaint();
        }
    }

    bool isEQEnabled() const { return eqEnabled; }

    //==========================================================================
    void paint (juce::Graphics& g) override
    {
        drawGrid (g);
        drawResponseCurve (g);
        drawBandMarkers (g);

        if (! eqEnabled)
        {
            g.setColour (ColorScheme::get().background.withAlpha (0.7f));
            g.fillRect (getLocalBounds());

            g.setColour (ColorScheme::get().textSecondary);
            g.setFont (juce::FontOptions (juce::jmax (14.0f, 24.0f * paintScale())));
            g.drawText (LOC ("eq.status.off"), getLocalBounds(), juce::Justification::centred);
        }
    }

    void resized() override { repaint(); }

    //==========================================================================
    // Mouse interaction (with multitouch pinch support, as in WFS-DIY)
    //==========================================================================
    void mouseDown (const juce::MouseEvent& e) override
    {
        int touchIndex = e.source.getIndex();

        TouchInfo touch;
        touch.position = e.position;
        touch.startPosition = e.position;
        activeTouches[touchIndex] = touch;

        if (activeTouches.size() == 2)
        {
            isPinching = true;
            pinchStartDistance = getTouchDistance();
            pinchStartQ = 0.0f;

            auto midpoint = getTouchMidpoint();
            int centeredBand = findBandNearestToPoint (midpoint);
            if (centeredBand >= 0)
            {
                selectedBand = centeredBand;
                isDragging = false;
            }
            if (selectedBand >= 0)
            {
                auto bandTree = eqTree.getChild (selectedBand);
                if (bandTree.isValid())
                    pinchStartQ = bandTree.getProperty (config.qId);
            }
            repaint();
            return;
        }

        int clickedBand = findBandAtPosition (e.position);
        if (clickedBand >= 0)
        {
            selectedBand = clickedBand;
            isDragging = true;
            dragMode = DragMode::Both;
            dragStartPos = e.position;
            setMouseCursor (juce::MouseCursor::DraggingHandCursor);
            beginDragAutoRepeat (50);
            grabKeyboardFocus();
        }
        else
        {
            auto crosshairMode = findCrosshairAtPosition (e.position);
            if (crosshairMode != DragMode::None)
            {
                isDragging = true;
                dragMode = crosshairMode;
                dragStartPos = e.position;

                auto bandTree = eqTree.getChild (selectedBand);
                dragStartFreq = static_cast<float> (static_cast<int> (bandTree.getProperty (config.frequencyId)));
                dragStartGain = bandTree.getProperty (config.gainId);

                setMouseCursor (crosshairMode == DragMode::GainOnly
                                    ? juce::MouseCursor::UpDownResizeCursor
                                    : juce::MouseCursor::LeftRightResizeCursor);
                beginDragAutoRepeat (50);
            }
            else
            {
                selectedBand = -1;
                isDragging = false;
                dragMode = DragMode::None;
            }
        }
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        int touchIndex = e.source.getIndex();
        auto it = activeTouches.find (touchIndex);
        if (it != activeTouches.end())
            it->second.position = e.position;

        if (isPinching && activeTouches.size() >= 2 && selectedBand >= 0)
        {
            float currentDistance = getTouchDistance();
            if (pinchStartDistance > 0.0f && pinchStartQ > 0.0f)
            {
                float scaleFactor = currentDistance / pinchStartDistance;
                float newQ = juce::jlimit (config.qMin, config.qMax, pinchStartQ / scaleFactor);
                setBandParameter (selectedBand, config.qId, newQ);
            }
            return;
        }

        if (! isDragging || selectedBand < 0)
            return;

        auto bandTree = eqTree.getChild (selectedBand);
        if (! bandTree.isValid())
            return;

        int shape = bandTree.getProperty (config.shapeId);
        EQFilterType filterType = shapeToFilterType (shape);

        if (dragMode != DragMode::GainOnly)
        {
            float newFreq;
            if (dragMode == DragMode::FrequencyOnly)
            {
                float startX = frequencyToX (dragStartFreq);
                newFreq = xToFrequency (startX + (e.position.x - dragStartPos.x));
            }
            else
            {
                newFreq = xToFrequency (e.position.x);
            }
            newFreq = juce::jlimit (20.0f, 20000.0f, newFreq);
            setBandParameter (selectedBand, config.frequencyId, static_cast<int> (newFreq));
        }

        if (dragMode != DragMode::FrequencyOnly && hasGainControl (filterType))
        {
            float newGain;
            if (dragMode == DragMode::GainOnly)
            {
                float startY = dBToY (dragStartGain);
                newGain = yTodB (startY + (e.position.y - dragStartPos.y));
            }
            else
            {
                newGain = yTodB (e.position.y);
            }
            newGain = juce::jlimit (mindB, maxdB, newGain);
            setBandParameter (selectedBand, config.gainId, newGain);
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        activeTouches.erase (e.source.getIndex());
        if (activeTouches.size() < 2)
            isPinching = false;
        if (activeTouches.empty())
        {
            isDragging = false;
            dragMode = DragMode::None;
            setMouseCursor (juce::MouseCursor::NormalCursor);
        }
        // Selection persists for wheel / keyboard adjustment.
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        if (isDragging)
            return;

        if (findBandAtPosition (e.position) >= 0)
        {
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
        }
        else
        {
            auto crosshairMode = findCrosshairAtPosition (e.position);
            if (crosshairMode == DragMode::GainOnly)
                setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
            else if (crosshairMode == DragMode::FrequencyOnly)
                setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
            else
                setMouseCursor (juce::MouseCursor::NormalCursor);
        }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        if (! isDragging)
            setMouseCursor (juce::MouseCursor::NormalCursor);
    }

    void mouseWheelMove (const juce::MouseEvent&,
                         const juce::MouseWheelDetails& wheel) override
    {
        if (selectedBand < 0)
            return;

        auto bandTree = eqTree.getChild (selectedBand);
        if (! bandTree.isValid())
            return;

        float currentQ = bandTree.getProperty (config.qId);
        float newQ = juce::jlimit (config.qMin, config.qMax,
                                   currentQ * (1.0f + wheel.deltaY * 0.5f));
        setBandParameter (selectedBand, config.qId, newQ);
    }

    void mouseMagnify (const juce::MouseEvent& e, float scaleFactor) override
    {
        int targetBand = selectedBand >= 0 ? selectedBand : findBandAtPosition (e.position);
        if (targetBand < 0)
            return;

        auto bandTree = eqTree.getChild (targetBand);
        if (! bandTree.isValid())
            return;

        float currentQ = bandTree.getProperty (config.qId);
        float newQ = juce::jlimit (config.qMin, config.qMax, currentQ * scaleFactor);
        setBandParameter (targetBand, config.qId, newQ);
        selectedBand = targetBand;
        repaint();
    }

    //==========================================================================
    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key.getKeyCode() == juce::KeyPress::escapeKey && selectedBand >= 0)
        {
            selectedBand = -1;
            repaint();
            return true;
        }

        if (selectedBand < 0)
            return false;

        auto bandTree = eqTree.getChild (selectedBand);
        if (! bandTree.isValid())
            return false;

        EQFilterType filterType = shapeToFilterType (bandTree.getProperty (config.shapeId));
        const int keyCode = key.getKeyCode();

        if (keyCode == juce::KeyPress::leftKey || keyCode == juce::KeyPress::rightKey)
        {
            int currentFreq = bandTree.getProperty (config.frequencyId);
            int increment = juce::jmax (1, currentFreq / 20);   // log-ish step
            int newFreq = juce::jlimit (20, 20000,
                                        keyCode == juce::KeyPress::rightKey ? currentFreq + increment
                                                                            : currentFreq - increment);
            setBandParameter (selectedBand, config.frequencyId, newFreq);
            return true;
        }

        if ((keyCode == juce::KeyPress::upKey || keyCode == juce::KeyPress::downKey)
            && hasGainControl (filterType))
        {
            float currentGain = bandTree.getProperty (config.gainId);
            float newGain = juce::jlimit (mindB, maxdB,
                                          currentGain + (keyCode == juce::KeyPress::upKey ? 0.1f : -0.1f));
            setBandParameter (selectedBand, config.gainId, newGain);
            return true;
        }

        return false;
    }

    //==========================================================================
    int getSelectedBand() const { return selectedBand; }

    void setSelectedBand (int band)
    {
        selectedBand = (band >= 0 && band < numBands) ? band : -1;
        repaint();
    }

    /** All writes go through here — the owning tab persists them via the store's
        scoped-undo / current-channel seam (see file header). */
    std::function<void (int, const juce::Identifier&, const juce::var&)> onParameterChanged;

    // WFS-DIY band rainbow — shared so band labels/toggles/dials can match.
    static juce::Colour getBandColour (int band)
    {
        static const juce::Colour colours[] = {
            juce::Colour (0xFFE74C3C),  // 1 red
            juce::Colour (0xFFE67E22),  // 2 orange
            juce::Colour (0xFFFFEB3B),  // 3 yellow
            juce::Colour (0xFF2ECC71),  // 4 green
            juce::Colour (0xFF3498DB),  // 5 blue
            juce::Colour (0xFF9B59B6),  // 6 purple
            juce::Colour (0xFF1ABC9C),  // 7 teal
            juce::Colour (0xFFE91E63),  // 8 pink
        };
        return colours[band % 8];
    }

private:
    /** Paint scale vs the 180px reference height (WFS-DIY convention). */
    float paintScale() const { return juce::jmax (0.65f, (float) getHeight() / 180.0f); }

    static bool hasGainControl (EQFilterType t)
    {
        return t != EQFilterType::LowCut && t != EQFilterType::HighCut
            && t != EQFilterType::BandPass && t != EQFilterType::AllPass;
    }

    //==========================================================================
    // ValueTree::Listener
    //==========================================================================
    void valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier&) override
    {
        for (int i = 0; i < numBands; ++i)
        {
            if (tree == eqTree.getChild (i))
            {
                updateBandCoefficients (i);
                repaint();
                return;
            }
        }
        if (tree == eqTree)
        {
            updateAllCoefficients();
            repaint();
        }
    }

    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override {}
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override {}
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override {}
    void valueTreeParentChanged (juce::ValueTree&) override {}

    //==========================================================================
    // Drawing
    //==========================================================================
    void drawGrid (juce::Graphics& g)
    {
        auto bounds = getLocalBounds().toFloat();

        g.setColour (ColorScheme::get().backgroundAlt.darker (0.3f));
        g.fillRect (bounds);

        const float freqLines[] = {
            20, 30, 40, 50, 60, 70, 80, 90,
            100, 200, 300, 400, 500, 600, 700, 800, 900,
            1000, 2000, 3000, 4000, 5000, 6000, 7000, 8000, 9000,
            10000, 20000
        };

        auto gridColor = ColorScheme::get().chromeDivider;
        for (float freq : freqLines)
        {
            float x = frequencyToX (freq);
            bool isMajor = (freq == 100 || freq == 1000 || freq == 10000);
            g.setColour (isMajor ? gridColor.withAlpha (0.6f) : gridColor.withAlpha (0.3f));
            g.drawVerticalLine (static_cast<int> (x), bounds.getY(), bounds.getBottom());
        }

        float ps = paintScale();
        g.setColour (ColorScheme::get().textSecondary);
        g.setFont (juce::jmax (7.0f, 10.0f * ps));

        const float labelFreqs[] = { 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000 };
        const char* labelTexts[] = { "20", "50", "100", "200", "500", "1k", "2k", "5k", "10k", "20k" };

        int freqLabelW = static_cast<int> (30 * ps);
        int freqLabelH = static_cast<int> (12 * ps);
        int freqLabelOff = static_cast<int> (15 * ps);
        for (int i = 0; i < 10; ++i)
        {
            float x = frequencyToX (labelFreqs[i]);
            g.drawText (labelTexts[i],
                        static_cast<int> (x) - freqLabelOff,
                        static_cast<int> (bounds.getBottom()) - freqLabelOff,
                        freqLabelW, freqLabelH,
                        juce::Justification::centred);
        }

        for (float dB = mindB; dB <= maxdB; dB += 6.0f)
        {
            float y = dBToY (dB);
            g.setColour (gridColor.withAlpha (std::abs (dB) < 0.1f ? 0.8f : 0.4f));
            g.drawHorizontalLine (static_cast<int> (y), bounds.getX(), bounds.getRight());

            g.setColour (ColorScheme::get().textSecondary);
            juce::String label = (dB > 0 ? "+" : "") + juce::String (static_cast<int> (dB));
            g.drawText (label, static_cast<int> (2 * ps), static_cast<int> (y) - static_cast<int> (6 * ps),
                        static_cast<int> (25 * ps), static_cast<int> (12 * ps),
                        juce::Justification::left);
        }
    }

    void drawResponseCurve (juce::Graphics& g)
    {
        juce::Path responseCurve;

        const int numPoints = juce::jmax (2, getWidth());
        const float zeroY = dBToY (0.0f);

        for (int x = 0; x < numPoints; ++x)
        {
            float freq = xToFrequency (static_cast<float> (x));
            float y = dBToY (calculateTotalResponse (freq));
            if (x == 0)
                responseCurve.startNewSubPath (static_cast<float> (x), y);
            else
                responseCurve.lineTo (static_cast<float> (x), y);
        }

        juce::Path filledCurve = responseCurve;
        filledCurve.lineTo (static_cast<float> (getWidth()), zeroY);
        filledCurve.lineTo (0.0f, zeroY);
        filledCurve.closeSubPath();

        g.setColour (ColorScheme::get().accentBlue.withAlpha (0.2f));
        g.fillPath (filledCurve);

        g.setColour (ColorScheme::get().textPrimary);
        g.strokePath (responseCurve, juce::PathStrokeType (2.0f));
    }

    void drawBandMarkers (juce::Graphics& g)
    {
        for (int band = 0; band < numBands; ++band)
        {
            auto bandTree = eqTree.getChild (band);
            if (! bandTree.isValid())
                continue;

            int shape = bandTree.getProperty (config.shapeId);
            bool isOff = (shape == 0);

            float freq = bandTree.getProperty (config.frequencyId);
            float gain = bandTree.getProperty (config.gainId);
            float x = frequencyToX (freq);
            float y = isOff ? dBToY (gain) : getBandMarkerPosition (band).y;

            juce::Colour bandColour = getBandColour (band);
            if (isOff)
                bandColour = bandColour.darker (0.6f);

            bool isSelected = (selectedBand == band);
            float ps = paintScale();
            float markerSize = isSelected ? 20.0f * ps : 14.0f * ps;

            g.setColour (bandColour);
            g.fillEllipse (x - markerSize / 2, y - markerSize / 2, markerSize, markerSize);

            if (isSelected)
            {
                g.setColour (ColorScheme::get().textPrimary);
                float ringOff = 3.0f * ps;
                g.drawEllipse (x - markerSize / 2 - ringOff, y - markerSize / 2 - ringOff,
                               markerSize + ringOff * 2, markerSize + ringOff * 2, 2.0f * ps);

                EQFilterType filterType = shapeToFilterType (shape);
                g.setColour (bandColour.withAlpha (0.35f));
                g.drawLine (x, 0.0f, x, static_cast<float> (getHeight()), 1.0f);
                if (isOff || hasGainControl (filterType))
                    g.drawLine (0.0f, y, static_cast<float> (getWidth()), y, 1.0f);
            }

            g.setColour (juce::Colours::black);
            g.setFont (juce::FontOptions (juce::jmax (8.0f, 13.0f * ps), juce::Font::bold));
            g.drawText (juce::String (band + 1),
                        static_cast<int> (x - markerSize / 2), static_cast<int> (y - markerSize / 2),
                        static_cast<int> (markerSize), static_cast<int> (markerSize),
                        juce::Justification::centred);
        }
    }

    //==========================================================================
    // Coordinate conversion
    //==========================================================================
    float frequencyToX (float freq) const
    {
        const float minFreq = 20.0f, maxFreq = 20000.0f;
        float normalized = std::log10 (freq / minFreq) / std::log10 (maxFreq / minFreq);
        return static_cast<float> (getWidth()) * normalized;
    }

    float xToFrequency (float x) const
    {
        const float minFreq = 20.0f, maxFreq = 20000.0f;
        float normalized = x / static_cast<float> (getWidth());
        return minFreq * std::pow (maxFreq / minFreq, normalized);
    }

    float dBToY (float dB) const
    {
        float normalized = (dB - mindB) / (maxdB - mindB);
        return static_cast<float> (getHeight()) * (1.0f - normalized);
    }

    float yTodB (float y) const
    {
        float normalized = 1.0f - (y / static_cast<float> (getHeight()));
        return mindB + normalized * (maxdB - mindB);
    }

    //==========================================================================
    // Shape mapping (both WFS-DIY encodings, selected by config.hasBandPass)
    //==========================================================================
    EQFilterType shapeToFilterType (int shape) const
    {
        if (config.hasBandPass)
        {
            // Output/speaker EQ: 0=Off 1=LowCut 2=LowShelf 3=Peak 4=BandPass
            //                    5=HighShelf 6=HighCut 7=AllPass
            switch (shape)
            {
                case 0: return EQFilterType::Off;
                case 1: return EQFilterType::LowCut;
                case 2: return EQFilterType::LowShelf;
                case 3: return EQFilterType::PeakNotch;
                case 4: return EQFilterType::BandPass;
                case 5: return EQFilterType::HighShelf;
                case 6: return EQFilterType::HighCut;
                case 7: return EQFilterType::AllPass;
                default: return EQFilterType::Off;
            }
        }
        // Reverb pre/post EQ: 0=Off 1=LowCut 2=LowShelf 3=Peak 4=HighShelf 5=HighCut 6=BandPass
        switch (shape)
        {
            case 0: return EQFilterType::Off;
            case 1: return EQFilterType::LowCut;
            case 2: return EQFilterType::LowShelf;
            case 3: return EQFilterType::PeakNotch;
            case 4: return EQFilterType::HighShelf;
            case 5: return EQFilterType::HighCut;
            case 6: return EQFilterType::BandPass;
            default: return EQFilterType::Off;
        }
    }

    //==========================================================================
    // Response math — mirrors spatcore/dsp/OutputEQBiquadFilter.h exactly
    // (normalized biquad; shelves take S from the slope parameter).
    //==========================================================================
    struct Biquad
    {
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
        bool active = false;

        float magnitudeDbAt (float freq, double fs) const
        {
            if (! active)
                return 0.0f;
            const double w = 2.0 * juce::MathConstants<double>::pi * freq / fs;
            const std::complex<double> z1 = std::polar (1.0, -w);
            const std::complex<double> z2 = z1 * z1;
            const std::complex<double> num = (double) b0 + (double) b1 * z1 + (double) b2 * z2;
            const std::complex<double> den = 1.0 + (double) a1 * z1 + (double) a2 * z2;
            const double mag = std::abs (num / den);
            return mag > 1.0e-6 ? (float) (20.0 * std::log10 (mag)) : -120.0f;
        }
    };

    void updateBandCoefficients (int bandIndex)
    {
        if (bandIndex < 0 || bandIndex >= numBands)
            return;

        auto& bq = bandCoefficients[static_cast<size_t> (bandIndex)];
        bq = {};

        auto bandTree = eqTree.getChild (bandIndex);
        if (! bandTree.isValid())
            return;

        const int shape = bandTree.getProperty (config.shapeId);
        const EQFilterType type = shapeToFilterType (shape);
        if (type == EQFilterType::Off || sampleRate <= 0.0)
            return;

        const float freq  = juce::jlimit (20.0f, 20000.0f, (float) bandTree.getProperty (config.frequencyId));
        const float gainDb = bandTree.getProperty (config.gainId);
        const float q     = juce::jlimit (config.qMin, config.qMax, (float) bandTree.getProperty (config.qId));
        const float slope = config.slopeId.isValid()
                                ? juce::jlimit (config.slopeMin, config.slopeMax,
                                                (float) bandTree.getProperty (config.slopeId))
                                : q;   // WFS-DIY behaviour: Q doubles as shelf S

        const float w0    = 2.0f * juce::MathConstants<float>::pi * freq / (float) sampleRate;
        const float cosw0 = std::cos (w0);
        const float sinw0 = std::sin (w0);
        float a0_inv = 1.0f;

        switch (type)
        {
            case EQFilterType::LowCut:
            {
                const float alpha = sinw0 / (2.0f * q);
                a0_inv = 1.0f / (1.0f + alpha);
                bq.b0 =  ((1.0f + cosw0) / 2.0f) * a0_inv;
                bq.b1 = -(1.0f + cosw0) * a0_inv;
                bq.b2 =  ((1.0f + cosw0) / 2.0f) * a0_inv;
                bq.a1 = (-2.0f * cosw0) * a0_inv;
                bq.a2 = (1.0f - alpha) * a0_inv;
                break;
            }
            case EQFilterType::LowShelf:
            {
                const float A = std::pow (10.0f, gainDb / 40.0f);
                const float alpha = (sinw0 / 2.0f) * std::sqrt (
                    (A + 1.0f / A) * (1.0f / slope - 1.0f) + 2.0f);
                const float sqrtA2alpha = 2.0f * std::sqrt (A) * alpha;

                a0_inv = 1.0f / ((A + 1.0f) + (A - 1.0f) * cosw0 + sqrtA2alpha);
                bq.b0 =       A * ((A + 1.0f) - (A - 1.0f) * cosw0 + sqrtA2alpha) * a0_inv;
                bq.b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cosw0)              * a0_inv;
                bq.b2 =       A * ((A + 1.0f) - (A - 1.0f) * cosw0 - sqrtA2alpha) * a0_inv;
                bq.a1 = -2.0f *    ((A - 1.0f) + (A + 1.0f) * cosw0)              * a0_inv;
                bq.a2 =            ((A + 1.0f) + (A - 1.0f) * cosw0 - sqrtA2alpha) * a0_inv;
                break;
            }
            case EQFilterType::PeakNotch:
            {
                const float A = std::pow (10.0f, gainDb / 40.0f);
                const float alpha = sinw0 / (2.0f * q);
                a0_inv = 1.0f / (1.0f + alpha / A);
                bq.b0 = (1.0f + alpha * A) * a0_inv;
                bq.b1 = (-2.0f * cosw0)    * a0_inv;
                bq.b2 = (1.0f - alpha * A) * a0_inv;
                bq.a1 = (-2.0f * cosw0)    * a0_inv;
                bq.a2 = (1.0f - alpha / A) * a0_inv;
                break;
            }
            case EQFilterType::BandPass:
            {
                const float alpha = sinw0 / (2.0f * q);
                a0_inv = 1.0f / (1.0f + alpha);
                bq.b0 =  alpha          * a0_inv;
                bq.b1 =  0.0f;
                bq.b2 = -alpha          * a0_inv;
                bq.a1 = (-2.0f * cosw0) * a0_inv;
                bq.a2 = (1.0f - alpha)  * a0_inv;
                break;
            }
            case EQFilterType::HighShelf:
            {
                const float A = std::pow (10.0f, gainDb / 40.0f);
                const float alpha = (sinw0 / 2.0f) * std::sqrt (
                    (A + 1.0f / A) * (1.0f / slope - 1.0f) + 2.0f);
                const float sqrtA2alpha = 2.0f * std::sqrt (A) * alpha;

                a0_inv = 1.0f / ((A + 1.0f) - (A - 1.0f) * cosw0 + sqrtA2alpha);
                bq.b0 =       A * ((A + 1.0f) + (A - 1.0f) * cosw0 + sqrtA2alpha) * a0_inv;
                bq.b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cosw0)              * a0_inv;
                bq.b2 =       A * ((A + 1.0f) + (A - 1.0f) * cosw0 - sqrtA2alpha) * a0_inv;
                bq.a1 =  2.0f *    ((A - 1.0f) - (A + 1.0f) * cosw0)              * a0_inv;
                bq.a2 =            ((A + 1.0f) - (A - 1.0f) * cosw0 - sqrtA2alpha) * a0_inv;
                break;
            }
            case EQFilterType::HighCut:
            {
                const float alpha = sinw0 / (2.0f * q);
                a0_inv = 1.0f / (1.0f + alpha);
                bq.b0 = ((1.0f - cosw0) / 2.0f) * a0_inv;
                bq.b1 =  (1.0f - cosw0)         * a0_inv;
                bq.b2 = ((1.0f - cosw0) / 2.0f) * a0_inv;
                bq.a1 = (-2.0f * cosw0)         * a0_inv;
                bq.a2 = (1.0f - alpha)          * a0_inv;
                break;
            }
            case EQFilterType::AllPass:
            {
                const float alpha = sinw0 / (2.0f * q);
                a0_inv = 1.0f / (1.0f + alpha);
                bq.b0 = (1.0f - alpha)  * a0_inv;
                bq.b1 = (-2.0f * cosw0) * a0_inv;
                bq.b2 = (1.0f + alpha)  * a0_inv;
                bq.a1 = (-2.0f * cosw0) * a0_inv;
                bq.a2 = (1.0f - alpha)  * a0_inv;
                break;
            }
            case EQFilterType::Off:
            default:
                return;
        }

        bq.active = true;
    }

    void updateAllCoefficients()
    {
        for (int i = 0; i < numBands; ++i)
            updateBandCoefficients (i);
    }

    float calculateTotalResponse (float frequency)
    {
        float totalGaindB = 0.0f;
        for (const auto& bq : bandCoefficients)
            totalGaindB += bq.magnitudeDbAt (frequency, sampleRate);
        return juce::jlimit (mindB - 6.0f, maxdB + 6.0f, totalGaindB);
    }

    //==========================================================================
    // Band marker positioning and hit testing
    //==========================================================================
    juce::Point<float> getBandMarkerPosition (int bandIndex) const
    {
        auto bandTree = eqTree.getChild (bandIndex);
        if (! bandTree.isValid())
            return { 0.0f, 0.0f };

        int shape = bandTree.getProperty (config.shapeId);
        float freq = bandTree.getProperty (config.frequencyId);
        float gain = bandTree.getProperty (config.gainId);

        float x = frequencyToX (freq);
        EQFilterType filterType = shapeToFilterType (shape);
        float y = hasGainControl (filterType) ? dBToY (gain) : dBToY (0.0f);
        return { x, y };
    }

    int findBandAtPosition (juce::Point<float> pos)
    {
        const float hitRadius = 15.0f * paintScale();
        const float hitRadiusSq = hitRadius * hitRadius;

        for (int band = 0; band < numBands; ++band)
        {
            auto p = markerPositionIncludingOff (band);
            float dx = pos.x - p.x, dy = pos.y - p.y;
            if (dx * dx + dy * dy < hitRadiusSq)
                return band;
        }
        return -1;
    }

    enum class DragMode { None, Both, FrequencyOnly, GainOnly };

    juce::Point<float> markerPositionIncludingOff (int band) const
    {
        auto bandTree = eqTree.getChild (band);
        if (! bandTree.isValid())
            return { -1000.0f, -1000.0f };

        int shape = bandTree.getProperty (config.shapeId);
        float freq = bandTree.getProperty (config.frequencyId);
        float x = frequencyToX (freq);
        float y;
        if (shape == 0)
        {
            float gain = bandTree.getProperty (config.gainId);
            y = dBToY (gain);
        }
        else
        {
            y = getBandMarkerPosition (band).y;
        }
        return { x, y };
    }

    DragMode findCrosshairAtPosition (juce::Point<float> pos) const
    {
        if (selectedBand < 0)
            return DragMode::None;

        auto bandTree = eqTree.getChild (selectedBand);
        if (! bandTree.isValid())
            return DragMode::None;

        int shape = bandTree.getProperty (config.shapeId);
        bool isOff = (shape == 0);
        auto marker = markerPositionIncludingOff (selectedBand);

        const float hitTolerance = 8.0f * paintScale();
        EQFilterType filterType = shapeToFilterType (shape);

        if (std::abs (pos.x - marker.x) < hitTolerance)
            return DragMode::FrequencyOnly;

        if ((isOff || hasGainControl (filterType)) && std::abs (pos.y - marker.y) < hitTolerance)
            return DragMode::GainOnly;

        return DragMode::None;
    }

    //==========================================================================
    // Multitouch helpers
    //==========================================================================
    float getTouchDistance() const
    {
        if (activeTouches.size() < 2)
            return 0.0f;
        auto it = activeTouches.begin();
        auto pos1 = it->second.position;
        ++it;
        return pos1.getDistanceFrom (it->second.position);
    }

    juce::Point<float> getTouchMidpoint() const
    {
        if (activeTouches.size() < 2)
            return { 0.0f, 0.0f };
        auto it = activeTouches.begin();
        auto pos1 = it->second.position;
        ++it;
        auto pos2 = it->second.position;
        return { (pos1.x + pos2.x) * 0.5f, (pos1.y + pos2.y) * 0.5f };
    }

    int findBandNearestToPoint (juce::Point<float> pos) const
    {
        int nearestBand = -1;
        float nearestDistSq = std::numeric_limits<float>::max();
        const float maxSearchRadius = 150.0f * paintScale();
        const float maxSearchRadiusSq = maxSearchRadius * maxSearchRadius;

        for (int band = 0; band < numBands; ++band)
        {
            auto p = markerPositionIncludingOff (band);
            float dx = pos.x - p.x, dy = pos.y - p.y;
            float distSq = dx * dx + dy * dy;
            if (distSq < nearestDistSq && distSq < maxSearchRadiusSq)
            {
                nearestDistSq = distSq;
                nearestBand = band;
            }
        }
        return nearestBand;
    }

    //==========================================================================
    void setBandParameter (int bandIndex, const juce::Identifier& paramId, const juce::var& value)
    {
        // The owner persists (store scoped-write with undo + array semantics);
        // its write lands on this same live tree, so our listener repaints.
        if (onParameterChanged)
            onParameterChanged (bandIndex, paramId, value);
    }

    //==========================================================================
    juce::ValueTree eqTree;
    int numBands;
    EQDisplayConfig config;

    float mindB = -24.0f;
    float maxdB = 24.0f;
    double sampleRate = 48000.0;
    bool eqEnabled = true;

    int selectedBand = -1;
    bool isDragging = false;
    DragMode dragMode = DragMode::None;
    juce::Point<float> dragStartPos;
    float dragStartFreq = 0.0f;
    float dragStartGain = 0.0f;

    std::vector<Biquad> bandCoefficients;

    struct TouchInfo
    {
        juce::Point<float> position;
        juce::Point<float> startPosition;
    };
    std::map<int, TouchInfo> activeTouches;
    bool isPinching = false;
    float pinchStartDistance = 0.0f;
    float pinchStartQ = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EQDisplayComponent)
};
