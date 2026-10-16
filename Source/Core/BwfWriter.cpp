#include "BwfWriter.h"
#include <cmath>
#include <cstring>

namespace omr
{
void writeLE16 (uint8_t* p, uint16_t v) noexcept { p[0] = uint8_t (v); p[1] = uint8_t (v >> 8); }
void writeLE32 (uint8_t* p, uint32_t v) noexcept { for (int k = 0; k < 4; ++k) p[k] = uint8_t (v >> (8 * k)); }
void writeLE64 (uint8_t* p, uint64_t v) noexcept { for (int k = 0; k < 8; ++k) p[k] = uint8_t (v >> (8 * k)); }
uint32_t readLE32 (const uint8_t* p) noexcept { return uint32_t (p[0]) | uint32_t (p[1]) << 8 | uint32_t (p[2]) << 16 | uint32_t (p[3]) << 24; }
uint16_t readLE16 (const uint8_t* p) noexcept { return uint16_t (p[0] | p[1] << 8); }

static std::string xmlEscape (const std::string& s)
{
    return juce::String (s).replace ("&", "&amp;").replace ("<", "&lt;").replace (">", "&gt;").toStdString();
}

std::vector<uint8_t> BwfWriter::buildHeader (const BwfInfo& info)
{
    std::vector<uint8_t> h;
    h.reserve (2048);
    auto tag   = [&] (const char* s) { h.insert (h.end(), s, s + 4); };
    auto u16   = [&] (uint16_t v) { for (int k = 0; k < 2; ++k) h.push_back (uint8_t (v >> (8 * k))); };
    auto u32   = [&] (uint32_t v) { for (int k = 0; k < 4; ++k) h.push_back (uint8_t (v >> (8 * k))); };
    auto u64   = [&] (uint64_t v) { for (int k = 0; k < 8; ++k) h.push_back (uint8_t (v >> (8 * k))); };
    auto zeros = [&] (size_t n) { h.insert (h.end(), n, uint8_t (0)); };
    auto fixed = [&] (const std::string& s, size_t n) { for (size_t k = 0; k < n; ++k) h.push_back (k < s.size() ? uint8_t (s[k]) : 0); };

    const uint16_t bps = uint16_t (info.bitsPerSample / 8);

    tag ("RIFF"); u32 (0); tag ("WAVE");
    tag ("JUNK"); u32 (28); zeros (28);                  // becomes ds64 past 4 GB

    tag ("bext"); u32 (602);
    fixed (info.description, 256); fixed (info.originator, 32); fixed (info.originatorRef, 32);
    fixed (info.date, 10); fixed (info.time, 8);
    u64 (info.timeReference);                            // TimeReferenceLow + High
    u16 (1);                                             // version
    zeros (64);                                          // UMID
    zeros (10);                                          // loudness fields (v2), unused
    zeros (180);                                         // reserved

    tag ("fmt "); u32 (16);
    const uint16_t ch = info.numChannels;
    u16 (info.isFloat ? 3 : 1); u16 (ch); u32 (info.sampleRate);
    u32 (info.sampleRate * bps * ch); u16 (uint16_t (bps * ch)); u16 (info.bitsPerSample);

    if (info.isFloat) { tag ("fact"); u32 (4); u32 (0); } // sample count; patched on close when < 4 GB

    // iXML: track name for Pro Tools / Reaper / Nuendo import.
    std::string ixml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><BWFXML><IXML_VERSION>1.61</IXML_VERSION>"
                       "<SCENE>" + xmlEscape (info.sceneName) + "</SCENE><TAKE>" + xmlEscape (info.takeName) + "</TAKE>"
                       "<TRACK_LIST><TRACK_COUNT>" + std::to_string (ch) + "</TRACK_COUNT>";
    for (int k = 1; k <= ch; ++k)
        ixml += "<TRACK><CHANNEL_INDEX>" + std::to_string (k) + "</CHANNEL_INDEX><INTERLEAVE_INDEX>" + std::to_string (k) + "</INTERLEAVE_INDEX><NAME>"
              + xmlEscape (info.trackName + (ch == 2 ? (k == 1 ? " L" : " R") : "")) + "</NAME></TRACK>";
    ixml += "</TRACK_LIST></BWFXML>";
    if (ixml.size() & 1) ixml.push_back (' ');
    tag ("iXML"); u32 (uint32_t (ixml.size())); h.insert (h.end(), ixml.begin(), ixml.end());

    tag ("data"); u32 (0);
    return h;
}

bool BwfWriter::open (const juce::File& f, const BwfInfo& i)
{
    info = i;
    path = f;
    numDataBytes = 0;
    rf64 = false;
    if (! file.create (f)) return false;

    const auto h = buildHeader (info);
    dataStart = h.size();
    allocated = 0;
    file.preallocateKeepSize (0, kAllocStep); // best effort
    allocated = kAllocStep;
    return file.writeAt (h.data(), h.size(), 0);
}

bool BwfWriter::appendFrames (const float* x, size_t n)
{
    const size_t bpf = bytesPerFrame();
    const size_t samples = n * info.numChannels;
    scratch.resize (n * bpf);

    if (info.isFloat)
    {
        std::memcpy (scratch.data(), x, samples * sizeof (float)); // little-endian hosts only (x86-64, arm64)
    }
    else
    {
        uint8_t* d = scratch.data();
        for (size_t i = 0; i < samples; ++i)
        {
            const float c = x[i] < -1.0f ? -1.0f : (x[i] > 1.0f ? 1.0f : x[i]);
            const int32_t s = (int32_t) std::lrint (c * 8388607.0f);
            d[0] = uint8_t (s); d[1] = uint8_t (s >> 8); d[2] = uint8_t (s >> 16);
            d += 3;
        }
    }

    const uint64_t end = dataStart + numDataBytes + scratch.size();
    if (end + kAllocStep / 2 > allocated)
    {
        file.preallocateKeepSize (allocated, kAllocStep); // best effort; failure is not fatal
        allocated += kAllocStep;
    }

    if (! file.writeAt (scratch.data(), scratch.size(), dataStart + numDataBytes)) return false;
    numDataBytes += scratch.size();
    return true;
}

bool BwfWriter::updateHeader()
{
    const uint64_t riffSize = dataStart + numDataBytes - 8;
    uint8_t b[8];

    if (! rf64 && riffSize <= 0xFFFFFFFFull)
    {
        writeLE32 (b, uint32_t (riffSize));
        if (! file.writeAt (b, 4, 4)) return false;
        writeLE32 (b, uint32_t (numDataBytes));
        if (! file.writeAt (b, 4, dataStart - 4)) return false;
        if (info.isFloat)
        {
            // fact chunk sits right before iXML; find it by rebuilding the header layout.
            const auto h = buildHeader (info);
            for (size_t p = 12; p + 8 <= h.size();)
            {
                const uint32_t len = readLE32 (&h[p + 4]);
                if (std::memcmp (&h[p], "fact", 4) == 0)
                {
                    writeLE32 (b, uint32_t (framesWritten()));
                    return file.writeAt (b, 4, p + 8);
                }
                p += 8 + len + (len & 1);
            }
        }
        return true;
    }

    // RF64 upgrade (EBU Tech 3306): RIFF->RF64, JUNK->ds64, 32-bit sizes -> 0xFFFFFFFF.
    rf64 = true;
    uint8_t ds64[8 + 28];
    std::memcpy (ds64, "ds64", 4);
    writeLE32 (ds64 + 4, 28);
    writeLE64 (ds64 + 8, riffSize);
    writeLE64 (ds64 + 16, numDataBytes);
    writeLE64 (ds64 + 24, framesWritten());
    writeLE32 (ds64 + 32, 0);

    writeLE32 (b, 0xFFFFFFFFu);
    return file.writeAt ("RF64", 4, 0)
        && file.writeAt (b, 4, 4)
        && file.writeAt (ds64, sizeof (ds64), kJunkOffset)
        && file.writeAt (b, 4, dataStart - 4);
}

bool BwfWriter::sync() { return file.dataSync(); }

bool BwfWriter::close()
{
    if (! file.isOpen()) return true;
    bool ok = true;
    if (numDataBytes & 1)
    {
        const uint8_t z = 0;
        ok &= file.writeAt (&z, 1, dataStart + numDataBytes); // RIFF pad byte (not counted in data size)
    }
    ok &= updateHeader();
    ok &= file.truncate (dataStart + numDataBytes + (numDataBytes & 1)); // release unused preallocation
    ok &= file.fullSync();
    file.close();
    return ok;
}
} // namespace omr
