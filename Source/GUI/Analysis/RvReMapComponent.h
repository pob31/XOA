/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    RvReMapComponent — the FR-18 decoder-inspection view (WP10 C8): an
    equirectangular lat/long raster of a chosen metric (energy / ||rV|| / rV error
    / ||rE|| / rE error) over the sphere, with speaker-direction markers, a
    tolerance stats strip, a hover readout, and CSV/JSON export of the analysis and
    the decode matrix. It renders exactly the AnalysisResult sample vector that the
    CSV export serializes.

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <memory>
#include <optional>

#include "RvReAnalysisCore.h"
#include "../ColorScheme.h"
#include "../XoaLookAndFeel.h"
#include "Helpers/XoaCoordinates.h"
#include "Localization/LocalizationManager.h"

namespace xoa::ui
{

class RvReMapComponent : public juce::Component
{
public:
    RvReMapComponent()
    {
        metricCombo.addItem (LOC ("analysis.energy"), 1);
        metricCombo.addItem (LOC ("analysis.rvMag"),  2);
        metricCombo.addItem (LOC ("analysis.rvErr"),  3);
        metricCombo.addItem (LOC ("analysis.reMag"),  4);
        metricCombo.addItem (LOC ("analysis.reErr"),  5);
        metricCombo.setSelectedId (1, juce::dontSendNotification);
        metricCombo.onChange = [this] { rebuildImage(); repaint(); };
        addAndMakeVisible (metricCombo);

        statsLabel.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (statsLabel);
        hoverLabel.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (hoverLabel);

        exportCsvButton.setButtonText (LOC ("analysis.exportCsv"));
        exportMatrixCsvButton.setButtonText (LOC ("analysis.exportMatrixCsv"));
        exportMatrixJsonButton.setButtonText (LOC ("analysis.exportMatrixJson"));
        addAndMakeVisible (exportCsvButton);
        addAndMakeVisible (exportMatrixCsvButton);
        addAndMakeVisible (exportMatrixJsonButton);
        exportCsvButton.onClick        = [this] { if (result) exportText (analysis::toCsv (result->samples), "rvre-analysis.csv"); };
        exportMatrixCsvButton.onClick  = [this] { if (result) exportText (analysis::decoderMatrixToCsv (result->matrix), "decode-matrix.csv"); };
        exportMatrixJsonButton.onClick = [this] { if (result) exportText (analysis::decoderMatrixToJsonString (result->matrix), "decode-matrix.json"); };
    }

    void setResult (std::shared_ptr<const AnalysisResult> newResult)
    {
        result = std::move (newResult);
        rebuildImage();
        repaint();
    }

    std::uint64_t currentGeneration() const { return result ? result->generation : 0; }

