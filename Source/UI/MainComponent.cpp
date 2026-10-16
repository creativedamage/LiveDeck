#include "MainComponent.h"
#include "Core/Benchmark.h"
#include "Core/Recovery.h"
#include "Core/Updater.h"

using namespace omr;

struct UpdateOffer
{
    update::ReleaseInfo release;
    juce::String currentVersion;
};

static int nextFreeTake (const juce::File& root)
{
    int maxTake = 0;
    const auto dir = SessionPaths::audioDir (root);
    if (dir.isDirectory())
        for (const auto& e : juce::RangedDirectoryIterator (dir, false, "*.wav"))
        {
            auto n = e.getFile().getFileNameWithoutExtension();
            if (n.endsWith ("_B")) n = n.dropLastCharacters (2);
            const auto num = n.fromLastOccurrenceOf ("_", false, false);
            if (num.length() == 3 && num.containsOnly ("0123456789")) maxTake = juce::jmax (maxTake, num.getIntValue());
        }
    return maxTake + 1;
}

MainComponent::MainComponent()
{
    setLookAndFeel (&lnf);
    juce::LookAndFeel::setDefaultLookAndFeel (&lnf);

    juce::PropertiesFile::Options opts;
    opts.applicationName = "LiveDeck";
    opts.filenameSuffix = ".settings";
    opts.osxLibrarySubFolder = "Application Support";
    props.setStorageParameters (opts);
    lastDir = juce::File (props.getUserSettings()->getValue ("lastDir", juce::File::getSpecialLocation (juce::File::userMusicDirectory).getFullPathName()));

    setWantsKeyboardFocus (true);
   #if JUCE_MAC
    juce::PopupMenu appleMenu;
    appleMenu.addItem ("About LiveDeck", [this] { showAbout(); });
    appleMenu.addItem ("Check for Updates...", [this] { checkForUpdates (true); });
    juce::MenuBarModel::setMacMainMenu (this, &appleMenu);
   #else
    menuBar = std::make_unique<juce::MenuBarComponent> (this);
    addAndMakeVisible (*menuBar);
   #endif
    showMixer = props.getUserSettings()->getBoolValue ("showMixer", true);
    showConsole = props.getUserSettings()->getBoolValue ("showConsole", true);
    showBridge = props.getUserSettings()->getBoolValue ("showBridge", true);
    autoUpdateCheck = props.getUserSettings()->getBoolValue ("autoUpdateCheck", true);

    addAndMakeVisible (bar);
    addAndMakeVisible (bridge);
    addAndMakeVisible (timeline);
    split.setItemLayout (0, 160, -1.0, -0.60);   // timeline
    split.setItemLayout (1, 7, 7, 7);            // divider
    split.setItemLayout (2, 250, 900, -0.40);    // mixer
    splitBar = std::make_unique<juce::StretchableLayoutResizerBar> (&split, 1, false);
    addAndMakeVisible (*splitBar);
    addAndMakeVisible (stripView);
    stripView.setViewedComponent (&stripHolder, false);
    stripView.setScrollBarsShown (false, true);
    stripView.setScrollBarThickness (10);

    console.setMultiLine (true);
    console.setReadOnly (true);
    console.setCaretVisible (false);
    console.setScrollbarsShown (true);
    console.setFont (Theme::mono (12.0f));
    addAndMakeVisible (console);

    bridge.onClick = [this] (int t)
    {
        if (auto* s = strips[t]) stripView.setViewPosition (juce::jmax (0, s->getX() - stripView.getWidth() / 2), 0);
    };

    bar.recordButton.onClick = [this] { startRecording(); };
    bar.stopButton.onClick = [this] { stopTransport(); };
    bar.playButton.onClick = [this] { startPlayback(); };
    bar.rtzButton.onClick = [this] { returnToZero(); };
    bar.takeCombo.onChange = [this] { cueFrames = 0; if (engine.isPlaying()) engine.stopPlayback(); refreshTimeline (true); };
    bar.viewMenu.onClick = [this] { showViewMenu(); };

    timeline.onLocate = [this] (uint64_t frame)
    {
        if (engine.isRecording()) return;
        cueFrames = frame;
        if (engine.isPlaying()) startPlayback(); // relocate while playing
    };
    timeline.onArm  = [this] (int t, bool v) { applyPatch ([this, t, v] (PatchState& p) { return setArmed (p, t, v, engine.isRecording()); }); };
    timeline.onMute = [this] (int t, bool v) { applyPatch ([t, v] (PatchState& p) { p.tracks[(size_t) t].mute = v; return juce::Result::ok(); }); };
    timeline.onSolo = [this] (int t, bool v) { applyPatch ([t, v] (PatchState& p) { p.tracks[(size_t) t].solo = v; return juce::Result::ok(); }); };
    timeline.onColour = [this] (int t, int c) { applyPatch ([t, c] (PatchState& p) { p.tracks[(size_t) t].colour = c; return juce::Result::ok(); }); };
    timeline.onInteraction = [this] { grabKeyboardFocus(); };
    timeline.onSelect = [this] (int row, juce::ModifierKeys mods) { selectRow (row, mods); };
    timeline.onTrackMenu = [this] (int row) { showTrackMenu (row); };
    timeline.onMoveSelected = [this] (int insertBefore) { moveSelectedTo (insertBefore); };
    timeline.onRename = [this] (int row, const juce::String& n) { applyPatch ([row, n] (PatchState& p) { return renameTrack (p, row, n); }); };

    stripDropMarker.setOpaque (true);
    stripDropMarker.setInterceptsMouseClicks (false, false);
    stripHolder.addChildComponent (stripDropMarker);
    bar.markButton.onClick = [this] { addMarker(); };
    bar.sessionMenu.onClick = [this] { showSessionMenu(); };
    bar.disksMenu.onClick = [this] { showDisksMenu(); };
    bar.audioSetup.onClick = [this] { showAudioSetup(); };
    bar.addTrack.onClick = [this] { applyPatch ([this] (PatchState& p) { return addTrack (p, engine.isRecording()); }); };
    bar.removeTrack.onClick = [this]
    {
        if (! selectedIds.empty()) deleteSelectedTracks();   // with confirmation
        else applyPatch ([this] (PatchState& p) { return removeLastTrack (p, engine.isRecording()); });
    };
    bar.removeTrack.setTooltip ("Delete the selected tracks (or the last track if none is selected)");
    bar.mirrorToggle.onClick = [this]
    {
        const bool want = bar.mirrorToggle.getToggleState();
        applyPatch ([this, want] (PatchState& p) { return setMirror (p, want, engine.isRecording()); });
    };
    bar.monitorLeft.onChange = [this] { const int v = bar.monitorLeft.getSelectedId() - 2; applyPatch ([v] (PatchState& p) { p.monitorLeft = v; return juce::Result::ok(); }); };
    bar.monitorRight.onChange = [this] { const int v = bar.monitorRight.getSelectedId() - 2; applyPatch ([v] (PatchState& p) { p.monitorRight = v; return juce::Result::ok(); }); };

    audio.onDeviceChanged = [this] { deviceChanged(); };
    std::unique_ptr<juce::XmlElement> saved (props.getUserSettings()->getXmlValue ("audioDevice"));
    audio.initialise (saved.get());
    deviceChanged();

    engine.log().post (Level::info, "LiveDeck ready. Choose Session > New Session to pick where audio is recorded.");
    setSize (1600, 940);
    startTimerHz (30);
}

MainComponent::~MainComponent()
{
    stopTimer();
   #if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu (nullptr);
   #endif
    if (engine.isRecording()) engine.stopRecording();
    saveDeviceState();
    juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    setLookAndFeel (nullptr);
}

void MainComponent::saveDeviceState()
{
    if (auto xml = audio.getDeviceManager().createStateXml())
        props.getUserSettings()->setValue ("audioDevice", xml.get());
    props.getUserSettings()->setValue ("lastDir", lastDir.getFullPathName());
    props.saveIfNeeded();
}

