#include "Recovery.h"
#include "BwfWriter.h"
#include "PlatformFile.h"
#include "Session.h"
#include <cstring>

namespace omr
{
RecoveredFile repairWavFile (const juce::File& wav)
{
    RecoveredFile r;
    r.file = wav;

    PlatformFile f;
    if (! f.openReadWrite (wav)) { r.message = "Cannot open: " + f.lastErrorString(); return r; }

    const uint64_t len = f.size();
    if (len < 44) { r.message = "Too short to be a WAV file"; return r; }

    const size_t probe = (size_t) std::min<uint64_t> (len, 64 * 1024);
    std::vector<uint8_t> h (probe);
    if (! f.readAt (h.data(), probe, 0)) { r.message = "Read failed"; return r; }

    const bool isRF64 = std::memcmp (h.data(), "RF64", 4) == 0;
    if (! isRF64 && std::memcmp (h.data(), "RIFF", 4) != 0) { r.message = "Not a RIFF file"; return r; }
    if (std::memcmp (h.data() + 8, "WAVE", 4) != 0) { r.message = "Not a WAVE file"; return r; }

    uint16_t blockAlign = 0;
    uint64_t dataHeader = 0, factOffset = 0, junkOffset = 0;
    bool isFloat = false;

    for (uint64_t p = 12; p + 8 <= probe;)
    {
        const uint8_t* c = h.data() + p;
        const uint32_t sz = readLE32 (c + 4);
        if (std::memcmp (c, "fmt ", 4) == 0 && p + 8 + 16 <= probe)
        {
            isFloat = readLE16 (c + 8) == 3;
            blockAlign = readLE16 (c + 8 + 12);
        }
        else if (std::memcmp (c, "fact", 4) == 0) factOffset = p + 8;
        else if (std::memcmp (c, "JUNK", 4) == 0 || std::memcmp (c, "ds64", 4) == 0) { if (junkOffset == 0 && sz == 28) junkOffset = p; }
        else if (std::memcmp (c, "data", 4) == 0) { dataHeader = p; break; }
        p += 8 + (uint64_t) sz + (sz & 1);
    }

    if (blockAlign == 0 || dataHeader == 0) { r.message = "Header incomplete (no fmt or data chunk)"; return r; }

    const uint64_t dataStart = dataHeader + 8;
    const uint64_t dataBytes = len > dataStart ? ((len - dataStart) / blockAlign) * blockAlign : 0;
    const uint64_t riffSize = dataStart + dataBytes - 8;
    r.frames = dataBytes / blockAlign;

    uint8_t b[8];
    bool ok = true;
    const uint32_t oldData = readLE32 (h.data() + dataHeader + 4);

    if (! isRF64 && riffSize <= 0xFFFFFFFFull)
    {
        r.repaired = oldData != (uint32_t) dataBytes;
        writeLE32 (b, (uint32_t) riffSize);   ok &= f.writeAt (b, 4, 4);
        writeLE32 (b, (uint32_t) dataBytes);  ok &= f.writeAt (b, 4, dataHeader + 4);
        if (isFloat && factOffset != 0) { writeLE32 (b, (uint32_t) r.frames); ok &= f.writeAt (b, 4, factOffset); }
    }
    else
    {
        if (junkOffset == 0) { r.message = "Over 4 GB but no ds64/JUNK space to upgrade to RF64"; return r; }
        r.repaired = true;
        uint8_t ds64[36];
        std::memcpy (ds64, "ds64", 4);
        writeLE32 (ds64 + 4, 28);
        writeLE64 (ds64 + 8, riffSize);
        writeLE64 (ds64 + 16, dataBytes);
        writeLE64 (ds64 + 24, r.frames);
        writeLE32 (ds64 + 32, 0);
        writeLE32 (b, 0xFFFFFFFFu);
        ok &= f.writeAt ("RF64", 4, 0);
        ok &= f.writeAt (b, 4, 4);
        ok &= f.writeAt (ds64, sizeof (ds64), junkOffset);
        ok &= f.writeAt (b, 4, dataHeader + 4);
    }

    ok &= f.truncate (dataStart + dataBytes);
    ok &= f.fullSync();
    r.ok = ok;
    r.message = ok ? juce::String (r.frames) + " frames" + (r.repaired ? " (header rebuilt)" : "") : "Write failed: " + f.lastErrorString();
    return r;
}

juce::Array<RecoveredFile> recoverSessionFolder (const juce::File& sessionRoot)
{
    juce::Array<RecoveredFile> out;
    for (const auto& entry : juce::RangedDirectoryIterator (SessionPaths::audioDir (sessionRoot), false, "*.wav", juce::File::findFiles))
        out.add (repairWavFile (entry.getFile()));

    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty ("t", "recovered");
    o->setProperty ("time", juce::Time::getCurrentTime().toISO8601 (true));
    o->setProperty ("files", out.size());
    SessionPaths::journalFile (sessionRoot).appendText (juce::JSON::toString (juce::var (o.get()), true) + "\n");
    return out;
}

bool journalHasUncleanTake (const juce::File& sessionRoot)
{
    const auto j = SessionPaths::journalFile (sessionRoot);
    if (! j.existsAsFile()) return false;
    juce::StringArray lines;
    j.readLines (lines);
    bool open = false;
    for (const auto& l : lines)
    {
        const auto v = juce::JSON::parse (l);
        const auto t = v.getProperty ("t", {}).toString();
        if (t == "start") open = true;
        else if (t == "stop" || t == "recovered") open = false;
    }
    return open;
}
} // namespace omr
