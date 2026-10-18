// LiveDeck test runner.  Usage: LiveDeckTests [--big]
//   --big  also runs the >4 GB RF64 upgrade test (writes ~4.3 GB to the temp folder)

#include <juce_audio_formats/juce_audio_formats.h>
#include "Core/BwfWriter.h"
#include "Core/Benchmark.h"
#include "Core/PatchModel.h"
#include "Core/RecordEngine.h"
#include "Core/Recovery.h"
#include "Core/Session.h"
#include "Core/SpscRing.h"
#include "Core/PeakStore.h"
#include "Core/Updater.h"
#include "Core/Remote.h"
#include "Core/AudioImport.h"
#include "Core/Feedback.h"
#include "Core/Player.h"
#include "Core/License.h"
#include "RsaTestVectors.h"

#include <set>
using namespace omr;

static int failures = 0, checks = 0;
#define CHECK(cond) do { ++checks; if (! (cond)) { ++failures; std::printf ("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define SECTION(name) std::printf ("[ %s ]\n", name)

static juce::File tempDir (const juce::String& name)
{
    auto d = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("omr_tests").getChildFile (name);
    d.deleteRecursively();
    d.createDirectory();
    return d;
}

static std::unique_ptr<juce::AudioFormatReader> openWav (const juce::File& f)
{
    juce::WavAudioFormat wav;
    return std::unique_ptr<juce::AudioFormatReader> (wav.createReaderFor (f.createInputStream().release(), true));
}

// Deterministic, never-zero test signal per track.
static float sig (int track, uint64_t frame) { return 0.05f + 0.4f * (float) ((frame * 7 + (uint64_t) track * 131) % 1000) / 1000.0f; }

static void testRing()
{
    SECTION ("SPSC ring wraps and preserves order");
    SpscRing<float> r (100); // rounds to 128
    CHECK (r.capacity() == 128);
    std::vector<float> in (300), out (300);
    for (int i = 0; i < 300; ++i) in[(size_t) i] = (float) i;
    size_t w = 0, rd = 0;
    for (int round = 0; round < 50; ++round)
    {
        w += r.write (in.data(), 77);
        rd += r.read (out.data(), 50);
    }
    CHECK (r.fill() == w - rd);
    CHECK (r.fill() <= r.capacity());
    SpscRing<float> r2 (16);
    float a[20]; for (int i = 0; i < 20; ++i) a[i] = (float) i;
    CHECK (r2.write (a, 20) == 16);         // full: short write, no block
    float b[20] = {};
    CHECK (r2.read (b, 10) == 10 && b[9] == 9.0f);
    CHECK (r2.write (a, 10) == 10);          // wraps
    float c[16] = {};
    CHECK (r2.read (c, 16) == 16 && c[5] == 15.0f && c[6] == 0.0f && c[15] == 9.0f);
}

static void testBwf (bool isFloat)
{
    SECTION (isFloat ? "BWF writer: 32-bit float round trip" : "BWF writer: 24-bit PCM round trip, bext + iXML");
    auto dir = tempDir (isFloat ? "bwf32" : "bwf24");
    auto f = dir.getChildFile ("t.wav");
    BwfInfo info;
    info.sampleRate = 48000; info.bitsPerSample = isFloat ? 32 : 24; info.isFloat = isFloat;
    info.timeReference = 48000ull * 3600 * 19; // 19:00:00
    info.description = "Arena / Kick In / take 1"; info.date = "2026-10-15"; info.time = "19:00:00";
    info.trackName = "Kick In";

    BwfWriter w;
    CHECK (w.open (f, info));
    std::vector<float> x (48001);
    for (size_t i = 0; i < x.size(); ++i) x[i] = std::sin ((float) i * 0.01f) * 0.8f;
    CHECK (w.appendFrames (x.data(), 30000));
    CHECK (w.appendFrames (x.data() + 30000, 18001));
    CHECK (w.close());
    CHECK (f.getSize() == (juce::int64) (w.dataOffset() + w.dataBytes() + (w.dataBytes() & 1)));

    auto r = openWav (f);
    CHECK (r != nullptr);
    if (r == nullptr) return;
    CHECK (r->sampleRate == 48000.0);
    CHECK (r->numChannels == 1);
    CHECK ((int) r->bitsPerSample == (isFloat ? 32 : 24));
    CHECK (r->usesFloatingPointData == isFloat);
    CHECK (r->lengthInSamples == 48001);
    CHECK (r->metadataValues["bwav description"] == "Arena / Kick In / take 1");
    CHECK (r->metadataValues["bwav time reference"] == juce::String ((juce::int64) info.timeReference));
    CHECK (r->metadataValues["bwav origination date"] == "2026-10-15");

    juce::AudioBuffer<float> b (1, 48001);
    r->read (&b, 0, 48001, 0, true, false);
    float maxErr = 0;
    for (int i = 0; i < 48001; ++i) maxErr = std::max (maxErr, std::abs (b.getSample (0, i) - x[(size_t) i]));
    CHECK (maxErr < (isFloat ? 1e-7f : 2.0f / 8388607.0f));
}

static void testRecovery()
{
    SECTION ("Crash recovery rebuilds headers from file length");
    auto root = tempDir ("recover");
    SessionPaths::createTree (root);
    auto f = SessionPaths::audioDir (root).getChildFile ("T01_crash_001.wav");
    {
        BwfWriter w;
        BwfInfo info;
        CHECK (w.open (f, info));
        std::vector<float> x (96000, 0.25f);
        CHECK (w.appendFrames (x.data(), x.size()));
        CHECK (w.sync());
        // no close(): simulates power loss. Header still says 0 bytes of data.
    }
    SessionPaths::journalFile (root).appendText ("{\"t\":\"start\",\"take\":1}\n");
    CHECK (journalHasUncleanTake (root));

    auto before = openWav (f);
    CHECK (before == nullptr || before->lengthInSamples == 0);

    auto results = recoverSessionFolder (root);
    CHECK (results.size() == 1);
    if (results.size() == 1) { CHECK (results[0].ok); CHECK (results[0].frames == 96000); CHECK (results[0].repaired); }
    CHECK (! journalHasUncleanTake (root));
    auto after = openWav (f);
    CHECK (after != nullptr && after->lengthInSamples == 96000);
}

// Device channels + one mono track per input, outputs mirrored (what most tests start from).
static void setupTracks (PatchState& p, const juce::StringArray& ins, const juce::StringArray& outs, bool mirror = true)
{
    setDeviceChannels (p, ins, outs);
    addTracks (p, ins.size(), 0, false, false);
    setMirror (p, mirror, false);
}

static void testPatch()
{
    SECTION ("Patching: mirror = outputs follow inputs, never creates tracks; mults; locks; active channels");
    PatchState s;
    juce::StringArray ins, outs;
    for (int i = 0; i < 64; ++i) { ins.add ("DVS " + juce::String (i + 1)); outs.add ("Out " + juce::String (i + 1)); }
    CHECK (setDeviceChannels (s, ins, outs).wasOk());
    CHECK (s.tracks.empty());                                  // a device never creates tracks by itself

    CHECK (addTracks (s, 8, 0, false, false).wasOk() && s.tracks.size() == 8);
    CHECK (s.mirror && s.tracks[5].input == 5 && s.tracks[5].outputs.size() == 1 && s.tracks[5].outputs[0] == 5);
    CHECK (s.tracks[5].name == "DVS 6");

    CHECK (setTrackInput (s, 5, 20, false).wasOk());           // mirror: output follows the new input
    CHECK (s.mirror && s.tracks[5].outputs[0] == 20);
    CHECK (setTrackInput (s, 5, 0, false).wasOk());            // mult: tracks 1 and 6 both take input 1
    CHECK (s.tracks[5].input == 0 && s.tracks[0].input == 0 && s.tracks[5].outputs[0] == 0);
    CHECK (setTrackInput (s, 2, 10, false, true).wasOk());     // stereo pair in 11-12 -> out 11-12 stereo
    CHECK (s.tracks[2].outStereo && s.tracks[2].outputs[0] == 10);

    CHECK (setTrackOutput (s, 3, 20).wasOk() && ! s.mirror);   // a manual output turns mirror off
    CHECK (setTrackInput (s, 4, 30, false).wasOk() && s.tracks[4].outputs[0] == 4);   // outputs no longer follow
    CHECK (setTrackInput (s, 3, 9, true).failed());            // armed track's input locked while recording
    CHECK (setArmed (s, 3, false, true).failed());
    CHECK (setMirror (s, true, true).wasOk() && s.tracks[4].outputs[0] == 30);   // allowed while recording

    // + Track takes the first input no track uses; inactive (unchecked) channels are skipped
    std::vector<bool> active (64, true);
    for (int ch = 1; ch <= 9; ++ch) active[(size_t) ch] = false;
    setDeviceChannels (s, ins, outs, active);
    // used: 0, 1, 3, 6, 7, 10-11 (stereo), 30; off: 2-9 -> first free mono input is 12, first free pair 14-15
    CHECK (addTrack (s, false).wasOk() && s.tracks.back().input == 12);
    CHECK (firstUnusedInput (s, true) == 14);

    // Regression: delete every track, then the device changes (rate, reconnect): nothing comes back.
    std::vector<int> all;
    for (int i = 0; i < (int) s.tracks.size(); ++i) all.push_back (i);
    CHECK (deleteTracks (s, all, false).wasOk() && s.tracks.empty());
    setDeviceChannels (s, ins, outs);
    setMirror (s, true, false);
    setDeviceChannels (s, ins, outs);
    CHECK (s.tracks.empty());

    // A device with fewer channels keeps the patch (reported, not erased)
    addTracks (s, 2, 40, false, false);
    renameTrack (s, 0, "Kick In");
    ins.clear(); outs.clear();
    for (int i = 0; i < 32; ++i) { ins.add ("MADI " + juce::String (i + 1)); outs.add ("O" + juce::String (i + 1)); }
    CHECK (setDeviceChannels (s, ins, outs).failed());          // warns ...
    CHECK (s.tracks.size() == 2 && s.tracks[0].input == 40 && s.tracks[0].name == "Kick In");   // ... but keeps it

    s.tracks[1].solo = true;
    auto snap = compileSnapshot (s, 7);
    CHECK (snap->numTracks == 2 && snap->anySolo && snap->generation == 7 && snap->tracks[1].input == -1);   // channel 42 absent: silence
    auto round = patchFromVar (juce::JSON::parse (juce::JSON::toString (patchToVar (s))));
    CHECK (round.tracks.size() == 2 && round.tracks[0].name == "Kick In" && round.tracks[1].solo && round.mirror);
}

