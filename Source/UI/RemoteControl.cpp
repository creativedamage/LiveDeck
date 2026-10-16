#include "RemoteControl.h"

using namespace omr::remote;

RemoteControl::RemoteControl (juce::PropertiesFile& p) : props (p)
{
    receiver.addListener (this);
    load();
    apply();
    startTimerHz (10);
}

RemoteControl::~RemoteControl()
{
    *alive = false;
    stopTimer();
    if (midiIn != nullptr) midiIn->stop();
    receiver.removeListener (this);
    receiver.disconnect();
    sender.disconnect();
}

// ------------------------------------------------------------------------------------ settings

void RemoteControl::load()
{
    const auto v = juce::JSON::parse (props.getValue ("remoteControl", "{}"));
    current.oscEnabled = (bool) v.getProperty ("oscEnabled", false);
    current.oscPort = (int) v.getProperty ("oscPort", 9010);
    current.feedbackEnabled = (bool) v.getProperty ("feedbackEnabled", false);
    current.feedbackHost = v.getProperty ("feedbackHost", "127.0.0.1").toString();
    current.feedbackPort = (int) v.getProperty ("feedbackPort", 9011);
    current.midiInputId = v.getProperty ("midiInputId", {}).toString();
    current.mmcEnabled = (bool) v.getProperty ("mmcEnabled", true);
    current.mmcDeviceId = (int) v.getProperty ("mmcDeviceId", 127);
    current.bindings.clear();
    if (auto* arr = v.getProperty ("bindings", {}).getArray())
        for (const auto& b : *arr) current.bindings.push_back (Binding::fromVar (b));
}

void RemoteControl::save()
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty ("oscEnabled", current.oscEnabled);
    o->setProperty ("oscPort", current.oscPort);
    o->setProperty ("feedbackEnabled", current.feedbackEnabled);
    o->setProperty ("feedbackHost", current.feedbackHost);
    o->setProperty ("feedbackPort", current.feedbackPort);
    o->setProperty ("midiInputId", current.midiInputId);
    o->setProperty ("mmcEnabled", current.mmcEnabled);
    o->setProperty ("mmcDeviceId", current.mmcDeviceId);
    juce::Array<juce::var> b;
    for (const auto& x : bindings()) b.add (x.toVar());
    o->setProperty ("bindings", b);
    props.setValue ("remoteControl", juce::JSON::toString (juce::var (o.get()), true));
    props.saveIfNeeded();
}

static juce::String localAddresses()
{
    juce::StringArray ips;
    for (const auto& a : juce::IPAddress::getAllAddresses (false))
        if (a != juce::IPAddress::local()) ips.add (a.toString());
    return ips.isEmpty() ? juce::String ("this computer") : ips.joinIntoString (", ");
}

void RemoteControl::apply()
{
    receiver.disconnect();
    if (current.oscEnabled)
    {
        if (current.oscPort < 1 || current.oscPort > 65535) oscStatusText = "OSC: invalid port";
        else if (receiver.connect (current.oscPort)) oscStatusText = "Listening for OSC on UDP " + juce::String (current.oscPort) + " at " + localAddresses();
        else oscStatusText = "OSC: port " + juce::String (current.oscPort) + " is in use by another app";
    }
    else oscStatusText = "OSC off";

    sender.disconnect();
    senderConnected = current.feedbackEnabled && current.feedbackPort > 0 && sender.connect (current.feedbackHost, current.feedbackPort);
    if (current.feedbackEnabled)
        oscStatusText << (senderConnected ? "; feedback to " + current.feedbackHost + ":" + juce::String (current.feedbackPort)
                                          : juce::String ("; feedback target not reachable"));
    lastSent.clear();

    if (midiIn != nullptr) { midiIn->stop(); midiIn.reset(); }
    midiStatusText = "MIDI off";
    if (current.midiInputId.isNotEmpty())
    {
        midiIn = juce::MidiInput::openDevice (current.midiInputId, this);
        if (midiIn != nullptr) { midiIn->start(); midiStatusText = "MIDI input: " + midiIn->getName(); }
        else midiStatusText = "MIDI input not found (unplugged?)";
    }

    save();
    if (onLog) onLog (oscStatusText + " | " + midiStatusText, oscStatusText.contains ("in use") || midiStatusText.contains ("not found"));
}

