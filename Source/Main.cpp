/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    Application startup.

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#include <juce_gui_extra/juce_gui_extra.h>

#include "App/AppShell.h"
#include "GUI/XoaLookAndFeel.h"
#include "GUI/WindowUtils.h"

//==============================================================================
class XOAApplication : public juce::JUCEApplication
{
public:
    XOAApplication() = default;

    const juce::String getApplicationName() override       { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override    { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override              { return false; }

    void initialise (const juce::String& commandLine) override
    {
        // Install the XOA look-and-feel BEFORE creating the window so the window
        // chrome (and every component) picks up the themed colours. A single
        // app-wide TooltipWindow serves all tabs.
        lookAndFeel = std::make_unique<XoaLookAndFeel>();
        juce::LookAndFeel::setDefaultLookAndFeel (lookAndFeel.get());
        tooltipWindow = std::make_unique<juce::TooltipWindow> (nullptr, 700);

        mainWindow = std::make_unique<MainWindow> (getApplicationName(), commandLine);
    }

    void shutdown() override
    {
        mainWindow = nullptr;
        tooltipWindow = nullptr;
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
        lookAndFeel = nullptr;
    }

    void systemRequestedQuit() override
    {
        quit();
    }

    //==============================================================================
    class MainWindow : public juce::DocumentWindow
    {
    public:
        MainWindow (const juce::String& name, const juce::String& commandLine)
            : DocumentWindow (name,
                              juce::Desktop::getInstance().getDefaultLookAndFeel()
                                  .findColour (juce::ResizableWindow::backgroundColourId),
                              DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new xoa::ui::AppShell (commandLine), true);
            setResizable (true, true);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);

            // Dark native title bar (Windows DWM / macOS dark Aqua); no-op on Linux.
            WindowUtils::enableDarkTitleBar (this);
        }

        void closeButtonPressed() override
        {
            JUCEApplication::getInstance()->systemRequestedQuit();
        }

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

private:
    // Declared before mainWindow so it outlives the window during teardown; the
    // explicit ordering in shutdown() is the authoritative cleanup path.
    std::unique_ptr<XoaLookAndFeel>      lookAndFeel;
    std::unique_ptr<juce::TooltipWindow> tooltipWindow;
    std::unique_ptr<MainWindow>          mainWindow;
};

//==============================================================================
START_JUCE_APPLICATION (XOAApplication)