// Drives the engine like a device would: blocks of `block` frames on this thread.
static void runBlocks (RecordEngine& e, int numIn, int block, uint64_t& frame, uint64_t frames, bool pace)
{
    std::vector<std::vector<float>> in ((size_t) numIn, std::vector<float> ((size_t) block));
    std::vector<const float*> ip ((size_t) numIn);
    std::vector<std::vector<float>> out (2, std::vector<float> ((size_t) block));
    float* op[2] = { out[0].data(), out[1].data() };
    const double blockMs = 1000.0 * block / e.sampleRate();
    double next = juce::Time::getMillisecondCounterHiRes();

    for (uint64_t done = 0; done < frames; done += (uint64_t) block)
    {
        for (int c = 0; c < numIn; ++c)
        {
            for (int i = 0; i < block; ++i) in[(size_t) c][(size_t) i] = sig (c, frame + (uint64_t) i);
            ip[(size_t) c] = in[(size_t) c].data();
        }
        e.process (ip.data(), numIn, op, 2, block);
        frame += (uint64_t) block;
        if (pace) { next += blockMs; while (juce::Time::getMillisecondCounterHiRes() < next) juce::Thread::sleep (1); }
    }
}

static void testEngine128()
{
    SECTION ("Engine: 128 tracks, redundant targets, sample-exact files");
    auto a = tempDir ("engineA"), b = tempDir ("engineB");
    RecordEngine e;
    e.deviceStarting (48000.0, 256);

    PatchState p;
    juce::StringArray ins, outs;
    for (int i = 0; i < 128; ++i) ins.add ("Ch " + juce::String (i + 1));
    outs.add ("L"); outs.add ("R");
    setupTracks (p, ins, outs);
    p.tracks[127].armed = false;
    e.setPatch (p);

    SessionSettings s;
    s.name = "Engine Test"; s.primaryRoot = a; s.secondaryRoot = b; s.ringSeconds = 20; s.minFreeHours = 0.01;
    auto r = e.startRecording (s, p);
    CHECK (r.wasOk());
    if (r.failed()) { std::printf ("  %s\n", r.getErrorMessage().toRawUTF8()); return; }
    CHECK (s.nextTake == 2);

    uint64_t frame = 0;
    const uint64_t total = 256 * 940; // ~5 s, whole blocks
    runBlocks (e, 128, 256, frame, total, false);
    e.stopRecording();
    e.housekeeping();

    for (auto root : { a, b })
    {
        juce::Array<juce::File> files;
        for (const auto& it : juce::RangedDirectoryIterator (SessionPaths::audioDir (root), false, "*.wav")) files.add (it.getFile());
        CHECK (files.size() == 127);
        for (int t : { 0, 63, 126 })
        {
            auto f = SessionPaths::audioDir (root).getChildFile (SessionPaths::trackFileName (t, "Ch " + juce::String (t + 1), 1, root == b));
            auto rd = openWav (f);
            CHECK (rd != nullptr);
            if (rd == nullptr) continue;
            CHECK ((uint64_t) rd->lengthInSamples == total);
            if ((uint64_t) rd->lengthInSamples != total) std::printf ("  track %d: %lld frames, expected %llu\n", t, (long long) rd->lengthInSamples, (unsigned long long) total);
            juce::AudioBuffer<float> buf (1, (int) total);
            rd->read (&buf, 0, (int) total, 0, true, false);
            float maxErr = 0;
            for (uint64_t i = 0; i < total; i += 97) maxErr = std::max (maxErr, std::abs (buf.getSample (0, (int) i) - sig (t, i)));
            CHECK (maxErr < 2.0f / 8388607.0f);
        }
        CHECK (! journalHasUncleanTake (root));
        CHECK (SessionPaths::journalFile (root).loadFileAsString().contains ("\"commit\"") || total < 48000 * 2);
    }
    CHECK (e.target (0)->droppedFrames() == 0 && e.target (1)->droppedFrames() == 0);
    {
        auto live = e.target (0)->peaksFor (63);
        CHECK (live != nullptr && live->isComplete() && live->frames() == total);
        CHECK (WaveformCache::cacheFileFor (e.target (0)->fileFor (63)).existsAsFile());
    }
}

static void testOverrun()
{
    SECTION ("Engine: stalled drive overruns its ring, files stay sample-aligned, other target unaffected");
    auto a = tempDir ("stallA"), b = tempDir ("stallB");
    RecordEngine e;
    e.deviceStarting (48000.0, 512);
    PatchState p;
    juce::StringArray ins, outs;
    for (int i = 0; i < 8; ++i) ins.add ("In " + juce::String (i + 1));
    setupTracks (p, ins, outs);
    e.setPatch (p);

    SessionSettings s;
    s.primaryRoot = a; s.secondaryRoot = b; s.ringSeconds = 2.0; s.minFreeHours = 0.01; // tiny ring: 2 s -> 131072 frames
    CHECK (e.startRecording (s, p).wasOk());
    e.target (1)->testStallMs = 4000;   // drive B freezes for 4 s

    uint64_t frame = 0;
    const uint64_t total = 48000 * 4;
    runBlocks (e, 8, 512, frame, total, true); // real time, 4 s
    e.target (1)->testStallMs = 0;
    e.stopRecording();
    e.housekeeping();

    CHECK (e.target (0)->droppedFrames() == 0);
    CHECK (e.target (1)->droppedFrames() > 0);
    std::printf ("  target B dropped %llu frames\n", (unsigned long long) e.target (1)->droppedFrames());

    for (auto root : { a, b })
    {
        auto f = SessionPaths::audioDir (root).getChildFile (SessionPaths::trackFileName (3, "In 4", 1, root == b));
        auto rd = openWav (f);
        CHECK (rd != nullptr);
        if (rd == nullptr) continue;
        CHECK ((uint64_t) rd->lengthInSamples == total); // gaps were filled: alignment kept
        juce::AudioBuffer<float> buf (1, (int) total);
        rd->read (&buf, 0, (int) total, 0, true, false);
        uint64_t zeros = 0, wrong = 0;
        for (uint64_t i = 0; i < total; ++i)
        {
            const float v = buf.getSample (0, (int) i);
            if (v == 0.0f) ++zeros;
            else if (std::abs (v - sig (3, i)) > 2.0f / 8388607.0f) ++wrong;
        }
        CHECK (wrong == 0);
        if (root == a) CHECK (zeros == 0);
        else CHECK (zeros == e.target (1)->droppedFrames() / 8);
    }

    size_t idx = 0;
    bool sawOverrun = false;
    for (auto& entry : e.log().entriesSince (idx)) sawOverrun |= entry.text.contains ("overrun");
    CHECK (sawOverrun);
}

