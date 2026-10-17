#include "Player.h"
#include "Session.h"

namespace omr
{
static constexpr int kChunk = 32768;               // frames per disk read
static constexpr double kRingSeconds = 6.0;

std::map<int, std::map<int, juce::File>> scanTakes (const juce::File& root)
{
    std::map<int, std::map<int, juce::File>> takes;
    const auto dir = SessionPaths::audioDir (root);
    if (! dir.isDirectory()) return takes;

    for (const auto& e : juce::RangedDirectoryIterator (dir, false, "T*.wav", juce::File::findFiles))
    {
        const auto name = e.getFile().getFileNameWithoutExtension();      // T07_Snare_Top_003
        if (name.endsWith ("_B")) continue;                                 // secondary copies
        const auto takeStr = name.fromLastOccurrenceOf ("_", false, false);
        const auto trackStr = name.substring (1).upToFirstOccurrenceOf ("_", false, false);
        if (takeStr.length() != 3 || ! takeStr.containsOnly ("0123456789") || ! trackStr.containsOnly ("0123456789") || trackStr.isEmpty())
            continue;
        const int track = trackStr.getIntValue() - 1;
        if (track >= 0 && track < kMaxTracks)
            takes[takeStr.getIntValue()][track] = e.getFile();
    }
    return takes;
}

Player::Player (EventLog& l) : juce::Thread ("omr-playback"), log (l)
{
    formats.registerBasicFormats(); // WAV incl. BWF + RF64
}

Player::~Player() { halt(); }

std::map<int, Clip> resolveTakeClips (const juce::File& root, int take, const PatchState& patch)
{
    std::map<int, Clip> out;
    const auto manifest = SessionPaths::takeManifest (root, take);
    if (manifest.existsAsFile())
    {
        const auto v = juce::JSON::parse (manifest);
        if (auto* tracks = v.getProperty ("tracks", {}).getArray())
        {
            for (const auto& e : *tracks)
            {
                const int row = rowForTrackId (patch, (int) e.getProperty ("id", 0));
                const auto f = SessionPaths::audioDir (root).getChildFile (e.getProperty ("file", {}).toString());
                const auto off = (juce::int64) e.getProperty ("offset", 0);
                const auto source = e.getProperty ("source", {}).toString();
                if (row >= 0 && f.existsAsFile())
                    out[row] = { f, (uint64_t) juce::jmax<juce::int64> (0, off), source.isNotEmpty() ? source : f.getFileNameWithoutExtension() };
            }
            return out;
        }
    }
    const auto takes = scanTakes (root);
    if (auto it = takes.find (take); it != takes.end())
        for (const auto& [index, file] : it->second)
            if (index < (int) patch.tracks.size()) out[index] = { file, 0, file.getFileNameWithoutExtension() };
    return out;
}

std::map<int, juce::File> resolveTakeFiles (const juce::File& root, int take, const PatchState& patch)
{
    std::map<int, juce::File> out;
    for (const auto& [row, c] : resolveTakeClips (root, take, patch)) out[row] = c.file;
    return out;
}

bool writeManifestIfMissing (const juce::File& root, int take, const PatchState& patch)
{
    const auto manifest = SessionPaths::takeManifest (root, take);
    if (manifest.existsAsFile()) return false;
    const auto takes = scanTakes (root);
    const auto it = takes.find (take);
    if (it == takes.end()) return false;

    juce::Array<juce::var> entries;
    for (const auto& [index, file] : it->second)
    {
        if (index >= (int) patch.tracks.size()) continue;
        juce::DynamicObject::Ptr e = new juce::DynamicObject();
        e->setProperty ("id", patch.tracks[(size_t) index].id);
        e->setProperty ("row", index);
        e->setProperty ("name", patch.tracks[(size_t) index].name);
        e->setProperty ("file", file.getFileName());
        entries.add (juce::var (e.get()));
    }
    juce::DynamicObject::Ptr m = new juce::DynamicObject();
    m->setProperty ("take", take);
    m->setProperty ("tracks", entries);
    m->setProperty ("migrated", true);
    return manifest.replaceWithText (juce::JSON::toString (juce::var (m.get())), false, false, "\n");
}

static juce::Array<juce::var> manifestEntries (const juce::File& root, int take)
{
    const auto manifest = SessionPaths::takeManifest (root, take);
    if (manifest.existsAsFile())
    {
        const auto parsed = juce::JSON::parse (manifest);    // keep it alive while copying the array
        const auto tracks = parsed.getProperty ("tracks", {});
        if (auto* existing = tracks.getArray()) return *existing;
    }
    return {};
}

static bool writeManifestEntries (const juce::File& root, int take, const juce::Array<juce::var>& entries)
{
    juce::DynamicObject::Ptr m = new juce::DynamicObject();
    m->setProperty ("take", take);
    m->setProperty ("tracks", entries);
    SessionPaths::audioDir (root).createDirectory();
    return SessionPaths::takeManifest (root, take).replaceWithText (juce::JSON::toString (juce::var (m.get())), false, false, "\n");
}

static juce::var entryVar (const ManifestEntry& a)
{
    juce::DynamicObject::Ptr e = new juce::DynamicObject();
    e->setProperty ("id", a.id);
    e->setProperty ("row", a.row);
    e->setProperty ("name", a.name);
    e->setProperty ("channels", a.channels);
    e->setProperty ("file", a.file.getFileName());
    if (a.offset > 0) e->setProperty ("offset", (juce::int64) a.offset);
    if (a.source.isNotEmpty()) e->setProperty ("source", a.source);
    e->setProperty ("imported", true);
    return juce::var (e.get());
}

static int indexOfId (const juce::Array<juce::var>& entries, int id)
{
    for (int i = 0; i < entries.size(); ++i)
        if ((int) entries[i].getProperty ("id", 0) == id) return i;
    return -1;
}

bool appendToManifest (const juce::File& root, int take, const std::vector<ManifestEntry>& add)
{
    auto entries = manifestEntries (root, take);
    for (const auto& a : add) entries.add (entryVar (a));
    return writeManifestEntries (root, take, entries);
}

juce::String readManifestText (const juce::File& root, int take)
{
    const auto f = SessionPaths::takeManifest (root, take);
    return f.existsAsFile() ? f.loadFileAsString() : juce::String();
}

bool restoreManifestText (const juce::File& root, int take, const juce::String& text)
{
    const auto f = SessionPaths::takeManifest (root, take);
    if (text.isEmpty()) return ! f.exists() || f.deleteFile();
    return f.replaceWithText (text, false, false, "\n");
}

bool setClip (const juce::File& root, int take, const ManifestEntry& e)
{
    auto entries = manifestEntries (root, take);
    if (const int i = indexOfId (entries, e.id); i >= 0) entries.remove (i);
    entries.add (entryVar (e));
    return writeManifestEntries (root, take, entries);
}

bool moveClip (const juce::File& root, int take, int fromId, int toId, uint64_t newOffset, bool swap)
{
    auto entries = manifestEntries (root, take);
    const int from = indexOfId (entries, fromId);
    if (from < 0) return false;
    auto set = [] (const juce::var& v, int id, uint64_t off)
    {
        if (auto* o = v.getDynamicObject())
        {
            o->setProperty ("id", id);
            if (off > 0) o->setProperty ("offset", (juce::int64) off); else o->removeProperty ("offset");
        }
    };
    const juce::var moving = entries[from];   // shares the object: edits below change the array entry
    if (fromId != toId)
    {
        if (const int to = indexOfId (entries, toId); to >= 0)
        {
            if (swap) set (entries[to], fromId, (uint64_t) juce::jmax<juce::int64> (0, (juce::int64) entries[to].getProperty ("offset", 0)));
            else entries.remove (to);
        }
    }
    set (moving, toId, newOffset);
    return writeManifestEntries (root, take, entries);
}

bool removeClip (const juce::File& root, int take, int trackId)
{
    auto entries = manifestEntries (root, take);
    const int i = indexOfId (entries, trackId);
    if (i < 0) return false;
    entries.remove (i);
    return writeManifestEntries (root, take, entries);
}

uint64_t takeLength (const std::map<int, Clip>& clips)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    uint64_t len = 0;
    for (const auto& [index, c] : clips)
        if (std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (c.file)); r != nullptr)
            len = std::max (len, c.offset + (uint64_t) r->lengthInSamples);
    return len;
}

