/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    EqDisplayStyle — the app-side seam between XOA and the shared spatcore EQ
    UI (spatcore/ui/EQDisplayComponent.h, spatcore/ui/EQBandToggle.h). spatcore
    may never name an app symbol, so the parameter ids, ranges, theme colours
    and localised strings its widgets need are supplied from here.

    Both providers below are invoked AT PAINT TIME by the widgets and are never
    cached, so they must read ColorScheme::get() / LOC() afresh on every call —
    that is exactly what makes live theme and language switching work.

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#pragma once

#include "spatcore/ui/EQBandToggle.h"
#include "spatcore/ui/EQDisplayComponent.h"

#include "ColorScheme.h"
#include "Localization/LocalizationManager.h"
#include "Parameters/XoaParameterIDs.h"
#include "Parameters/XoaParameterDefaults.h"

namespace xoa
{

//==============================================================================
/** Theme colours for the speaker-EQ graph, mapped onto the six palette roles
    the spatcore header documents. Every role is the raw scheme colour except
    panelBackground, whose .darker (0.3f) the original component applied at
    paint time and which must therefore be applied here to keep the look. */
inline spatcore::ui::EQDisplayPalette speakerEqPalette()
{
    const auto& scheme = ColorScheme::get();
    return { scheme.background,                    // disabled-state scrim
             scheme.backgroundAlt.darker (0.3f),   // graph plotting area
             scheme.chromeDivider,                 // frequency + dB grid rules
             scheme.textPrimary,                   // curve stroke + selection ring
             scheme.textSecondary,                 // axis labels + "EQ OFF"
             scheme.accentBlue };                  // fill under the curve
}

/** Config for XOA's per-speaker EQ: the speaker-EQ parameter ids and ranges,
    the Output-EQ shape order (hasBandPass), and the two injected providers. */
inline spatcore::ui::EQDisplayConfig makeSpeakerEQConfig()
{
    spatcore::ui::EQDisplayConfig c;
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
    c.paletteProvider = [] { return speakerEqPalette(); };
    c.disabledTextProvider = [] { return LOC ("eq.status.off"); };
    return c;
}

//==============================================================================
/** Theme colours for an EQ band toggle; assign as its colourProvider. The ON
    fill is the band colour and is owned by the widget. */
inline spatcore::ui::EQBandToggleColours speakerEqToggleColours()
{
    return { ColorScheme::get().sliderTrackBg,     // OFF fill
             ColorScheme::get().buttonBorder };    // outline, both states
}

} // namespace xoa
