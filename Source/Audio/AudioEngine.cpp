#include "AudioEngine.h"

AudioEngine::AudioEngine (omr::RecordEngine& e) : engine (e) {}

AudioEngine::~AudioEngine()
{
    deviceManager.removeChangeListener (this);
    for (auto& x : extras) closeExtra (*x);
    deviceManager.removeAudioCallback (&primaryIO);
    deviceManager.closeAudioDevice();
}

void AudioEngine::initialise (const juce::XmlElement* savedState, const juce::var& savedExtras)
{
    // Ask for up to 256 in / 256 out; the device decides what it really has. On first run every
    // channel is enabled; after that the channels ticked in Audio Setup are respected.
    const auto err = deviceManager.initialise (256, 256, savedState, true);
    if (err.isNotEmpty()) engine.log().post (omr::Level::error, "Audio device: " + err);
    deviceManager.addChangeListener (this);
    deviceManager.addAudioCallback (&primaryIO);

    if (auto* arr = savedExtras.getArray())
        for (const auto& v : *arr)
        {
            auto x = std::make_unique<Extra>();
            x->name = v.getProperty ("name", {}).toString();
            x->rememberedIns = (int) v.getProperty ("ins", 0);
            x->rememberedOuts = (int) v.getProperty ("outs", 0);
            if (x->name.isNotEmpty() && (int) extras.size() < omr::kMaxDevices - 1) extras.push_back (std::move (x));
        }
    reopenExtrasIfNeeded();
}

// ------------------------------------------------------------------------------------ extra devices

juce::StringArray AudioEngine::availableExtraDevices() const
{
    juce::StringArray names;
    auto* type = const_cast<juce::AudioDeviceManager&> (deviceManager).getCurrentDeviceTypeObject();
    if (type == nullptr) return names;
    names.addArray (type->getDeviceNames (true));
    names.addArray (type->getDeviceNames (false));
    names.removeDuplicates (false);
    if (auto* cur = deviceManager.getCurrentAudioDevice()) names.removeString (cur->getName());
    return names;
}

juce::StringArray AudioEngine::extraDeviceNames() const
{
    juce::StringArray n;
    for (const auto& x : extras) n.add (x->name);
    return n;
}

bool AudioEngine::isExtraOnline (int i) const
{
    return i >= 0 && i < (int) extras.size() && extras[(size_t) i]->device != nullptr && extras[(size_t) i]->device->isPlaying();
}

juce::var AudioEngine::extraDevicesState() const
{
    juce::Array<juce::var> arr;
    for (const auto& x : extras)
    {
        juce::DynamicObject::Ptr o = new juce::DynamicObject();
        o->setProperty ("name", x->name);
        o->setProperty ("ins", x->rememberedIns);
        o->setProperty ("outs", x->rememberedOuts);
        arr.add (juce::var (o.get()));
    }
    return arr;
}

void AudioEngine::setExtraDevices (const juce::StringArray& names)
{
    // keep the order of devices already in use (stable numbering), append new ones at the end
    std::vector<std::unique_ptr<Extra>> next;
    for (auto& x : extras)
    {
        if (names.contains (x->name)) next.push_back (std::move (x));
        else if (x != nullptr) closeExtra (*x);
    }
    for (const auto& n : names)
    {
        const bool have = std::any_of (next.begin(), next.end(), [&] (const auto& x) { return x->name == n; });
        if (! have && (int) next.size() < omr::kMaxDevices - 1) { auto x = std::make_unique<Extra>(); x->name = n; next.push_back (std::move (x)); }
    }
    extras = std::move (next);
    extrasRate = 0;   // force (re)open of everything at the primary's settings
    reopenExtrasIfNeeded();
    if (onDeviceChanged) onDeviceChanged();
}

void AudioEngine::closeExtra (Extra& x)
{
    if (x.device != nullptr)
    {
        x.device->stop();
        x.device->close();
        x.device.reset();
    }
    x.io.reset();
}