    void paint (juce::Graphics& g) override
    {
        const auto& col = ColorScheme::get();
        g.setColour (col.background);
        g.fillRect (mapArea);
        g.setColour (col.chromeDivider);
        g.drawRect (mapArea, 1);

        const bool haveExport = (result != nullptr);
        exportCsvButton.setEnabled (haveExport);
        exportMatrixCsvButton.setEnabled (haveExport);
        exportMatrixJsonButton.setEnabled (haveExport);

        if (! result || result->samples.empty() || ! raster.isValid() || mapArea.isEmpty())
        {
            g.setColour (col.textSecondary);
            g.drawText (LOC ("analysis.noData"), mapArea, juce::Justification::centred);
            return;
        }

        g.setImageResamplingQuality (juce::Graphics::mediumResamplingQuality);
        g.drawImage (raster, mapArea.toFloat());

        // Speaker markers.
        g.setColour (col.textPrimary);
        for (int s = 0; s < result->layout.count; ++s)
        {
            const auto sph = coords::cartesianToSpherical (result->layout.positions[s]);
            const auto p = sphereToScreen (sph.azimuthDeg, sph.elevationDeg);
            g.drawEllipse (p.x - 3.0f, p.y - 3.0f, 6.0f, 6.0f, 1.5f);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        if (! result || ! mapArea.contains (e.getPosition()))
        {
            hoverLabel.setText ({}, juce::dontSendNotification);
            return;
        }
        // Nearest grid sample to the cursor.
        const float fx = (float) (e.x - mapArea.getX()) / (float) juce::jmax (1, mapArea.getWidth());
        const float fy = (float) (e.y - mapArea.getY()) / (float) juce::jmax (1, mapArea.getHeight());
        const double az = -180.0 + 360.0 * fx;
        const double el =   90.0 - 180.0 * fy;
        hoverLabel.setText ("az " + juce::String (juce::roundToInt (az)) + "°, el "
                                + juce::String (juce::roundToInt (el)) + "°",
                            juce::dontSendNotification);
    }

    void resized() override
    {
        const float sc = XoaLookAndFeel::uiScale;
        auto px = [sc] (int v) { return juce::roundToInt ((float) v * sc); };
        auto area = getLocalBounds();

        auto top = area.removeFromTop (px (26));
        metricCombo.setBounds (top.removeFromLeft (px (150)));
        top.removeFromLeft (px (8));
        hoverLabel.setBounds (top);

        auto bottom = area.removeFromBottom (px (26));
        exportCsvButton.setBounds (bottom.removeFromLeft (px (110)));       bottom.removeFromLeft (px (4));
        exportMatrixCsvButton.setBounds (bottom.removeFromLeft (px (120))); bottom.removeFromLeft (px (4));
        exportMatrixJsonButton.setBounds (bottom.removeFromLeft (px (120)));

        statsLabel.setBounds (area.removeFromBottom (px (22)));
        mapArea = area.reduced (px (2));
    }

private:
    enum class Metric { energyDb = 0, rvMag, rvErr, reMag, reErr };

    static std::optional<double> metricValue (const analysis::DirectionSample& s, Metric m)
    {
        switch (m)
        {
            case Metric::energyDb: return 10.0 * std::log10 (juce::jmax (1.0e-12, s.energy));
            case Metric::rvMag:    return s.rvValid ? std::optional<double> (s.rvMagnitude)   : std::nullopt;
            case Metric::rvErr:    return s.rvValid ? std::optional<double> (s.rvDirErrorDeg) : std::nullopt;
            case Metric::reMag:    return s.reValid ? std::optional<double> (s.reMagnitude)   : std::nullopt;
            case Metric::reErr:    return s.reValid ? std::optional<double> (s.reDirErrorDeg) : std::nullopt;
        }
        return std::nullopt;
    }

    static juce::Colour ramp (float t)
    {
        t = juce::jlimit (0.0f, 1.0f, t);
        return juce::Colour::fromHSV (0.66f * (1.0f - t), 0.85f, 0.92f, 1.0f);   // blue(low) -> red(high)
    }

    juce::Point<float> sphereToScreen (double azDeg, double elDeg) const
    {
        const float fx = (float) ((azDeg + 180.0) / 360.0);
        const float fy = (float) ((90.0 - elDeg) / 180.0);
        return { mapArea.getX() + fx * mapArea.getWidth(),
                 mapArea.getY() + fy * mapArea.getHeight() };
    }

    void rebuildImage()
    {
        raster = juce::Image();
        if (! result || result->samples.empty())
            return;

        const auto metric = (Metric) metricCombo.getSelectedItemIndex();
        const analysis::GridOptions grid;   // the samples were produced with the default grid
        const int w = grid.azimuthSteps, h = grid.elevationSteps;
        if ((int) result->samples.size() != w * h)
            return;

        // Per-metric range across valid samples.
        double lo = 1.0e300, hi = -1.0e300;
        double minRv = 1.0e300, maxRv = -1.0e300, maxReErr = 0.0;
        for (const auto& s : result->samples)
        {
            if (auto v = metricValue (s, metric)) { lo = juce::jmin (lo, *v); hi = juce::jmax (hi, *v); }
            if (s.rvValid) { minRv = juce::jmin (minRv, s.rvMagnitude); maxRv = juce::jmax (maxRv, s.rvMagnitude); }
            if (s.reValid) maxReErr = juce::jmax (maxReErr, s.reDirErrorDeg);
        }
        const double span = (hi > lo) ? (hi - lo) : 1.0;

        raster = juce::Image (juce::Image::RGB, w, h, false);
        juce::Image::BitmapData bmp (raster, juce::Image::BitmapData::writeOnly);
        for (int ei = 0; ei < h; ++ei)
            for (int ai = 0; ai < w; ++ai)
            {
                const auto& s = result->samples[(size_t) ei * w + ai];
                const auto v = metricValue (s, metric);
                const juce::Colour c = v ? ramp ((float) ((*v - lo) / span))
                                         : juce::Colours::black;
                bmp.setPixelColour (ai, h - 1 - ei, c);   // flip: +90 at top
            }

        juce::String stats;
        stats << "||rV|| " << juce::String (minRv, 2) << "–" << juce::String (maxRv, 2)
              << " (tol " << juce::String (analysis::kRvMagnitudeMin, 2) << "–"
              << juce::String (analysis::kRvMagnitudeMax, 2) << ")   ·   max rE err "
              << juce::String (maxReErr, 1) << "° (tol " << juce::String (analysis::kReDirectionErrorMaxDeg, 1) << "°)";
        statsLabel.setText (stats, juce::dontSendNotification);
    }

    void exportText (const juce::String& text, const juce::String& suggestedName)
    {
        if (! result)
            return;
        fileChooser = std::make_unique<juce::FileChooser> (LOC ("analysis.exportTitle"),
                                                           juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                                                               .getChildFile (suggestedName),
                                                           "*");
        fileChooser->launchAsync (juce::FileBrowserComponent::saveMode
                                      | juce::FileBrowserComponent::canSelectFiles
                                      | juce::FileBrowserComponent::warnAboutOverwriting,
                                  [text] (const juce::FileChooser& fc)
        {
            const auto f = fc.getResult();
            if (f != juce::File())
                f.replaceWithText (text);
        });
    }

    std::shared_ptr<const AnalysisResult> result;
    juce::Image raster;
    juce::Rectangle<int> mapArea;

    juce::ComboBox metricCombo;
    juce::Label    statsLabel, hoverLabel;
    juce::TextButton exportCsvButton, exportMatrixCsvButton, exportMatrixJsonButton;

    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RvReMapComponent)
};

} // namespace xoa::ui
