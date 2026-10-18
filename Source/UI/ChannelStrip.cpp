#include "ChannelStrip.h"

static constexpr int kStereoId = 10000;

static std::unique_ptr<juce::TextButton> makeToggle (const juce::String& text, juce::Colour on)
{
    auto b = std::make_unique<juce::TextButton> (text);
    b->setColour (juce::TextButton::buttonOnColourId, on);
    b->setColour (juce::TextButton::textColourOnId, juce::Colours::black);
    b->setClickingTogglesState (false);
    return b;
}

ChannelStrip::ChannelStrip (int t) : track (t)
{
    name.setFont (Theme::ui (12.0f, true));
    name.setJustificationType (juce::Justification::centred);
    name.setEditable (false, true, false);
    name.setTooltip ("Double-click to rename");
    name.onTextChange = [this] { if (onRename) onRename (track, name.getText()); };
    name.onTab = [this] (bool back) { if (onRenameTab) onRenameTab (track, back); };
    name.addMouseListener (this, false);   // clicks on the name also select / drag the strip
    addAndMakeVisible (name);

    for (auto* c : { &input, &output })
    {
        c->setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (*c);
    }
    input.setTooltip ("Input source (device channel)");
    output.setTooltip ("Direct out / monitoring destination");
    // Item ids: 1 = none, 2 + n = mono channel n, kStereo + n = stereo pair (n, n + 1)
    input.onChange = [this]
    {
        const int id = input.getSelectedId();
        if (onInput) onInput (track, id >= kStereoId ? id - kStereoId : id - 2, id >= kStereoId);
    };
    output.onChange = [this]
    {
        const int id = output.getSelectedId();
        if (onOutput) onOutput (track, id >= kStereoId ? id - kStereoId : id - 2, id >= kStereoId);
    };

    trim.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    trim.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    trim.setRange (-24.0, 24.0, 0.5);
    trim.setDoubleClickReturnValue (true, 0.0);
    trim.setTooltip ("Trim (monitor + direct out only; files are recorded untouched). Double-click = 0 dB");
    trim.onValueChange = [this] { if (onTrim) onTrim (track, (float) trim.getValue()); repaint(); };
    addAndMakeVisible (trim);

    arm = makeToggle ("REC", Theme::red);
    mute = makeToggle ("M", Theme::yellow);
    solo = makeToggle ("S", Theme::green);
    arm->onClick  = [this] { if (onArm)  onArm (track, ! arm->getToggleState()); };
    mute->onClick = [this] { if (onMute) onMute (track, ! mute->getToggleState()); };
    solo->onClick = [this] { if (onSolo) onSolo (track, ! solo->getToggleState()); };
    for (auto* b : { arm.get(), mute.get(), solo.get() }) { b->setWantsKeyboardFocus (false); b->setMouseClickGrabsKeyboardFocus (false); }
    addAndMakeVisible (*arm);
    addAndMakeVisible (*mute);
    addAndMakeVisible (*solo);
}

void ChannelStrip::setChannelLists (const juce::StringArray& ins, const juce::StringArray& outs,
                                    const std::vector<bool>& inActive, const std::vector<bool>& outActive)
{
    input.clear (juce::dontSendNotification);
    output.clear (juce::dontSendNotification);
    auto fill = [] (juce::ComboBox& c, const juce::StringArray& names, const std::vector<bool>& active)
    {
        auto on = [&] (int i) { return active.empty() || (i < (int) active.size() && active[(size_t) i]); };
        c.addItem ("-- none --", 1);
        c.addSectionHeading ("Mono");
        for (int i = 0; i < names.size(); ++i)
            if (on (i)) c.addItem (juce::String (i + 1) + "  " + names[i], i + 2);
        if (names.size() >= 2)
        {
            c.addSectionHeading ("Stereo");
            for (int i = 0; i + 1 < names.size(); i += 2)
                if (on (i) && on (i + 1))
                    c.addItem (juce::String (i + 1) + "-" + juce::String (i + 2) + " ST  " + names[i] + " / " + names[i + 1], kStereoId + i);
        }
    };
    fill (input, ins, inActive);
    fill (output, outs, outActive);
}

