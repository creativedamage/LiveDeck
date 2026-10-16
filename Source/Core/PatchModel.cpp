#include "PatchModel.h"
#include <set>

namespace omr
{
static juce::String defaultInputName (const PatchState& s, int input)
{
    if (input >= 0 && input < s.inputNames.size() && s.inputNames[input].isNotEmpty()) return s.inputNames[input];
    return "In " + juce::String (input + 1);
}

static bool validTrack (const PatchState& s, int t) { return t >= 0 && t < (int) s.tracks.size(); }

void applyMirror (PatchState& s)
{
    const int n = juce::jmin (s.numInputs, kMaxTracks);
    s.tracks.resize ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        auto& t = s.tracks[(size_t) i];
        t.input = i;
        t.stereo = false;
        t.outStereo = false;
        t.outputs.clear();
        if (i < s.numOutputs) t.outputs.push_back (i);
        if (t.autoName || t.name.isEmpty()) { t.name = defaultInputName (s, i); t.autoName = true; }
    }
    ensureTrackIds (s);
}

void ensureTrackIds (PatchState& s)
{
    int maxId = 0;
    for (const auto& t : s.tracks) maxId = juce::jmax (maxId, t.id);
    std::set<int> seen;
    for (auto& t : s.tracks)
    {
        if (t.id <= 0 || seen.count (t.id) > 0) t.id = ++maxId;
        seen.insert (t.id);
    }
}

int rowForTrackId (const PatchState& s, int id)
{
    for (size_t i = 0; i < s.tracks.size(); ++i)
        if (s.tracks[i].id == id) return (int) i;
    return -1;
}

juce::Result deleteTracks (PatchState& s, std::vector<int> rows, bool recording)
{
    if (recording) return juce::Result::fail ("Tracks cannot be deleted while recording");
    std::sort (rows.begin(), rows.end());
    rows.erase (std::unique (rows.begin(), rows.end()), rows.end());
    for (auto it = rows.rbegin(); it != rows.rend(); ++it)
        if (*it >= 0 && *it < (int) s.tracks.size()) s.tracks.erase (s.tracks.begin() + *it);
    if (! rows.empty()) s.mirror = false;
    return juce::Result::ok();
}

juce::Result moveTracks (PatchState& s, std::vector<int> rows, int insertBefore, bool recording, std::vector<int>* newRows)
{
    if (recording) return juce::Result::fail ("Tracks cannot be reordered while recording");
    std::sort (rows.begin(), rows.end());
    rows.erase (std::unique (rows.begin(), rows.end()), rows.end());
    rows.erase (std::remove_if (rows.begin(), rows.end(), [&] (int r) { return r < 0 || r >= (int) s.tracks.size(); }), rows.end());
    if (rows.empty()) return juce::Result::ok();
    insertBefore = juce::jlimit (0, (int) s.tracks.size(), insertBefore);

    std::vector<TrackState> moving, rest;
    int insertAt = insertBefore;
    for (int i = 0; i < (int) s.tracks.size(); ++i)
    {
        if (std::binary_search (rows.begin(), rows.end(), i)) { moving.push_back (s.tracks[(size_t) i]); if (i < insertBefore) --insertAt; }
        else rest.push_back (s.tracks[(size_t) i]);
    }
    rest.insert (rest.begin() + insertAt, moving.begin(), moving.end());
    const bool changed = [&] { for (size_t i = 0; i < rest.size(); ++i) if (rest[i].id != s.tracks[i].id) return true; return false; }();
    s.tracks = std::move (rest);
    if (changed) s.mirror = false;
    if (newRows != nullptr)
    {
        newRows->clear();
        for (int k = 0; k < (int) moving.size(); ++k) newRows->push_back (insertAt + k);
    }
    return juce::Result::ok();
}

