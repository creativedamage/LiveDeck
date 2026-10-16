#pragma once
// Waveform overview for one mono file: peak magnitude per 512 frames, plus two coarser
// mip levels (x16, x256) so a 3-hour track draws as fast as a 3-second one.
//
// Written by ONE non-real-time thread (the disk writer while recording, or the cache
// builder), read by the UI. A short mutex is fine here: neither side is the audio thread.

#include <juce_audio_formats/juce_audio_formats.h>
#include <array>
#include <mutex>

namespace omr
{
class PeakStore
{
public:
    static constexpr int kFramesPerPeak = 512;
    static constexpr int kRatio = 16;
    static constexpr int kLevels = 3;
    static constexpr int kMaxChannels = 2;     // mono or stereo; stereo keeps separate L/R peaks

    void append (const float* x, size_t frames, int channels = 1);   // writer; interleaved when channels > 1
    void finish();                               // writer: flush the last partial bucket
    void clear();

    uint64_t frames() const;                     // frames appended so far
    int numChannels() const;
    bool isComplete() const { return complete.load(); }

    // Fill `out[0..width)` with peak magnitude 0..1 for pixels starting at `startFrame`,
    // `framesPerPixel` frames each. Pixels past the data are set to -1.
    // channel 0 = left/mono, 1 = right, -1 = the louder of both.
    void render (double startFrame, double framesPerPixel, int width, float* out, int channel = -1) const;

    bool save (const juce::File& f) const;      // "OMRPK2" (per channel); loads the older mono "OMRPK1" too
    bool load (const juce::File& f);

private:
    struct Chan
    {
        std::vector<uint8_t> lv[kLevels];
        float acc = 0.0f;
        uint8_t upAcc[kLevels] {};
        int upN[kLevels] {};
    };
    static void pushLevel (Chan& c, int level, uint8_t v);
    void renderChannel (const Chan& c, double startFrame, double fpp, int width, float* out) const;

    mutable std::mutex lock;
    std::array<Chan, kMaxChannels> chans;
    int numCh = 1;
    int accN = 0;
    uint64_t total = 0;
    std::atomic<bool> complete { false };
};

// Background builder/cache: one PeakStore per WAV file, loaded from Audio/.peaks/<name>.pk
// when present, otherwise computed from the WAV (progressively, so it draws as it loads).
class WaveformCache : private juce::Thread
{
public:
    WaveformCache();
    ~WaveformCache() override;

    std::shared_ptr<PeakStore> get (const juce::File& wav);              // never null
    void put (const juce::File& wav, std::shared_ptr<PeakStore> store);  // e.g. the live store after recording
    void clear();

    static juce::File cacheFileFor (const juce::File& wav);

private:
    void run() override;

    std::mutex lock;
    std::map<juce::String, std::shared_ptr<PeakStore>> stores;
    std::vector<std::pair<juce::File, std::shared_ptr<PeakStore>>> queue;
    juce::AudioFormatManager formats;
};
} // namespace omr