void MainComponent::useSessionFolder (const juce::File& folder)
{
    const auto existing = folder.findChildFiles (juce::File::findFiles, false, "*.livedeck;*.omr");
    if (! existing.isEmpty())
    {
        SessionSettings s; PatchState p;
        if (loadSession (existing.getFirst(), s, p).wasOk()) { session = s; patch = p; deviceChanged(); }
    }
    else session.name = folder.getFileName();
    setPrimaryRoot (folder);
    saveSession();
}

void MainComponent::paint (juce::Graphics& g) { g.fillAll (Theme::bg); }

void MainComponent::resized()
{
    auto r = getLocalBounds();
    if (menuBar != nullptr) menuBar->setBounds (r.removeFromTop (24));
    bar.setBounds (r.removeFromTop (TransportBar::kHeight));
    bridge.setVisible (showBridge);
    if (showBridge) bridge.setBounds (r.removeFromTop (patch.tracks.size() > 64 ? 92 : 64));
    console.setVisible (showConsole);
    if (showConsole) console.setBounds (r.removeFromBottom (96).reduced (6, 3));

    stripView.setVisible (showMixer);
    splitBar->setVisible (showMixer);
    if (showMixer)
    {
        juce::Component* comps[] = { &timeline, splitBar.get(), &stripView };
        split.layOutComponents (comps, 3, r.getX(), r.getY(), r.getWidth(), r.getHeight(), true, true);
    }
    else timeline.setBounds (r);

    const int h = juce::jmax (250, stripView.getHeight() - stripView.getScrollBarThickness());
    stripHolder.setSize (juce::jmax (1, strips.size() * ChannelStrip::kWidth), h);
    for (int i = 0; i < strips.size(); ++i) strips[i]->setBounds (i * ChannelStrip::kWidth, 0, ChannelStrip::kWidth, h);
}

// ------------------------------------------------------------------------------------ patching

void MainComponent::applyPatch (const std::function<juce::Result (PatchState&)>& op)
{
    const bool wasMirror = patch.mirror;
    const size_t before = patch.tracks.size();
    auto r = op (patch);
    if (r.failed())
    {
        engine.log().post (Level::warning, r.getErrorMessage());
        refreshStrips(); // undo the widget change the user just made
        return;
    }
    for (auto it = selectedIds.begin(); it != selectedIds.end();)
        it = rowForTrackId (patch, *it) < 0 ? selectedIds.erase (it) : std::next (it);
    if (wasMirror && ! patch.mirror) engine.log().post (Level::info, "Mirror 1:1 turned off by a manual routing edit; current mapping kept.");
    engine.setPatch (patch);
    if (patch.tracks.size() != before) rebuildStrips();
    else refreshStrips();
    autosave();
}

void MainComponent::deviceChanged()
{
    const auto r = setDeviceChannels (patch, audio.inputNames(), audio.outputNames());
    if (r.failed()) engine.log().post (Level::warning, r.getErrorMessage());
    if (auto* dev = audio.getDeviceManager().getCurrentAudioDevice())
    {
        session.deviceType = dev->getTypeName();
        session.deviceName = dev->getName();
        session.sampleRate = dev->getCurrentSampleRate();
        session.bufferSize = dev->getCurrentBufferSizeSamples();
    }
    engine.setPatch (patch);
    rebuildStrips();
    refreshMonitorCombos();
    saveDeviceState();
}

void MainComponent::rebuildStrips()
{
    const auto ins = audio.inputNames(), outs = audio.outputNames();
    while (strips.size() > (int) patch.tracks.size()) strips.removeLast();
    while (strips.size() < (int) patch.tracks.size())
    {
        auto* s = strips.add (new ChannelStrip (strips.size()));
        s->onInput = [this] (int t, int in, bool st) { applyPatch ([this, t, in, st] (PatchState& p) { return setTrackInput (p, t, in, engine.isRecording(), st); }); };
        s->onOutput = [this] (int t, int o, bool st) { applyPatch ([t, o, st] (PatchState& p) { return setTrackOutput (p, t, o, st); }); };
        s->onTrim = [this] (int t, float db) { applyPatch ([t, db] (PatchState& p) { p.tracks[(size_t) t].trimDb = db; return juce::Result::ok(); }); };
        s->onArm = [this] (int t, bool v) { applyPatch ([this, t, v] (PatchState& p) { return setArmed (p, t, v, engine.isRecording()); }); };
        s->onMute = [this] (int t, bool v) { applyPatch ([t, v] (PatchState& p) { p.tracks[(size_t) t].mute = v; return juce::Result::ok(); }); };
        s->onSolo = [this] (int t, bool v) { applyPatch ([t, v] (PatchState& p) { p.tracks[(size_t) t].solo = v; return juce::Result::ok(); }); };
        s->onRename = [this] (int t, const juce::String& n) { applyPatch ([t, n] (PatchState& p) { return renameTrack (p, t, n); }); };
        s->onClearClip = [this] (int t) { engine.clearClip (t); };
        s->onSelect = [this] (int t, juce::ModifierKeys mods) { selectRow (t, mods); };
        s->onTrackMenu = [this] (int t) { showTrackMenu (t); };
        s->onRenameTab = [this] (int t, bool back)
        {
            const int next = t + (back ? -1 : 1);
            if (auto* ns = strips[next])
            {
                stripView.setViewPosition (juce::jmax (0, ns->getX() - stripView.getWidth() / 2), 0);
                juce::MessageManager::callAsync ([this, next] { if (auto* s2 = strips[next]) s2->startRename(); });
            }
        };
        s->onDragMove = [this] (int, int x)
        {
            stripDropIndex = juce::jlimit (0, strips.size(), (int) std::lround ((double) x / ChannelStrip::kWidth));
            stripDropMarker.setBounds (stripDropIndex * ChannelStrip::kWidth - 2, 0, 4, stripHolder.getHeight());
            stripDropMarker.setVisible (true);
            stripDropMarker.toFront (false);
        };
        s->onDragEnd = [this] (int, bool drop)
        {
            stripDropMarker.setVisible (false);
            if (drop && stripDropIndex >= 0) moveSelectedTo (stripDropIndex);
            stripDropIndex = -1;
        };
        stripHolder.addAndMakeVisible (s);
    }
    for (auto* s : strips) s->setChannelLists (ins, outs);
    bridge.setTracks (patch.tracks);
    refreshStrips();
    if (! engine.isRecording()) refreshTimeline (false);
    resized();
}

void MainComponent::refreshStrips()
{
    const bool rec = engine.isRecording();
    for (int i = 0; i < strips.size() && i < (int) patch.tracks.size(); ++i)
        strips[i]->showTrack (patch.tracks[(size_t) i], patch.mirror, rec);
    bar.mirrorToggle.setToggleState (patch.mirror, juce::dontSendNotification);
    bar.mirrorToggle.setEnabled (! rec);
    bar.addTrack.setEnabled (! rec);
    bar.removeTrack.setEnabled (! rec);
    bridge.setTracks (patch.tracks);
    timeline.setTracks (patch.tracks, rec, selectedIds);
    for (int i = 0; i < strips.size() && i < (int) patch.tracks.size(); ++i)
        strips[i]->setSelected (selectedIds.count (patch.tracks[(size_t) i].id) > 0);
    bar.monitorLeft.setSelectedId (patch.monitorLeft + 2, juce::dontSendNotification);
    bar.monitorRight.setSelectedId (patch.monitorRight + 2, juce::dontSendNotification);
}

void MainComponent::refreshMonitorCombos()
{
    const auto outs = audio.outputNames();
    for (auto* c : { &bar.monitorLeft, &bar.monitorRight })
    {
        c->clear (juce::dontSendNotification);
        c->addItem ("Off", 1);
        for (int i = 0; i < outs.size(); ++i) c->addItem (juce::String (i + 1) + "  " + outs[i], i + 2);
    }
    bar.monitorLeft.setSelectedId (patch.monitorLeft + 2, juce::dontSendNotification);
    bar.monitorRight.setSelectedId (patch.monitorRight + 2, juce::dontSendNotification);
}