juce::String RemoteControl::addressReference()
{
    return
        "Send to LiveDeck (UDP):\n"
        "  /livedeck/record\n"
        "  /livedeck/stop                 (second stop = return to zero)\n"
        "  /livedeck/play\n"
        "  /livedeck/playstop             (toggle; never stops a recording)\n"
        "  /livedeck/rtz\n"
        "  /livedeck/marker [label]\n"
        "  /livedeck/locate <seconds | \"h:mm:ss\">\n"
        "  /livedeck/take <n>   /livedeck/take/next   /livedeck/take/prev\n"
        "  /livedeck/track/<n>/arm  [0|1]  (no value = toggle)\n"
        "  /livedeck/track/<n>/mute [0|1]\n"
        "  /livedeck/track/<n>/solo [0|1]\n"
        "  /livedeck/arm/all <0|1>\n"
        "  /livedeck/ping                 (answers /livedeck/pong)\n"
        "\n"
        "Feedback from LiveDeck (when enabled):\n"
        "  /livedeck/state/recording i    /livedeck/state/playing i\n"
        "  /livedeck/state/timecode s     /livedeck/state/seconds f\n"
        "  /livedeck/state/take i         /livedeck/state/nexttake i\n"
        "  /livedeck/state/session s      /livedeck/state/dropouts i\n"
        "  /livedeck/state/disk/a/hours f /livedeck/state/disk/a/ok i  (and disk/b)\n"
        "  /livedeck/state/tracks i\n"
        "  /livedeck/state/track/<n>/name s, /arm i, /mute i, /solo i\n";
}

// ------------------------------------------------------------------------------------ incoming

void RemoteControl::dispatch (const Command& c)
{
    if (c.action == Action::none) return;
    if (c.action == Action::ping)
    {
        if (senderConnected) sender.send (juce::OSCAddressPattern ("/livedeck/pong"), juce::String ("LiveDeck"));
        sendFeedback (true);
        return;
    }
    if (onCommand) onCommand (c);
}

void RemoteControl::oscMessageReceived (const juce::OSCMessage& m)
{
    juce::Array<juce::var> args;
    for (const auto& a : m)
    {
        if (a.isInt32()) args.add (a.getInt32());
        else if (a.isFloat32()) args.add ((double) a.getFloat32());
        else if (a.isString()) args.add (a.getString());
    }
    const auto c = parseOsc (m.getAddressPattern().toString(), args);
    if (c.action == Action::none && onLog) onLog ("OSC: unknown address " + m.getAddressPattern().toString(), false);
    dispatch (c);
}

void RemoteControl::handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& m)
{
    // MIDI thread: work out the command here, run it on the message thread.
    Command c;
    {
        std::lock_guard<std::mutex> g (learnLock);
        if (learning != Action::none)
        {
            const auto b = Binding::learnFrom (m, learning);
            if (b.kind == Binding::Kind::none) return;
            auto done = std::move (learnDone);
            learning = Action::none;
            auto flag = alive;
            auto& list = current.bindings;   // still under learnLock
            list.erase (std::remove_if (list.begin(), list.end(), [&] (const Binding& x) { return x.action == b.action; }), list.end());
            list.push_back (b);
            juce::MessageManager::callAsync ([this, flag, b, done]
            {
                if (! *flag) return;
                save();
                if (done) done (b);
            });
            return;
        }
    }
    if (m.isSysEx() && current.mmcEnabled) c = parseMmc (m.getSysExData(), m.getSysExDataSize(), current.mmcDeviceId);
    if (c.action == Action::none)
    {
        std::lock_guard<std::mutex> g (learnLock);
        for (const auto& b : current.bindings)
            if (b.matches (m)) { c.action = b.action; break; }
    }
    if (c.action == Action::none) return;

    auto flag = alive;
    juce::MessageManager::callAsync ([this, flag, c] { if (*flag) dispatch (c); });
}

void RemoteControl::startLearn (Action action, std::function<void (const Binding&)> done)
{
    std::lock_guard<std::mutex> g (learnLock);
    learning = action;
    learnDone = std::move (done);
}

void RemoteControl::clearBinding (Action a)
{
    {
        std::lock_guard<std::mutex> g (learnLock);
        auto& list = current.bindings;
        list.erase (std::remove_if (list.begin(), list.end(), [a] (const Binding& b) { return b.action == a; }), list.end());
    }
    save();
}

std::vector<Binding> RemoteControl::bindings() const
{
    std::lock_guard<std::mutex> g (learnLock);
    return current.bindings;
}

