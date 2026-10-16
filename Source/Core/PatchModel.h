#pragma once
// Editable patch state (what the user sees) + the real-time RoutingSnapshot it compiles to
// (what the audio thread reads). All mutations go through the functions below, which
// enforce the record-time locks and the 1:1 mirror rules.

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <memory>
#include <vector>

namespace omr
{
constexpr int kMaxTracks = 256;
constexpr int kMaxDirectOuts = 4;
constexpr int kMaxDevices = 8;

// One open audio device. Channels of all devices are numbered globally in device order
// (device 0's inputs first), so tracks keep a single input/output number.
struct DeviceLayout
{
    juce::String name;
    int numInputs = 0, numOutputs = 0;
};

struct TrackState
{
    int id = 0;                  // permanent, unique per session: recorded takes follow the track through reorders
    juce::String name;
    int input = -1;              // device input channel (0-based), -1 = unpatched
    std::vector<int> outputs;    // direct outs (device output channels), 0..kMaxDirectOuts
    float trimDb = 0.0f;         // affects monitor + direct outs only; files are recorded unaltered
    bool armed = true, mute = false, solo = false;
    bool autoName = true;        // name follows the driver's channel name until the user renames it
    int colour = -1;             // palette index, -1 = automatic (by track number)
    bool stereo = false;         // input is the pair (input, input + 1); records an interleaved stereo file
    bool outStereo = false;      // outputs[0] is the left of a pair (o, o + 1)
};

struct PatchState
{
    bool mirror = true;                  // "Mirror Input to Output": each track's output follows its input (in 7 -> out 7)
    int numInputs = 0, numOutputs = 0;   // of the open device
    juce::StringArray inputNames, outputNames;
    std::vector<bool> inputActive, outputActive;   // runtime only: channels enabled in Audio Setup (empty = all)
    std::vector<DeviceLayout> devices;               // runtime only: empty = one device holding every channel
    std::vector<TrackState> tracks;
    int monitorLeft = -1, monitorRight = -1; // monitor bus outputs, -1 = off
    float monitorDb = 0.0f;
};

// ---- operations (control / message thread). `recording` enables the record-time locks.
// Records the device's channels. Never adds, removes or re-patches tracks: a track whose channel
// is missing (device unplugged, fewer channels) keeps its patch and is reported, not changed.
juce::Result setDeviceChannels (PatchState&, const juce::StringArray& inputNames, const juce::StringArray& outputNames,
                                const std::vector<bool>& inputActive = {}, const std::vector<bool>& outputActive = {});
bool isInputActive (const PatchState&, int channel);
// Global channel -> (device, channel on that device). False if the channel does not exist.
bool locateInput (const PatchState&, int global, int& device, int& local);
bool locateOutput (const PatchState&, int global, int& device, int& local);
bool isOutputActive (const PatchState&, int channel);
juce::Result setMirror (PatchState&, bool on, bool recording);
// `stereo` picks the pair (input, input + 1) and makes the track stereo.
juce::Result setTrackInput (PatchState&, int track, int input, bool recording, bool stereo = false);
// Replaces the primary direct out; -1 clears. `stereo` = the pair (output, output + 1).
juce::Result setTrackOutput (PatchState&, int track, int output, bool stereo = false);
// Adds a track on the first active input no other track uses (or unpatched if all are used).
juce::Result addTrack (PatchState&, bool recording);
// Adds `count` tracks on consecutive active inputs from `firstInput` (pairs when stereo).
juce::Result addTracks (PatchState&, int count, int firstInput, bool stereo, bool recording);
int firstUnusedInput (const PatchState&, bool stereo);
juce::Result removeLastTrack (PatchState&, bool recording);
// Deletes the given rows (any order). Recorded audio is never touched.
juce::Result deleteTracks (PatchState&, std::vector<int> rows, bool recording);
// Moves the given rows, as one block in their current order, so they start at `insertBefore`
// (an index in the list as it was before the move; tracks.size() = end). Returns their new rows.
juce::Result moveTracks (PatchState&, std::vector<int> rows, int insertBefore, bool recording, std::vector<int>* newRows = nullptr);
void ensureTrackIds (PatchState&);   // gives every track a unique id > 0
int rowForTrackId (const PatchState&, int id);
juce::Result setArmed (PatchState&, int track, bool armed, bool recording);
juce::Result renameTrack (PatchState&, int track, const juce::String& name);
void applyMirror (PatchState&);   // outputs follow inputs; tracks are never created or removed

juce::var patchToVar (const PatchState&);
PatchState patchFromVar (const juce::var&);

// ---- real-time snapshot
struct TrackRT
{
    int16_t input = -1, input2 = -1;      // channels on device `inDev`; input2 >= 0 for stereo tracks
    uint8_t armed = 0, mute = 0, solo = 0, numOuts = 0, outStereo = 0;
    uint8_t inDev = 0, outDev = 0;        // outs[] are channels on device `outDev`
    float gain = 1.0f;
    std::array<int16_t, kMaxDirectOuts> outs {};
};

struct RoutingSnapshot
{
    uint32_t generation = 0;
    int numTracks = 0;
    bool anySolo = false;
    int monitorLeft = -1, monitorRight = -1;   // channels on device `monitorDev`
    int monitorDev = 0;
    float monitorGain = 1.0f;
    int crossDeviceOuts = 0;                  // direct outs dropped because they sit on another device than the input
    std::array<TrackRT, kMaxTracks> tracks {};
};

std::unique_ptr<RoutingSnapshot> compileSnapshot (const PatchState&, uint32_t generation);
} // namespace omr