// ------------------------------------------------------------------------------------ transport

void MainComponent::startRecording()
{
    if (engine.isRecording()) return;
    if (session.primaryRoot == juce::File())
    {
        newSession (session.name == "Untitled" ? juce::String ("Show") : session.name, [this] { startRecording(); });
        return;
    }
    if (audio.getDeviceManager().getCurrentAudioDevice() == nullptr)
    {
        alert ("No audio device", "Open Audio Setup and choose an input device first.", true);
        return;
    }
    session.nextTake = juce::jmax (session.nextTake, nextFreeTake (session.primaryRoot));
    auto r = engine.startRecording (session, patch);
    if (r.failed()) { alert ("Cannot record", r.getErrorMessage(), true); return; }
    timeline.clearPeaks();
    for (int t = 0; t < (int) patch.tracks.size(); ++t)
        if (auto pk = engine.target (0)->peaksFor (t))
        {
            timeline.setPeaks (t, pk);
            waveforms.put (engine.target (0)->fileFor (t), pk); // reused after Stop, no rebuild
        }
    timeline.setMarkers ({});
    timeline.setLength (0);
    timeline.setTitle ("RECORDING  Take " + juce::String (engine.currentTake()));
    timeline.zoomToFit();
    timeline.setFollow (true);
    lastBytes[0] = lastBytes[1] = 0;
    lastBytesTime = juce::Time::getMillisecondCounterHiRes();
    noAudioLogged = false;
    saveSession();
    refreshStrips();
}

void MainComponent::stopRecording()
{
    if (! engine.isRecording()) return;
    engine.stopRecording();
    for (int i = 0; i < 2; ++i)
        if (auto* t = engine.target (i); t != nullptr && t->droppedFrames() > 0)
            engine.log().post (Level::error, "Disk " + t->label() + " dropped " + juce::String ((juce::int64) t->droppedFrames())
                                             + " frames in this take (filled with silence, files stay aligned). See journal for positions.");
    saveSession();
    refreshTakes (true);
    refreshStrips();
}

void MainComponent::stopTransport()
{
    if (engine.isRecording()) { stopRecording(); return; }
    if (engine.isPlaying())
    {
        engine.stopPlayback();
        cueFrames = engine.playbackPosition();
        engine.log().post (Level::info, "Playback stopped at " + juce::String ((double) cueFrames / engine.sampleRate(), 1) + " s");
    }
    else returnToZero(); // a second Stop returns to zero
}

void MainComponent::returnToZero()
{
    cueFrames = 0;
    timeline.setPlayhead (0);
    if (engine.isPlaying()) startPlayback();
}

void MainComponent::startPlayback()
{
    if (engine.isRecording()) return;
    if (session.primaryRoot == juce::File()) { alert ("Playback", "Open a session first (Session > Open Session).", false); return; }
    const int take = bar.takeCombo.getSelectedId();
    if (take <= 0) { alert ("Playback", "This session has no recorded takes yet.", false); return; }
    if (audio.getDeviceManager().getCurrentAudioDevice() == nullptr) { alert ("Playback", "Choose an output device in Audio Setup first.", true); return; }
    const auto len = takeLengths.count (take) ? takeLengths[take] : 0;
    if (cueFrames >= len) cueFrames = 0;
    if (auto r = engine.startPlayback (resolveTakeFiles (session.primaryRoot, take, patch), take, cueFrames); r.failed())
        alert ("Cannot play take " + juce::String (take), r.getErrorMessage(), true);
    else timeline.setFollow (true);
}

void MainComponent::refreshTakes (bool selectNewest)
{
    const int previous = bar.takeCombo.getSelectedId();
    bar.takeCombo.clear (juce::dontSendNotification);
    takeLengths.clear();
    if (session.primaryRoot == juce::File()) return;

    for (const auto& [take, files] : scanTakes (session.primaryRoot))
        if (writeManifestIfMissing (session.primaryRoot, take, patch))
            engine.log().post (Level::info, "Take " + juce::String (take) + ": linked its files to the current tracks");

    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    for (const auto& [take, files] : scanTakes (session.primaryRoot))
    {
        uint64_t len = 0;
        if (! files.empty())
            if (std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (files.begin()->second)); r != nullptr)
                len = (uint64_t) r->lengthInSamples;
        takeLengths[take] = len;
        const double secs = (double) len / juce::jmax (1.0, engine.sampleRate());
        bar.takeCombo.addItem ("Take " + juce::String (take) + "  (" + juce::String ((int) files.size()) + " tr, "
                               + juce::String ((int) secs / 60) + ":" + juce::String ((int) secs % 60).paddedLeft ('0', 2) + ")", take);
    }
    const int n = bar.takeCombo.getNumItems();
    if (n == 0) return;
    if (selectNewest || bar.takeCombo.indexOfItemId (previous) < 0) bar.takeCombo.setSelectedItemIndex (n - 1, juce::dontSendNotification);
    else bar.takeCombo.setSelectedId (previous, juce::dontSendNotification);
    cueFrames = 0;
    refreshTimeline (true);
}

void MainComponent::refreshTimeline (bool fit)
{
    if (engine.isRecording()) return;
    const int take = bar.takeCombo.getSelectedId();
    timeline.setSampleRate (engine.sampleRate());
    timeline.clearPeaks();
    if (take > 0 && session.primaryRoot != juce::File())
    {
        for (const auto& [row, file] : resolveTakeFiles (session.primaryRoot, take, patch))
            timeline.setPeaks (row, waveforms.get (file));
    }
    timeline.setLength (takeLengths.count (take) ? takeLengths[take] : 0);
    timeline.setMarkers (loadMarkers (take));
    timeline.setTitle (take > 0 ? "Take " + juce::String (take) : juce::String ("No takes yet"));
    if (fit) timeline.zoomToFit();
}

std::vector<TimelineView::Marker> MainComponent::loadMarkers (int take) const
{
    std::vector<TimelineView::Marker> out;
    if (session.primaryRoot == juce::File() || take <= 0) return out;
    juce::StringArray lines;
    SessionPaths::markersDir (session.primaryRoot).getChildFile ("markers.csv").readLines (lines);
    for (const auto& l : lines)
    {
        juce::StringArray c;
        c.addTokens (l, ",", "\"");
        if (c.size() < 3 || c[0].getIntValue() != take || ! c[0].containsOnly ("0123456789")) continue;
        out.push_back ({ (uint64_t) c[2].getLargeIntValue(), c.size() >= 5 && c[4].isNotEmpty() ? c[4] : "M" + juce::String ((int) out.size() + 1) });
    }
    return out;
}

void MainComponent::showViewMenu()
{
    juce::PopupMenu m;
    m.addItem ("Show Mixer", true, showMixer, [this] { showMixer = ! showMixer; props.getUserSettings()->setValue ("showMixer", showMixer); resized(); });
    m.addItem ("Show Meter Bridge", true, showBridge, [this] { showBridge = ! showBridge; props.getUserSettings()->setValue ("showBridge", showBridge); resized(); });
    m.addItem ("Show Event Console", true, showConsole, [this] { showConsole = ! showConsole; props.getUserSettings()->setValue ("showConsole", showConsole); resized(); });
    m.addSeparator();
    m.addItem ("Zoom to Fit", [this] { timeline.zoomToFit(); });
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&bar.viewMenu));
}

juce::StringArray MainComponent::getMenuBarNames()
{
    return { "File", "Transport", "Disks", "View", "Help" };
}

static juce::PopupMenu::Item menuItem (const juce::String& text, const juce::String& shortcut, bool enabled, bool ticked,
                                       std::function<void()> fn)
{
    juce::PopupMenu::Item i (text);
    i.shortcutKeyDescription = shortcut;
    i.isEnabled = enabled;
    i.isTicked = ticked;
    i.action = std::move (fn);
    return i;
}