static void testPlayback()
{
    SECTION ("Playback: recorded take plays back sample-exact to each track's direct out, seek works");
    auto root = tempDir ("playback");
    RecordEngine e;
    e.deviceStarting (48000.0, 256);
    PatchState p;
    juce::StringArray ins, outs;
    for (int i = 0; i < 8; ++i) { ins.add ("In " + juce::String (i + 1)); outs.add ("Out " + juce::String (i + 1)); }
    setupTracks (p, ins, outs);                   // mirror: track n -> output n
    p.tracks[5].armed = false;                    // track 6 not recorded -> silent in playback
    e.setPatch (p);
    SessionSettings s; s.primaryRoot = root; s.minFreeHours = 0.01;
    CHECK (e.startRecording (s, p).wasOk());
    uint64_t frame = 0;
    const uint64_t total = 256 * 800;             // ~4.3 s
    runBlocks (e, 8, 256, frame, total, false);
    e.stopRecording();

    auto takes = scanTakes (root);
    CHECK (takes.size() == 1 && takes.count (1) == 1 && takes[1].size() == 7);

    // Play from a non-zero position with silent live inputs: outputs must carry the recording.
    const uint64_t from = 12345;
    CHECK (e.startPlayback (root, 1, from).wasOk());
    CHECK (e.isPlaying());
    std::vector<std::vector<float>> in (8, std::vector<float> (256, 0.9f)), out (8, std::vector<float> (256));
    std::vector<const float*> ip; std::vector<float*> op;
    for (auto& v : in) ip.push_back (v.data());
    for (auto& v : out) op.push_back (v.data());

    uint64_t playPos = from, wrong = 0, silentSix = 0, blocks = 0;
    while (! e.playbackFinished() && blocks < 2000)
    {
        e.process (ip.data(), 8, op.data(), 8, 256);
        for (int ch : { 0, 3, 7 })
            for (int i = 0; i < 256; ++i)
            {
                const uint64_t f = playPos + (uint64_t) i;
                const float expect = f < total ? sig (ch, f) : 0.0f;
                if (std::abs (op[(size_t) ch][(size_t) i] - expect) > 2.0f / 8388607.0f) ++wrong;
            }
        for (int i = 0; i < 256; ++i) silentSix += op[5][(size_t) i] == 0.0f ? 1 : 0;
        playPos += 256; ++blocks;
        if (blocks % 64 == 0) juce::Thread::sleep (20); // let the reader thread refill
    }
    CHECK (e.playbackFinished());
    CHECK (wrong == 0);
    CHECK (silentSix == blocks * 256);           // unrecorded track ignores its live input during playback
    CHECK (e.playbackPosition() >= total);
    e.stopPlayback();
    CHECK (! e.isPlaying());

    // Recording is refused during playback? No: starting a take stops playback first.
    CHECK (e.startPlayback (root, 1, 0).wasOk());
    CHECK (e.startRecording (s, p).wasOk() && ! e.isPlaying());
    e.stopRecording();
    CHECK (e.startPlayback (root, 99, 0).failed());
}

static void testPeaks()
{
    SECTION ("Waveform peaks: mip levels, zoomed renders, save/load, background cache");
    // 10 s at 48 kHz: silence for 2 s, then a 0.5 burst for 3 s, then a 1.0 burst, then silence
    const int sr = 48000, n = sr * 10;
    std::vector<float> x ((size_t) n, 0.0f);
    for (int i = 2 * sr; i < 5 * sr; ++i) x[(size_t) i] = 0.5f * std::sin ((float) i * 0.05f);
    for (int i = 6 * sr; i < 7 * sr; ++i) x[(size_t) i] = (i % 2 ? 1.0f : -1.0f);

    PeakStore p;
    for (int i = 0; i < n; i += 4096) p.append (x.data() + i, (size_t) std::min (4096, n - i));
    CHECK (p.frames() == (uint64_t) n);
    std::vector<float> px (1000);
    p.render (0, 1, 100, px.data());                       // very zoomed in (level 0)
    CHECK (px[0] < 0.01f);
    p.render (0, (double) n / 1000.0, 1000, px.data());  // whole file in 1000 px (coarse level)
    CHECK (px[50] < 0.01f);                               // 0.5 s  -> silence
    CHECK (std::abs (px[300] - 0.5f) < 0.02f);            // 3 s    -> 0.5 burst
    CHECK (px[650] > 0.99f);                              // 6.5 s  -> full scale
    CHECK (px[999] < 0.01f);
    p.render ((double) n, 100, 10, px.data());            // past the end
    CHECK (px[0] < 0.0f);
    p.finish();
    CHECK (p.isComplete());

    auto dir = tempDir ("peaks");
    auto pk = dir.getChildFile ("a.pk");
    CHECK (p.save (pk));
    PeakStore q;
    CHECK (q.load (pk) && q.frames() == (uint64_t) n);
    q.render (0, (double) n / 1000.0, 1000, px.data());
    CHECK (std::abs (px[300] - 0.5f) < 0.02f && px[650] > 0.99f);

    // cache builds from a WAV in the background and writes .peaks/<name>.pk
    auto wav = dir.getChildFile ("T01_x_001.wav");
    { BwfWriter w; BwfInfo info; CHECK (w.open (wav, info)); w.appendFrames (x.data(), x.size()); w.close(); }
    WaveformCache cache;
    auto s = cache.get (wav);
    for (int i = 0; i < 200 && ! s->isComplete(); ++i) juce::Thread::sleep (10);
    CHECK (s->isComplete() && s->frames() == (uint64_t) n);
    CHECK (WaveformCache::cacheFileFor (wav).existsAsFile());
    CHECK (cache.get (wav) == s);                          // same store on second request
}

static void testStereo()
{
    SECTION ("Stereo tracks: pair input -> interleaved stereo BWF, stalled-disk alignment, playback to a stereo pair");
    auto a = tempDir ("stereoA"), b = tempDir ("stereoB");
    RecordEngine e;
    e.deviceStarting (48000.0, 256);
    PatchState p;
    juce::StringArray ins, outs;
    for (int i = 0; i < 6; ++i) { ins.add ("In " + juce::String (i + 1)); outs.add ("Out " + juce::String (i + 1)); }
    setupTracks (p, ins, outs, false);
    while (p.tracks.size() > 3) removeLastTrack (p, false);
    CHECK (setTrackInput (p, 0, 0, false, false).wasOk());          // track 1: mono, input 1
    CHECK (setTrackInput (p, 1, 2, false, true).wasOk());           // track 2: stereo, inputs 3-4
    CHECK (setTrackInput (p, 2, 5, false, true).failed());          // no pair at input 6
    CHECK (setTrackOutput (p, 1, 2, true).wasOk());                 // track 2 -> outputs 3-4
    CHECK (setTrackOutput (p, 0, 4, true).wasOk());                 // mono track 1 -> outputs 5-6 (both sides)
    p.tracks[2].armed = false;
    CHECK (p.tracks[1].stereo && ! p.tracks[0].stereo);
    e.setPatch (p);

    SessionSettings s; s.primaryRoot = a; s.secondaryRoot = b; s.minFreeHours = 0.01; s.ringSeconds = 2.0;
    CHECK (e.startRecording (s, p).wasOk());
    e.target (1)->testStallMs = 4000;                               // stall disk B to force stereo gaps
    uint64_t frame = 0;
    const uint64_t total = 512 * 375;                               // 4 s
    runBlocks (e, 6, 512, frame, total, true);
    e.target (1)->testStallMs = 0;
    e.stopRecording();
    CHECK (e.target (1)->droppedFrames() > 0);

    for (auto root : { a, b })
    {
        auto f = SessionPaths::audioDir (root).getChildFile (SessionPaths::trackFileName (1, "In 3", 1, root == b));
        auto rd = openWav (f);
        CHECK (rd != nullptr);
        if (rd == nullptr) continue;
        CHECK (rd->numChannels == 2 && (uint64_t) rd->lengthInSamples == total);
        juce::AudioBuffer<float> buf (2, (int) total);
        rd->read (&buf, 0, (int) total, 0, true, true);
        uint64_t wrong = 0, zeroFrames = 0;
        for (uint64_t i = 0; i < total; ++i)
        {
            const float l = buf.getSample (0, (int) i), r = buf.getSample (1, (int) i);
            if (l == 0.0f && r == 0.0f) { ++zeroFrames; continue; }
            if (std::abs (l - sig (2, i)) > 2.0f / 8388607.0f || std::abs (r - sig (3, i)) > 2.0f / 8388607.0f) ++wrong;
        }
        CHECK (wrong == 0);                                          // L/R never swap or slip, even across gaps
        if (root == a) CHECK (zeroFrames == 0);
        auto mono = openWav (SessionPaths::audioDir (root).getChildFile (SessionPaths::trackFileName (0, "In 1", 1, root == b)));
        CHECK (mono != nullptr && mono->numChannels == 1);
    }

    // Playback: stereo file -> outputs 3/4 as L/R; mono track 1 -> both 5 and 6.
    CHECK (e.startPlayback (a, 1, 1000).wasOk());
    std::vector<std::vector<float>> in (6, std::vector<float> (256, 0.0f)), out (6, std::vector<float> (256));
    std::vector<const float*> ip; std::vector<float*> op;
    for (auto& v : in) ip.push_back (v.data());
    for (auto& v : out) op.push_back (v.data());
    e.process (ip.data(), 6, op.data(), 6, 256);
    uint64_t bad = 0;
    for (int i = 0; i < 256; ++i)
    {
        const uint64_t f = 1000 + (uint64_t) i;
        bad += std::abs (op[2][(size_t) i] - sig (2, f)) > 1e-6f ? 1 : 0;
        bad += std::abs (op[3][(size_t) i] - sig (3, f)) > 1e-6f ? 1 : 0;
        bad += std::abs (op[4][(size_t) i] - sig (0, f)) > 1e-6f ? 1 : 0;
        bad += std::abs (op[5][(size_t) i] - sig (0, f)) > 1e-6f ? 1 : 0;
    }
    CHECK (bad == 0);
    e.stopPlayback();

    auto round = patchFromVar (juce::JSON::parse (juce::JSON::toString (patchToVar (p))));
    CHECK (round.tracks[1].stereo && round.tracks[1].outStereo && ! round.tracks[0].stereo);
}