juce::Result setDeviceChannels (PatchState& s, const juce::StringArray& ins, const juce::StringArray& outs)
{
    s.inputNames = ins;
    s.outputNames = outs;
    s.numInputs = ins.size();
    s.numOutputs = outs.size();

    if (s.mirror) { applyMirror (s); return juce::Result::ok(); }

    // Manual routing: keep the patch, but flag channels the new device does not have.
    int lost = 0;
    for (auto& t : s.tracks)
    {
        if (t.input >= s.numInputs) { t.input = -1; ++lost; }
        if (t.stereo && t.input + 1 >= s.numInputs) t.stereo = false;
        if (t.outStereo && (t.outputs.empty() || t.outputs.front() + 1 >= s.numOutputs)) t.outStereo = false;
        t.outputs.erase (std::remove_if (t.outputs.begin(), t.outputs.end(), [&] (int o) { return o >= s.numOutputs; }), t.outputs.end());
        if (t.autoName && t.input >= 0) t.name = defaultInputName (s, t.input);
    }
    if (s.monitorLeft >= s.numOutputs) s.monitorLeft = -1;
    if (s.monitorRight >= s.numOutputs) s.monitorRight = -1;
    return lost == 0 ? juce::Result::ok()
                     : juce::Result::fail (juce::String (lost) + " track(s) lost their input: the new device has only "
                                           + juce::String (s.numInputs) + " inputs");
}

juce::Result setMirror (PatchState& s, bool on, bool recording)
{
    if (recording) return juce::Result::fail ("Mirror 1:1 is locked while recording");
    s.mirror = on;
    if (on) applyMirror (s);
    return juce::Result::ok();
}

juce::Result setTrackInput (PatchState& s, int track, int input, bool recording, bool stereo)
{
    if (! validTrack (s, track)) return juce::Result::fail ("No such track");
    auto& t = s.tracks[(size_t) track];
    if (recording && t.armed) return juce::Result::fail ("Input is locked on armed tracks while recording");
    if (input >= s.numInputs) return juce::Result::fail ("No such input");
    if (stereo && input + 1 >= s.numInputs) return juce::Result::fail ("No stereo pair at input " + juce::String (input + 1));
    const bool wasMirror = s.mirror;
    s.mirror = false; // manual edit leaves mirror mode but keeps the mapping
    t.input = input;
    t.stereo = stereo && input >= 0;
    if (t.autoName && input >= 0) t.name = defaultInputName (s, input);
    juce::ignoreUnused (wasMirror); // caller compares s.mirror before/after to show the "mirror off" notice
    return juce::Result::ok();
}

juce::Result setTrackOutput (PatchState& s, int track, int output, bool stereo)
{
    if (! validTrack (s, track)) return juce::Result::fail ("No such track");
    if (output >= s.numOutputs) return juce::Result::fail ("No such output");
    if (stereo && output + 1 >= s.numOutputs) return juce::Result::fail ("No stereo pair at output " + juce::String (output + 1));
    auto& t = s.tracks[(size_t) track];
    t.outStereo = stereo && output >= 0;
    const bool wasMirror = s.mirror;
    s.mirror = false;
    if (t.outputs.empty()) { if (output >= 0) t.outputs.push_back (output); }
    else if (output >= 0) t.outputs[0] = output;
    else t.outputs.erase (t.outputs.begin());
    juce::ignoreUnused (wasMirror); // caller compares s.mirror before/after to show the "mirror off" notice
    return juce::Result::ok();
}

juce::Result addTrack (PatchState& s, bool recording)
{
    if (recording) return juce::Result::fail ("Tracks cannot be added while recording");
    if ((int) s.tracks.size() >= kMaxTracks) return juce::Result::fail ("Track limit reached");
    s.mirror = false;
    TrackState t;
    t.name = "Track " + juce::String ((int) s.tracks.size() + 1);
    t.autoName = false;
    s.tracks.push_back (t);
    ensureTrackIds (s);
    return juce::Result::ok();
}

juce::Result removeLastTrack (PatchState& s, bool recording)
{
    if (recording) return juce::Result::fail ("Tracks cannot be removed while recording");
    if (s.tracks.empty()) return juce::Result::ok();
    s.mirror = false;
    s.tracks.pop_back();
    return juce::Result::ok();
}

juce::Result setArmed (PatchState& s, int track, bool armed, bool recording)
{
    if (! validTrack (s, track)) return juce::Result::fail ("No such track");
    if (recording) return juce::Result::fail ("Arm state is locked while recording");
    s.tracks[(size_t) track].armed = armed;
    return juce::Result::ok();
}

juce::Result renameTrack (PatchState& s, int track, const juce::String& name)
{
    if (! validTrack (s, track)) return juce::Result::fail ("No such track");
    auto& t = s.tracks[(size_t) track];
    t.name = name.trim();
    t.autoName = t.name.isEmpty();
    if (t.autoName && t.input >= 0) t.name = defaultInputName (s, t.input);
    return juce::Result::ok();
}

