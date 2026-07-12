/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    WindowUtils.mm — macOS dark-Aqua title bar (Objective-C++). Compiled only on
    Apple (guarded by the CMake `if(APPLE)` source branch and JUCE_MAC).

    Ported from WFS-DIY (Source/gui/WindowUtils.mm); both projects are GPLv3.
    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#include "WindowUtils.h"

#if JUCE_MAC
#import <Cocoa/Cocoa.h>

void enableDarkTitleBarMac(void* nsWindow)
{
    if (nsWindow == nullptr)
        return;

    NSWindow* window = (__bridge NSWindow*)nsWindow;

    // Set the window appearance to dark Aqua (Mojave+)
    if (@available(macOS 10.14, *))
    {
        window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    }
}
#endif