static void testUpdater()
{
    SECTION ("Updater: versions, GitHub release JSON, checksums, swap-and-relaunch script");
    using namespace omr::update;
    CHECK (Version::parse ("v0.2.0") < Version::parse ("0.3.0"));
    CHECK (Version::parse ("0.10.0").minor == 10 && Version::parse ("0.9.9") < Version::parse ("0.10.0"));
    CHECK (Version::parse ("1.2") == Version::parse ("v1.2.0"));
    CHECK (Version::parse ("v1.4.2-beta.1").patch == 2);
    CHECK (! Version::parse ("latest").valid && ! Version::parse ("1..2").valid);
    CHECK (! (Version::parse ("0.3.0") < Version::parse ("0.3.0")));

    const char* json = R"({"tag_name":"v0.3.0","name":"LiveDeck 0.3.0","body":"- OSC control","html_url":"https://github.com/o/LiveDeck/releases/tag/v0.3.0",
        "draft":false,"prerelease":false,"assets":[
        {"name":"LiveDeck-0.3.0.dmg","browser_download_url":"https://github.com/o/LiveDeck/releases/download/v0.3.0/LiveDeck-0.3.0.dmg","size":9},
        {"name":"LiveDeck-0.3.0-mac.zip","browser_download_url":"https://github.com/o/LiveDeck/releases/download/v0.3.0/LiveDeck-0.3.0-mac.zip","size":1234},
        {"name":"LiveDeck-0.3.0-mac.zip.sha256","browser_download_url":"https://github.com/o/LiveDeck/releases/download/v0.3.0/LiveDeck-0.3.0-mac.zip.sha256","size":90}]})";
    auto r = parseRelease (juce::JSON::parse (json));
    CHECK (r.valid && r.version.toString() == "0.3.0" && r.notes == "- OSC control" && r.zipSize == 1234);
    CHECK (r.zipUrl.endsWith ("-mac.zip") && r.shaUrl.endsWith (".sha256") && r.dmgUrl.endsWith (".dmg"));
    auto pre = juce::JSON::parse (json);
    pre.getDynamicObject()->setProperty ("prerelease", true);
    CHECK (! parseRelease (pre).valid);                      // pre-releases are never offered

    const juce::String abc = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    CHECK (parseSha256File (abc + "  LiveDeck-0.3.0-mac.zip\n") == abc);
    CHECK (parseSha256File ("not a hash").isEmpty());
    auto dir = tempDir ("updater");
    auto f = dir.getChildFile ("abc.txt");
    f.replaceWithText ("abc");
    CHECK (sha256OfFile (f) == abc);

    // Run the generated installer against a fake bundle. PID 2147483 is not running, so it proceeds at once.
    auto oldApp = dir.getChildFile ("Applications/LiveDeck.app");
    auto stage = dir.getChildFile ("stage dir's");               // space + quote: checks shell quoting
    auto newApp = stage.getChildFile ("LiveDeck.app");
    oldApp.getChildFile ("Contents").createDirectory();
    newApp.getChildFile ("Contents").createDirectory();
    oldApp.getChildFile ("Contents/version").replaceWithText ("0.2.0");
    newApp.getChildFile ("Contents/version").replaceWithText ("0.3.0");
    auto log = dir.getChildFile ("update.log");
    auto script = dir.getChildFile ("install.sh");
    script.replaceWithText (makeInstallScript (oldApp, newApp, 2147483, log), false, false, "\n");   // as prepareInstall() writes it
    CHECK (std::system (("bash -n " + script.getFullPathName().quoted()).toRawUTF8()) == 0);
    std::system (("PATH=/usr/bin:/bin bash " + script.getFullPathName().quoted() + " >/dev/null 2>&1").toRawUTF8());
    CHECK (oldApp.getChildFile ("Contents/version").loadFileAsString() == "0.3.0");   // swapped
    CHECK (! dir.getChildFile ("Applications/LiveDeck.app.previous").exists());      // backup cleaned up
    CHECK (! stage.exists());                                                        // staging cleaned up
    CHECK (log.loadFileAsString().contains ("update installed"));
}

static void testTrackEditing()
{
    SECTION ("Track editing: ids, delete, drag-reorder; recorded audio follows its track");
    PatchState p;
    juce::StringArray ins, outs;
    for (int i = 0; i < 6; ++i) { ins.add ("In " + juce::String (i + 1)); outs.add ("Out " + juce::String (i + 1)); }
    setupTracks (p, ins, outs);
    CHECK (p.tracks.size() == 6);
    std::set<int> ids;
    for (auto& t : p.tracks) ids.insert (t.id);
    CHECK (ids.size() == 6 && *ids.begin() > 0);

    // Record a take with the mirror layout: track n has input n -> signal sig(n).
    auto root = tempDir ("edit");
    RecordEngine e;
    e.deviceStarting (48000.0, 256);
    e.setPatch (p);
    SessionSettings s; s.primaryRoot = root; s.minFreeHours = 0.01;
    CHECK (e.startRecording (s, p).wasOk());
    uint64_t frame = 0;
    runBlocks (e, 6, 256, frame, 256 * 100, false);
    e.stopRecording();
    CHECK (SessionPaths::takeManifest (root, 1).existsAsFile());
    const int idOf3 = p.tracks[3].id, idOf0 = p.tracks[0].id;

    // Drag tracks 4 and 5 (rows 3,4) to the top; then delete what is now row 2 (old track 1).
    std::vector<int> moved;
    CHECK (moveTracks (p, { 3, 4 }, 0, false, &moved).wasOk());
    CHECK (p.mirror && moved.size() == 2 && moved[0] == 0 && moved[1] == 1);   // reordering keeps mirror mode
    CHECK (p.tracks[0].id == idOf3 && p.tracks[2].id == idOf0);
    CHECK (moveTracks (p, { 0 }, 3, true).failed());                 // locked while recording
    CHECK (deleteTracks (p, { 2 }, false).wasOk() && p.tracks.size() == 5 && rowForTrackId (p, idOf0) < 0);
    CHECK (deleteTracks (p, { 0 }, true).failed());

    // moving down: row 0 dropped before index 3 lands at row 2
    { auto q = p; std::vector<int> nr; moveTracks (q, { 0 }, 3, false, &nr); CHECK (nr.size() == 1 && nr[0] == 2 && q.tracks[2].id == p.tracks[0].id); }

    auto files = resolveTakeFiles (root, 1, p);
    CHECK (files.size() == 5);                                        // deleted track's file is ignored (and kept on disk)
    CHECK (files.count (0) && files[0].getFileName().startsWith ("T04_"));   // old track 4 now on row 0
    CHECK (SessionPaths::audioDir (root).getChildFile (SessionPaths::trackFileName (0, "In 1", 1, false)).existsAsFile());

    // Playback by current rows: row 0 must carry the old track 4 audio (sig(3)) on its own direct out.
    e.setPatch (p);
    CHECK (e.startPlayback (files, 1, 0).wasOk());
    std::vector<std::vector<float>> in (6, std::vector<float> (256, 0.0f)), out (6, std::vector<float> (256));
    std::vector<const float*> ip; std::vector<float*> op;
    for (auto& v : in) ip.push_back (v.data());
    for (auto& v : out) op.push_back (v.data());
    e.process (ip.data(), 6, op.data(), 6, 256);
    uint64_t bad = 0;
    for (int i = 0; i < 256; ++i) bad += std::abs (op[3][(size_t) i] - sig (3, (uint64_t) i)) > 1e-6f ? 1 : 0; // track keeps output 4
    CHECK (bad == 0);
    e.stopPlayback();

    auto round = patchFromVar (juce::JSON::parse (juce::JSON::toString (patchToVar (p))));
    CHECK (round.tracks[0].id == idOf3);

    // A take recorded before manifests existed: linking it once makes it follow later reorders.
    SessionPaths::takeManifest (root, 1).deleteFile();
    PatchState q;
    setupTracks (q, ins, outs);
    CHECK (writeManifestIfMissing (root, 1, q) && ! writeManifestIfMissing (root, 1, q));
    const int idTrack6 = q.tracks[5].id;
    moveTracks (q, { 5 }, 0, false);
    auto legacy = resolveTakeFiles (root, 1, q);
    CHECK (legacy.count (0) && legacy[0].getFileName().startsWith ("T06_") && q.tracks[0].id == idTrack6);
}