juce::PopupMenu MainComponent::getMenuForIndex (int index, const juce::String&)
{
    const bool rec = engine.isRecording();
    const bool hasSession = session.primaryRoot != juce::File();
   #if JUCE_MAC
    const juce::String cmd = juce::String::charToString (0x2318);   // the Command key symbol
   #else
    const juce::String cmd = "Ctrl+";
   #endif
    juce::PopupMenu m;
    switch (index)
    {
        case 0: // File
            m.addItem (menuItem ("New Session...", cmd + "N", ! rec, false, [this] { newSession ("Show"); }));
            m.addItem (menuItem ("Open Session...", cmd + "O", ! rec, false, [this] { openSession(); }));
            m.addItem (menuItem ("Open Template...", "", ! rec, false, [this] { openTemplate(); }));
            m.addSeparator();
            m.addItem (menuItem ("Save Session", cmd + "S", true, false, [this] { saveSession(); }));
            m.addItem (menuItem ("Save as Template...", "", true, false, [this] { saveTemplate(); }));
            m.addSeparator();
            {
                juce::PopupMenu format;
                format.addItem (menuItem ("24-bit PCM BWF", "", ! rec, session.bitsPerSample == 24, [this] { session.bitsPerSample = 24; saveSession(); }));
                format.addItem (menuItem ("32-bit float BWF", "", ! rec, session.bitsPerSample == 32, [this] { session.bitsPerSample = 32; saveSession(); }));
                m.addSubMenu ("Recording Format", format, ! rec);
            }
            m.addItem (menuItem ("Recover Session Folder...", "", ! rec, false, [this] { recoverFolder(); }));
            m.addItem (menuItem ("Reveal Session Folder", "", hasSession, false, [this] { session.primaryRoot.revealToUser(); }));
            break;

        case 1: // Transport
            m.addItem (menuItem ("Record", cmd + "R", ! rec, false, [this] { startRecording(); }));
            m.addItem (menuItem (engine.isPlaying() ? "Stop Playback" : "Play", "Space", ! rec, false,
                                 [this] { if (engine.isPlaying()) stopTransport(); else startPlayback(); }));
            m.addItem (menuItem ("Stop", cmd + ".", true, false, [this] { stopTransport(); }));
            m.addItem (menuItem ("Return to Zero", "Return", ! rec, false, [this] { returnToZero(); }));
            m.addItem (menuItem ("Add Marker", "M", hasSession, false, [this] { addMarker(); }));
            break;

        case 2: // Disks
            m.addItem (menuItem ("Change Primary Folder (Disk A)...", "", ! rec, false, [this] { newSession (session.name); }));
            m.addItem (menuItem ("Set Redundant Folder (Disk B)...", "", ! rec, false, [this] { chooseSecondary(); }));
            m.addItem (menuItem ("Remove Disk B", "", ! rec && session.secondaryRoot != juce::File(), false,
                                 [this] { session.secondaryRoot = juce::File(); saveSession(); engine.log().post (Level::info, "Disk B removed"); }));
            m.addSeparator();
            m.addItem (menuItem ("Benchmark Disks", "", ! rec && ! benchmarkRunning && hasSession, false, [this] { runBenchmark(); }));
            break;

        case 3: // View
            m.addItem (menuItem ("Show Mixer", "", true, showMixer, [this] { showMixer = ! showMixer; props.getUserSettings()->setValue ("showMixer", showMixer); resized(); }));
            m.addItem (menuItem ("Show Meter Bridge", "", true, showBridge, [this] { showBridge = ! showBridge; props.getUserSettings()->setValue ("showBridge", showBridge); resized(); }));
            m.addItem (menuItem ("Show Event Console", "", true, showConsole, [this] { showConsole = ! showConsole; props.getUserSettings()->setValue ("showConsole", showConsole); resized(); }));
            m.addSeparator();
            m.addItem (menuItem ("Zoom In", cmd + "=", true, false, [this] { timeline.zoomBy (1.0 / 1.6); }));
            m.addItem (menuItem ("Zoom Out", cmd + "-", true, false, [this] { timeline.zoomBy (1.6); }));
            m.addItem (menuItem ("Zoom to Fit", cmd + "0", true, false, [this] { timeline.zoomToFit(); }));
            m.addSeparator();
            m.addItem (menuItem ("Audio Setup...", cmd + ",", true, false, [this] { showAudioSetup(); }));
            break;

        case 4: // Help
            m.addItem (menuItem ("About LiveDeck", "", true, false, [this] { showAbout(); }));
            m.addItem (menuItem ("Check for Updates...", "", ! updateBusy, false, [this] { checkForUpdates (true); }));
            m.addItem (menuItem ("Check for Updates Automatically", "", true, autoUpdateCheck, [this]
            {
                autoUpdateCheck = ! autoUpdateCheck;
                props.getUserSettings()->setValue ("autoUpdateCheck", autoUpdateCheck);
                menuItemsChanged();
            }));
            m.addSeparator();
            m.addItem (menuItem ("Show Logs Folder", "", hasSession, false, [this] { SessionPaths::logsDir (session.primaryRoot).revealToUser(); }));
            break;

        default: break;
    }
    return m;
}

void MainComponent::showAbout()
{
    juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                      .withIconType (juce::MessageBoxIconType::InfoIcon)
                                      .withTitle ("LiveDeck " + juce::JUCEApplication::getInstance()->getApplicationVersion())
                                      .withMessage ("Multitrack recorder and virtual-soundcheck player for live events.\n"
                                                    "Broadcast WAV / RF64, redundant disks, crash recovery.\n\n"
                                                    "Wayne Goodman Productions")
                                      .withButton ("OK")
                                      .withAssociatedComponent (this),
                                  nullptr);
}

bool MainComponent::keyPressed (const juce::KeyPress& k)
{
    const auto mods = k.getModifiers();
    if (mods.isCommandDown())
    {
        const auto c = juce::CharacterFunctions::toLowerCase (k.getTextCharacter() != 0 ? k.getTextCharacter() : (juce::juce_wchar) k.getKeyCode());
        switch (c)
        {
            case 'n': if (! engine.isRecording()) newSession ("Show"); return true;
            case 'o': if (! engine.isRecording()) openSession(); return true;
            case 's': saveSession(); return true;
            case 'r': startRecording(); return true;
            case '.': stopTransport(); return true;
            case '=': case '+': timeline.zoomBy (1.0 / 1.6); return true;
            case '-': timeline.zoomBy (1.6); return true;
            case '0': timeline.zoomToFit(); return true;
            case ',': showAudioSetup(); return true;
            default: break;
        }
    }
    if (! mods.isAnyModifierKeyDown() && (k.getTextCharacter() == 'm' || k.getTextCharacter() == 'M')) { addMarker(); return true; }
    if ((k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey) && ! selectedIds.empty()) { deleteSelectedTracks(); return true; }
    if (k == juce::KeyPress::escapeKey && ! selectedIds.empty()) { selectedIds.clear(); refreshStrips(); return true; }
    if (mods.isCommandDown() && (k.getTextCharacter() == 'a' || k.getKeyCode() == 'A'))
    {
        for (const auto& t : patch.tracks) selectedIds.insert (t.id);
        refreshStrips();
        return true;
    }

    if (k == juce::KeyPress::spaceKey)
    {
        if (engine.isRecording()) engine.log().post (Level::info, "Recording: press STOP to end the take (space never stops a recording)");
        else if (engine.isPlaying()) stopTransport();
        else startPlayback();
        return true;
    }
    if (k == juce::KeyPress::returnKey || k == juce::KeyPress::homeKey) { if (! engine.isRecording()) returnToZero(); return true; }
    return false;
}

