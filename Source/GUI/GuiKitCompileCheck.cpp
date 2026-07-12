/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    GuiKitCompileCheck — TEMPORARY translation unit (WP10 C2). It includes every
    ported GUI-kit header so the build validates the whole kit compiles before any
    tab wires it up. Deleted in WP10 C10 once the tabs reference these headers
    directly.

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#include "ColorScheme.h"
#include "ColorUtilities.h"
#include "XoaLookAndFeel.h"
#include "ColumnFocusTraverser.h"
#include "StatusBar.h"
#include "HelpCard.h"
#include "XoaHelpCards.h"

#include "Binding/UiParameterDescriptors.h"
#include "Binding/TabParameterRegistry.h"
#include "Binding/ParamBindings.h"

#include "../Accessibility/TTSManager.h"

#include "Widgets/XoaSliderBase.h"
#include "Widgets/XoaStandardSlider.h"
#include "Widgets/XoaBidirectionalSlider.h"
#include "Widgets/XoaRangeSlider.h"
#include "Widgets/XoaAutoCenterSlider.h"
#include "Widgets/XoaBasicDial.h"
#include "Widgets/XoaRotationDial.h"
#include "Widgets/XoaEndlessDial.h"
#include "Widgets/CountdownTextButton.h"
#include "Widgets/WrappingTextButton.h"
#include "Widgets/EQBandToggle.h"
#include "Widgets/LongPressButton.h"
#include "Widgets/XoaLevelMeterBar.h"
#include "Widgets/XoaThreadPerformanceBar.h"

namespace
{
// Referencing a couple of default-constructible widgets forces the compiler past
// mere header parsing into instantiating their inline bodies.
[[maybe_unused]] void xoaGuiKitCompileCheck()
{
    XoaLevelMeterBar        meter;
    XoaThreadPerformanceBar perf;
    juce::ignoreUnused (meter, perf);
}
} // namespace