static void testStereoPeaks()
{
    SECTION ("Stereo waveforms: separate L/R peaks, v2 cache, old mono caches rebuilt");
    const int sr = 48000, n = sr * 5;
    std::vector<float> x ((size_t) n * 2, 0.0f);                    // interleaved
    for (int i = sr; i < 2 * sr; ++i) x[(size_t) (2 * i)] = (i % 2 ? 1.0f : -1.0f);      // L: full scale 1-2 s
    for (int i = 3 * sr; i < 4 * sr; ++i) x[(size_t) (2 * i + 1)] = (i % 2 ? 0.3f : -0.3f); // R: 0.3 at 3-4 s
    PeakStore p;
    p.append (x.data(), (size_t) n, 2);
    p.finish();
    CHECK (p.numChannels() == 2 && p.frames() == (uint64_t) n);
    std::vector<float> l (500), r (500), both (500);
    const double fpp = (double) n / 500.0;
    p.render (0, fpp, 500, l.data(), 0);
    p.render (0, fpp, 500, r.data(), 1);
    p.render (0, fpp, 500, both.data(), -1);
    CHECK (l[150] > 0.99f && r[150] < 0.01f);                       // 1.5 s: only L
    CHECK (l[350] < 0.01f && std::abs (r[350] - 0.3f) < 0.02f);    // 3.5 s: only R
    CHECK (both[150] > 0.99f && std::abs (both[350] - 0.3f) < 0.02f);

    auto dir = tempDir ("stpeaks");
    CHECK (p.save (dir.getChildFile ("s.pk")));
    PeakStore q;
    CHECK (q.load (dir.getChildFile ("s.pk")) && q.numChannels() == 2);
    q.render (0, fpp, 500, r.data(), 1);
    CHECK (std::abs (r[350] - 0.3f) < 0.02f);

    // A stereo WAV whose cache was written by 0.3 (mono "OMRPK1") gets rebuilt with L/R.
    auto wav = dir.getChildFile ("T01_st_001.wav");
    { BwfWriter w; BwfInfo info; info.numChannels = 2; CHECK (w.open (wav, info)); w.appendFrames (x.data(), (size_t) n); w.close(); }
    {
        PeakStore mono; mono.append (x.data(), (size_t) n, 1); mono.finish();
        auto cf = WaveformCache::cacheFileFor (wav);
        cf.getParentDirectory().createDirectory();
        juce::FileOutputStream os (cf);
        os.write ("OMRPK1", 6); os.writeInt64 ((juce::int64) n); for (int k = 0; k < 3; ++k) os.writeInt (0);
    }
    WaveformCache cache;
    auto s = cache.get (wav);
    for (int i = 0; i < 300 && ! (s->isComplete() && s->numChannels() == 2); ++i) juce::Thread::sleep (10);
    CHECK (s->numChannels() == 2 && s->frames() == (uint64_t) n);
}

static void testRemote()
{
    SECTION ("Remote: OSC address map, MMC, MIDI learn");
    using namespace omr::remote;
    juce::Array<juce::var> none;
    CHECK (parseOsc ("/livedeck/record", none).action == Action::record);
    CHECK (parseOsc ("/LiveDeck/Stop/", none).action == Action::stop);
    CHECK (parseOsc ("/livedeck/playstop", none).action == Action::playStop);
    CHECK (parseOsc ("/livedeck/rtz", none).action == Action::rtz);
    CHECK (parseOsc ("/other/record", none).action == Action::none);
    auto m = parseOsc ("/livedeck/marker", { juce::var ("Song 2") });
    CHECK (m.action == Action::marker && m.text == "Song 2");
    CHECK (parseOsc ("/livedeck/locate", { juce::var (90.5) }).seconds == 90.5);
    CHECK (std::abs (parseOsc ("/livedeck/locate", { juce::var ("1:02:03") }).seconds - 3723.0) < 1e-9);
    CHECK (parseOsc ("/livedeck/locate", { juce::var ("bogus") }).action == Action::none);
    auto t = parseOsc ("/livedeck/take", { juce::var (3) });
    CHECK (t.action == Action::selectTake && t.value == 3);
    CHECK (parseOsc ("/livedeck/take/next", none).action == Action::nextTake);
    auto a = parseOsc ("/livedeck/track/12/arm", { juce::var (0) });
    CHECK (a.action == Action::armTrack && a.track == 11 && a.value == 0);
    auto mu = parseOsc ("/livedeck/track/2/mute", none);
    CHECK (mu.action == Action::muteTrack && mu.value == -1);         // no argument = toggle
    CHECK (parseOsc ("/livedeck/track/2/solo", { juce::var (1.0f) }).value == 1);
    CHECK (parseOsc ("/livedeck/track/0/arm", none).action == Action::none);
    CHECK (parseOsc ("/livedeck/arm/all", { juce::var (0) }).value == 0);

    const uint8_t play[] = { 0xF0, 0x7F, 0x7F, 0x06, 0x02, 0xF7 };
    const uint8_t rec[]  = { 0xF0, 0x7F, 0x05, 0x06, 0x06, 0xF7 };
    const uint8_t loc[]  = { 0xF0, 0x7F, 0x7F, 0x06, 0x44, 0x06, 0x01, 0x21, 0x02, 0x03, 0x0C, 0x00, 0xF7 }; // 25 fps 01:02:03:12
    CHECK (parseMmc (play, 6).action == Action::play);
    CHECK (parseMmc (rec, 6, 5).action == Action::record && parseMmc (rec, 6, 9).action == Action::none);
    auto l = parseMmc (loc, 13);
    CHECK (l.action == Action::locate && std::abs (l.seconds - (3723.0 + 12.0 / 25.0)) < 1e-6);

    auto b = Binding::learnFrom (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), Action::record);
    CHECK (b.kind == Binding::Kind::note && b.matches (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 1)));
    CHECK (! b.matches (juce::MidiMessage::noteOn (2, 60, (juce::uint8) 100)) && ! b.matches (juce::MidiMessage::noteOff (1, 60)));
    auto cc = Binding::learnFrom (juce::MidiMessage::controllerEvent (3, 20, 127), Action::stop);
    CHECK (cc.matches (juce::MidiMessage::controllerEvent (3, 20, 100)) && ! cc.matches (juce::MidiMessage::controllerEvent (3, 20, 10)));
    auto rt = Binding::fromVar (cc.toVar());
    CHECK (rt.kind == cc.kind && rt.number == 20 && rt.channel == 3 && rt.action == Action::stop);
}

static void testMultiDevice()
{
    SECTION ("Several devices: per-device processing, own clocks, stereo pairs never span devices, playback on the output's device");
    // Device A: 4 in / 4 out (globals in 0-3, out 0-3). Device B: 6 in / 2 out (globals in 4-9, out 4-5).
    PatchState p;
    juce::StringArray ins, outs;
    for (int i = 0; i < 4; ++i) { ins.add ("A " + juce::String (i + 1)); outs.add ("A out " + juce::String (i + 1)); }
    for (int i = 0; i < 6; ++i) ins.add ("B " + juce::String (i + 1));
    for (int i = 0; i < 2; ++i) outs.add ("B out " + juce::String (i + 1));
    setDeviceChannels (p, ins, outs);
    p.devices = { { "A", 4, 4 }, { "B", 6, 2 } };
    p.mirror = false;
    addTracks (p, 3, 0, false, false);
    setTrackInput (p, 0, 1, false);             // A in 2
    setTrackOutput (p, 0, 0);                   // -> A out 1
    setTrackInput (p, 1, 6, false, true);       // B in 3-4 stereo
    setTrackOutput (p, 1, 4, true);             // -> B out 1-2
    setTrackInput (p, 2, 3, false, true);       // "A in 4 + B in 1": pair spans devices
    setTrackOutput (p, 2, 5);                   // -> B out 2: cross-device direct out

    auto snap = compileSnapshot (p, 1);
    CHECK (snap->tracks[0].inDev == 0 && snap->tracks[0].input == 1 && snap->tracks[0].outDev == 0 && snap->tracks[0].outs[0] == 0);
    CHECK (snap->tracks[1].inDev == 1 && snap->tracks[1].input == 2 && snap->tracks[1].input2 == 3);
    CHECK (snap->tracks[1].outDev == 1 && snap->tracks[1].outs[0] == 0 && snap->tracks[1].outStereo == 1);
    CHECK (snap->tracks[2].inDev == 0 && snap->tracks[2].input == 3 && snap->tracks[2].input2 == -1);   // never spans devices
    CHECK (snap->crossDeviceOuts == 1);

    auto root = tempDir ("multidev");
    RecordEngine e;
    e.deviceStarting (0, 48000.0, 256);
    e.deviceStarting (1, 48000.0, 256);
    p.tracks[2].armed = false;
    e.setPatch (p);
    SessionSettings s; s.primaryRoot = root; s.minFreeHours = 0.01;
    CHECK (e.startRecording (s, p).wasOk());

    // Device signals: A channel c -> sig(c, f); B channel c -> sig(100 + c, f). B delivers 3 extra blocks (its clock runs fast).
    auto runDevice = [&] (int d, int numIn, int numOut, int blocks, uint64_t& frame, std::vector<std::vector<float>>* outCapture)
    {
        std::vector<std::vector<float>> in ((size_t) numIn, std::vector<float> (256)), out ((size_t) numOut, std::vector<float> (256));
        std::vector<const float*> ip; std::vector<float*> op;
        for (auto& v : in) ip.push_back (v.data());
        for (auto& v : out) op.push_back (v.data());
        for (int b = 0; b < blocks; ++b)
        {
            for (int c = 0; c < numIn; ++c)
                for (int i = 0; i < 256; ++i) in[(size_t) c][(size_t) i] = sig ((d == 0 ? 0 : 100) + c, frame + (uint64_t) i);
            e.process (d, ip.data(), numIn, op.data(), numOut, 256);
            frame += 256;
        }
        if (outCapture != nullptr) *outCapture = out;
    };
    uint64_t fa = 0, fb = 0;
    std::vector<std::vector<float>> outA, outB;
    runDevice (0, 4, 4, 200, fa, &outA);
    runDevice (1, 6, 2, 203, fb, &outB);
    // live direct out on the same device works; the cross-device one stays silent
    bool liveOk = true;
    for (int i = 0; i < 256; ++i) liveOk &= std::abs (outA[0][(size_t) i] - sig (1, fa - 256 + (uint64_t) i)) < 1e-6f;
    CHECK (liveOk);
    bool crossSilent = true;
    for (int i = 0; i < 256; ++i) crossSilent &= outB[1][(size_t) i] == 0.0f || std::abs (outB[1][(size_t) i] - sig (103, fb - 256 + (uint64_t) i)) < 1e-6f;
    CHECK (crossSilent);
    e.stopRecording();

    auto f0 = openWav (SessionPaths::audioDir (root).getChildFile (SessionPaths::trackFileName (0, p.tracks[0].name, 1, false)));
    auto f1 = openWav (SessionPaths::audioDir (root).getChildFile (SessionPaths::trackFileName (1, p.tracks[1].name, 1, false)));
    CHECK (f0 != nullptr && f1 != nullptr);
    if (f0 == nullptr || f1 == nullptr) return;
    CHECK ((uint64_t) f0->lengthInSamples == 256 * 200);                  // each track runs on its own device's clock
    CHECK ((uint64_t) f1->lengthInSamples == 256 * 203 && f1->numChannels == 2);
    juce::AudioBuffer<float> b1 (2, (int) f1->lengthInSamples);
    f1->read (&b1, 0, (int) f1->lengthInSamples, 0, true, true);
    float err = 0;
    for (int i = 0; i < b1.getNumSamples(); i += 37)
        err = std::max ({ err, std::abs (b1.getSample (0, i) - sig (102, (uint64_t) i)), std::abs (b1.getSample (1, i) - sig (103, (uint64_t) i)) });
    CHECK (err < 2.0f / 8388607.0f);

    // Playback: track 2 (stereo) must come out of device B's outputs 1-2, rendered by B's callback.
    CHECK (e.startPlayback (resolveTakeFiles (root, 1, p), 1, 500).wasOk());
    std::vector<std::vector<float>> silentA, playB;
    uint64_t pa = 0, pb = 0;
    runDevice (0, 4, 4, 1, pa, &silentA);
    runDevice (1, 6, 2, 1, pb, &playB);
    float perr = 0;
    for (int i = 0; i < 256; ++i)
        perr = std::max ({ perr, std::abs (playB[0][(size_t) i] - sig (102, 500 + (uint64_t) i)), std::abs (playB[1][(size_t) i] - sig (103, 500 + (uint64_t) i)) });
    CHECK (perr < 1e-6f);
    bool aQuiet = true;
    for (int c = 1; c < 4; ++c) for (float v : silentA[(size_t) c]) aQuiet &= v == 0.0f;
    CHECK (aQuiet);                                                        // B's track never leaks into A
    e.stopPlayback();
}

