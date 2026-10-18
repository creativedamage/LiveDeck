#pragma once
// Bridges audio devices to RecordEngine::process().
//
//  - The primary device comes from juce::AudioDeviceManager (Audio Setup: driver, rate, buffer,
//    active channels). CoreAudio / ASIO / WASAPI; SoundGrid, Dante DVS, MADI and USB
//    interfaces all appear as devices.
//  - Extra devices (Dante + MADI together, ...) are opened directly at the primary's sample rate
//    and buffer size, each with its own callback thread. Each device keeps its own clock: its
//    tracks are recorded bit-exact at that device's clock and drift is measured, never resampled.
//
// Channels are numbered globally in device order. An extra device that is unplugged keeps its
// channel range (remembered counts), so later devices never get renumbered.
//
// JUCE hands each callback only the *enabled* channels, packed together; DeviceIO maps them back
// to device channel numbers once per device start so routing always uses physical channels.

#include "Core/RecordEngine.h"
#include <juce_audio_devices/juce_audio_devices.h>

class AudioEngine final : private juce::ChangeListener
{
public:
    static constexpr int kMaxDeviceChannels = 512;

    explicit AudioEngine (omr::RecordEngine& engine);
    ~AudioEngine() override;

    void initialise (const juce::XmlElement* savedState, const juce::var& savedExtraDevices);
    juce::AudioDeviceManager& getDeviceManager() noexcept { return deviceManager; }

    // Global channel lists across all devices (names prefixed with the device when there are several).
    juce::StringArray inputNames() const;
    juce::StringArray outputNames() const;
    std::vector<bool> inputActive() const;
    std::vector<bool> outputActive() const;
    std::vector<omr::DeviceLayout> layouts() const;

    // Extra devices
    juce::StringArray availableExtraDevices() const;     // same driver type, excluding the primary
    juce::StringArray extraDeviceNames() const;
    void setExtraDevices (const juce::StringArray& names);
    juce::var extraDevicesState() const;                 // for saving: names + remembered channel counts
    bool isExtraOnline (int extraIndex) const;

    juce::String describeDevice() const;
    bool enableAllChannels();   // ticks every input and output of the primary device   // "Dante Virtual Soundcard  48.0 kHz  256 smp (64 in / 64 out) + 1 more"
    double cpuLoad() const { return deviceManager.getCpuUsage(); }
    int driverXruns() const;
    int bufferSize() const;
    double sampleRate() const;

    std::function<void()> onDeviceChanged; // message thread

private:
    struct DeviceIO final : public juce::AudioIODeviceCallback
    {
        DeviceIO (AudioEngine& o, int idx) : owner (o), index (idx) { inMap.fill (-1); outMap.fill (-1); }
        void audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut,
                                               int numSamples, const juce::AudioIODeviceCallbackContext&) override;
        void audioDeviceAboutToStart (juce::AudioIODevice*) override;
        void audioDeviceStopped() override;
        void audioDeviceError (const juce::String& message) override;

        AudioEngine& owner;
        const int index;
        std::array<int, kMaxDeviceChannels> inMap {}, outMap {};
        int numDeviceIns = 0, numDeviceOuts = 0;
        std::array<const float*, kMaxDeviceChannels> inPtrs {};
        std::array<float*, kMaxDeviceChannels> outPtrs {};
    };

    struct Extra
    {
        juce::String name;
        int rememberedIns = 0, rememberedOuts = 0;   // keeps channel numbering stable while offline
        std::unique_ptr<juce::AudioIODevice> device;
        std::unique_ptr<DeviceIO> io;
    };

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void openExtra (Extra& x);
    void closeExtra (Extra& x);
    void reopenExtrasIfNeeded();
    juce::String shortName (int deviceIndex) const;

    omr::RecordEngine& engine;
    juce::AudioDeviceManager deviceManager;
    DeviceIO primaryIO { *this, 0 };
    std::vector<std::unique_ptr<Extra>> extras;
    double extrasRate = 0;
    int extrasBuffer = 0;
    juce::String extrasType;
};
