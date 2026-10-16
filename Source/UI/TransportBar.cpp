#include "TransportBar.h"

TransportBar::TransportBar()
{
    recordButton.setColour (juce::TextButton::buttonColourId, Theme::red.withAlpha (0.25f));
    recordButton.setColour (juce::TextButton::buttonOnColourId, Theme::red);
    recordButton.setTooltip ("Start recording all armed tracks");
    stopButton.setTooltip ("Stop and finalise the take");
    playButton.setColour (juce::TextButton::buttonColourId, Theme::green.withAlpha (0.18f));
    playButton.setColour (juce::TextButton::buttonOnColourId, Theme::green);
    playButton.setColour (juce::TextButton::textColourOnId, juce::Colours::black);
    playButton.setTooltip ("Play the selected take: every recorded track plays to its direct out (virtual soundcheck) and the monitor bus");
    rtzButton.setTooltip ("Return to zero");
    takeCombo.setTooltip ("Take to play back");
    takeCombo.setTextWhenNothingSelected ("No takes");
    markButton.setTooltip ("Drop a marker at the current position");
    mirrorToggle.setTooltip ("Track n records input n and plays to output n. Editing any strip turns this off and keeps the mapping.");
    monitorLeft.setTooltip ("Monitor bus left output");
    monitorRight.setTooltip ("Monitor bus right output");

    for (auto* c : std::initializer_list<juce::Component*> { &recordButton, &stopButton, &playButton, &rtzButton, &markButton, &takeCombo, &sessionMenu, &viewMenu, &disksMenu, &audioSetup,
                                                             &addTrack, &removeTrack, &mirrorToggle, &monitorLeft, &monitorRight })
        addAndMakeVisible (c);

    // Space / Return are transport shortcuts handled by the main window: buttons must not
    // keep keyboard focus, or Space would re-press the last clicked button.
    for (auto* b : { &recordButton, &stopButton, &playButton, &rtzButton, &markButton, &sessionMenu, &disksMenu, &audioSetup, &viewMenu, &addTrack, &removeTrack })
    {
        b->setWantsKeyboardFocus (false);
        b->setMouseClickGrabsKeyboardFocus (false);
    }
    mirrorToggle.setWantsKeyboardFocus (false);
}

void TransportBar::setStatus (const TransportStatus& s)
{
    status = s;
    recordButton.setToggleState (s.recording, juce::dontSendNotification);
    playButton.setToggleState (s.playing, juce::dontSendNotification);
    playButton.setEnabled (! s.recording);
    takeCombo.setEnabled (! s.recording && ! s.playing);
    repaint();
}

static juce::String timecode (double secs)
{
    const auto total = (juce::int64) secs;
    const int frames = (int) ((secs - (double) total) * 30.0);
    return juce::String::formatted ("%02d:%02d:%02d:%02d", (int) (total / 3600), (int) (total / 60 % 60), (int) (total % 60), frames);
}

void TransportBar::drawChip (juce::Graphics& g, juce::Rectangle<int>& row, const juce::String& title, const juce::String& value, juce::Colour c, int width)
{
    auto r = row.removeFromLeft (width).reduced (3, 0);
    row.removeFromLeft (2);
    g.setColour (Theme::strip);
    g.fillRoundedRectangle (r.toFloat(), 4.0f);
    g.setColour (c);
    g.fillRoundedRectangle (r.removeFromLeft (4).toFloat(), 2.0f);
    r.removeFromLeft (4);
    g.setColour (Theme::textDim);
    g.setFont (Theme::ui (10.0f));
    g.drawText (title, r.removeFromTop (r.getHeight() / 2).withTrimmedTop (2), juce::Justification::bottomLeft);
    g.setColour (Theme::text);
    g.setFont (Theme::mono (12.0f));
    g.drawText (value, r, juce::Justification::centredLeft, true);
}

