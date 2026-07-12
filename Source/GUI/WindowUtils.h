/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    WindowUtils — enable a dark native title bar on Windows 10/11 (DWM) and
    macOS (dark Aqua appearance). No-op on Linux.

    Ported from WFS-DIY (Source/gui/WindowUtils.{h,mm}); both projects are GPLv3.
    Scoped to the dark-title-bar helper only — the WFS App-Nap / process-priority
    opt-outs belong to a later performance-hardening pass, not the GUI framework
    port. This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#if JUCE_WINDOWS
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#endif

#if JUCE_MAC
// Implemented in WindowUtils.mm (Objective-C++).
void enableDarkTitleBarMac(void* nsWindow);
#endif

namespace WindowUtils
{
    /**
     * Enable dark mode title bar on Windows 10/11 and macOS.
     * Call this after the window is made visible.
     */
    inline void enableDarkTitleBar(juce::Component* window)
    {
#if JUCE_WINDOWS
        if (auto* peer = window->getPeer())
        {
            if (auto* handle = static_cast<HWND>(peer->getNativeHandle()))
            {
                // DWMWA_USE_IMMERSIVE_DARK_MODE = 20 (Windows 10 20H1+)
                BOOL darkMode = TRUE;
                ::DwmSetWindowAttribute(handle, 20, &darkMode, sizeof(darkMode));
            }
        }
#elif JUCE_MAC
        if (auto* peer = window->getPeer())
        {
            enableDarkTitleBarMac(peer->getNativeHandle());
        }
#else
        juce::ignoreUnused(window);
#endif
    }
}
