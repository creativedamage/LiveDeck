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
    for (auto& t : s.tracks)
    {
        t.outputs.clear();
        t.outStereo = false;
        if (t.input < 0) continue;
        t.outputs.push_back (t.input);   // same channel number, same width
        t.outStereo = t.stereo;
    }
}

std::vector<int> tracksWithDeadOutputs (const PatchState& s)
{
    std::vector<int> rows;
    for (int i = 0; i < (int) s.tracks.size(); ++i)
    {
        const auto& t = s.tracks[(size_t) i];
        bool dead = false;
        for (int o : t.outputs)
        {
            if (! (o >= 0 && o < s.numOutputs && (s.outputActive.empty() || (o < (int) s.outputActive.size() && s.outputActive[(size_t) o])))) dead = true;
            if (t.outStereo && ! (o + 1 < s.numOutputs && (s.outputActive.empty() || (o + 1 < (int) s.outputActive.size() && s.outputActive[(size_t) o + 1])))) dead = true;
        }
        if (dead) rows.push_back (i);
    }
    return rows;
}

std::vector<int> tracksWithDeadInputs (const PatchState& s)
{
    std::vector<int> rows;
    for (int i = 0; i < (int) s.tracks.size(); ++i)
    {
        const auto& t = s.tracks[(size_t) i];
        if (t.input < 0) continue;
        bool ok = isInputActive (s, t.input);
        if (t.stereo) ok = ok && isInputActive (s, t.input + 1);
        if (! ok) rows.push_back (i);
    }
    return rows;
}

static bool locate (const std::vector<DeviceLayout>& devs, bool inputs, int global, int total, int& device, int& local)
{
    if (global < 0 || global >= total) return false;
    if (devs.empty()) { device = 0; local = global; return true; }
    int start = 0;
    for (int d = 0; d < (int) devs.size() && d < kMaxDevices; ++d)
    {
        const int n = inputs ? devs[(size_t) d].numInputs : devs[(size_t) d].numOutputs;
        if (global < start + n) { device = d; local = global - start; return true; }
        start += n;
    }
    return false;
}

bool locateInput (const PatchState& s, int global, int& device, int& local)  { return locate (s.devices, true, global, s.numInputs, device, local); }
bool locateOutput (const PatchState& s, int global, int& device, int& local) { return locate (s.devices, false, global, s.numOutputs, device, local); }

bool isInputActive (const PatchState& s, int ch)
{
    if (ch < 0 || ch >= s.numInputs) return false;
    return s.inputActive.empty() || (ch < (int) s.inputActive.size() && s.inputActive[(size_t) ch]);
}

bool isOutputActive (const PatchState& s, int ch)
{
    if (ch < 0 || ch >= s.numOutputs) return false;
    return s.outputActive.empty() || (ch < (int) s.outputActive.size() && s.outputActive[(size_t) ch]);
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
    juce::ignoreUnused (changed);
    if (newRows != nullptr)
    {
        newRows->clear();
        for (int k = 0; k < (int) moving.size(); ++k) newRows->push_back (insertAt + k);
    }
    return juce::Result::ok();
}

juce::Result setDeviceChannels (PatchState& s, const juce::StringArray& ins, const juce::StringArray& outs,
                                const std::vector<bool>& inActive, const std::vector<bool>& outActive)
{
    s.inputNames = ins;
    s.outputNames = outs;
    s.numInputs = ins.size();
    s.numOutputs = outs.size();
    s.inputActive = inActive;
    s.outputActive = outActive;

    int missing = 0;
    for (auto& t : s.tracks)
    {
        if (t.input >= 0 && (! isInputActive (s, t.input) || (t.stereo && ! isInputActive (s, t.input + 1)))) ++missing;
        if (t.autoName && t.input >= 0 && t.input < s.numInputs) t.name = defaultInputName (s, t.input);
    }
    if (s.mirror) applyMirror (s);
    return missing == 0 ? juce::Result::ok()
                        : juce::Result::fail (juce::String (missing) + " track(s) are patched to inputs that are not available right now; "
                                              "their patch is kept and they record silence until the input returns");
}

