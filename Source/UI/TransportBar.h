#pragma once
// Transport + master status bar: Record / Stop / Mark, timecode, session, driver, CPU,
// clock, dropouts, per-disk health, the 1:1 mirror master toggle and session/disk menus.

#include "Theme.h"

struct TransportStatus
{
    bool recording = false, playing = false, hasSession = false, noAudio = false;
    int playTake = 0;
    juce::String sessionName, sessionPath;
    int take = 1;
    double recSeconds = 0;
    juce::String device;
    double cpu = 0;
    double ppm = 0;
    bool clockKnown = false;
    int dropouts = 0;

    struct Disk
    {
        bool present = false, failed = false;
        juce::String label;
        double hoursLeft = 0;
        float ringFill = 0;     // 0..1, worst track
        double mbPerSec = 0;
    } diskA, diskB;
};

class TransportBar final : public juce::Component
{
public:
    TransportBar();

    void setStatus (const TransportStatus& s);

    juce::TextButton recordButton { "REC" }, stopButton { "STOP" }, playButton { "PLAY" }, rtzButton { "|<" }, markButton { "MARK" };
    juce::ComboBox takeCombo;
    juce::TextButton sessionMenu { "Session" }, disksMenu { "Disks" }, audioSetup { "Audio Setup" }, viewMenu { "View" };
    juce::TextButton addTrack { "+ Track" }, removeTrack { "- Track" };
    juce::ToggleButton mirrorToggle { "Mirror Input to Output (1:1)" };
    juce::ComboBox monitorLeft, monitorRight;

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int kHeight = 104;

private:
    TransportStatus status;
    juce::Rectangle<int> timeArea, chipsArea, monitorLabelArea;
    void drawChip (juce::Graphics&, juce::Rectangle<int>& row, const juce::String& title, const juce::String& value, juce::Colour c, int width);
};