void AudioEngine::openExtra (Extra& x)
{
    closeExtra (x);
    auto* primary = deviceManager.getCurrentAudioDevice();
    auto* type = deviceManager.getCurrentDeviceTypeObject();
    if (primary == nullptr || type == nullptr) return;

    const auto ins = type->getDeviceNames (true), outs = type->getDeviceNames (false);
    std::unique_ptr<juce::AudioIODevice> d (type->createDevice (outs.contains (x.name) ? x.name : juce::String(),
                                                                ins.contains (x.name) ? x.name : juce::String()));
    if (d == nullptr) { engine.log().post (omr::Level::error, "Extra device \"" + x.name + "\" is not available (unplugged?)"); return; }

    juce::BigInteger inBits, outBits;
    inBits.setRange (0, d->getInputChannelNames().size(), true);
    outBits.setRange (0, d->getOutputChannelNames().size(), true);
    const double rate = primary->getCurrentSampleRate();
    const int buffer = primary->getCurrentBufferSizeSamples();
    const auto err = d->open (inBits, outBits, rate, buffer);
    if (err.isNotEmpty()) { engine.log().post (omr::Level::error, "Could not open \"" + x.name + "\": " + err); return; }
    if (std::abs (d->getCurrentSampleRate() - rate) > 0.5)
    {
        engine.log().post (omr::Level::error, "\"" + x.name + "\" cannot run at " + juce::String (rate / 1000.0, 1) + " kHz; it was not started");
        d->close();
        return;
    }

    x.rememberedIns = d->getInputChannelNames().size();
    x.rememberedOuts = d->getOutputChannelNames().size();
    const int index = 1 + (int) (std::find_if (extras.begin(), extras.end(), [&] (const auto& e) { return e.get() == &x; }) - extras.begin());
    x.io = std::make_unique<DeviceIO> (*this, index);
    x.device = std::move (d);
    x.device->start (x.io.get());
    engine.log().post (omr::Level::info, "Extra device " + juce::String (index + 1) + ": " + x.name + " (" + juce::String (x.rememberedIns) + " in / "
                                         + juce::String (x.rememberedOuts) + " out). It keeps its own clock; drift is measured against the primary.");
}

void AudioEngine::reopenExtrasIfNeeded()
{
    auto* primary = deviceManager.getCurrentAudioDevice();
    const double rate = primary != nullptr ? primary->getCurrentSampleRate() : 0;
    const int buffer = primary != nullptr ? primary->getCurrentBufferSizeSamples() : 0;
    const auto type = deviceManager.getCurrentAudioDeviceType();
    if (std::abs (rate - extrasRate) < 0.5 && buffer == extrasBuffer && type == extrasType) return;
    extrasRate = rate;
    extrasBuffer = buffer;
    extrasType = type;
    for (auto& x : extras) closeExtra (*x);
    if (primary == nullptr) return;
    for (auto& x : extras) openExtra (*x);
}

void AudioEngine::changeListenerCallback (juce::ChangeBroadcaster*)
{
    reopenExtrasIfNeeded();   // primary rate / buffer / driver changed: extras follow
    if (onDeviceChanged) onDeviceChanged();
}

// ------------------------------------------------------------------------------------ channel lists

juce::String AudioEngine::shortName (int d) const
{
    juce::String n = d == 0 ? (deviceManager.getCurrentAudioDevice() != nullptr ? deviceManager.getCurrentAudioDevice()->getName() : juce::String ("Primary"))
                            : extras[(size_t) d - 1]->name;
    return n.length() > 14 ? n.substring (0, 13) + "~" : n;
}

std::vector<omr::DeviceLayout> AudioEngine::layouts() const
{
    std::vector<omr::DeviceLayout> v;
    auto* p = deviceManager.getCurrentAudioDevice();
    v.push_back ({ p != nullptr ? p->getName() : juce::String ("None"),
                   p != nullptr ? p->getInputChannelNames().size() : 0, p != nullptr ? p->getOutputChannelNames().size() : 0 });
    for (const auto& x : extras)
    {
        const bool on = x->device != nullptr;
        v.push_back ({ x->name, on ? x->device->getInputChannelNames().size() : x->rememberedIns,
                                on ? x->device->getOutputChannelNames().size() : x->rememberedOuts });
    }
    return v;
}

juce::StringArray AudioEngine::inputNames() const
{
    juce::StringArray names;
    const bool prefix = ! extras.empty();
    if (auto* d = deviceManager.getCurrentAudioDevice())
        for (const auto& n : d->getInputChannelNames()) names.add (prefix ? shortName (0) + ": " + n : n);
    for (int i = 0; i < (int) extras.size(); ++i)
    {
        const auto& x = *extras[(size_t) i];
        if (x.device != nullptr) for (const auto& n : x.device->getInputChannelNames()) names.add (shortName (i + 1) + ": " + n);
        else for (int c = 0; c < x.rememberedIns; ++c) names.add (shortName (i + 1) + " (offline) " + juce::String (c + 1));
    }
    return names;
}

juce::StringArray AudioEngine::outputNames() const
{
    juce::StringArray names;
    const bool prefix = ! extras.empty();
    if (auto* d = deviceManager.getCurrentAudioDevice())
        for (const auto& n : d->getOutputChannelNames()) names.add (prefix ? shortName (0) + ": " + n : n);
    for (int i = 0; i < (int) extras.size(); ++i)
    {
        const auto& x = *extras[(size_t) i];
        if (x.device != nullptr) for (const auto& n : x.device->getOutputChannelNames()) names.add (shortName (i + 1) + ": " + n);
        else for (int c = 0; c < x.rememberedOuts; ++c) names.add (shortName (i + 1) + " (offline) " + juce::String (c + 1));
    }
    return names;
}