uint64_t takeLength (const std::map<int, juce::File>& files)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    uint64_t len = 0;
    for (const auto& [index, file] : files)
        if (std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (file)); r != nullptr)
            len = std::max (len, (uint64_t) r->lengthInSamples);
    return len;
}

juce::Result Player::load (const juce::File& root, int take, double sr, uint64_t startFrame)
{
    const auto takes = scanTakes (root);
    const auto it = takes.find (take);
    if (it == takes.end()) return juce::Result::fail ("Take " + juce::String (take) + " not found in " + root.getFullPathName());
    return load (it->second, take, sr, startFrame);
}

juce::Result Player::load (const std::map<int, juce::File>& filesByRow, int take, double sr, uint64_t startFrame)
{
    std::map<int, Clip> clips;
    for (const auto& [row, f] : filesByRow) clips[row] = { f, 0, f.getFileNameWithoutExtension() };
    return load (clips, take, sr, startFrame);
}

juce::Result Player::load (const std::map<int, Clip>& clipsByRow, int take, double sr, uint64_t startFrame)
{
    halt();
    for (auto& t : tracks) t.reset();
    list.clear();
    lengthFrames = 0;
    takeNumber = take;
    underruns = 0;
    underrunLogged = false;

    if (clipsByRow.empty()) return juce::Result::fail ("Take " + juce::String (take) + " has no audio for the current tracks");

    for (const auto& [trackIndex, clip] : clipsByRow)
    {
        const auto& file = clip.file;
        if (trackIndex < 0 || trackIndex >= kMaxTracks) continue;
        auto t = std::make_unique<TrackIn>();
        t->track = trackIndex;
        t->reader.reset (formats.createReaderFor (file));
        if (t->reader == nullptr) return juce::Result::fail ("Cannot read " + file.getFileName());
        if (std::abs (t->reader->sampleRate - sr) > 0.5)
            return juce::Result::fail (file.getFileName() + " is " + juce::String (t->reader->sampleRate / 1000.0, 1)
                                       + " kHz but the device runs at " + juce::String (sr / 1000.0, 1) + " kHz. Change the device rate to play this take.");
        t->channels = t->reader->numChannels >= 2 ? 2 : 1;
        t->offset = clip.offset;
        t->length = clip.offset + (uint64_t) t->reader->lengthInSamples;
        t->readPos = std::min (startFrame, t->length);
        t->ring = std::make_unique<SpscRing<float>> ((size_t) (sr * kRingSeconds) * (size_t) t->channels);
        t->chunk.setSize (2, kChunk);
        t->interleaved.resize ((size_t) kChunk * 2);
        lengthFrames = std::max (lengthFrames, t->length);
        list.push_back (t.get());
        tracks[(size_t) trackIndex] = std::move (t);
    }

    for (auto& p : pos) p = std::min (startFrame, lengthFrames);

    // Pre-roll: fill every ring before the audio thread sees us.
    for (auto* t : list)
        for (int i = 0; i < 8 && fillOnce (*t); ++i) {}

    log.post (Level::info, "Loaded take " + juce::String (take) + " for playback: " + juce::String ((int) list.size()) + " tracks, "
                           + juce::String ((double) lengthFrames / sr, 1) + " s");
    return juce::Result::ok();
}