juce::Result setMirror (PatchState& s, bool on, bool recording)
{
    juce::ignoreUnused (recording);   // outputs only: safe to change while recording
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
    t.input = input;
    t.stereo = stereo && input >= 0;
    if (t.autoName && input >= 0) t.name = defaultInputName (s, input);
    if (s.mirror) applyMirror (s);   // in mirror mode the output follows the new input
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

int firstUnusedInput (const PatchState& s, bool stereo)
{
    std::vector<bool> used ((size_t) juce::jmax (0, s.numInputs), false);
    for (const auto& t : s.tracks)
        for (int k = 0; k < (t.stereo ? 2 : 1); ++k)
            if (t.input + k >= 0 && t.input + k < s.numInputs) used[(size_t) (t.input + k)] = true;
    for (int ch = 0; ch < s.numInputs; ch += (stereo ? 2 : 1))
    {
        if (stereo && ch + 1 >= s.numInputs) break;
        const bool free = ! used[(size_t) ch] && isInputActive (s, ch)
                          && (! stereo || (! used[(size_t) ch + 1] && isInputActive (s, ch + 1)));
        if (free) return ch;
    }
    return -1;
}

static TrackState makeTrack (const PatchState& s, int input, bool stereo)
{
    TrackState t;
    t.input = input;
    t.stereo = stereo && input >= 0;
    if (input >= 0) { t.name = defaultInputName (s, input); t.autoName = true; }
    else { t.name = "Track " + juce::String ((int) s.tracks.size() + 1); t.autoName = false; }
    return t;
}

juce::Result addTrack (PatchState& s, bool recording)
{
    if (recording) return juce::Result::fail ("Tracks cannot be added while recording");
    if ((int) s.tracks.size() >= kMaxTracks) return juce::Result::fail ("Track limit reached");
    s.tracks.push_back (makeTrack (s, firstUnusedInput (s, false), false));
    ensureTrackIds (s);
    if (s.mirror) applyMirror (s);
    return juce::Result::ok();
}

juce::Result addTracks (PatchState& s, int count, int firstInput, bool stereo, bool recording)
{
    if (recording) return juce::Result::fail ("Tracks cannot be added while recording");
    if (count < 1) return juce::Result::fail ("Nothing to add");
    if ((int) s.tracks.size() + count > kMaxTracks) return juce::Result::fail ("That would exceed " + juce::String (kMaxTracks) + " tracks");
    int ch = juce::jmax (0, firstInput);
    for (int n = 0; n < count; ++n)
    {
        // next active channel (pair) from ch; tracks past the device's channels are added unpatched
        while (ch < s.numInputs && ! (isInputActive (s, ch) && (! stereo || isInputActive (s, ch + 1)))) ++ch;
        const bool ok = ch < s.numInputs && (! stereo || ch + 1 < s.numInputs);
        s.tracks.push_back (makeTrack (s, ok ? ch : -1, stereo));
        ch += stereo ? 2 : 1;
    }
    ensureTrackIds (s);
    if (s.mirror) applyMirror (s);
    return juce::Result::ok();
}

juce::Result addNamedTrack (PatchState& s, const juce::String& name, bool stereo, bool recording, int* newId)
{
    if (recording) return juce::Result::fail ("Tracks cannot be added while recording");
    if ((int) s.tracks.size() >= kMaxTracks) return juce::Result::fail ("Track limit reached");
    auto t = makeTrack (s, firstUnusedInput (s, stereo), stereo);
    t.name = name.trim().isNotEmpty() ? name.trim().substring (0, 64) : t.name;
    t.autoName = false;
    t.armed = false;
    s.tracks.push_back (t);
    ensureTrackIds (s);
    if (s.mirror) applyMirror (s);
    if (newId != nullptr) *newId = s.tracks.back().id;
    return juce::Result::ok();
}

juce::Result removeLastTrack (PatchState& s, bool recording)
{
    if (recording) return juce::Result::fail ("Tracks cannot be removed while recording");
    if (s.tracks.empty()) return juce::Result::ok();
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
    snap->monitorGain = juce::Decibels::decibelsToGain (s.monitorDb);

    int d = 0, l = 0;
    if (locateOutput (s, s.monitorLeft, d, l)) { snap->monitorDev = d; snap->monitorLeft = l; }
    if (locateOutput (s, s.monitorRight, d, l))
    {
        if (snap->monitorLeft < 0) { snap->monitorDev = d; snap->monitorRight = l; }
        else if (d == snap->monitorDev) snap->monitorRight = l;
    }

    for (int i = 0; i < snap->numTracks; ++i)
    {
        const auto& t = s.tracks[(size_t) i];
        auto& r = snap->tracks[(size_t) i];
        r.armed = t.armed ? 1 : 0;
        r.mute = t.mute ? 1 : 0;
        r.solo = t.solo ? 1 : 0;
        r.gain = juce::Decibels::decibelsToGain (t.trimDb);
        snap->anySolo |= t.solo;

        r.input = r.input2 = -1;
        if (locateInput (s, t.input, d, l))
        {
            r.inDev = (uint8_t) d;
            r.input = (int16_t) l;
            int d2 = 0, l2 = 0;
            if (t.stereo && locateInput (s, t.input + 1, d2, l2) && d2 == d) r.input2 = (int16_t) l2;  // pairs never span devices
        }

        r.numOuts = 0;
        r.outDev = r.inDev;
        bool first = true;
        for (int o : t.outputs)
        {
            if (r.numOuts >= kMaxDirectOuts || ! locateOutput (s, o, d, l)) continue;
            if (first) { r.outDev = (uint8_t) d; first = false; }
            else if (d != r.outDev) continue;
            r.outs[(size_t) r.numOuts++] = (int16_t) l;
        }
        if (r.numOuts > 0 && r.outDev != r.inDev) ++snap->crossDeviceOuts;
        r.outStereo = 0;
        if (t.outStereo && r.numOuts > 0)
        {
            int d2 = 0, l2 = 0;
            r.outStereo = (locateOutput (s, t.outputs.front() + 1, d2, l2) && d2 == r.outDev) ? 1 : 0;
        }
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