void RemoteControl::cancelLearn()
{
    std::lock_guard<std::mutex> g (learnLock);
    learning = Action::none;
    learnDone = nullptr;
}

// ------------------------------------------------------------------------------------ feedback

template <typename T>
void RemoteControl::sendIfChanged (const juce::String& address, T value, bool force)
{
    const juce::var v (value);
    auto it = lastSent.find (address);
    if (! force && it != lastSent.end() && it->second == v) return;
    lastSent[address] = v;
    sender.send (juce::OSCAddressPattern (address), value);
}

void RemoteControl::sendFeedback (bool everything)
{
    if (! senderConnected || ! getStatus) return;
    const auto s = getStatus();
    const juce::String p = "/livedeck/state/";
    sendIfChanged (p + "recording", (int) s.recording, everything);
    sendIfChanged (p + "playing", (int) s.playing, everything);
    sendIfChanged (p + "timecode", s.timecode, everything);
    sendIfChanged (p + "seconds", (float) s.seconds, everything);
    sendIfChanged (p + "take", s.take, everything);
    sendIfChanged (p + "nexttake", s.nextTake, everything);
    sendIfChanged (p + "session", s.session, everything);
    sendIfChanged (p + "dropouts", s.dropouts, everything);
    sendIfChanged (p + "disk/a/hours", (float) s.diskAHours, everything);
    sendIfChanged (p + "disk/b/hours", (float) s.diskBHours, everything);
    sendIfChanged (p + "disk/a/ok", (int) s.diskAOk, everything);
    sendIfChanged (p + "disk/b/ok", (int) s.diskBOk, everything);
    sendIfChanged (p + "tracks", (int) s.tracks.size(), everything);
    for (size_t i = 0; i < s.tracks.size(); ++i)
    {
        const auto t = p + "track/" + juce::String ((int) i + 1) + "/";
        sendIfChanged (t + "name", s.tracks[i].name, everything);
        sendIfChanged (t + "arm", (int) s.tracks[i].arm, everything);
        sendIfChanged (t + "mute", (int) s.tracks[i].mute, everything);
        sendIfChanged (t + "solo", (int) s.tracks[i].solo, everything);
    }
}

void RemoteControl::timerCallback()
{
    // changes 10x a second; everything every 5 s so a restarted Companion catches up
    if (++ticks % 50 == 0) sendFeedback (true);
    else sendFeedback (false);
}

// ------------------------------------------------------------------------------------ settings window

RemoteSettingsComponent::RemoteSettingsComponent (RemoteControl& rc) : remote (rc)
{
    const auto& s = remote.settings();
    for (auto* c : std::initializer_list<juce::Component*> { &oscEnable, &fbEnable, &mmcEnable, &oscPort, &fbHost, &fbPort, &mmcId,
                                                             &oscStatus, &midiStatus, &midiLabel, &midiIn, &applyButton, &learnTitle, &reference })
        addAndMakeVisible (c);

    oscEnable.setToggleState (s.oscEnabled, juce::dontSendNotification);
    fbEnable.setToggleState (s.feedbackEnabled, juce::dontSendNotification);
    mmcEnable.setToggleState (s.mmcEnabled, juce::dontSendNotification);
    oscPort.setText (juce::String (s.oscPort));
    oscPort.setInputRestrictions (5, "0123456789");
    fbHost.setText (s.feedbackHost);
    fbPort.setText (juce::String (s.feedbackPort));
    fbPort.setInputRestrictions (5, "0123456789");
    mmcId.setText (juce::String (s.mmcDeviceId));
    mmcId.setInputRestrictions (3, "0123456789");
    learnTitle.setFont (Theme::ui (13.0f, true));

    midiDevices = juce::MidiInput::getAvailableDevices();
    midiIn.addItem ("None", 1);
    for (int i = 0; i < midiDevices.size(); ++i) midiIn.addItem (midiDevices[i].name, i + 2);
    midiIn.setSelectedId (1, juce::dontSendNotification);
    for (int i = 0; i < midiDevices.size(); ++i)
        if (midiDevices[i].identifier == s.midiInputId) midiIn.setSelectedId (i + 2, juce::dontSendNotification);

    reference.setMultiLine (true);
    reference.setReadOnly (true);
    reference.setFont (Theme::mono (11.5f));
    reference.setText (RemoteControl::addressReference());

    for (auto a : learnableActions())
    {
        LearnRow r;
        r.action = a;
        r.name = std::make_unique<juce::Label> (juce::String(), actionName (a));
        r.binding = std::make_unique<juce::Label>();
        r.binding->setColour (juce::Label::textColourId, Theme::textDim);
        r.learn = std::make_unique<juce::TextButton> ("Learn");
        r.clear = std::make_unique<juce::TextButton> ("Clear");
        r.learn->onClick = [this, a]
        {
            for (auto& row : rows) if (row.action == a) row.binding->setText ("move a control...", juce::dontSendNotification);
            juce::Component::SafePointer<RemoteSettingsComponent> safe (this);
            remote.startLearn (a, [safe] (const omr::remote::Binding&) { if (safe != nullptr) safe->refreshBindings(); });
        };
        r.clear->onClick = [this, a] { remote.clearBinding (a); refreshBindings(); };
        for (auto* c : std::initializer_list<juce::Component*> { r.name.get(), r.binding.get(), r.learn.get(), r.clear.get() }) addAndMakeVisible (c);
        rows.push_back (std::move (r));
    }
    refreshBindings();

    applyButton.onClick = [this] { applyFromUi(); };
    setSize (940, 600);
    startTimerHz (4);
}