bool Player::fillOnce (TrackIn& t)
{
    const size_t space = t.ring->capacity() - t.ring->fill();
    if (space < (size_t) kChunk * (size_t) t.channels) return false;

    int n = kChunk;
    if (t.readPos < t.length) n = (int) std::min<uint64_t> ((uint64_t) kChunk, t.length - t.readPos);
    else return false; // end of file: the audio side pads with silence

    if (t.readPos < t.offset)
    {
        // Before the clip starts: silence (a dropped or moved file that starts later in the take).
        n = (int) std::min<uint64_t> ((uint64_t) n, t.offset - t.readPos);
        std::fill (t.interleaved.begin(), t.interleaved.begin() + (std::ptrdiff_t) n * t.channels, 0.0f);
        t.ring->write (t.interleaved.data(), (size_t) n * (size_t) t.channels);
        t.readPos += (uint64_t) n;
        return true;
    }
    const auto filePos = (juce::int64) (t.readPos - t.offset);
    if (t.channels == 1)
    {
        t.reader->read (&t.chunk, 0, n, filePos, true, false);
        t.ring->write (t.chunk.getReadPointer (0), (size_t) n);
    }
    else
    {
        t.reader->read (&t.chunk, 0, n, filePos, true, true);
        const float* l = t.chunk.getReadPointer (0);
        const float* r = t.chunk.getReadPointer (1);
        for (int i = 0; i < n; ++i) { t.interleaved[(size_t) (2 * i)] = l[i]; t.interleaved[(size_t) (2 * i + 1)] = r[i]; }
        t.ring->write (t.interleaved.data(), (size_t) n * 2);
    }
    t.readPos += (uint64_t) n;
    return true;
}