std::vector<bool> AudioEngine::inputActive() const
{
    std::vector<bool> v;
    if (auto* d = deviceManager.getCurrentAudioDevice())
    {
        const auto bits = d->getActiveInputChannels();
        for (int i = 0; i < d->getInputChannelNames().size(); ++i) v.push_back (bits[i]);
    }
    for (const auto& x : extras)
    {
        const int n = x->device != nullptr ? x->device->getInputChannelNames().size() : x->rememberedIns;
        for (int c = 0; c < n; ++c) v.push_back (x->device != nullptr);   // offline device: not offered in menus
    }
    return v;
}

std::vector<bool> AudioEngine::outputActive() const
{
    std::vector<bool> v;
    if (auto* d = deviceManager.getCurrentAudioDevice())
    {
        const auto bits = d->getActiveOutputChannels();
        for (int i = 0; i < d->getOutputChannelNames().size(); ++i) v.push_back (bits[i]);
    }
    for (const auto& x : extras)
    {
        const int n = x->device != nullptr ? x->device->getOutputChannelNames().size() : x->rememberedOuts;
        for (int c = 0; c < n; ++c) v.push_back (x->device != nullptr);
    }
    return v;
}

bool AudioEngine::enableAllChannels()
{
    auto* d = deviceManager.getCurrentAudioDevice();
    if (d == nullptr) return false;
    auto setup = deviceManager.getAudioDeviceSetup();
    setup.useDefaultInputChannels = false;
    setup.useDefaultOutputChannels = false;
    setup.inputChannels.clear();
    setup.outputChannels.clear();
    setup.inputChannels.setRange (0, d->getInputChannelNames().size(), true);
    setup.outputChannels.setRange (0, d->getOutputChannelNames().size(), true);
    return deviceManager.setAudioDeviceSetup (setup, true).isEmpty();
}

juce::String AudioEngine::describeDevice() const
{
    auto* d = deviceManager.getCurrentAudioDevice();
    if (d == nullptr) return "No audio device";
    const int nIn = d->getInputChannelNames().size(), nOut = d->getOutputChannelNames().size();
    const int onIn = d->getActiveInputChannels().countNumberOfSetBits(), onOut = d->getActiveOutputChannels().countNumberOfSetBits();
    juce::String s = d->getName() + "  " + juce::String (d->getCurrentSampleRate() / 1000.0, 1) + " kHz  "
                   + juce::String (d->getCurrentBufferSizeSamples()) + " smp  ("
                   + juce::String (nIn) + " in" + (onIn < nIn ? " [" + juce::String (onIn) + " on]" : juce::String()) + " / "
                   + juce::String (nOut) + " out" + (onOut < nOut ? " [" + juce::String (onOut) + " on]" : juce::String()) + ")";
    if (! extras.empty())
    {
        int online = 0;
        for (int i = 0; i < (int) extras.size(); ++i) online += isExtraOnline (i) ? 1 : 0;
        s << "  + " << (int) extras.size() << " more" << (online < (int) extras.size() ? " (" + juce::String ((int) extras.size() - online) + " offline)" : juce::String());
    }
    return s;
}

int AudioEngine::driverXruns() const
{
    int n = 0;
    if (auto* d = deviceManager.getCurrentAudioDevice()) n += juce::jmax (0, d->getXRunCount());
    for (const auto& x : extras) if (x->device != nullptr) n += juce::jmax (0, x->device->getXRunCount());
    return n;
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

// ------------------------------------------------------------------------------------ callbacks

void AudioEngine::DeviceIO::audioDeviceAboutToStart (juce::AudioIODevice* dev)
{
    inMap.fill (-1);
    outMap.fill (-1);
    const auto ins = dev->getActiveInputChannels();
    const auto outs = dev->getActiveOutputChannels();
    numDeviceIns = juce::jmin (kMaxDeviceChannels, dev->getInputChannelNames().size());
    numDeviceOuts = juce::jmin (kMaxDeviceChannels, dev->getOutputChannelNames().size());
    for (int ch = 0, packed = 0; ch < numDeviceIns; ++ch)  if (ins[ch])  inMap[(size_t) ch] = packed++;
    for (int ch = 0, packed = 0; ch < numDeviceOuts; ++ch) if (outs[ch]) outMap[(size_t) ch] = packed++;

    owner.engine.deviceStarting (index, dev->getCurrentSampleRate(), dev->getCurrentBufferSizeSamples());
    if (index == 0) owner.engine.log().post (omr::Level::info, "Device started: " + owner.describeDevice());
}

void AudioEngine::DeviceIO::audioDeviceStopped()
{
    owner.engine.deviceStopped (index);
}

void AudioEngine::DeviceIO::audioDeviceError (const juce::String& message)
{
    owner.engine.log().post (omr::Level::error, "Driver error (device " + juce::String (index + 1) + "): " + message);
}

void AudioEngine::DeviceIO::audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut,
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

    owner.engine.process (index, inPtrs.data(), numDeviceIns, outPtrs.data(), numDeviceOuts, numSamples);
}