RemoteSettingsComponent::~RemoteSettingsComponent()
{
    remote.cancelLearn();
    applyFromUi();
}

void RemoteSettingsComponent::refreshBindings()
{
    for (auto& r : rows)
    {
        juce::String text = "-";
        for (const auto& b : remote.bindings())
            if (b.action == r.action) text = b.describe();
        r.binding->setText (text, juce::dontSendNotification);
    }
}

void RemoteSettingsComponent::applyFromUi()
{
    auto& s = remote.settings();
    s.oscEnabled = oscEnable.getToggleState();
    s.oscPort = juce::jlimit (1, 65535, oscPort.getText().getIntValue());
    s.feedbackEnabled = fbEnable.getToggleState();
    s.feedbackHost = fbHost.getText().trim().isEmpty() ? juce::String ("127.0.0.1") : fbHost.getText().trim();
    s.feedbackPort = juce::jlimit (1, 65535, fbPort.getText().getIntValue());
    s.mmcEnabled = mmcEnable.getToggleState();
    s.mmcDeviceId = juce::jlimit (0, 127, mmcId.getText().getIntValue());
    const int sel = midiIn.getSelectedId() - 2;
    s.midiInputId = sel >= 0 && sel < midiDevices.size() ? midiDevices[sel].identifier : juce::String();
    remote.apply();
}

void RemoteSettingsComponent::timerCallback()
{
    oscStatus.setText (remote.oscStatus(), juce::dontSendNotification);
    midiStatus.setText (remote.midiStatus(), juce::dontSendNotification);
}

void RemoteSettingsComponent::paint (juce::Graphics& g)
{
    g.fillAll (Theme::panel);
}

void RemoteSettingsComponent::resized()
{
    auto r = getLocalBounds().reduced (16);
    auto left = r.removeFromLeft (390);
    r.removeFromLeft (14);

    auto row = [&] (int h) { auto x = left.removeFromTop (h); left.removeFromTop (6); return x; };
    { auto x = row (26); oscEnable.setBounds (x.removeFromLeft (210)); oscPort.setBounds (x.removeFromLeft (70)); }
    { auto x = row (26); fbEnable.setBounds (x.removeFromLeft (150)); fbHost.setBounds (x.removeFromLeft (140)); x.removeFromLeft (6); fbPort.setBounds (x.removeFromLeft (64)); }
    oscStatus.setBounds (row (36));
    left.removeFromTop (8);
    { auto x = row (26); midiLabel.setBounds (x.removeFromLeft (90)); midiIn.setBounds (x); }
    { auto x = row (26); mmcEnable.setBounds (x.removeFromLeft (330)); mmcId.setBounds (x.removeFromLeft (50)); }
    midiStatus.setBounds (row (22));
    left.removeFromTop (8);
    learnTitle.setBounds (row (22));
    for (auto& rr : rows)
    {
        auto x = row (24);
        rr.name->setBounds (x.removeFromLeft (120));
        rr.clear->setBounds (x.removeFromRight (54));
        x.removeFromRight (4);
        rr.learn->setBounds (x.removeFromRight (60));
        rr.binding->setBounds (x);
    }
    applyButton.setBounds (left.removeFromBottom (28).removeFromLeft (100));
    reference.setBounds (r);
}