static void testFailedTarget()
{
    SECTION ("Engine: unwritable primary is refused; missing secondary falls back to primary only");
    RecordEngine e;
    e.deviceStarting (48000.0, 256);
    PatchState p;
    setupTracks (p, { "a", "b" }, {});
    e.setPatch (p);
    SessionSettings s;
    s.primaryRoot = juce::File ("/proc/omr_cannot_write_here");
    CHECK (e.startRecording (s, p).failed());

    s.primaryRoot = tempDir ("fallbackA");
    s.secondaryRoot = juce::File ("/proc/omr_cannot_write_here");
    CHECK (e.startRecording (s, p).wasOk());
    CHECK (e.target (0) != nullptr && e.target (1) == nullptr);
    uint64_t frame = 0;
    runBlocks (e, 2, 256, frame, 48000, false);
    e.stopRecording();
    auto rd = openWav (SessionPaths::audioDir (s.primaryRoot).getChildFile (SessionPaths::trackFileName (0, "a", 1, false)));
    CHECK (rd != nullptr && rd->lengthInSamples == (juce::int64) frame);
}

static void testSession()
{
    SECTION ("Session + template files, target validation");
    auto root = tempDir ("session");
    SessionSettings s; PatchState p;
    s.name = "Arena Show"; s.primaryRoot = root; s.nextTake = 4;
    setupTracks (p, { "1", "2", "3" }, { "1", "2" });
    auto file = SessionPaths::sessionFile (root, s.name);
    CHECK (file.getFileName() == "Arena_Show.livedeck");
    CHECK (saveSession (file, s, p).wasOk());
    SessionSettings s2; PatchState p2;
    CHECK (loadSession (file, s2, p2).wasOk());
    CHECK (s2.name == "Arena Show" && s2.primaryRoot == root && s2.nextTake == 4 && p2.tracks.size() == 3);

    auto tpl = root.getChildFile ("Arena.ldtemplate");
    CHECK (saveTemplate (tpl, s, p).wasOk());
    SessionSettings s3; PatchState p3;
    CHECK (loadSession (tpl, s3, p3).wasOk());
    CHECK (s3.primaryRoot == juce::File() && s3.nextTake == 1 && p3.tracks.size() == 3);

    auto c = validateTarget (root, 48000, 24, 128, 0.01);
    CHECK (c.ok && c.hoursAvailable > 0);
    std::printf ("  fs=%s, %.1f h available for 128 tracks\n", c.filesystem.toRawUTF8(), c.hoursAvailable);
    CHECK (SessionPaths::trackFileName (0, "Kick In/Out", 3, true) == "T01_Kick_In_Out_003_B.wav");
}

static void testBenchmark()
{
    SECTION ("Disk benchmark");
    auto r = runDiskBenchmark (tempDir ("bench"), 16, 2.0, 48000.0 * 3 * 128, 20.0);
    CHECK (r.completed);
    std::printf ("  %s\n", r.message.toRawUTF8());
}

static void testRF64()
{
    SECTION ("RF64: file upgrades itself past 4 GB and stays readable");
    auto f = tempDir ("rf64").getChildFile ("big.wav");
    BwfWriter w;
    BwfInfo info; info.bitsPerSample = 32; info.isFloat = true;
    CHECK (w.open (f, info));
    std::vector<float> x (1 << 20, 0.125f); // 4 MiB per call
    const uint64_t target = (4ull << 30) / 4 + (1 << 20);
    uint64_t done = 0;
    while (done < target) { CHECK (w.appendFrames (x.data(), x.size())); done += x.size(); if ((done & ((1 << 28) - 1)) == 0) w.updateHeader(); }
    CHECK (w.close());
    CHECK (w.isRF64());
    auto rd = openWav (f);
    CHECK (rd != nullptr && (uint64_t) rd->lengthInSamples == done);
    f.deleteFile();
}


static juce::File writeTestWav (const juce::File& f, double rate, int channels, int frames, const std::function<float (int ch, int i)>& fn)
{
    f.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> os (f.createOutputStream().release());
    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (os.get(), rate, (unsigned) channels, 24, {}, 0));
    if (w != nullptr) os.release();
    juce::AudioBuffer<float> b (channels, frames);
    for (int c = 0; c < channels; ++c) for (int i = 0; i < frames; ++i) b.setSample (c, i, fn (c, i));
    if (w != nullptr) w->writeFromAudioSampleBuffer (b, 0, frames);
    return f;
}