std::unique_ptr<RoutingSnapshot> compileSnapshot (const PatchState& s, uint32_t generation)
{
    auto snap = std::make_unique<RoutingSnapshot>();
    snap->generation = generation;
    snap->numTracks = juce::jmin ((int) s.tracks.size(), kMaxTracks);
    snap->monitorLeft = s.monitorLeft;
    snap->monitorRight = s.monitorRight;
    snap->monitorGain = juce::Decibels::decibelsToGain (s.monitorDb);

    for (int i = 0; i < snap->numTracks; ++i)
    {
        const auto& t = s.tracks[(size_t) i];
        auto& r = snap->tracks[(size_t) i];
        r.input = (int16_t) t.input;
        r.input2 = (int16_t) (t.stereo && t.input >= 0 ? t.input + 1 : -1);
        r.outStereo = t.outStereo ? 1 : 0;
        r.armed = t.armed ? 1 : 0;
        r.mute = t.mute ? 1 : 0;
        r.solo = t.solo ? 1 : 0;
        r.gain = juce::Decibels::decibelsToGain (t.trimDb);
        r.numOuts = (uint8_t) juce::jmin ((int) t.outputs.size(), kMaxDirectOuts);
        for (int k = 0; k < r.numOuts; ++k) r.outs[(size_t) k] = (int16_t) t.outputs[(size_t) k];
        snap->anySolo |= t.solo;
    }
    return snap;
}

juce::var patchToVar (const PatchState& s)
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty ("mirror", s.mirror);
    o->setProperty ("monitorLeft", s.monitorLeft);
    o->setProperty ("monitorRight", s.monitorRight);
    o->setProperty ("monitorDb", s.monitorDb);
    juce::Array<juce::var> tracks;
    for (const auto& t : s.tracks)
    {
        juce::DynamicObject::Ptr to = new juce::DynamicObject();
        to->setProperty ("id", t.id);
        to->setProperty ("name", t.name);
        to->setProperty ("autoName", t.autoName);
        to->setProperty ("input", t.input);
        juce::Array<juce::var> outs;
        for (int x : t.outputs) outs.add (x);
        to->setProperty ("outputs", outs);
        to->setProperty ("trimDb", t.trimDb);
        to->setProperty ("armed", t.armed);
        to->setProperty ("mute", t.mute);
        to->setProperty ("solo", t.solo);
        to->setProperty ("colour", t.colour);
        to->setProperty ("stereo", t.stereo);
        to->setProperty ("outStereo", t.outStereo);
        tracks.add (juce::var (to.get()));
    }
    o->setProperty ("tracks", tracks);
    return juce::var (o.get());
}

PatchState patchFromVar (const juce::var& v)
{
    PatchState s;
    s.mirror = (bool) v.getProperty ("mirror", true);
    s.monitorLeft = (int) v.getProperty ("monitorLeft", -1);
    s.monitorRight = (int) v.getProperty ("monitorRight", -1);
    s.monitorDb = (float) (double) v.getProperty ("monitorDb", 0.0);
    if (auto* arr = v.getProperty ("tracks", {}).getArray())
    {
        for (const auto& tv : *arr)
        {
            TrackState t;
            t.id = (int) tv.getProperty ("id", 0);
            t.name = tv.getProperty ("name", {}).toString();
            t.autoName = (bool) tv.getProperty ("autoName", true);
            t.input = (int) tv.getProperty ("input", -1);
            if (auto* outs = tv.getProperty ("outputs", {}).getArray())
                for (const auto& o : *outs) t.outputs.push_back ((int) o);
            t.trimDb = (float) (double) tv.getProperty ("trimDb", 0.0);
            t.armed = (bool) tv.getProperty ("armed", true);
            t.mute = (bool) tv.getProperty ("mute", false);
            t.solo = (bool) tv.getProperty ("solo", false);
            t.colour = (int) tv.getProperty ("colour", -1);
            t.stereo = (bool) tv.getProperty ("stereo", false);
            t.outStereo = (bool) tv.getProperty ("outStereo", false);
            s.tracks.push_back (t);
        }
    }
    ensureTrackIds (s);
    return s;
}
} // namespace omr