void TransportBar::paint (juce::Graphics& g)
{
    g.fillAll (Theme::panel);
    g.setColour (Theme::edge);
    g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());

    // timecode block
    auto t = timeArea;
    g.setColour (status.recording ? Theme::red : (status.playing ? Theme::green : Theme::textDim));
    g.setFont (Theme::ui (11.0f, true));
    const auto head = status.recording ? "RECORDING  TAKE " + juce::String (status.take)
                    : status.playing   ? "PLAYBACK  TAKE " + juce::String (status.playTake)
                                       : (status.hasSession ? "READY  NEXT TAKE " + juce::String (status.take) : "NO SESSION FOLDER");
    g.drawText (head, t.removeFromTop (14), juce::Justification::centredLeft);
    g.setColour (status.recording ? Theme::red : (status.playing ? Theme::green : Theme::text));
    g.setFont (Theme::mono (28.0f));
    g.drawText (timecode (status.recSeconds), t.removeFromTop (32), juce::Justification::centredLeft);
    g.setColour (Theme::textDim);
    g.setFont (Theme::ui (11.0f));
    g.drawText (status.hasSession ? status.sessionName + "  " + status.sessionPath : juce::String ("Session > New Session to choose where audio is saved"),
                t.removeFromTop (14), juce::Justification::centredLeft, true);

    // status chips
    auto row = chipsArea;
    const bool audioOk = ! status.noAudio && status.device != "No audio device";
    drawChip (g, row, "DRIVER", status.noAudio ? "NO AUDIO FROM DRIVER" : status.device, audioOk ? Theme::green : Theme::red, 300);
    drawChip (g, row, "CPU", juce::String (juce::roundToInt (status.cpu * 100.0)) + " %", status.cpu > 0.7 ? Theme::red : (status.cpu > 0.5 ? Theme::yellow : Theme::green), 64);
    drawChip (g, row, "CLOCK", status.clockKnown ? (status.ppm >= 0 ? "+" : "") + juce::String (juce::roundToInt (status.ppm)) + " ppm" : juce::String ("measuring"),
              ! status.clockKnown ? Theme::textDim : (std::abs (status.ppm) > 100.0 ? Theme::yellow : Theme::green), 92);
    drawChip (g, row, "DROPOUTS", juce::String (status.dropouts), status.dropouts > 0 ? Theme::yellow : Theme::green, 74);

    for (auto* d : { &status.diskA, &status.diskB })
    {
        if (! d->present) { drawChip (g, row, "DISK " + d->label, "not set", Theme::textDim, 104); continue; }
        juce::Colour c = Theme::green;
        if (d->failed || d->ringFill > 0.9f) c = Theme::red;
        else if (d->ringFill > 0.6f) c = Theme::orange;
        else if (d->ringFill > 0.25f || d->hoursLeft < 1.0) c = Theme::yellow;
        const auto value = d->failed ? juce::String ("FAILED")
                                     : juce::String (d->hoursLeft, 1) + " h  " + juce::String ((int) (d->ringFill * 100)) + "%"
                                         + (status.recording ? "  " + juce::String (d->mbPerSec, 1) + " MB/s" : juce::String());
        drawChip (g, row, "DISK " + d->label + "  (left / buffer)", value, c, status.recording ? 178 : 124);
    }

    g.setColour (Theme::textDim);
    g.setFont (Theme::ui (11.0f));
    g.drawText ("Monitor L / R", monitorLabelArea, juce::Justification::centredRight);
}

void TransportBar::resized()
{
    auto r = getLocalBounds().reduced (8, 6);
    auto top = r.removeFromTop (58);
    auto bottom = r.withTrimmedTop (8);

    recordButton.setBounds (top.removeFromLeft (84).reduced (0, 4));
    top.removeFromLeft (6);
    stopButton.setBounds (top.removeFromLeft (70).reduced (0, 4));
    top.removeFromLeft (6);
    playButton.setBounds (top.removeFromLeft (70).reduced (0, 4));
    top.removeFromLeft (6);
    rtzButton.setBounds (top.removeFromLeft (40).reduced (0, 4));
    top.removeFromLeft (6);
    markButton.setBounds (top.removeFromLeft (64).reduced (0, 4));
    top.removeFromLeft (14);
    timeArea = top.removeFromLeft (300);
    top.removeFromLeft (8);
    chipsArea = top.reduced (0, 8);

    sessionMenu.setBounds (bottom.removeFromLeft (90));
    bottom.removeFromLeft (6);
    disksMenu.setBounds (bottom.removeFromLeft (80));
    bottom.removeFromLeft (6);
    audioSetup.setBounds (bottom.removeFromLeft (104));
    bottom.removeFromLeft (6);
    viewMenu.setBounds (bottom.removeFromLeft (64));
    bottom.removeFromLeft (16);
    mirrorToggle.setBounds (bottom.removeFromLeft (240));
    bottom.removeFromLeft (8);
    addTrack.setBounds (bottom.removeFromLeft (72));
    bottom.removeFromLeft (4);
    removeTrack.setBounds (bottom.removeFromLeft (72));
    bottom.removeFromLeft (16);
    takeCombo.setBounds (bottom.removeFromLeft (190));
    bottom.removeFromLeft (6);

    monitorRight.setBounds (bottom.removeFromRight (150));
    bottom.removeFromRight (4);
    monitorLeft.setBounds (bottom.removeFromRight (150));
    monitorLabelArea = bottom.removeFromRight (100);
}
