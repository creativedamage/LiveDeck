#pragma once
#include "Audio/AudioEngine.h"
#include "ChannelStrip.h"
#include "RemoteControl.h"
#include "TimelineView.h"
#include "TransportBar.h"
#include <juce_audio_utils/juce_audio_utils.h>

class MainComponent final : public juce::Component,
                            public juce::MenuBarModel,
                            private juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

    bool isRecording() const noexcept { return engine.isRecording(); }
    void stopRecording();
    void saveDeviceState();
    // Launch option: --session "<folder>" records into that folder without dialogs
    // (creates it if needed; loads the .livedeck inside if one exists).
    void useSessionFolder (const juce::File& folder);
    // Double-clicked .livedeck sessions / .ldtemplate templates and files dropped on the Dock icon end up here.
    void openSessionFile (const juce::File& file);
    void openTemplateFile (const juce::File& file);

    // Native menu bar (macOS main menu; an in-window menu bar elsewhere)
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex (int index, const juce::String& name) override;
    void menuItemSelected (int, int) override {}
    void showAbout();

    // In-app updates from GitHub Releases
    void checkForUpdates (bool userInitiated);
private:
    void timerCallback() override;

    // patching
    void applyPatch (const std::function<juce::Result (omr::PatchState&)>& op);
    void deviceChanged();
    void rebuildStrips();
    void refreshStrips();
    void refreshMonitorCombos();

    // track selection / editing (selection is kept by track id, so it survives reorders)
    void selectRow (int row, juce::ModifierKeys mods);
    std::vector<int> selectedRows() const;
    void showTrackMenu (int row);
    void deleteSelectedTracks();
    void moveSelectedTo (int insertBefore);
    void stopPlaybackForEdit();

    // transport
    void startRecording();
    void addMarker (const juce::String& label = {});
    void handleRemote (const omr::remote::Command& c);
    RemoteStatus remoteStatus();
    void showRemoteSettings();
    void startPlayback();
    void stopTransport();
    void returnToZero();
    void refreshTakes (bool selectNewest);
    void refreshTimeline (bool fit);
    std::vector<TimelineView::Marker> loadMarkers (int take) const;
    void showViewMenu();

    // session & disks
    void showSessionMenu();
    void showDisksMenu();
    void newSession (const juce::String& defaultName, std::function<void()> then = {});
    void openSession();
    void openTemplate();
    void saveSession();
    void saveTemplate();
    void recoverFolder();
    void chooseSecondary();
    void runBenchmark();
    void showAudioSetup();
    void setPrimaryRoot (const juce::File& root);
    void autosave();
    void updateStatus();

    // async helpers (results come back on the message thread)
    void askText (const juce::String& title, const juce::String& message, const juce::String& initial,
                  std::function<void (juce::String)> onOk);
    void chooseFolder (const juce::String& title, std::function<void (juce::File)> onChosen);
    void alert (const juce::String& title, const juce::String& message, bool error = false);

    Theme::LookAndFeel lnf;
    juce::TooltipWindow tooltips { this, 600 };

    omr::RecordEngine engine;
    AudioEngine audio { engine };
    omr::PatchState patch;
    omr::SessionSettings session;

    TransportBar bar;
    MeterBridge bridge;
    TimelineView timeline;
    omr::WaveformCache waveforms;
    juce::StretchableLayoutManager split;
    std::unique_ptr<juce::StretchableLayoutResizerBar> splitBar;
    std::unique_ptr<juce::MenuBarComponent> menuBar;   // Windows / Linux only
    bool showMixer = true, showConsole = true, showBridge = true;
    juce::Viewport stripView;
    juce::Component stripHolder;
    juce::OwnedArray<ChannelStrip> strips;
    juce::TextEditor console;

    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<juce::AlertWindow> textDialog;
    size_t logIndex = 0;
    int consoleLines = 0;
    int tick = 0;
    int lastMenuState = -1;
    juce::String lastDeviceWarning;
    void showAddTracksDialog();
    void showExtraDevices();
    std::array<bool, omr::kMaxDevices> driftWarned {};
    std::set<int> selectedIds;
    int anchorRow = -1;
    juce::Component stripDropMarker;
    int stripDropIndex = -1;

    // library folders, templates, feedback
    void revealLibrary();
    void addTemplateItems (juce::PopupMenu& m, bool enabled);
    void showFeedback (int kind);
    juce::String systemInfo() const;

    // drag-and-drop audio import (new tracks in the shown take)
    void importAudioFiles (const juce::StringArray& paths, uint64_t startFrame);
    struct ImportJob;
    void finishImport (std::shared_ptr<ImportJob> job);
    std::shared_ptr<std::atomic<bool>> importCancel;
    bool importRunning = false;

    void showUpdateOffer (const struct UpdateOffer& offer);
    void installUpdate (std::shared_ptr<struct UpdateOffer> offer);
    std::atomic<bool> updateBusy { false }, updateCancel { false };
    double updateProgress = 0.0;
    std::unique_ptr<juce::AlertWindow> updateWindow;
    bool autoUpdateCheck = true;
    bool noAudioLogged = false;
    uint64_t lastBytes[2] = { 0, 0 };
    double lastBytesTime = 0, mbps[2] = { 0, 0 };
    std::atomic<bool> benchmarkRunning { false };
    juce::File lastDir;
    uint64_t cueFrames = 0;
    std::map<int, uint64_t> takeLengths;     // take -> frames (longest track)
    juce::ApplicationProperties props;
    std::unique_ptr<RemoteControl> remote;   // after props (it reads its settings from them)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
