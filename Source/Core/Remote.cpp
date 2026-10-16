#include "Remote.h"

namespace omr::remote
{
juce::String actionName (Action a)
{
    switch (a)
    {
        case Action::record:     return "Record";
        case Action::stop:       return "Stop";
        case Action::play:       return "Play";
        case Action::playStop:   return "Play / Stop";
        case Action::rtz:        return "Return to Zero";
        case Action::marker:     return "Add Marker";
        case Action::locate:     return "Locate";
        case Action::selectTake: return "Select Take";
        case Action::nextTake:   return "Next Take";
        case Action::prevTake:   return "Previous Take";
        case Action::armTrack:   return "Arm Track";
        case Action::muteTrack:  return "Mute Track";
        case Action::soloTrack:  return "Solo Track";
        case Action::armAll:     return "Arm All";
        case Action::ping:       return "Ping";
        case Action::none:       break;
    }
    return "None";
}

const std::vector<Action>& learnableActions()
{
    static const std::vector<Action> a { Action::record, Action::stop, Action::play, Action::playStop,
                                         Action::rtz, Action::marker, Action::nextTake, Action::prevTake };
    return a;
}

static int argToSwitch (const juce::Array<juce::var>& args)
{
    if (args.isEmpty()) return -1;
    const auto& v = args.getReference (0);
    if (v.isString())
    {
        const auto s = v.toString().trim().toLowerCase();
        if (s == "on" || s == "true" || s == "1") return 1;
        if (s == "off" || s == "false" || s == "0") return 0;
        if (s == "toggle") return -1;
        return -1;
    }
    return (double) v >= 0.5 ? 1 : 0;
}

double parseTimeText (const juce::String& text)
{
    const auto s = text.trim();
    if (s.isEmpty()) return -1.0;
    juce::StringArray parts;
    parts.addTokens (s, ":", "");
    if (parts.size() > 3) return -1.0;
    double total = 0.0;
    for (const auto& p : parts)
    {
        if (p.isEmpty() || ! p.containsOnly ("0123456789.")) return -1.0;
        total = total * 60.0 + p.getDoubleValue();
    }
    return total;
}

Command parseOsc (const juce::String& addressIn, const juce::Array<juce::var>& args)
{
    Command c;
    auto address = addressIn.trim().toLowerCase();
    if (address.endsWith ("/")) address = address.dropLastCharacters (1);
    if (! address.startsWith ("/livedeck/")) return c;

    juce::StringArray parts;
    parts.addTokens (address.substring (10), "/", "");
    parts.removeEmptyStrings();
    if (parts.isEmpty()) return c;
    const auto& head = parts[0];

    if (parts.size() == 1)
    {
        if (head == "record" || head == "rec")  { c.action = Action::record; return c; }
        if (head == "stop")                     { c.action = Action::stop; return c; }
        if (head == "play")                     { c.action = Action::play; return c; }
        if (head == "playstop")                 { c.action = Action::playStop; return c; }
        if (head == "rtz" || head == "home")    { c.action = Action::rtz; return c; }
        if (head == "ping")                     { c.action = Action::ping; return c; }
        if (head == "marker" || head == "mark")
        {
            c.action = Action::marker;
            if (! args.isEmpty() && args.getReference (0).isString()) c.text = args.getReference (0).toString().trim().substring (0, 40);
            return c;
        }
        if (head == "locate")
        {
            if (args.isEmpty()) return c;
            const auto& v = args.getReference (0);
            const double secs = v.isString() ? parseTimeText (v.toString()) : (double) v;
            if (secs < 0.0) return c;
            c.action = Action::locate;
            c.seconds = secs;
            return c;
        }
        if (head == "take")
        {
            if (args.isEmpty()) return c;
            const int t = (int) args.getReference (0);
            if (t <= 0) return c;
            c.action = Action::selectTake;
            c.value = t;
            return c;
        }
        return c;
    }

    if (head == "take" && parts.size() == 2)
    {
        if (parts[1] == "next") c.action = Action::nextTake;
        else if (parts[1] == "prev" || parts[1] == "previous") c.action = Action::prevTake;
        return c;
    }

    if (head == "arm" && parts.size() == 2 && parts[1] == "all")
    {
        c.action = Action::armAll;
        c.value = args.isEmpty() ? 1 : argToSwitch (args);
        return c;
    }

    if (head == "track" && parts.size() == 3 && parts[1].containsOnly ("0123456789") && parts[1].isNotEmpty())
    {
        const int n = parts[1].getIntValue();
        if (n < 1) return c;
        const auto& what = parts[2];
        if (what == "arm" || what == "rec") c.action = Action::armTrack;
        else if (what == "mute") c.action = Action::muteTrack;
        else if (what == "solo") c.action = Action::soloTrack;
        else return c;
        c.track = n - 1;
        c.value = argToSwitch (args);
    }
    return c;
}

Command parseMmc (const uint8_t* d, int size, int deviceId)
{
    Command c;
    if (d == nullptr || size < 4) return c;
    if (d[0] == 0xF0) { ++d; --size; }                 // strip framing
    if (size > 0 && d[size - 1] == 0xF7) --size;
    // 7F <dev> 06 <cmd> ...
    if (size < 4 || d[0] != 0x7F || d[2] != 0x06) return c;
    if (deviceId != 127 && d[1] != 0x7F && d[1] != deviceId) return c;

    switch (d[3])
    {
        case 0x01: c.action = Action::stop; break;      // Stop
        case 0x02:                                      // Play
        case 0x03: c.action = Action::play; break;      // Deferred Play
        case 0x06: c.action = Action::record; break;    // Record Strobe
        case 0x07:                                      // Record Exit
        case 0x09: c.action = Action::stop; break;      // Pause
        case 0x44:                                      // Locate: 44 06 01 hr mn sc fr ff
            if (size >= 11 && d[4] == 0x06 && d[5] == 0x01)
            {
                const int hr = d[6] & 0x1F, mn = d[7] & 0x7F, sc = d[8] & 0x7F, fr = d[9] & 0x1F;
                static const double fps[] = { 24.0, 25.0, 29.97, 30.0 };
                c.action = Action::locate;
                c.seconds = hr * 3600.0 + mn * 60.0 + sc + fr / fps[(d[6] >> 5) & 3];
            }
            break;
        default: break;
    }
    return c;
}

// ------------------------------------------------------------------------------------ MIDI learn

bool Binding::matches (const juce::MidiMessage& m) const
{
    if (kind == Kind::none || action == Action::none) return false;
    if (channel != 0 && m.getChannel() != channel) return false;
    switch (kind)
    {
        case Kind::note:    return m.isNoteOn() && m.getVelocity() > 0 && m.getNoteNumber() == number;
        case Kind::cc:      return m.isController() && m.getControllerNumber() == number && m.getControllerValue() >= 64;
        case Kind::program: return m.isProgramChange() && m.getProgramChangeNumber() == number;
        case Kind::none:    break;
    }
    return false;
}

Binding Binding::learnFrom (const juce::MidiMessage& m, Action a)
{
    Binding b;
    b.action = a;
    b.channel = m.getChannel();
    if (m.isNoteOn() && m.getVelocity() > 0) { b.kind = Kind::note; b.number = m.getNoteNumber(); }
    else if (m.isController() && m.getControllerValue() >= 64) { b.kind = Kind::cc; b.number = m.getControllerNumber(); }
    else if (m.isProgramChange()) { b.kind = Kind::program; b.number = m.getProgramChangeNumber(); }
    else b.kind = Kind::none;
    return b;
}

juce::String Binding::describe() const
{
    const auto ch = channel == 0 ? juce::String ("any ch") : "ch " + juce::String (channel);
    switch (kind)
    {
        case Kind::note:    return "Note " + juce::MidiMessage::getMidiNoteName (number, true, true, 3) + "  " + ch;
        case Kind::cc:      return "CC " + juce::String (number) + "  " + ch;
        case Kind::program: return "Program " + juce::String (number + 1) + "  " + ch;
        case Kind::none:    break;
    }
    return "-";
}

juce::var Binding::toVar() const
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty ("kind", (int) kind);
    o->setProperty ("channel", channel);
    o->setProperty ("number", number);
    o->setProperty ("action", (int) action);
    return juce::var (o.get());
}

Binding Binding::fromVar (const juce::var& v)
{
    Binding b;
    b.kind = (Kind) juce::jlimit (0, 3, (int) v.getProperty ("kind", 0));
    b.channel = juce::jlimit (0, 16, (int) v.getProperty ("channel", 0));
    b.number = juce::jlimit (0, 127, (int) v.getProperty ("number", 0));
    b.action = (Action) juce::jlimit (0, (int) Action::ping, (int) v.getProperty ("action", 0));
    return b;
}
} // namespace omr::remote