static void testImport()
{
    SECTION ("Audio import: plan, convert, offset, resample, manifest");
    const auto dir = tempDir ("import");
    const auto srcDir = dir.getChildFile ("src");
    srcDir.createDirectory();
    const auto mono = writeTestWav (srcDir.getChildFile ("Kick In.wav"), 48000, 1, 4800, [] (int, int i) { return 0.25f * std::sin ((float) i * 0.05f); });
    const auto st = writeTestWav (srcDir.getChildFile ("Playback.wav"), 44100, 2, 44100, [] (int c, int i)
                                  { return (c == 0 ? 0.5f : -0.5f) * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 1000.0 * i / 44100.0); });
    const auto poly = writeTestWav (srcDir.getChildFile ("Show.wav"), 48000, 4, 2000, [] (int c, int) { return 0.1f * (float) (c + 1); });
    srcDir.getChildFile ("notes.mp3").replaceWithText ("x");

    auto plan = planImport ({ mono.getFullPathName(), st.getFullPathName(), poly.getFullPathName(), srcDir.getChildFile ("notes.mp3").getFullPathName() });
    CHECK (plan.sources.size() == 3);
    CHECK (plan.skipped.size() == 1);
    CHECK (plan.numTracks() == 1 + 1 + 4);
    CHECK (plan.sources[0].outputs[0].trackName == "Kick In");
    CHECK (plan.sources[1].outputs[0].sourceChannels.size() == 2);
    CHECK (plan.sources[2].outputs[3].trackName == "Show 4" && plan.sources[2].outputs[3].sourceChannels[0] == 3);
    CHECK (planImport ({ srcDir.getFullPathName() }).numTracks() == 6);   // a dropped folder

    const auto root = dir.getChildFile ("Session");
    SessionPaths::createTree (root);
    auto dest = [&] (const juce::String& n) { return SessionPaths::audioDir (root).getChildFile (n); };

    // mono, same rate, 1000 frames of leading silence
    {
        auto r = importSource (plan.sources[0], { dest ("T01_Kick_In_001.wav") }, 48000, 24, 1000);
        CHECK (r.ok);
        auto rd = openWav (dest ("T01_Kick_In_001.wav"));
        CHECK (rd != nullptr && rd->lengthInSamples == 5800 && rd->numChannels == 1);
        juce::AudioBuffer<float> b (1, 5800);
        rd->read (&b, 0, 5800, 0, true, false);
        CHECK (std::abs (b.getSample (0, 999)) < 1.0e-6f);
        CHECK (std::abs (b.getSample (0, 1000 + 300) - 0.25f * std::sin (300.0f * 0.05f)) < 1.0e-4f);
    }
    // stereo 44.1k -> 48k: right length, aligned (latency compensated), L/R kept
    {
        auto r = importSource (plan.sources[1], { dest ("T02_Playback_001.wav") }, 48000, 32, 0);
        CHECK (r.ok);
        auto rd = openWav (dest ("T02_Playback_001.wav"));
        CHECK (rd != nullptr && std::abs ((int) rd->lengthInSamples - 48000) <= 1 && rd->numChannels == 2 && rd->sampleRate == 48000.0);
        juce::AudioBuffer<float> b (2, 48000);
        rd->read (&b, 0, 48000, 0, true, true);
        float worst = 0;
        for (int i = 2000; i < 46000; ++i)
        {
            const float ideal = 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 1000.0 * i / 48000.0);
            worst = juce::jmax (worst, std::abs (b.getSample (0, i) - ideal), std::abs (b.getSample (1, i) + ideal));
        }
        CHECK (worst < 0.01f);   // aligned to the sample (a 1-sample offset would be > 0.06)
    }
    // 96 kHz -> 48 kHz: aligned too (anti-alias filter + resampler)
    {
        const auto hi = writeTestWav (dir.getChildFile ("src/Hi.wav"), 96000, 1, 96000, [] (int, int i)
                                      { return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * i / 96000.0); });
        auto hp = planImport ({ hi.getFullPathName() });
        auto r = importSource (hp.sources[0], { dest ("T08_Hi_001.wav") }, 48000, 32, 0);
        auto rd = openWav (dest ("T08_Hi_001.wav"));
        CHECK (r.ok && rd != nullptr && rd->lengthInSamples == 48000);
        juce::AudioBuffer<float> b (1, 48000);
        if (rd != nullptr) rd->read (&b, 0, 48000, 0, true, false);
        float worst = 0;
        for (int i = 2000; i < 46000; ++i)
            worst = juce::jmax (worst, std::abs (b.getSample (0, i) - 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * i / 48000.0)));
        CHECK (worst < 0.01f);
    }
    // polyphonic -> one mono file per channel
    {
        std::vector<juce::File> d;
        for (int c = 0; c < 4; ++c) d.push_back (dest ("T0" + juce::String (3 + c) + "_Show_" + juce::String (c + 1) + "_001.wav"));
        auto r = importSource (plan.sources[2], d, 48000, 24, 0);
        CHECK (r.ok && r.written.size() == 4);
        bool good = true;
        for (int c = 0; c < 4; ++c)
        {
            auto rd = openWav (d[(size_t) c]);
            juce::AudioBuffer<float> b (1, 2000);
            good = good && rd != nullptr && rd->numChannels == 1 && rd->lengthInSamples == 2000;
            if (rd != nullptr) rd->read (&b, 0, 2000, 0, true, false);
            good = good && std::abs (b.getSample (0, 1000) - 0.1f * (float) (c + 1)) < 1.0e-4f;
        }
        CHECK (good);
    }
    // cancel removes partial files
    {
        std::atomic<bool> cancel { true };
        auto r = importSource (plan.sources[0], { dest ("T09_X_001.wav") }, 48000, 24, 0, &cancel);
        CHECK (! r.ok && ! dest ("T09_X_001.wav").exists());
    }

    // manifest: imported files follow their (new) tracks, even after a reorder
    PatchState p;
    setDeviceChannels (p, { "1", "2", "3", "4" }, { "1", "2", "3", "4" });
    addTracks (p, 2, 0, false, false);
    int idKick = 0, idPb = 0;
    CHECK (addNamedTrack (p, "Kick In", false, false, &idKick).wasOk());
    CHECK (addNamedTrack (p, "Playback", true, false, &idPb).wasOk());
    CHECK (p.tracks[2].name == "Kick In" && ! p.tracks[2].armed && ! p.tracks[2].autoName);
    CHECK (p.tracks[3].stereo == false && p.tracks[3].input == -1);   // no free pair left on a 4-input device
    CHECK (appendToManifest (root, 1, { { idKick, 2, 1, "Kick In", dest ("T01_Kick_In_001.wav") },
                                        { idPb, 3, 2, "Playback", dest ("T02_Playback_001.wav") } }));
    auto files = resolveTakeFiles (root, 1, p);
    CHECK (files.size() == 2 && files[2] == dest ("T01_Kick_In_001.wav") && files[3] == dest ("T02_Playback_001.wav"));
    moveTracks (p, { 3 }, 0, false);
    files = resolveTakeFiles (root, 1, p);
    CHECK (files[0] == dest ("T02_Playback_001.wav"));
    CHECK (takeLength (files) == 48000 || std::abs ((int) takeLength (files) - 48000) <= 1);
}

static void testFeedback()
{
    SECTION ("Feedback: GitHub issue links");
    feedback::Report r;
    r.kind = feedback::Kind::feature;
    r.title = "Ripple delete & more";
    r.description = "Line one\nLine two";
    r.systemInfo = "LiveDeck 0.5.0\nmacOS";
    const auto url = feedback::newIssueUrl ("creativedamage/LiveDeck", r);
    CHECK (url.startsWith ("https://github.com/creativedamage/LiveDeck/issues/new?"));
    CHECK (url.contains ("labels=enhancement"));
    CHECK (url.contains ("%5BFeature%5D") && url.contains ("%26"));   // title encoded
    CHECK (feedback::issueBody (r).contains ("LiveDeck 0.5.0"));
    r.includeSystemInfo = false;
    CHECK (! feedback::issueBody (r).contains ("LiveDeck 0.5.0"));

    r.kind = feedback::Kind::bug;
    r.steps = "1. Drop a file";
    CHECK (feedback::issueBody (r).contains ("Steps to reproduce"));
    CHECK (feedback::issueTitle (r).startsWith ("[Bug] "));
    r.description = juce::String::repeatedString ("very long text ", 2000);
    const auto longUrl = feedback::newIssueUrl ("creativedamage/LiveDeck", r);
    CHECK (longUrl.length() <= 7000);
    CHECK (longUrl.contains ("Shortened"));
    CHECK (feedback::issuesUrl ("o/r", feedback::Kind::bug).contains ("label%3Abug"));
}

static void testLibrary()
{
    SECTION ("Library folders: Documents/LiveDeck/Shows and Templates");
    // Never the real ~/Documents: this test deletes folders.
    const auto home = tempDir ("home");
    CHECK (Library::root().getFileName() == "LiveDeck");
    Library::setRootForTesting (home.getChildFile ("Documents").getChildFile ("LiveDeck"));
    const auto root = Library::root();
    CHECK (root.isAChildOf (home));
    CHECK (Library::ensure());
    CHECK (Library::shows().isDirectory() && Library::templates().isDirectory());
    CHECK (root.getChildFile ("About this folder.txt").existsAsFile());
    Library::templates().getChildFile ("B Band.ldtemplate").replaceWithText ("{}");
    Library::templates().getChildFile ("A Band.ldtemplate").replaceWithText ("{}");
    Library::templates().getChildFile ("ignore.txt").replaceWithText ("x");
    const auto t = Library::listTemplates();
    CHECK (t.size() == 2 && t[0].getFileName() == "A Band.ldtemplate");
    Library::shows().deleteRecursively();
    CHECK (Library::ensure() && Library::shows().isDirectory());   // re-created if deleted
    Library::setRootForTesting ({});
}

static void testDeadOutputs()
{
    SECTION ("Routing: tracks patched to outputs that are unticked or missing are reported");
    PatchState p;
    juce::StringArray ins, outs;
    for (int i = 0; i < 8; ++i) ins.add ("In " + juce::String (i + 1));
    for (int i = 0; i < 2; ++i) outs.add ("Out " + juce::String (i + 1));
    setupTracks (p, ins, outs);                      // mirror: tracks 3-8 point at outputs 3-8, which don't exist
    auto dead = tracksWithDeadOutputs (p);
    CHECK (dead.size() == 6 && dead.front() == 2 && dead.back() == 7);
    // 8 outputs, but only 1-2 ticked in Audio Setup
    juce::StringArray outs8;
    for (int i = 0; i < 8; ++i) outs8.add ("Out " + juce::String (i + 1));
    std::vector<bool> active (8, false); active[0] = active[1] = true;
    setDeviceChannels (p, ins, outs8, {}, active);
    CHECK (tracksWithDeadOutputs (p).size() == 6);
    CHECK (tracksWithDeadInputs (p).empty());
    std::vector<bool> inOn (8, false); inOn[0] = true;   // only input 1 ticked
    setDeviceChannels (p, ins, outs8, inOn, {});
    CHECK (tracksWithDeadInputs (p).size() == 7 && tracksWithDeadInputs (p).front() == 1);
    setDeviceChannels (p, ins, outs8);               // all ticked
    CHECK (tracksWithDeadOutputs (p).empty() && tracksWithDeadInputs (p).empty());
    // a stereo out pair whose right side is missing
    p.tracks[0].outputs = { 7 };
    p.tracks[0].outStereo = true;
    CHECK (tracksWithDeadOutputs (p).size() == 1);
}