void MainComponent::addMarker()
{
    if (session.primaryRoot == juce::File()) return;
    const bool rec = engine.isRecording();
    const int take = rec ? engine.currentTake() : bar.takeCombo.getSelectedId();
    if (take <= 0) return;
    const uint64_t frame = rec ? engine.recordedFrames() : (engine.isPlaying() ? engine.playbackPosition() : cueFrames);
    const double secs = (double) frame / engine.sampleRate();
    const auto label = "M" + juce::String ((int) loadMarkers (take).size() + 1);
    const auto f = SessionPaths::markersDir (session.primaryRoot).getChildFile ("markers.csv");
    if (! f.existsAsFile()) f.replaceWithText ("take,seconds,sample,time_of_day,label\n", false, false, "\n");
    f.appendText (juce::String (take) + "," + juce::String (secs, 3) + "," + juce::String ((juce::int64) frame) + ","
                  + juce::Time::getCurrentTime().formatted ("%H:%M:%S") + "," + label + "\n");
    timeline.setMarkers (loadMarkers (take));
    engine.log().post (Level::info, "Marker " + label + " at " + juce::String (secs, 2) + " s (take " + juce::String (take) + ")");
}

// ------------------------------------------------------------------------------------ timer

void MainComponent::timerCallback()
{
    engine.housekeeping();

    for (int t = 0; t < (int) patch.tracks.size() && t < strips.size(); ++t)
    {
        const auto m = engine.readMeter (t);
        strips[t]->pushMeter (m.peak, m.rms, m.clip);
        bridge.push (t, m.peak, m.rms, m.clip);
    }
    bridge.repaint();

    for (const auto& e : engine.log().entriesSince (logIndex))
    {
        if (++consoleLines > 3000) { console.clear(); consoleLines = 0; }
        console.moveCaretToEnd();
        console.setColour (juce::TextEditor::textColourId,
                           e.level == Level::error ? Theme::red : (e.level == Level::warning ? Theme::yellow : Theme::textDim));
        console.insertTextAtCaret (e.time.formatted ("%H:%M:%S  ") + e.text + "\n");
    }

    // Driver watchdog: catches drivers that stall without reporting an error.
    if (engine.isRecording())
    {
        const double limit = juce::jmax (0.5, 8.0 * audio.bufferSize() / juce::jmax (1.0, engine.sampleRate()));
        const double since = engine.secondsSinceLastCallback();
        if (since > limit && ! noAudioLogged)
        {
            engine.log().post (Level::error, "NO AUDIO from driver for " + juce::String (since, 1) + " s while recording");
            noAudioLogged = true;
        }
        else if (since <= limit && noAudioLogged)
        {
            engine.log().post (Level::warning, "Driver audio resumed");
            noAudioLogged = false;
        }
    }

    if (engine.playbackFinished())
    {
        engine.stopPlayback();
        cueFrames = 0;
        engine.log().post (Level::info, "Playback reached the end of the take");
    }
    {
        const bool rec = engine.isRecording(), playing = engine.isPlaying();
        timeline.setSampleRate (engine.sampleRate());
        timeline.setTransport (playing, rec);
        if (rec) timeline.setLength (engine.recordedFrames());
        timeline.setPlayhead (rec ? engine.recordedFrames() : (playing ? engine.playbackPosition() : cueFrames));
    }

    if (getCurrentlyFocusedComponent() == nullptr && isShowing()) grabKeyboardFocus();

    if (tick == 150 && autoUpdateCheck)   // 5 s after launch, at most once a day
    {
        const auto last = props.getUserSettings()->getValue ("lastUpdateCheck", "0").getLargeIntValue();
        if (juce::Time::currentTimeMillis() - last > 20LL * 3600 * 1000) checkForUpdates (false);
    }

    if (++tick % 6 == 0)
    {
        updateStatus();
        // rebuild the native menus only when something they show has changed
        const int menuState = (engine.isRecording() ? 1 : 0) | (engine.isPlaying() ? 2 : 0) | (session.primaryRoot != juce::File() ? 4 : 0)
                            | (session.secondaryRoot != juce::File() ? 8 : 0) | (showMixer ? 16 : 0) | (showBridge ? 32 : 0)
                            | (showConsole ? 64 : 0) | (session.bitsPerSample == 32 ? 128 : 0) | (benchmarkRunning ? 256 : 0);
        if (menuState != lastMenuState) { lastMenuState = menuState; menuItemsChanged(); }
        if (! engine.isRecording() && ! engine.isPlaying()) timeline.repaint(); // waveforms still loading
    }
}

void MainComponent::updateStatus()
{
    TransportStatus s;
    s.recording = engine.isRecording();
    s.hasSession = session.primaryRoot != juce::File();
    s.sessionName = session.name;
    s.sessionPath = session.primaryRoot.getFullPathName();
    s.take = s.recording ? engine.currentTake() : session.nextTake;
    s.playing = engine.isPlaying();
    s.playTake = bar.takeCombo.getSelectedId();
    const double rate = juce::jmax (1.0, engine.sampleRate());
    s.recSeconds = s.recording ? (double) engine.recordedFrames() / rate
                 : s.playing   ? (double) engine.playbackPosition() / rate
                               : (double) cueFrames / rate;
    s.device = audio.describeDevice();
    s.cpu = audio.cpuLoad();
    s.dropouts = (int) engine.xrunCount() + audio.driverXruns();
    s.noAudio = noAudioLogged;
    const double measured = engine.measuredSampleRate();
    s.clockKnown = measured > 0 && engine.isDeviceRunning();
    s.ppm = s.clockKnown ? (measured / engine.sampleRate() - 1.0) * 1.0e6 : 0.0;

    int armed = 0;
    for (const auto& t : patch.tracks) armed += (t.armed && t.input >= 0) ? 1 : 0;
    const double bytesPerSec = engine.sampleRate() * (session.bitsPerSample / 8) * juce::jmax (1, armed);

    const double now = juce::Time::getMillisecondCounterHiRes();
    const double dt = (now - lastBytesTime) / 1000.0;
    for (int i = 0; i < 2; ++i)
    {
        auto& d = i == 0 ? s.diskA : s.diskB;
        const auto root = i == 0 ? session.primaryRoot : session.secondaryRoot;
        d.label = i == 0 ? "A" : "B";
        d.present = root != juce::File();
        if (! d.present) continue;
        const auto probe = root.exists() ? root : root.getParentDirectory();
        d.hoursLeft = (double) probe.getBytesFreeOnVolume() / bytesPerSec / 3600.0;
        if (auto* t = engine.target (i); t != nullptr && s.recording)
        {
            d.failed = t->failed.load();
            d.ringFill = t->maxRingFill();
            const auto b = t->bytesWritten();
            if (dt > 0.9) { mbps[i] = (double) (b - lastBytes[i]) / 1.0e6 / dt; lastBytes[i] = b; }
            d.mbPerSec = mbps[i];
        }
    }
    if (dt > 0.9) lastBytesTime = now;
    bar.setStatus (s);
}

// ------------------------------------------------------------------------------------ session

void MainComponent::setPrimaryRoot (const juce::File& root)
{
    session.primaryRoot = root;
    SessionPaths::createTree (root);
    session.nextTake = juce::jmax (1, nextFreeTake (root));
    engine.log().setLogFile (SessionPaths::logsDir (root).getChildFile ("events_" + juce::Time::getCurrentTime().formatted ("%Y-%m-%dT%H-%M-%S") + ".log"));
    waveforms.clear();
    engine.log().post (Level::info, "Session folder: " + root.getFullPathName());
    refreshTakes (true);
    const auto check = validateTarget (root, engine.sampleRate(), session.bitsPerSample, juce::jmax (1, (int) patch.tracks.size()));
    if (! check.ok) engine.log().post (Level::error, "Disk A: " + check.message);
    else if (check.message.isNotEmpty()) engine.log().post (Level::warning, "Disk A: " + check.message);
    else engine.log().post (Level::info, "Disk A: " + check.filesystem + ", " + juce::String (check.hoursAvailable, 1) + " h available");
}