void ChannelStrip::showTrack (const omr::TrackState& t, bool mirror, bool recording)
{
    if (! name.isBeingEdited()) name.setText (t.name, juce::dontSendNotification);
    if (const auto c = Theme::trackColour (t.id - 1, t.colour); c != colour) { colour = c; repaint(); }
    // A patch to a channel that is unticked / missing right now stays as it is, shown as "(off)".
    auto show = [] (juce::ComboBox& c, int id, int ch, bool stereo)
    {
        c.setColour (juce::ComboBox::textColourId, Theme::text);
        if (c.indexOfItemId (id) >= 0) { c.setSelectedId (id, juce::dontSendNotification); return; }
        c.setColour (juce::ComboBox::textColourId, Theme::orange);   // patched to a channel that is off: make it obvious
        c.setText ((stereo ? juce::String (ch + 1) + "-" + juce::String (ch + 2) + " ST" : juce::String (ch + 1)) + " (off)", juce::dontSendNotification);
    };
    if (t.input < 0) { input.setColour (juce::ComboBox::textColourId, Theme::text); input.setSelectedId (1, juce::dontSendNotification); }
    else show (input, t.stereo ? kStereoId + t.input : t.input + 2, t.input, t.stereo);
    if (t.outputs.empty()) { output.setColour (juce::ComboBox::textColourId, Theme::text); output.setSelectedId (1, juce::dontSendNotification); }
    else show (output, t.outStereo ? kStereoId + t.outputs.front() : t.outputs.front() + 2, t.outputs.front(), t.outStereo);
    if (stereoState != t.stereo) { stereoState = t.stereo; repaint(); }
    if (! trim.isMouseButtonDown()) trim.setValue (t.trimDb, juce::dontSendNotification);

    arm->setToggleState (t.armed, juce::dontSendNotification);
    mute->setToggleState (t.mute, juce::dontSendNotification);
    solo->setToggleState (t.solo, juce::dontSendNotification);

    // Visual lockouts: in mirror mode the routing is automatic; while recording an armed
    // track's source and every arm button are frozen.
    output.setAlpha (mirror ? 0.55f : 1.0f);   // in mirror mode the output follows the input
    input.setEnabled (! (recording && t.armed));
    arm->setEnabled (! recording);

    if (armedState != t.armed || recordingState != recording)
    {
        armedState = t.armed;
        recordingState = recording;
        repaint();
    }
}

void ChannelStrip::pushMeter (float peak, float rms, bool clip)
{
    meter.push (peak, rms, clip);
    repaint (meterRepaintArea());
}

void ChannelStrip::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (1.0f, 0);
    g.setColour ((track & 1) ? Theme::stripAlt : Theme::strip);
    g.fillRect (r);
    if (selected)
    {
        g.setColour (Theme::blue.withAlpha (0.16f));
        g.fillRect (r);
        g.setColour (Theme::blue);
        g.fillRect (r.removeFromBottom (3.0f));
    }
    if (recordingState && armedState)
    {
        g.setColour (Theme::red);
        g.fillRect (r.withTrimmedTop (r.getHeight() - 3.0f));
    }

    g.setColour (colour);
    g.fillRect (r.withHeight (14.0f));   // track colour, matches the timeline
    g.setColour (juce::Colours::black.withAlpha (0.8f));
    g.setFont (Theme::mono (10.0f));
    g.drawText (juce::String (track + 1), getLocalBounds().removeFromTop (14).reduced (4, 0), juce::Justification::centredLeft);
    g.drawText (stereoState ? "ST" : "MONO", getLocalBounds().removeFromTop (14).reduced (5, 0), juce::Justification::centredRight);
    g.drawText ((trim.getValue() >= 0 ? "+" : "") + juce::String (trim.getValue(), 1), trim.getBounds().translated (0, trim.getHeight() - 4).withHeight (12),
                juce::Justification::centred);

    MeterDraw::draw (g, meterArea.toFloat(), meter);

    // dB scale beside the meter
    g.setFont (Theme::mono (8.5f));
    auto scale = meterArea.withX (meterArea.getRight() + 3).withWidth (getWidth() - meterArea.getRight() - 4).toFloat();
    scale.removeFromTop (6.0f);
    for (float db : { 0.0f, -6.0f, -18.0f, -30.0f, -48.0f })
    {
        const float y = scale.getY() + MeterDraw::toY (juce::Decibels::decibelsToGain (db), scale.getHeight());
        g.drawText (juce::String ((int) db), juce::Rectangle<float> (scale.getX(), y - 5.0f, scale.getWidth(), 10.0f), juce::Justification::centredLeft);
    }
}

