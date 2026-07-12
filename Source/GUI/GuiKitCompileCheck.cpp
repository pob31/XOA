/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    GuiKitCompileCheck — the GUI-kit compile guard. It includes every ported
    widget-kit header so the whole kit keeps compiling. Originally a temporary C2
    scaffold; kept permanently at C10 because the tabs exercise only a SUBSET of
    the ported kit (the remaining widgets are a reusable library for later work,
    e.g. WP12), and this TU ensures they never rot.

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