void MainComponent::newSession (const juce::String& defaultName, std::function<void()> then)
{
    if (engine.isRecording()) return;
    askText ("New session", "Session name", defaultName, [this, then] (juce::String name)
    {
        chooseFolder ("Choose where to create the \"" + name + "\" session folder", [this, name, then] (juce::File parent)
        {
            const auto root = parent.getChildFile (juce::Time::getCurrentTime().formatted ("%Y-%m-%d") + "_" + SessionPaths::sanitise (name));
            session.name = name;
            session.secondaryRoot = juce::File();
            setPrimaryRoot (root);
            saveSession();
            if (then) then();
        });
    });
}

void MainComponent::openSession()
{
    if (engine.isRecording()) return;
    chooser = std::make_unique<juce::FileChooser> ("Open session", lastDir, "*.livedeck;*.omr");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc)
    {
        if (const auto file = fc.getResult(); file != juce::File()) openSessionFile (file);
    });
}

void MainComponent::openSessionFile (const juce::File& file)
{
    if (engine.isRecording()) { alert ("Recording in progress", "Stop recording before opening another session.", true); return; }
    if (file.hasFileExtension ("ldtemplate;omrt")) { openTemplateFile (file); return; }
    lastDir = file.getParentDirectory();
    auto loaded = std::make_shared<SessionSettings>();
    auto loadedPatch = std::make_shared<PatchState>();
    if (auto r = loadSession (file, *loaded, *loadedPatch); r.failed()) { alert ("Open session", r.getErrorMessage(), true); return; }
    // A session copied from another machine or drive records next to its .livedeck file.
    if (loaded->primaryRoot == juce::File() || ! loaded->primaryRoot.isDirectory()) loaded->primaryRoot = file.getParentDirectory();

    auto adopt = [this, loaded, loadedPatch] (bool keepFolder)
    {
        session = *loaded;
        patch = *loadedPatch;
        deviceChanged(); // fit the loaded patch to the current device
        if (keepFolder) setPrimaryRoot (session.primaryRoot); // also lists the takes for playback
        else newSession (session.name);
    };

    // The open-time storage prompt.
    auto askFolder = [this, loaded, adopt]
    {
        juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                          .withIconType (juce::MessageBoxIconType::QuestionIcon)
                                          .withTitle ("Where should this session record?")
                                          .withMessage ("Record new takes into:\n" + loaded->primaryRoot.getFullPathName())
                                          .withButton ("Keep this folder")
                                          .withButton ("Choose new folder")
                                          .withButton ("Cancel")
                                          .withAssociatedComponent (this),
                                      [adopt] (int result) { if (result == 1) adopt (true); else if (result == 2) adopt (false); });
    };

    if (journalHasUncleanTake (loaded->primaryRoot))
    {
        juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                          .withIconType (juce::MessageBoxIconType::WarningIcon)
                                          .withTitle ("Unfinished take found")
                                          .withMessage ("This session was not stopped cleanly (crash or power loss). Repair the audio files now? "
                                                        "All audio that reached the disk is kept.")
                                          .withButton ("Recover")
                                          .withButton ("Skip")
                                          .withAssociatedComponent (this),
                                      [this, loaded, askFolder] (int result)
                                      {
                                          if (result == 1)
                                              for (const auto& f : recoverSessionFolder (loaded->primaryRoot))
                                                  engine.log().post (f.ok ? Level::info : Level::error, "Recover " + f.file.getFileName() + ": " + f.message);
                                          askFolder();
                                      });
    }
    else askFolder();
}

void MainComponent::openTemplate()
{
    if (engine.isRecording()) return;
    chooser = std::make_unique<juce::FileChooser> ("Open template", lastDir, "*.ldtemplate;*.omrt;*.livedeck;*.omr");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc)
    {
        if (const auto file = fc.getResult(); file != juce::File()) openTemplateFile (file);
    });
}

void MainComponent::openTemplateFile (const juce::File& file)
{
    if (engine.isRecording()) { alert ("Recording in progress", "Stop recording before opening another session.", true); return; }
    lastDir = file.getParentDirectory();
    SessionSettings s; PatchState p;
    if (auto r = loadSession (file, s, p); r.failed()) { alert ("Open template", r.getErrorMessage(), true); return; }
    // Templates never record next to themselves: always ask for a new destination.
    session = s;
    session.primaryRoot = juce::File();
    session.secondaryRoot = juce::File();
    session.nextTake = 1;
    patch = p;
    deviceChanged();
    newSession (s.name);
}

void MainComponent::saveSession()
{
    if (session.primaryRoot == juce::File()) { newSession (session.name); return; }
    const auto r = omr::saveSession (SessionPaths::sessionFile (session.primaryRoot, session.name), session, patch);
    if (r.failed()) engine.log().post (Level::error, r.getErrorMessage());
}

void MainComponent::saveTemplate()
{
    chooser = std::make_unique<juce::FileChooser> ("Save template", lastDir.getChildFile (SessionPaths::sanitise (session.name) + ".ldtemplate"), "*.ldtemplate");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this] (const juce::FileChooser& fc)
    {
        auto f = fc.getResult();
        if (f == juce::File()) return;
        f = f.withFileExtension ("ldtemplate");
        lastDir = f.getParentDirectory();
        const auto r = omr::saveTemplate (f, session, patch);
        engine.log().post (r.wasOk() ? Level::info : Level::error, r.wasOk() ? "Template saved: " + f.getFullPathName() : r.getErrorMessage());
    });
}

void MainComponent::autosave()
{
    if (session.primaryRoot == juce::File() || ! SessionPaths::recoveryDir (session.primaryRoot).isDirectory()) return;
    omr::saveSession (SessionPaths::recoveryDir (session.primaryRoot).getChildFile (SessionPaths::sanitise (session.name) + ".livedeck.autosave"), session, patch);
}

void MainComponent::recoverFolder()
{
    chooseFolder ("Choose a session folder to recover", [this] (juce::File root)
    {
        const auto results = recoverSessionFolder (root);
        int repaired = 0, failed = 0;
        for (const auto& f : results)
        {
            repaired += f.repaired ? 1 : 0;
            failed += f.ok ? 0 : 1;
            engine.log().post (f.ok ? Level::info : Level::error, "Recover " + f.file.getFileName() + ": " + f.message);
        }
        alert ("Recovery", juce::String (results.size()) + " files checked, " + juce::String (repaired) + " repaired, " + juce::String (failed) + " failed.", failed > 0);
    });
}

void MainComponent::chooseSecondary()
{
    chooseFolder ("Choose the redundant (secondary) disk", [this] (juce::File parent)
    {
        const auto base = session.primaryRoot != juce::File() ? session.primaryRoot.getFileName()
                                                               : juce::Time::getCurrentTime().formatted ("%Y-%m-%d") + "_" + SessionPaths::sanitise (session.name);
        session.secondaryRoot = parent.getChildFile (base + "_B");
        const auto c = validateTarget (session.secondaryRoot, engine.sampleRate(), session.bitsPerSample, juce::jmax (1, (int) patch.tracks.size()));
        engine.log().post (c.ok ? (c.message.isEmpty() ? Level::info : Level::warning) : Level::error,
                           "Disk B: " + session.secondaryRoot.getFullPathName() + "  " + (c.message.isEmpty() ? juce::String (c.hoursAvailable, 1) + " h available" : c.message));
        saveSession();
    });
}