void ChannelStrip::resized()
{
    auto r = getLocalBounds().reduced (4, 2);
    r.removeFromTop (12);
    name.setBounds (r.removeFromTop (18));
    r.removeFromTop (3);
    input.setBounds (r.removeFromTop (20));
    r.removeFromTop (3);
    output.setBounds (r.removeFromTop (20));
    r.removeFromTop (4);
    trim.setBounds (r.removeFromTop (38).withSizeKeepingCentre (38, 38));
    r.removeFromTop (12);

    auto buttons = r.removeFromBottom (48);
    arm->setBounds (buttons.removeFromTop (22));
    buttons.removeFromTop (3);
    mute->setBounds (buttons.removeFromLeft (buttons.getWidth() / 2 - 1));
    buttons.removeFromLeft (2);
    solo->setBounds (buttons);
    r.removeFromBottom (5);

    meterArea = r.withWidth (22).withX (r.getX() + 12);
}

void ChannelStrip::mouseDown (const juce::MouseEvent& e)
{
    const auto p = e.getEventRelativeTo (this).getPosition();
    if (meterArea.contains (p)) { if (onClearClip) onClearClip (track); return; }
    if (e.mods.isPopupMenu()) { if (onTrackMenu) onTrackMenu (track); return; }
    if (name.isBeingEdited()) return;
    if (onSelect) onSelect (track, e.mods);
    dragArmed = true;
    dragging = false;
}

void ChannelStrip::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragArmed) return;
    if (! dragging && e.getDistanceFromDragStart() > 6) dragging = true;
    if (dragging && onDragMove && getParentComponent() != nullptr)
        onDragMove (track, e.getEventRelativeTo (getParentComponent()).x);
}

void ChannelStrip::mouseUp (const juce::MouseEvent&)
{
    if (dragArmed && dragging && onDragEnd) onDragEnd (track, true);
    dragArmed = dragging = false;
}

// ---------------------------------------------------------------------------------------------

void MeterBridge::setTracks (const std::vector<omr::TrackState>& tracks)
{
    meters.resize (tracks.size());
    armed.resize (tracks.size());
    for (size_t i = 0; i < tracks.size(); ++i) armed[i] = tracks[i].armed;
    repaint();
}

void MeterBridge::push (int t, float peak, float rms, bool clip)
{
    if (t < (int) meters.size()) meters[(size_t) t].push (peak, rms, clip);
}

int MeterBridge::perRow() const
{
    const int n = juce::jmax (1, (int) meters.size());
    const int maxPerRow = juce::jmax (1, (getWidth() - 8) / 11);   // 11 px minimum per meter
    const int rows = (n + maxPerRow - 1) / maxPerRow;
    return (n + rows - 1) / rows;
}

juce::Rectangle<float> MeterBridge::cell (int i) const
{
    const int n = juce::jmax (1, (int) meters.size());
    const int pr = perRow();
    const int rows = (n + pr - 1) / pr;
    const float w = (float) (getWidth() - 8) / (float) pr;
    const float h = (float) (getHeight() - 4) / (float) rows;
    return { 4.0f + (float) (i % pr) * w, 2.0f + (float) (i / pr) * h, w, h };
}

void MeterBridge::paint (juce::Graphics& g)
{
    g.fillAll (Theme::panel);
    g.setFont (Theme::mono (8.0f));
    for (int i = 0; i < (int) meters.size(); ++i)
    {
        auto c = cell (i);
        auto label = c.removeFromBottom (10.0f);
        g.setColour (armed[(size_t) i] ? Theme::textDim : Theme::textDim.withAlpha (0.35f));
        // label every meter when there is room, otherwise every 8th so numbers never collide
        const int every = c.getWidth() >= 20.0f ? 1 : (c.getWidth() >= 12.0f ? 4 : 8);
        if (i % every == 0 || every == 1) g.drawText (juce::String (i + 1), label.withWidth (30.0f), juce::Justification::centredLeft);
        MeterDraw::draw (g, c.reduced (juce::jmin (2.0f, c.getWidth() * 0.15f), 1.0f), meters[(size_t) i]);
    }
}

void MeterBridge::mouseDown (const juce::MouseEvent& e)
{
    for (int i = 0; i < (int) meters.size(); ++i)
        if (cell (i).contains (e.position)) { if (onClick) onClick (i); return; }
}
