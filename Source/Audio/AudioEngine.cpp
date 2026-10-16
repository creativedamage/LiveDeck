#include "AudioEngine.h"

AudioEngine::AudioEngine (omr::RecordEngine& e) : engine (e)
{
    inMap.fill (-1);
    outMap.fill (-1);
}

AudioEngine::~AudioEngine()
{
    deviceManager.removeChangeListener (this);
    deviceManager.removeAudioCallback (this);
    deviceManager.closeAudioDevice();
}

void AudioEngine::initialise (const juce::XmlElement* savedState)
{
    // Ask for up to 256 in / 256 out; the device decides what it really has.
    const auto err = deviceManager.initialise (256, 256, savedState, true);
    if (err.isNotEmpty())
        engine.log().post (omr::Level::error, "Audio device: " + err);
    deviceManager.addChangeListener (this);
    deviceManager.addAudioCallback (this);
    enableAllChannels();
}

void AudioEngine::enableAllChannels()
{
    auto* dev = deviceManager.getCurrentAudioDevice();
    if (dev == nullptr) return;
    const int ni = dev->getInputChannelNames().size();
    const int no = dev->getOutputChannelNames().size();
    if (dev->getActiveInputChannels().countNumberOfSetBits() == ni
        && dev->getActiveOutputChannels().countNumberOfSetBits() == no)
        return;

    auto setup = deviceManager.getAudioDeviceSetup();
    setup.useDefaultInputChannels = false;
    setup.useDefaultOutputChannels = false;
    setup.inputChannels.clear();
    setup.outputChannels.clear();
    setup.inputChannels.setRange (0, ni, true);
    setup.outputChannels.setRange (0, no, true);
    const auto err = deviceManager.setAudioDeviceSetup (setup, true);
    if (err.isNotEmpty()) engine.log().post (omr::Level::warning, "Could not enable all channels: " + err);
}

void AudioEngine::changeListenerCallback (juce::ChangeBroadcaster*)
{
    enableAllChannels();
    if (onDeviceChanged) onDeviceChanged();
}

juce::StringArray AudioEngine::inputNames() const
{
    if (auto* d = deviceManager.getCurrentAudioDevice()) return d->getInputChannelNames();
    return {};
}

juce::StringArray AudioEngine::outputNames() const
{
    if (auto* d = deviceManager.getCurrentAudioDevice()) return d->getOutputChannelNames();
    return {};
}

juce::String AudioEngine::describeDevice() const
{
    if (auto* d = deviceManager.getCurrentAudioDevice())
        return d->getName() + "  " + juce::String (d->getCurrentSampleRate() / 1000.0, 1) + " kHz  "
             + juce::String (d->getCurrentBufferSizeSamples()) + " smp  ("
             + juce::String (d->getInputChannelNames().size()) + " in / " + juce::String (d->getOutputChannelNames().size()) + " out)";
    return "No audio device";
}

int AudioEngine::driverXruns() const
{
    if (auto* d = deviceManager.getCurrentAudioDevice()) return juce::jmax (0, d->getXRunCount());
    return 0;
}

int AudioEngine::bufferSize() const
{
    if (auto* d = deviceManager.getCurrentAudioDevice()) return d->getCurrentBufferSizeSamples();
    return 0;
}

double AudioEngine::sampleRate() const
{
    if (auto* d = deviceManager.getCurrentAudioDevice()) return d->getCurrentSampleRate();
    return 0;
}

void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* dev)
{
    inMap.fill (-1);
    outMap.fill (-1);
    const auto ins = dev->getActiveInputChannels();
    const auto outs = dev->getActiveOutputChannels();
    numDeviceIns = juce::jmin (kMaxDeviceChannels, dev->getInputChannelNames().size());
    numDeviceOuts = juce::jmin (kMaxDeviceChannels, dev->getOutputChannelNames().size());
    for (int ch = 0, packed = 0; ch < numDeviceIns; ++ch)  if (ins[ch])  inMap[(size_t) ch] = packed++;
    for (int ch = 0, packed = 0; ch < numDeviceOuts; ++ch) if (outs[ch]) outMap[(size_t) ch] = packed++;

    engine.deviceStarting (dev->getCurrentSampleRate(), dev->getCurrentBufferSizeSamples());
    engine.log().post (omr::Level::info, "Device started: " + describeDevice());
}

void AudioEngine::audioDeviceStopped()
{
    engine.deviceStopped();
}

void AudioEngine::audioDeviceError (const juce::String& message)
{
    engine.log().post (omr::Level::error, "Driver error: " + message);
}

void AudioEngine::audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut,
                                                    int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    for (int ch = 0; ch < numDeviceIns; ++ch)
    {
        const int p = inMap[(size_t) ch];
        inPtrs[(size_t) ch] = (p >= 0 && p < numIn) ? in[p] : nullptr;
    }
    for (int ch = 0; ch < numDeviceOuts; ++ch)
    {
        const int p = outMap[(size_t) ch];
        outPtrs[(size_t) ch] = (p >= 0 && p < numOut) ? out[p] : nullptr;
    }
    // Packed outputs that map to no device channel still need silence.
    for (int i = 0; i < numOut; ++i)
        if (out[i] != nullptr) juce::FloatVectorOperations::clear (out[i], numSamples);

    engine.process (inPtrs.data(), numDeviceIns, outPtrs.data(), numDeviceOuts, numSamples);
}
