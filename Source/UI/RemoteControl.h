#pragma once
// OSC server + feedback (Bitfocus Companion, TouchOSC, QLab...) and MIDI input
// (MIDI Machine Control + learnable note/CC/program bindings).
//
// Everything arrives on the message thread as an omr::remote::Command; MainComponent
// executes it exactly like the matching button, so the same safety rules apply
// (e.g. Space / play-stop never stops a recording).

#include "Core/Remote.h"
#include "Theme.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_osc/juce_osc.h>

struct RemoteStatus
{
    bool recording = false, playing = false;
    double seconds = 0;
    juce::String timecode, session;
    int take = 0, nextTake = 1, dropouts = 0;
    double diskAHours = 0, diskBHours = 0;
    bool diskAOk = false, diskBOk = false;
    struct Track { juce::String name; bool arm = false, mute = false, solo = false; };
    std::vector<Track> tracks;
};

class RemoteControl final : private juce::OSCReceiver::Listener<juce::OSCReceiver::MessageLoopCallback>,
                            private juce::MidiInputCallback,
                            private juce::Timer
{
public:
    struct Settings
    {
        bool oscEnabled = false;
        int oscPort = 9010;
        bool feedbackEnabled = false;
        juce::String feedbackHost = "127.0.0.1";
        int feedbackPort = 9011;
        juce::String midiInputId;            // empty = none
        bool mmcEnabled = true;
        int mmcDeviceId = 127;               // 127 = respond to all
        std::vector<omr::remote::Binding> bindings;
    };

    explicit RemoteControl (juce::PropertiesFile& props);
    ~RemoteControl() override;

    std::function<void (const omr::remote::Command&)> onCommand;   // message thread
    std::function<RemoteStatus()> getStatus;                        // polled for feedback
    std::function<void (const juce::String&, bool error)> onLog;

    Settings& settings() noexcept { return current; }
    void apply();                 // (re)open OSC port, feedback target and MIDI input; saves settings
    juce::String oscStatus() const { return oscStatusText; }
    juce::String midiStatus() const { return midiStatusText; }

    // MIDI learn: the next usable note / CC / program change is bound to `action`.
    void startLearn (omr::remote::Action action, std::function<void (const omr::remote::Binding&)> done);
    void cancelLearn();
    void clearBinding (omr::remote::Action action);
    std::vector<omr::remote::Binding> bindings() const;   // thread-safe copy
    static juce::String addressReference();

private:
    void oscMessageReceived (const juce::OSCMessage&) override;
    void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage&) override;
    void timerCallback() override;
    void sendFeedback (bool everything);
    template <typename T> void sendIfChanged (const juce::String& address, T value, bool force);
    void dispatch (const omr::remote::Command& c);
    void load();
    void save();

    juce::PropertiesFile& props;
    Settings current;
    juce::OSCReceiver receiver;
    juce::OSCSender sender;
    bool senderConnected = false;
    std::unique_ptr<juce::MidiInput> midiIn;
    juce::String oscStatusText = "OSC off", midiStatusText = "MIDI off";
    std::map<juce::String, juce::var> lastSent;
    int ticks = 0;

    mutable std::mutex learnLock;   // guards learning state and current.bindings (read on the MIDI thread)
    omr::remote::Action learning = omr::remote::Action::none;
    std::function<void (const omr::remote::Binding&)> learnDone;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);
};

// Settings window: OSC, feedback, MIDI input, MMC, MIDI learn and the OSC address map.
class RemoteSettingsComponent final : public juce::Component, private juce::Timer
{
public:
    explicit RemoteSettingsComponent (RemoteControl& rc);
    ~RemoteSettingsComponent() override;
    void resized() override;
    void paint (juce::Graphics&) override;

private:
    void timerCallback() override;
    void applyFromUi();
    void refreshBindings();

    RemoteControl& remote;
    juce::ToggleButton oscEnable { "Receive OSC on UDP port" }, fbEnable { "Send feedback to" }, mmcEnable { "Respond to MIDI Machine Control (MMC), device ID" };
    juce::TextEditor oscPort, fbHost, fbPort, mmcId, reference;
    juce::Label oscStatus, midiStatus, midiLabel { {}, "MIDI input" }, learnTitle { {}, "MIDI Learn (transport)" };
    juce::ComboBox midiIn;
    juce::TextButton applyButton { "Apply" };
    juce::Array<juce::MidiDeviceInfo> midiDevices;

    struct LearnRow { omr::remote::Action action; std::unique_ptr<juce::Label> name, binding; std::unique_ptr<juce::TextButton> learn, clear; };
    std::vector<LearnRow> rows;
};
