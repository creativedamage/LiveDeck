#include "UI/MainComponent.h"

class LiveDeckApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override          { return false; }

    void initialise (const juce::String& commandLine) override
    {
        mainWindow = std::make_unique<MainWindow> (getApplicationName());
        const auto args = getCommandLineParameterArray();
        const int i = args.indexOf ("--session");
        auto* mc = dynamic_cast<MainComponent*> (mainWindow->getContentComponent());
        if (mc != nullptr && i >= 0 && i + 1 < args.size())
            mc->useSessionFolder (juce::File::getCurrentWorkingDirectory().getChildFile (args[i + 1].unquoted()));
        juce::ignoreUnused (commandLine);
        openDocumentsIn (args);
    }

    // macOS sends double-clicked / dropped .livedeck files here while the app is running.
    void anotherInstanceStarted (const juce::String& commandLine) override
    {
        juce::StringArray args;
        args.addTokens (commandLine, true);
        openDocumentsIn (args);
    }

    void openDocumentsIn (const juce::StringArray& args)
    {
        auto* mc = mainWindow != nullptr ? dynamic_cast<MainComponent*> (mainWindow->getContentComponent()) : nullptr;
        if (mc == nullptr) return;
        for (auto a : args)
        {
            a = a.unquoted();
            if (! juce::File::isAbsolutePath (a)) continue;
            const juce::File f (a);
            if (f.existsAsFile() && f.hasFileExtension ("livedeck;ldtemplate;omr;omrt"))
            {
                juce::MessageManager::callAsync ([mc, f] { mc->openSessionFile (f); });
                break;
            }
        }
    }
    void shutdown() override                       { mainWindow = nullptr; }

    void systemRequestedQuit() override
    {
        auto* mc = mainWindow != nullptr ? dynamic_cast<MainComponent*> (mainWindow->getContentComponent()) : nullptr;
        if (mc == nullptr || ! mc->isRecording()) { quit(); return; }

        // Never let a stray Cmd+Q end a live take silently.
        juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                          .withIconType (juce::MessageBoxIconType::WarningIcon)
                                          .withTitle ("Recording in progress")
                                          .withMessage ("Stop the take and quit?")
                                          .withButton ("Stop and Quit")
                                          .withButton ("Keep Recording"),
                                      [mc] (int result) { if (result == 1) { mc->stopRecording(); juce::JUCEApplication::quit(); } });
    }

    class MainWindow final : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (const juce::String& name)
            : DocumentWindow (name, juce::Colour (0xff0f1114), DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent(), true);
            setResizable (true, true);
            setResizeLimits (1100, 700, 10000, 10000);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    };

private:
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION (LiveDeckApplication)
