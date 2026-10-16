#pragma once
// Session folders, .livedeck session files, .ldtemplate templates and target validation.
//
//   <root>/
//     <Name>.livedeck
//     Audio/      T01_Kick_In_001.wav ...
//     Recovery/   journal.ndjson, <Name>.livedeck.autosave
//     Logs/       events_<timestamp>.log
//     Markers/    markers.csv

#include "PatchModel.h"

namespace omr
{
namespace SessionPaths
{
    juce::File audioDir (const juce::File& root);
    juce::File recoveryDir (const juce::File& root);
    juce::File logsDir (const juce::File& root);
    juce::File markersDir (const juce::File& root);
    juce::File journalFile (const juce::File& root);
    juce::File takeManifest (const juce::File& root, int take);   // Audio/.take_003.json
    juce::File sessionFile (const juce::File& root, const juce::String& name);
    bool createTree (const juce::File& root);
    juce::String sanitise (const juce::String& s);
    juce::String trackFileName (int trackIndex, const juce::String& trackName, int take, bool secondary);
}

struct SessionSettings
{
    juce::String name = "Untitled";
    juce::File primaryRoot;     // empty until the user picks a folder
    juce::File secondaryRoot;   // optional redundant target
    int bitsPerSample = 24;     // 24 = PCM, 32 = float
    double ringSeconds = 20.0;
    int nextTake = 1;
    juce::String deviceType, deviceName; // remembered for reopening
    double sampleRate = 48000.0;
    int bufferSize = 256;
    double minFreeHours = 0.5;  // refuse to start a take with less space than this
};

juce::var sessionToVar (const SessionSettings&, const PatchState&);
void sessionFromVar (const juce::var&, SessionSettings&, PatchState&);

juce::Result saveSession (const juce::File& file, const SessionSettings&, const PatchState&);
juce::Result loadSession (const juce::File& file, SessionSettings&, PatchState&);
// A template is a session with no folders and no take counter.
juce::Result saveTemplate (const juce::File& file, const SessionSettings&, const PatchState&);

struct TargetCheck
{
    bool ok = false;
    juce::String message;       // reason for failure, or a warning when ok
    double hoursAvailable = 0;
    juce::String filesystem;
};

TargetCheck validateTarget (const juce::File& root, double sampleRate, int bitsPerSample, int armedTracks, double minFreeHours = 0.5);
juce::String filesystemType (const juce::File& f);
} // namespace omr