void MainComponent::runBenchmark()
{
    if (engine.isRecording() || benchmarkRunning.exchange (true)) return;
    int armed = 0;
    for (const auto& t : patch.tracks) armed += t.armed ? 1 : 0;
    const double need = engine.sampleRate() * (session.bitsPerSample / 8) * juce::jmax (1, armed);
    const auto roots = juce::Array<juce::File> { session.primaryRoot, session.secondaryRoot };
    const double ring = session.ringSeconds;
    engine.log().post (Level::info, "Benchmarking disks for 15 s each (" + juce::String (armed) + " tracks need " + juce::String (need / 1e6, 1) + " MB/s)...");

    juce::Thread::launch ([this, roots, need, ring, armed]
    {
        for (int i = 0; i < 2; ++i)
        {
            if (roots[i] == juce::File()) continue;
            const auto folder = roots[i].exists() ? roots[i] : roots[i].getParentDirectory();
            const auto r = runDiskBenchmark (folder, juce::jlimit (8, 64, armed), 15.0, need, ring);
            engine.log().post (r.completed && r.pass ? Level::info : Level::error, "Disk " + juce::String (i == 0 ? "A" : "B") + " benchmark: " + (r.completed ? r.message : r.message));
        }
        benchmarkRunning = false;
    });
}

void MainComponent::showAudioSetup()
{
    auto* selector = new juce::AudioDeviceSelectorComponent (audio.getDeviceManager(), 0, 256, 0, 256, false, false, false, false);
    selector->setSize (560, 520);
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (selector);
    o.dialogTitle = "Audio Setup (driver, sample rate, buffer size)";
    o.dialogBackgroundColour = Theme::panel;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.componentToCentreAround = this;
    o.launchAsync();
}

void MainComponent::showSessionMenu()
{
    const bool rec = engine.isRecording();
    juce::PopupMenu format;
    format.addItem ("24-bit PCM BWF", ! rec, session.bitsPerSample == 24, [this] { session.bitsPerSample = 24; saveSession(); });
    format.addItem ("32-bit float BWF", ! rec, session.bitsPerSample == 32, [this] { session.bitsPerSample = 32; saveSession(); });

    juce::PopupMenu m;
    m.addItem ("New Session...", ! rec, false, [this] { newSession ("Show"); });
    m.addItem ("Open Session...", ! rec, false, [this] { openSession(); });
    m.addItem ("Open Template...", ! rec, false, [this] { openTemplate(); });
    m.addSeparator();
    m.addItem ("Save Session", true, false, [this] { saveSession(); });
    m.addItem ("Save as Template...", true, false, [this] { saveTemplate(); });
    m.addSeparator();
    m.addSubMenu ("Recording Format", format, ! rec);
    m.addItem ("Recover Session Folder...", ! rec, false, [this] { recoverFolder(); });
    if (session.primaryRoot != juce::File())
        m.addItem ("Reveal Session Folder", true, false, [this] { session.primaryRoot.revealToUser(); });
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&bar.sessionMenu));
}

void MainComponent::showDisksMenu()
{
    const bool rec = engine.isRecording();
    juce::PopupMenu ring;
    for (int secs : { 10, 20, 30, 60 })
        ring.addItem (juce::String (secs) + " s  (" + juce::String (juce::roundToInt (secs * engine.sampleRate() * 4 * juce::jmax (1, (int) patch.tracks.size()) / 1.0e6)) + " MB per disk)",
                      ! rec, (int) session.ringSeconds == secs, [this, secs] { session.ringSeconds = secs; saveSession(); });

    juce::PopupMenu m;
    m.addItem ("Change Primary Folder (Disk A)...", ! rec, false, [this] { newSession (session.name); });
    m.addItem ("Set Redundant Folder (Disk B)...", ! rec, false, [this] { chooseSecondary(); });
    m.addItem ("Remove Disk B", ! rec && session.secondaryRoot != juce::File(), false, [this] { session.secondaryRoot = juce::File(); saveSession(); engine.log().post (Level::info, "Disk B removed"); });
    m.addSeparator();
    m.addItem ("Benchmark Disks", ! rec && ! benchmarkRunning && session.primaryRoot != juce::File(), false, [this] { runBenchmark(); });
    m.addSubMenu ("Buffer Depth", ring, ! rec);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&bar.disksMenu));
}

// ------------------------------------------------------------------------------------ dialogs

void MainComponent::askText (const juce::String& title, const juce::String& message, const juce::String& initial,
                             std::function<void (juce::String)> onOk)
{
    textDialog = std::make_unique<juce::AlertWindow> (title, message, juce::MessageBoxIconType::NoIcon, this);
    textDialog->addTextEditor ("text", initial);
    textDialog->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    textDialog->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    textDialog->enterModalState (true, juce::ModalCallbackFunction::create ([this, onOk] (int result)
    {
        const auto text = textDialog->getTextEditorContents ("text").trim();
        juce::MessageManager::callAsync ([this] { textDialog.reset(); });
        if (result == 1 && text.isNotEmpty()) onOk (text);
    }), false);
}

void MainComponent::chooseFolder (const juce::String& title, std::function<void (juce::File)> onChosen)
{
    chooser = std::make_unique<juce::FileChooser> (title, lastDir);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [this, onChosen] (const juce::FileChooser& fc)
    {
        const auto f = fc.getResult();
        if (f == juce::File()) return;
        lastDir = f;
        onChosen (f);
    });
}

void MainComponent::alert (const juce::String& title, const juce::String& message, bool error)
{
    if (error) engine.log().post (Level::error, title + ": " + message);
    juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                      .withIconType (error ? juce::MessageBoxIconType::WarningIcon : juce::MessageBoxIconType::InfoIcon)
                                      .withTitle (title).withMessage (message).withButton ("OK").withAssociatedComponent (this),
                                  nullptr);
}

// ------------------------------------------------------------------------------------ updates

void MainComponent::checkForUpdates (bool userInitiated)
{
    if (updateBusy.exchange (true)) return;
    props.getUserSettings()->setValue ("lastUpdateCheck", juce::String (juce::Time::currentTimeMillis()));
    const auto current = juce::JUCEApplication::getInstance()->getApplicationVersion();
    juce::Component::SafePointer<MainComponent> safe (this);

    juce::Thread::launch ([safe, userInitiated, current]
    {
        update::ReleaseInfo info;
        const auto r = update::fetchLatestRelease (update::configuredRepository(), info);
        juce::MessageManager::callAsync ([safe, userInitiated, current, info, r]
        {
            if (safe == nullptr) return;
            safe->updateBusy = false;
            if (r.failed())
            {
                if (userInitiated) safe->alert ("Check for Updates", r.getErrorMessage(), false);
                else safe->engine.log().post (Level::info, "Update check: " + r.getErrorMessage());
                return;
            }
            const auto have = update::Version::parse (current);
            if (! (have < info.version))
            {
                if (userInitiated) safe->alert ("You're up to date", "LiveDeck " + current + " is the latest version.", false);
                return;
            }
            if (! userInitiated && safe->props.getUserSettings()->getValue ("skippedVersion") == info.version.toString()) return;
            safe->engine.log().post (Level::info, "LiveDeck " + info.version.toString() + " is available (you have " + current + ")");
            safe->showUpdateOffer ({ info, current });
        });
    });
}