void Player::start()
{
    startThread (juce::Thread::Priority::high);
}

void Player::halt()
{
    stopThread (2000);
}

void Player::run()
{
    while (! threadShouldExit())
    {
        bool didWork = false;
        for (auto* t : list) didWork |= fillOnce (*t);
        if (! didWork) wait (10);
    }
}

int Player::readTrack (int track, float* dstL, float* dstR, int n, uint64_t posForEvents, int device) noexcept
{
    auto* t = tracks[(size_t) track].get();
    const size_t ch = (size_t) t->channels;
    float tmp[512];

    while (t->owed > 0) // catch up after an underrun so this track stays sample-aligned with the others
    {
        const size_t r = t->ring->read (tmp, (size_t) std::min<uint64_t> (t->owed, 256) * ch) / ch;
        if (r == 0) break;
        t->owed -= r;
    }

    const uint64_t p = pos[(size_t) device].load (std::memory_order_relaxed);
    // Frames past this track's end are silence by design, not an underrun.
    const int wanted = p >= t->length ? 0 : (int) std::min<uint64_t> ((uint64_t) n, t->length - p);
    int got = 0;
    while (got < wanted)
    {
        const size_t k = (size_t) std::min (256, wanted - got);
        const int r = (int) (t->ring->read (tmp, k * ch) / ch);
        if (r == 0) break;
        if (ch == 1) std::copy (tmp, tmp + r, dstL + got);
        else for (int i = 0; i < r; ++i) { dstL[got + i] = tmp[2 * i]; dstR[got + i] = tmp[2 * i + 1]; }
        got += r;
    }
    std::fill (dstL + got, dstL + n, 0.0f);
    if (ch == 2) std::fill (dstR + got, dstR + n, 0.0f);

    if (got < wanted)
    {
        underruns.fetch_add ((uint64_t) (wanted - got), std::memory_order_relaxed);
        t->owed += (uint64_t) (wanted - got);
        if (! underrunLogged.exchange (true))
            log.pushRealtime ({ RtEvent::playUnderrun, 0, (uint16_t) track, posForEvents, (uint32_t) (wanted - got) }, device);
    }
    return (int) ch;
}

void Player::advance (int device, int n) noexcept
{
    auto& p = pos[(size_t) device];
    p.store (p.load (std::memory_order_relaxed) + (uint64_t) n, std::memory_order_relaxed);
}
} // namespace omr
