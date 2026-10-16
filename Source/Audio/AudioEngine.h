#pragma once
// Bridges juce::AudioDeviceManager (CoreAudio / ASIO / WASAPI exclusive; SoundGrid, Dante DVS,
// MADI and USB interfaces all appear here as devices) to RecordEngine::process().
//
// JUCE hands the callback only the *enabled* channels, packed together. We map them back to
// device channel numbers once per device start so routing always uses physical channel numbers.

#include "Core/RecordEngine.h"
#include <juce_audio_devices/juce_audio_devices.h>

class AudioEngine final : public juce::AudioIODeviceCallback,
                          private juce::ChangeListener
{
public:
    static constexpr int kMaxDeviceChannels = 512;

    explicit AudioEngine (omr::RecordEngine& engine);
    ~AudioEngine() override;

    void initialise (const juce::XmlElement* savedState);
    juce::AudioDeviceManager& getDeviceManager() noexcept { return deviceManager; }

    juce::StringArray inputNames() const;
    juce::StringArray outputNames() const;
    juce::String describeDevice() const;   // "Dante Virtual Soundcard · 48 kHz · 256"
    double cpuLoad() const { return deviceManager.getCpuUsage(); }
    int driverXruns() const;
    int bufferSize() const;
    double sampleRate() const;

    std::function<void()> onDeviceChanged; // message thread

    // juce::AudioIODeviceCallback
    void audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut,
                                           int numSamples, const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override;
    void audioDeviceError (const juce::String& message) override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void enableAllChannels();

    omr::RecordEngine& engine;
    juce::AudioDeviceManager deviceManager;

    // packed-index -> device-channel maps, rebuilt in audioDeviceAboutToStart (before callbacks run)
    std::array<int, kMaxDeviceChannels> inMap {}, outMap {};
    int numDeviceIns = 0, numDeviceOuts = 0;
    std::array<const float*, kMaxDeviceChannels> inPtrs {};
    std::array<float*, kMaxDeviceChannels> outPtrs {};
};