void MainComponent::showUpdateOffer (const UpdateOffer& offer)
{
    auto shared = std::make_shared<UpdateOffer> (offer);
    const bool canInstall = update::currentAppBundle() != juce::File();

    auto* w = new juce::AlertWindow ("LiveDeck " + offer.release.version.toString() + " is available",
                                     "You have " + offer.currentVersion + ". What's new:", juce::MessageBoxIconType::InfoIcon, this);
    auto* notes = new juce::TextEditor();
    notes->setMultiLine (true);
    notes->setReadOnly (true);
    notes->setScrollbarsShown (true);
    notes->setSize (520, 220);
    notes->setText (offer.release.notes.isNotEmpty() ? offer.release.notes : juce::String ("(no release notes)"));
    w->addCustomComponent (notes);
    w->addButton (canInstall ? "Install and Relaunch" : "Open Release Page", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Skip This Version", 2);
    w->addButton ("Later", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    juce::Component::SafePointer<MainComponent> safe (this);
    w->enterModalState (true, juce::ModalCallbackFunction::create ([safe, shared, canInstall, notes] (int result)
    {
        juce::MessageManager::callAsync ([notes] { delete notes; }); // after the alert window is gone
        if (safe == nullptr) return;
        if (result == 2) { safe->props.getUserSettings()->setValue ("skippedVersion", shared->release.version.toString()); return; }
        if (result != 1) return;
        if (! canInstall) { juce::URL (shared->release.pageUrl).launchInDefaultBrowser(); return; }
        safe->installUpdate (shared);
    }), true);
}

void MainComponent::installUpdate (std::shared_ptr<UpdateOffer> offer)
{
    if (engine.isRecording() || engine.isPlaying())
    {
        alert ("Update postponed", "Stop recording and playback first. LiveDeck never updates during a take.", false);
        return;
    }
    if (updateBusy.exchange (true)) return;

    updateCancel = false;
    updateProgress = 0.0;
    updateWindow = std::make_unique<juce::AlertWindow> ("Downloading LiveDeck " + offer->release.version.toString(),
                                                        "The app will restart when the update is installed.",
                                                        juce::MessageBoxIconType::NoIcon, this);
    updateWindow->addProgressBarComponent (updateProgress);
    updateWindow->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    updateWindow->enterModalState (true, juce::ModalCallbackFunction::create ([this] (int) { updateCancel = true; }), false);

    saveSession();
    juce::Component::SafePointer<MainComponent> safe (this);
    double* progress = &updateProgress;
    std::atomic<bool>* cancel = &updateCancel;

    juce::Thread::launch ([safe, offer, progress, cancel]
    {
        const auto& rel = offer->release;
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("LiveDeck-download");
        dir.createDirectory();
        const auto zip = dir.getChildFile ("LiveDeck-" + rel.version.toString() + "-mac.zip");
        juce::File script;

        auto r = update::downloadFile (rel.zipUrl, zip, [progress, cancel] (double p) { if (p >= 0) *progress = p; return ! cancel->load(); });
        juce::String shaText;
        if (r.wasOk()) r = update::downloadText (rel.shaUrl, shaText);
        if (r.wasOk())
        {
            const auto expected = update::parseSha256File (shaText);
            const auto actual = update::sha256OfFile (zip);
            if (expected.isEmpty() || expected != actual)
                r = juce::Result::fail ("The download does not match its published checksum, so it was not installed.");
        }
        if (r.wasOk()) r = update::prepareInstall (zip, update::currentAppBundle(), rel.version, script);
        zip.deleteFile();

        juce::MessageManager::callAsync ([safe, r, script, version = rel.version.toString()]
        {
            if (safe == nullptr) return;
            safe->updateWindow.reset();
            safe->updateBusy = false;
            if (r.failed())
            {
                if (r.getErrorMessage() != "Cancelled") safe->alert ("Update failed", r.getErrorMessage(), true);
                return;
            }
            if (safe->engine.isRecording() || safe->engine.isPlaying())
            {
                safe->alert ("Update postponed", "A take was started during the download; the update was not installed.", false);
                return;
            }
            if (auto lr = update::launchDetached (script); lr.failed()) { safe->alert ("Update failed", lr.getErrorMessage(), true); return; }
            safe->engine.log().post (Level::info, "Installing LiveDeck " + version + " and relaunching");
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        });
    });
}

// ------------------------------------------------------------------------------------ track editing

std::vector<int> MainComponent::selectedRows() const
{
    std::vector<int> rows;
    for (int i = 0; i < (int) patch.tracks.size(); ++i)
        if (selectedIds.count (patch.tracks[(size_t) i].id) > 0) rows.push_back (i);
    return rows;
}

void MainComponent::selectRow (int row, juce::ModifierKeys mods)
{
    if (row < 0 || row >= (int) patch.tracks.size()) return;
    const int id = patch.tracks[(size_t) row].id;
    if (mods.isShiftDown() && anchorRow >= 0 && anchorRow < (int) patch.tracks.size())
    {
        if (! mods.isCommandDown()) selectedIds.clear();
        for (int i = juce::jmin (anchorRow, row); i <= juce::jmax (anchorRow, row); ++i)
            selectedIds.insert (patch.tracks[(size_t) i].id);
    }
    else if (mods.isCommandDown())
    {
        if (selectedIds.count (id) > 0) selectedIds.erase (id); else selectedIds.insert (id);
        anchorRow = row;
    }
    else
    {
        // a plain click on an already-selected track keeps the selection so it can be dragged as a block
        if (selectedIds.count (id) == 0) { selectedIds.clear(); selectedIds.insert (id); }
        anchorRow = row;
    }
    refreshStrips();
}

void MainComponent::showTrackMenu (int row)
{
    if (row < 0 || row >= (int) patch.tracks.size()) return;
    if (selectedIds.count (patch.tracks[(size_t) row].id) == 0) { selectedIds = { patch.tracks[(size_t) row].id }; anchorRow = row; refreshStrips(); }
    const auto rows = selectedRows();
    const int n = (int) rows.size();
    const bool rec = engine.isRecording();

    juce::PopupMenu m;
    m.addSectionHeader (n == 1 ? patch.tracks[(size_t) row].name : juce::String (n) + " tracks selected");
    m.addItem ("Rename", n == 1, false, [this, row] { timeline.startRename (row); });
    juce::PopupMenu colours;
    const auto& pal = Theme::trackPalette();
    for (int k = 0; k < (int) pal.size(); ++k)
        colours.addColouredItem (k + 1, "Colour " + juce::String (k + 1), pal[(size_t) k], true, false);
    colours.addSeparator();
    colours.addItem (100, "Automatic");
    m.addSubMenu ("Colour", colours);
    m.addSeparator();
    m.addItem (n == 1 ? juce::String ("Delete Track") : "Delete " + juce::String (n) + " Tracks", ! rec, false, [this] { deleteSelectedTracks(); });

    m.showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [this, rows] (int result)
    {
        if (result >= 1 && result <= 100)
        {
            const int c = result == 100 ? -1 : result - 1;
            applyPatch ([rows, c] (PatchState& p) { for (int r : rows) if (r < (int) p.tracks.size()) p.tracks[(size_t) r].colour = c; return juce::Result::ok(); });
        }
    });
}

void MainComponent::stopPlaybackForEdit()
{
    if (! engine.isPlaying()) return;
    engine.stopPlayback();
    cueFrames = engine.playbackPosition();
}

void MainComponent::deleteSelectedTracks()
{
    const auto rows = selectedRows();
    if (rows.empty()) return;
    if (engine.isRecording()) { engine.log().post (Level::warning, "Tracks cannot be deleted while recording"); return; }

    juce::StringArray names;
    for (int r : rows) names.add (patch.tracks[(size_t) r].name);
    const auto title = rows.size() == 1 ? "Delete \"" + names[0] + "\"?" : "Delete " + juce::String ((int) rows.size()) + " tracks?";
    juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                      .withIconType (juce::MessageBoxIconType::QuestionIcon)
                                      .withTitle (title)
                                      .withMessage ((rows.size() > 1 ? names.joinIntoString (", ") + "\n\n" : juce::String())
                                                    + "Recorded audio files are kept in the session folder.")
                                      .withButton ("Delete")
                                      .withButton ("Cancel")
                                      .withAssociatedComponent (this),
                                  [this, rows] (int result)
                                  {
                                      if (result != 1) return;
                                      stopPlaybackForEdit();
                                      applyPatch ([this, rows] (PatchState& p) { return deleteTracks (p, rows, engine.isRecording()); });
                                      selectedIds.clear();
                                      anchorRow = -1;
                                      refreshStrips();
                                      refreshTimeline (false);
                                  });
}

void MainComponent::moveSelectedTo (int insertBefore)
{
    const auto rows = selectedRows();
    if (rows.empty()) return;
    stopPlaybackForEdit();
    std::vector<int> newRows;
    applyPatch ([this, rows, insertBefore, &newRows] (PatchState& p) { return moveTracks (p, rows, insertBefore, engine.isRecording(), &newRows); });
    if (! newRows.empty()) anchorRow = newRows.front();
    refreshTimeline (false);   // waveforms follow their tracks (take manifest)
}
