#pragma once
// Mono Broadcast WAV (EBU Tech 3285) writer: 24-bit PCM or 32-bit float.
// Reserves a JUNK chunk so the file upgrades itself in place to RF64 (EBU Tech 3306)
// when it passes 4 GB. Disk-thread only.

#include "PlatformFile.h"
#include <string>
#include <vector>

namespace omr
{
struct BwfInfo
{
    uint32_t sampleRate = 48000;
    uint16_t bitsPerSample = 24;   // 24 (PCM) or 32 (float)
    bool isFloat = false;
    uint16_t numChannels = 1;      // 1 = mono, 2 = interleaved stereo
    uint64_t timeReference = 0;    // samples since midnight at the first sample
    std::string description, originator = "LiveDeck", originatorRef, date /*YYYY-MM-DD*/, time /*HH:MM:SS*/;
    std::string trackName, sceneName, takeName; // iXML
};

class BwfWriter
{
public:
    static constexpr uint64_t kAllocStep = 32ull << 20; // preallocate ahead in 32 MiB steps

    bool open (const juce::File& file, const BwfInfo& info);
    bool appendFrames (const float* samples, size_t numFrames);   // interleaved when stereo
    bool updateHeader();     // rewrite RIFF/data sizes (RF64 when needed)
    bool sync();
    bool close();            // pad byte, final header, full sync, trim preallocation

    uint64_t dataBytes() const noexcept { return numDataBytes; }
    uint64_t framesWritten() const noexcept { return numDataBytes / bytesPerFrame(); }
    uint64_t dataOffset() const noexcept { return dataStart; }
    bool isRF64() const noexcept { return rf64; }
    const juce::File& getFile() const noexcept { return path; }
    juce::String lastError() const { return file.lastErrorString(); }

    static std::vector<uint8_t> buildHeader (const BwfInfo& info);
    static constexpr size_t kJunkOffset = 12; // JUNK/ds64 chunk header starts right after "RIFF....WAVE"

private:
    uint32_t bytesPerFrame() const noexcept { return (info.bitsPerSample / 8u) * info.numChannels; }

    PlatformFile file;
    juce::File path;
    BwfInfo info;
    uint64_t dataStart = 0, numDataBytes = 0, allocated = 0;
    bool rf64 = false;
    std::vector<uint8_t> scratch;
};

void writeLE16 (uint8_t* p, uint16_t v) noexcept;
void writeLE32 (uint8_t* p, uint32_t v) noexcept;
void writeLE64 (uint8_t* p, uint64_t v) noexcept;
uint32_t readLE32 (const uint8_t* p) noexcept;
uint16_t readLE16 (const uint8_t* p) noexcept;
} // namespace omr