static void testClips()
{
    SECTION ("Clips: offsets, move between tracks, swap/replace, remove, undo text, offset playback");
    const auto root = tempDir ("clips");
    SessionPaths::createTree (root);
    auto dest = [&] (const juce::String& n) { return SessionPaths::audioDir (root).getChildFile (n); };
    const auto a = writeTestWav (dest ("T01_A_001.wav"), 48000, 1, 2000, [] (int, int i) { return 0.001f * (float) (i + 1); });
    const auto b = writeTestWav (dest ("T02_B_001.wav"), 48000, 1, 3000, [] (int, int) { return -0.5f; });

    PatchState p;
    setDeviceChannels (p, { "1", "2", "3" }, { "1", "2", "3" });
    addTracks (p, 3, 0, false, false);
    const int id0 = p.tracks[0].id, id1 = p.tracks[1].id, id2 = p.tracks[2].id;
    CHECK (appendToManifest (root, 1, { { id0, 0, 1, "A", a, 0 }, { id1, 1, 1, "B", b, 0 } }));

    // move A later on the same track
    const auto before = readManifestText (root, 1);
    CHECK (moveClip (root, 1, id0, id0, 4800, false));
    auto c = resolveTakeClips (root, 1, p);
    CHECK (c.size() == 2 && c[0].offset == 4800 && c[0].file == a);
    CHECK (takeLength (c) == 4800 + 2000);

    // move A to the empty track 3
    CHECK (moveClip (root, 1, id0, id2, 100, false));
    c = resolveTakeClips (root, 1, p);
    CHECK (c.count (0) == 0 && c[2].file == a && c[2].offset == 100 && c[1].file == b);

    // swap A (track 3) with B (track 2): B keeps its own offset
    CHECK (moveClip (root, 1, id2, id1, 50, true));
    c = resolveTakeClips (root, 1, p);
    CHECK (c[1].file == a && c[1].offset == 50 && c[2].file == b && c[2].offset == 0);

    // replace: B (track 3) onto track 2 unlinks A
    CHECK (moveClip (root, 1, id2, id1, 0, false));
    c = resolveTakeClips (root, 1, p);
    CHECK (c.size() == 1 && c[1].file == b);
    CHECK (a.existsAsFile());   // never deleted

    // setClip puts a file on a track (replacing), removeClip unlinks
    CHECK (setClip (root, 1, { id0, 0, 1, "A", a, 960 }));
    c = resolveTakeClips (root, 1, p);
    CHECK (c[0].file == a && c[0].offset == 960);
    CHECK (removeClip (root, 1, id1));
    CHECK (resolveTakeClips (root, 1, p).count (1) == 0);

    // undo = restore the earlier manifest text
    CHECK (restoreManifestText (root, 1, before));
    c = resolveTakeClips (root, 1, p);
    CHECK (c.size() == 2 && c[0].offset == 0 && c[0].file == a && c[1].file == b);

    // several clips at once: shift (never before 0) and remove
    {
        const auto keep = readManifestText (root, 1);
        CHECK (shiftClips (root, 1, { id0 }, 2000));
        CHECK (shiftClips (root, 1, { id0, id1 }, -1500));
        auto cs = resolveTakeClips (root, 1, p);
        CHECK (cs[0].offset == 500 && cs[1].offset == 0);   // clamped at the take start
        CHECK (removeClips (root, 1, { id0, id1 }));
        CHECK (resolveTakeClips (root, 1, p).empty());
        CHECK (! removeClips (root, 1, { id0 }));          // nothing left to remove
        CHECK (restoreManifestText (root, 1, keep));
    }


    // playback: a clip starting at 1000 plays silence first, then the file from its first sample
    std::map<int, Clip> clips { { 0, { a, 1000 } } };
    EventLog log;
    Player pl (log);
    CHECK (pl.load (clips, 1, 48000, 0).wasOk());
    CHECK (pl.length() == 3000);
    pl.start();
    juce::Thread::sleep (150);
    std::vector<float> L (1500), R (1500);
    CHECK (pl.readTrack (0, L.data(), R.data(), 1500, 0) == 1);
    CHECK (std::abs (L[999]) < 1.0e-6f && std::abs (L[1000] - 0.001f) < 1.0e-4f && std::abs (L[1499] - 0.5f) < 1.0e-3f);
    pl.halt();
    // seek into the clip
    CHECK (pl.load (clips, 1, 48000, 1500).wasOk());
    pl.start();
    juce::Thread::sleep (150);
    pl.readTrack (0, L.data(), R.data(), 10, 1500);
    CHECK (std::abs (L[0] - 0.501f) < 1.0e-3f);
    pl.halt();
}

static void testLicense()
{
    SECTION ("License: RSA token verification, offline grace, clock rollback, machine id, PKCE");
    using namespace omr::license;
    auto t = parse (kTestToken1, kTestN, kTestE);
    CHECK (t.signatureOk);
    CHECK (t.userId == "u1" && t.email == "a@b.c" && t.company == "Acme" && t.kind == "annual" && t.seats == 2);
    CHECK (t.expiresAt == 2000000000000LL && t.validUntil == 1791209600000LL && ! t.admin);

    const juce::int64 issued = 1790000000000LL;
    CHECK (evaluate (t, "MACHINE-123456", issued + 3600000, issued) == Verdict::ok);
    CHECK (evaluate (t, "OTHER-MACHINE", issued + 3600000, issued) == Verdict::wrongMachine);
    CHECK (evaluate (t, "MACHINE-123456", 1791209600000LL + 1, issued) == Verdict::offlineGraceOver);
    CHECK (evaluate (t, "MACHINE-123456", issued + 3600000, issued + 5 * 86400000LL) == Verdict::clockRolledBack);
    CHECK (daysLeft (t, issued) == 13 || daysLeft (t, issued) == 14);

    // tampering: change one character of the payload -> signature fails
    juce::String bad (kTestToken1);
    bad = bad.replaceSection (5, 1, bad[5] == 'A' ? "B" : "A");
    CHECK (! parse (bad, kTestN, kTestE).signatureOk);
    CHECK (evaluate (parse (bad, kTestN, kTestE), "MACHINE-123456", issued, issued) == Verdict::badSignature);
    // wrong key (LiveDeck's real key) -> rejected
    CHECK (! parse (kTestToken1).signatureOk);
    CHECK (! parse ("garbage").signatureOk && ! parse ("").signatureOk);

    auto p = parse (kTestToken2, kTestN, kTestE);
    CHECK (p.signatureOk && p.kind == "perpetual" && p.expiresAt == 0 && p.admin);
    CHECK (evaluate (p, "MACHINE-123456", issued + 3600000, 0) == Verdict::ok);   // perpetual: no expiry

    // real public key decodes to 2048 bits
    CHECK (base64UrlDecode (kPublicKeyN).getSize() == 256 && base64UrlDecode (kPublicKeyE).getSize() == 3);

    // base64url round trip
    const char raw[] = "\xfb\xff\x00\x10hello";
    const auto enc = base64UrlEncode (raw, sizeof (raw) - 1);
    CHECK (! enc.containsAnyOf ("+/="));
    CHECK (base64UrlDecode (enc) == juce::MemoryBlock (raw, sizeof (raw) - 1));

    // machine id: stable, hex, not the raw device id
    CHECK (machineId() == machineId() && machineId().length() == 40 && machineId().containsOnly ("0123456789abcdef"));

    // PKCE (RFC 7636 appendix B example)
    CHECK (codeChallengeS256 ("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk") == "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
    juce::Random rng (42);
    const auto v = makeCodeVerifier (rng);
    CHECK (v.length() == 64 && v.containsOnly ("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"));
    CHECK (formatPrice (5000) == "$50.00" && formatPrice (11610) == "$116.10" && formatPrice (4500) == "$45.00");
}

int main (int argc, char** argv)
{

    const bool big = argc > 1 && juce::String (argv[1]) == "--big";
    testRing();
    testBwf (false);
    testBwf (true);
    testRecovery();
    testPatch();
    testSession();
    testEngine128();
    testOverrun();
    testPlayback();
    testPeaks();
    testStereoPeaks();
    testStereo();
    testUpdater();
    testTrackEditing();
    testRemote();
    testMultiDevice();
    testFailedTarget();
    testImport();
    testClips();
    testDeadOutputs();
    testLicense();
    testFeedback();
    testLibrary();
    testBenchmark();
    if (big) testRF64();
    std::printf ("\n%d checks, %d failures\n", checks, failures);
    juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("omr_tests").deleteRecursively();
    return failures == 0 ? 0 : 1;
}
