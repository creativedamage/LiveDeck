#pragma once
// Remote control commands, independent of the transport they arrive on.
//
// OSC address map (all case-insensitive; prefix /livedeck):
//   /livedeck/record                    start recording
//   /livedeck/stop                      stop recording / playback (a second stop returns to zero)
//   /livedeck/play                      play the selected take
//   /livedeck/playstop                  toggle play / stop (never stops a recording)
//   /livedeck/rtz                       return to zero
//   /livedeck/marker [label]            add a marker (optional label)
//   /livedeck/locate <seconds|"h:mm:ss">
//   /livedeck/take <n>   /livedeck/take/next   /livedeck/take/prev
//   /livedeck/track/<n>/arm   [0|1]     (no argument = toggle; n is 1-based)
//   /livedeck/track/<n>/mute  [0|1]
//   /livedeck/track/<n>/solo  [0|1]
//   /livedeck/arm/all <0|1>
//   /livedeck/ping                      replies /livedeck/pong to the feedback target
//
// MIDI: MIDI Machine Control (Stop, Play, Deferred Play, Record Strobe, Record Exit,
// Pause, Locate) plus learnable note / CC / program-change bindings for transport actions.

#include <juce_audio_basics/juce_audio_basics.h>

namespace omr::remote
{
enum class Action
{
    none, record, stop, play, playStop, rtz, marker, locate,
    selectTake, nextTake, prevTake, armTrack, muteTrack, soloTrack, armAll, ping
};

juce::String actionName (Action a);

struct Command
{
    Action action = Action::none;
    int track = -1;          // 0-based row for track commands
    int value = -1;          // 1 = on, 0 = off, -1 = toggle; take number for selectTake
    double seconds = 0.0;    // locate
    juce::String text;       // marker label
};

// OSC: address + already-decoded arguments (int / float / string vars).
Command parseOsc (const juce::String& address, const juce::Array<juce::var>& args);

// MMC: a complete SysEx message (with or without the F0 / F7 framing).
// deviceId 127 accepts every device id.
Command parseMmc (const uint8_t* data, int size, int deviceId = 127);

// "h:mm:ss", "mm:ss", "ss.s" or a plain number of seconds; -1 when unparseable.
double parseTimeText (const juce::String& s);

// MIDI learn bindings for one-shot actions.
struct Binding
{
    enum class Kind { none, note, cc, program } kind = Kind::none;
    int channel = 0;         // 1..16, 0 = any
    int number = 0;          // note / controller / program
    Action action = Action::none;

    bool matches (const juce::MidiMessage& m) const;   // note-on (vel > 0), CC >= 64, or program change
    static Binding learnFrom (const juce::MidiMessage& m, Action a);   // kind none if m is not usable
    juce::String describe() const;                      // "Note C3 ch 1", "CC 20 any ch"
    juce::var toVar() const;
    static Binding fromVar (const juce::var& v);
};

// Actions offered for MIDI learn, in display order.
const std::vector<Action>& learnableActions();
} // namespace omr::remote
