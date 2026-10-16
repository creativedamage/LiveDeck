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

static void testPatch()
{
    SECTION ("Patching: 1:1 mirror, manual edits, record locks, mults");
    PatchState s;
    juce::StringArray ins, outs;
    for (int i = 0; i < 64; ++i) { ins.add ("DVS " + juce::String (i + 1)); outs.add ("Out " + juce::String (i + 1)); }
    CHECK (setDeviceChannels (s, ins, outs).wasOk());
    CHECK (s.mirror && s.tracks.size() == 64);
    CHECK (s.tracks[10].input == 10 && s.tracks[10].outputs.size() == 1 && s.tracks[10].outputs[0] == 10);
    CHECK (s.tracks[10].name == "DVS 11");

    CHECK (setTrackInput (s, 5, 0, false).wasOk());           // mult: track 1 and 6 both take input 1
    CHECK (! s.mirror);
    CHECK (s.tracks[5].input == 0 && s.tracks[0].input == 0 && s.tracks[6].input == 6);

    CHECK (setMirror (s, true, true).failed());               // locked while recording
    CHECK (setTrackInput (s, 3, 9, true).failed());           // armed track locked while recording
    CHECK (setArmed (s, 3, false, true).failed());
    CHECK (setTrackOutput (s, 3, 20).wasOk());                // outputs stay editable

    CHECK (setMirror (s, true, false).wasOk());
    CHECK (s.tracks[5].input == 5);

    renameTrack (s, 0, "Kick In");
    CHECK (s.tracks[0].name == "Kick In" && ! s.tracks[0].autoName);
    ins.clear(); outs.clear();
    for (int i = 0; i < 32; ++i) { ins.add ("MADI " + juce::String (i + 1)); outs.add ("O" + juce::String (i + 1)); }
    setDeviceChannels (s, ins, outs);
    CHECK (s.tracks.size() == 32 && s.tracks[0].name == "Kick In" && s.tracks[1].name == "MADI 2");

    s.tracks[2].solo = true;
    auto snap = compileSnapshot (s, 7);
    CHECK (snap->numTracks == 32 && snap->anySolo && snap->generation == 7 && snap->tracks[31].input == 31);

    auto round = patchFromVar (juce::JSON::parse (juce::JSON::toString (patchToVar (s))));
    CHECK (round.tracks.size() == 32 && round.tracks[0].name == "Kick In" && round.tracks[2].solo && round.mirror);
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
    setDeviceChannels (p, ins, outs);
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
    setDeviceChannels (p, ins, outs);
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
    setDeviceChannels (p, ins, outs);             // mirror: track n -> output n
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
    setDeviceChannels (p, ins, outs);
    setMirror (p, false, false);
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
    setDeviceChannels (p, ins, outs);
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
    CHECK (! p.mirror && moved.size() == 2 && moved[0] == 0 && moved[1] == 1);
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
    setDeviceChannels (q, ins, outs);
    CHECK (writeManifestIfMissing (root, 1, q) && ! writeManifestIfMissing (root, 1, q));
    const int idTrack6 = q.tracks[5].id;
    moveTracks (q, { 5 }, 0, false);
    auto legacy = resolveTakeFiles (root, 1, q);
    CHECK (legacy.count (0) && legacy[0].getFileName().startsWith ("T06_") && q.tracks[0].id == idTrack6);
}

static void testFailedTarget()
{
    SECTION ("Engine: unwritable primary is refused; missing secondary falls back to primary only");
    RecordEngine e;
    e.deviceStarting (48000.0, 256);
    PatchState p;
    setDeviceChannels (p, { "a", "b" }, {});
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
    setDeviceChannels (p, { "1", "2", "3" }, { "1", "2" });
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
    testStereo();
    testUpdater();
    testTrackEditing();
    testFailedTarget();
    testBenchmark();
    if (big) testRF64();
    std::printf ("\n%d checks, %d failures\n", checks, failures);
    juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("omr_tests").deleteRecursively();
    return failures == 0 ? 0 : 1;
}
