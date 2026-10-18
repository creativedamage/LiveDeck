#include "MainComponent.h"
#include "Core/Benchmark.h"
#include "Core/Recovery.h"
#include "Core/Updater.h"
#include "Core/AudioImport.h"
#include "FeedbackDialog.h"

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
    // ~/Documents/LiveDeck/Shows and /Templates (created on first launch, re-created if deleted).
    Library::ensure();
    lastDir = juce::File (props.getUserSettings()->getValue ("lastDir", Library::shows().getFullPathName()));
    if (! lastDir.isDirectory()) lastDir = Library::shows();

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
    timeline.onFilesDropped = [this] (const juce::StringArray& files, uint64_t frame, int row) { importAudioFiles (files, frame, row); };
    timeline.onClipMoved = [this] (int fromRow, int toRow, uint64_t offset) { moveClipTo (fromRow, toRow, offset); };
    timeline.onClipMenu = [this] (int row, uint64_t frame) { showClipMenu (row, frame); };
    timeline.onClipClicked = [this] (int row, juce::ModifierKeys mods) { selectClip (row, mods); };
    timeline.onClipSelectionCleared = [this] { clearClipSelection(); };
    timeline.onClipsShifted = [this] (std::vector<int>, juce::int64 delta) { shiftSelectedClips (delta, "Move Clips"); };
    timeline.canAcceptDrop = [this] { return ! engine.isRecording(); };

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

    remote = std::make_unique<RemoteControl> (*props.getUserSettings());
    remote->onCommand = [this] (const omr::remote::Command& c) { handleRemote (c); };
    remote->getStatus = [this] { return remoteStatus(); };
    remote->onLog = [this] (const juce::String& t, bool err) { engine.log().post (err ? Level::warning : Level::info, t); };

    audio.onDeviceChanged = [this] { deviceChanged(); };
    std::unique_ptr<juce::XmlElement> saved (props.getUserSettings()->getXmlValue ("audioDevice"));
    audio.initialise (saved.get(), juce::JSON::parse (props.getUserSettings()->getValue ("extraDevices", "[]")));
    deviceChanged();

    engine.log().post (Level::info, "LiveDeck ready. Choose Session > New Session to pick where audio is recorded.");
    setSize (1600, 940);
    startTimerHz (30);

    // Licensing: every launch checks this computer's license (signed token, 14 days offline grace).
   #if LIVEDECK_LICENSING
    account = std::make_unique<AccountService> (*props.getUserSettings());
    gate = std::make_unique<LicenseGate> (*account);
    gate->onLicensed = [this] (const AccountService::Check& c) { licenseGranted (c); };
    gate->onOpenAdmin = [this] { showAdminPanel(); };
    addAndMakeVisible (*gate);
    resized();
    gate->start();
   #else
    licensed = true;
   #endif
}

MainComponent::~MainComponent()
{
    stopTimer();
    gate.reset();
    if (account != nullptr && licensed) account->release (true);   // frees this computer's seat right away
    if (importCancel) importCancel->store (true);
    remote.reset();
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
    props.getUserSettings()->setValue ("extraDevices", juce::JSON::toString (audio.extraDevicesState(), true));
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
    if (gate != nullptr) { gate->setBounds (r); gate->toFront (false); }
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
    if (wasMirror && ! patch.mirror) engine.log().post (Level::info, "Mirror off: an output was patched by hand. Turn it back on to make outputs follow inputs again.");
    engine.setPatch (patch);
    warnDeadOutputs();
    if (patch.tracks.size() != before) rebuildStrips();
    else refreshStrips();
    autosave();
}

void MainComponent::deviceChanged()
{
    const auto r = setDeviceChannels (patch, audio.inputNames(), audio.outputNames(), audio.inputActive(), audio.outputActive());
    patch.devices = audio.layouts();
    if (r.failed() && r.getErrorMessage() != lastDeviceWarning) engine.log().post (Level::warning, r.getErrorMessage());
    lastDeviceWarning = r.failed() ? r.getErrorMessage() : juce::String();
    if (auto* dev = audio.getDeviceManager().getCurrentAudioDevice())
    {
        session.deviceType = dev->getTypeName();
        session.deviceName = dev->getName();
        session.sampleRate = dev->getCurrentSampleRate();
        session.bufferSize = dev->getCurrentBufferSizeSamples();
    }
    engine.setPatch (patch);
    warnDeadOutputs();
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
    const auto inAct = audio.inputActive(), outAct = audio.outputActive();
    for (auto* s : strips) s->setChannelLists (ins, outs, inAct, outAct);
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
    bar.mirrorToggle.setEnabled (true);   // only ties outputs to inputs: safe during a take
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
    if (engine.isRecording() || ! licensed) return;
    if (importRunning && importCancel != nullptr)
    {
        // Recording always wins: an import competing for the disk is stopped (its partial files are removed).
        importCancel->store (true);
        engine.log().post (Level::warning, "Audio import stopped: recording has priority. Drop the files again afterwards.");
    }
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
    if (! licensed) return;
    if (engine.isRecording()) return;
    if (session.primaryRoot == juce::File()) { alert ("Playback", "Open a session first (Session > Open Session).", false); return; }
    const int take = bar.takeCombo.getSelectedId();
    if (take <= 0) { alert ("Playback", "This session has no recorded takes yet.", false); return; }
    if (audio.getDeviceManager().getCurrentAudioDevice() == nullptr) { alert ("Playback", "Choose an output device in Audio Setup first.", true); return; }
    const auto len = takeLengths.count (take) ? takeLengths[take] : 0;
    if (cueFrames >= len) cueFrames = 0;
    if (auto r = engine.startPlayback (resolveTakeClips (session.primaryRoot, take, patch), take, cueFrames); r.failed())
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

    for (const auto& [take, files] : scanTakes (session.primaryRoot))
    {
        const uint64_t len = takeLength (resolveTakeClips (session.primaryRoot, take, patch));   // end of the last clip
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
        for (const auto& [row, clip] : resolveTakeClips (session.primaryRoot, take, patch))
            timeline.setPeaks (row, waveforms.get (clip.file), clip.offset, clip.name);
    }
    timeline.setLength (takeLengths.count (take) ? takeLengths[take] : 0);
    timeline.setMarkers (loadMarkers (take));
    timeline.setTitle (take > 0 ? "Take " + juce::String (take) : juce::String ("No takes yet"));
    if (fit) timeline.zoomToFit();
    pushClipSelection();
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
    return { "File", "Edit", "Transport", "Tracks", "Disks", "View", "Account", "Help" };
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
            addTemplateItems (m, ! rec);
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
            m.addItem (menuItem ("Reveal LiveDeck Folder", "", true, false, [this] { revealLibrary(); }));
            break;

        case 1: // Edit
            m.addItem (menuItem (undoStack.empty() ? juce::String ("Undo") : "Undo " + undoStack.back().label, cmd + "Z",
                                 ! rec && ! undoStack.empty(), false, [this] { undo(); }));
            m.addSeparator();
            m.addItem (menuItem ("Select All Clips", cmd + "A", ! rec && bar.takeCombo.getSelectedId() > 0, false, [this] { selectAllClips(); }));
            m.addItem (menuItem ("Deselect Clips", "Esc", ! selectedClipIds.empty(), false, [this] { clearClipSelection(); }));
            m.addSeparator();
            m.addItem (menuItem ("Move Selected Clips to Start", "", ! rec && ! selectedClipIds.empty(), false, [this]
            {
                juce::int64 earliest = std::numeric_limits<juce::int64>::max();
                for (const auto& [row, c] : resolveTakeClips (session.primaryRoot, bar.takeCombo.getSelectedId(), patch))
                    if (selectedClipIds.count (patch.tracks[(size_t) row].id)) earliest = juce::jmin (earliest, (juce::int64) c.offset);
                if (earliest != std::numeric_limits<juce::int64>::max()) shiftSelectedClips (-earliest, "Move Clips to Start");
            }));
            m.addItem (menuItem ("Remove Selected Clips from Tracks", "Delete", ! rec && ! selectedClipIds.empty(), false, [this] { removeSelectedClips(); }));
            break;

        case 2: // Transport
            m.addItem (menuItem ("Record", cmd + "R", ! rec, false, [this] { startRecording(); }));
            m.addItem (menuItem (engine.isPlaying() ? "Stop Playback" : "Play", "Space", ! rec, false,
                                 [this] { if (engine.isPlaying()) stopTransport(); else startPlayback(); }));
            m.addItem (menuItem ("Stop", cmd + ".", true, false, [this] { stopTransport(); }));
            m.addItem (menuItem ("Return to Zero", "Return", ! rec, false, [this] { returnToZero(); }));
            m.addItem (menuItem ("Add Marker", "M", hasSession, false, [this] { addMarker(); }));
            m.addSeparator();
            m.addItem (menuItem ("Remote Control (OSC / MIDI)...", "", true, false, [this] { showRemoteSettings(); }));
            break;

        case 3: // Tracks
            m.addItem (menuItem ("Add Track", "", ! rec, false, [this] { applyPatch ([this] (PatchState& p) { return addTrack (p, engine.isRecording()); }); }));
            m.addItem (menuItem ("Add Multiple Tracks...", "", ! rec, false, [this] { showAddTracksDialog(); }));
            m.addSeparator();
            m.addItem (menuItem ("Select All Tracks", juce::String::charToString (0x21e7) + cmd + "A", ! patch.tracks.empty(), false, [this] { for (const auto& t : patch.tracks) selectedIds.insert (t.id); refreshStrips(); }));
            m.addItem (menuItem ("Delete Selected Tracks", "Delete", ! rec && ! selectedIds.empty(), false, [this] { deleteSelectedTracks(); }));
            m.addSeparator();
            m.addItem (menuItem ("Mirror Input to Output", "", true, patch.mirror, [this]
            {
                const bool want = ! patch.mirror;
                applyPatch ([want] (PatchState& p) { return setMirror (p, want, false); });
            }));
            break;

        case 4: // Disks
            m.addItem (menuItem ("Change Primary Folder (Disk A)...", "", ! rec, false, [this] { newSession (session.name); }));
            m.addItem (menuItem ("Set Redundant Folder (Disk B)...", "", ! rec, false, [this] { chooseSecondary(); }));
            m.addItem (menuItem ("Remove Disk B", "", ! rec && session.secondaryRoot != juce::File(), false,
                                 [this] { session.secondaryRoot = juce::File(); saveSession(); engine.log().post (Level::info, "Disk B removed"); }));
            m.addSeparator();
            m.addItem (menuItem ("Benchmark Disks", "", ! rec && ! benchmarkRunning && hasSession, false, [this] { runBenchmark(); }));
            break;

        case 5: // View
            m.addItem (menuItem ("Show Mixer", "", true, showMixer, [this] { showMixer = ! showMixer; props.getUserSettings()->setValue ("showMixer", showMixer); resized(); }));
            m.addItem (menuItem ("Show Meter Bridge", "", true, showBridge, [this] { showBridge = ! showBridge; props.getUserSettings()->setValue ("showBridge", showBridge); resized(); }));
            m.addItem (menuItem ("Show Event Console", "", true, showConsole, [this] { showConsole = ! showConsole; props.getUserSettings()->setValue ("showConsole", showConsole); resized(); }));
            m.addSeparator();
            m.addItem (menuItem ("Zoom In", cmd + "=", true, false, [this] { timeline.zoomBy (1.0 / 1.6); }));
            m.addItem (menuItem ("Zoom Out", cmd + "-", true, false, [this] { timeline.zoomBy (1.6); }));
            m.addItem (menuItem ("Zoom to Fit", cmd + "0", true, false, [this] { timeline.zoomToFit(); }));
            m.addSeparator();
            m.addItem (menuItem ("Audio Setup...", cmd + ",", true, false, [this] { showAudioSetup(); }));
            m.addItem (menuItem ("Turn On All Inputs and Outputs", "", ! rec, false, [this]
            {
                if (audio.enableAllChannels()) engine.log().post (Level::info, "All inputs and outputs turned on: " + audio.describeDevice());
                else alert ("Turn on all channels", "Couldn't change the device's channels. Open Audio Setup to check the device.", true);
            }));
            m.addItem (menuItem ("Additional Audio Devices...", "", ! rec, false, [this] { showExtraDevices(); }));
            break;

        case 6: // Account
            if (account == nullptr) { m.addItem (menuItem ("Licensing is off in this build", "", false, false, nullptr)); break; }
            m.addItem (menuItem (account->isSignedIn() ? "Signed in as " + account->email() : juce::String ("Not signed in"), "", false, false, nullptr));
            m.addSeparator();
            m.addItem (menuItem ("Account and License...", "", licensed, false, [this] { showAccountPanel(); }));
            m.addItem (menuItem ("Edit Registration...", "", licensed, false, [this] { showRegistration(); }));
            m.addItem (menuItem ("Change Password...", "", licensed, false, [this] { changePassword(); }));
            if (account->lastCheck().isAdmin())
                m.addItem (menuItem ("Admin Panel...", "", true, false, [this] { showAdminPanel(); }));
            m.addSeparator();
            m.addItem (menuItem ("Sign Out of This Computer", "", account->isSignedIn() && ! rec, false, [this] { signOutOfAccount(); }));
            break;

        case 7: // Help
            m.addItem (menuItem ("About LiveDeck", "", true, false, [this] { showAbout(); }));
            m.addItem (menuItem ("Check for Updates...", "", ! updateBusy, false, [this] { checkForUpdates (true); }));
            m.addItem (menuItem ("Check for Updates Automatically", "", true, autoUpdateCheck, [this]
            {
                autoUpdateCheck = ! autoUpdateCheck;
                props.getUserSettings()->setValue ("autoUpdateCheck", autoUpdateCheck);
                menuItemsChanged();
            }));
            m.addSeparator();
            m.addItem (menuItem ("Request a Feature...", "", true, false, [this] { showFeedback (0); }));
            m.addItem (menuItem ("Report a Bug...", "", true, false, [this] { showFeedback (1); }));
            m.addItem (menuItem ("Send Feedback...", "", true, false, [this] { showFeedback (2); }));
            m.addItem (menuItem ("Feature Requests and Known Issues (GitHub)", "", update::configuredRepository().isNotEmpty(), false,
                                 [] { juce::URL (feedback::issuesUrl (update::configuredRepository())).launchInDefaultBrowser(); }));
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
        // Ctrl+letter can arrive as a control character (Windows / Linux): use the key code then.
        const auto c = juce::CharacterFunctions::toLowerCase (k.getTextCharacter() >= 32 ? k.getTextCharacter() : (juce::juce_wchar) k.getKeyCode());
        switch (c)
        {
            case 'n': if (! engine.isRecording()) newSession ("Show"); return true;
            case 'o': if (! engine.isRecording()) openSession(); return true;
            case 's': saveSession(); return true;
            case 'r': startRecording(); return true;
            case 'z': undo(); return true;
            case '.': stopTransport(); return true;
            case '=': case '+': timeline.zoomBy (1.0 / 1.6); return true;
            case '-': timeline.zoomBy (1.6); return true;
            case '0': timeline.zoomToFit(); return true;
            case ',': showAudioSetup(); return true;
            default: break;
        }
    }
    if (! mods.isAnyModifierKeyDown() && (k.getTextCharacter() == 'm' || k.getTextCharacter() == 'M')) { addMarker(); return true; }
    if ((k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey) && ! selectedClipIds.empty()) { removeSelectedClips(); return true; }
    if ((k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey) && ! selectedIds.empty()) { deleteSelectedTracks(); return true; }
    if (k == juce::KeyPress::escapeKey && ! selectedClipIds.empty()) { clearClipSelection(); return true; }
    if (k == juce::KeyPress::escapeKey && ! selectedIds.empty()) { selectedIds.clear(); refreshStrips(); return true; }
    if (mods.isCommandDown() && (k.getKeyCode() == 'A' || k.getKeyCode() == 'a' || k.getTextCharacter() == 'a' || k.getTextCharacter() == 'A'))
    {
        if (mods.isShiftDown())   // Shift+Cmd+A: all tracks
        {
            for (const auto& t : patch.tracks) selectedIds.insert (t.id);
            refreshStrips();
        }
        else selectAllClips();    // Cmd+A: all clips (audio files) in the take on screen
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

void MainComponent::addMarker (const juce::String& requestedLabel)
{
    if (session.primaryRoot == juce::File()) return;
    const bool rec = engine.isRecording();
    const int take = rec ? engine.currentTake() : bar.takeCombo.getSelectedId();
    if (take <= 0) return;
    const uint64_t frame = rec ? engine.recordedFrames() : (engine.isPlaying() ? engine.playbackPosition() : cueFrames);
    const double secs = (double) frame / engine.sampleRate();
    const auto clean = requestedLabel.removeCharacters (",\"\r\n").trim().substring (0, 40);
    const auto label = clean.isNotEmpty() ? clean : "M" + juce::String ((int) loadMarkers (take).size() + 1);
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
    if (licensed && account != nullptr && juce::Time::getMillisecondCounterHiRes() - lastHeartbeatMs > AccountService::kHeartbeatMinutes * 60000.0)
        heartbeat();

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
        const double since = engine.worstSecondsSinceCallback();
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
    s.devices = engine.numRunningDevices();
    for (int d = 1; d < omr::kMaxDevices; ++d)
    {
        if (! engine.isDeviceRunning (d) || engine.measuredSampleRate (d) <= 0 || ! s.clockKnown) continue;
        const double drift = engine.driftPpm (d);
        s.driftKnown = true;
        s.worstDrift = std::abs (drift) > std::abs (s.worstDrift) ? drift : s.worstDrift;
        if (std::abs (drift) > 5.0 && ! driftWarned[(size_t) d])
        {
            driftWarned[(size_t) d] = true;
            engine.log().post (Level::warning, "Device " + juce::String (d + 1) + " drifts " + juce::String (drift, 1)
                                               + " ppm against the primary: it is not on the same word clock. Its tracks are recorded at its own clock (never resampled); "
                                                 "clock both devices from one source to keep them sample-locked.");
        }
    }

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
    Library::ensure();
    textDialog = std::make_unique<juce::AlertWindow> ("New session",
                                                      "Created in Documents > LiveDeck > Shows, or choose another folder (an external SSD, for example).",
                                                      juce::MessageBoxIconType::NoIcon, this);
    textDialog->addTextEditor ("text", defaultName, "Session name");
    textDialog->addButton ("Create", 1, juce::KeyPress (juce::KeyPress::returnKey));
    textDialog->addButton ("Choose Folder...", 2);
    textDialog->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    textDialog->enterModalState (true, juce::ModalCallbackFunction::create ([this, then] (int result)
    {
        const auto name = textDialog->getTextEditorContents ("text").trim();
        juce::MessageManager::callAsync ([this] { textDialog.reset(); });
        if (result == 0 || name.isEmpty()) return;

        auto create = [this, name, then] (const juce::File& parent)
        {
            const auto root = parent.getChildFile (juce::Time::getCurrentTime().formatted ("%Y-%m-%d") + "_" + SessionPaths::sanitise (name));
            session.name = name;
            session.secondaryRoot = juce::File();
            setPrimaryRoot (root);
            saveSession();
            if (then) then();
        };
        if (result == 1) { create (Library::shows()); return; }
        chooseFolder ("Choose where to create the \"" + name + "\" session folder", create);
    }), false);
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
    chooser = std::make_unique<juce::FileChooser> ("Open template", Library::templates(), "*.ldtemplate;*.omrt;*.livedeck;*.omr");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc)
    {
        if (const auto file = fc.getResult(); file != juce::File()) openTemplateFile (file);
    });
}

void MainComponent::openTemplateFile (const juce::File& file)
{
    if (engine.isRecording()) { alert ("Recording in progress", "Stop recording before opening another session.", true); return; }
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
    Library::ensure();
    chooser = std::make_unique<juce::FileChooser> ("Save template", Library::templates().getChildFile (SessionPaths::sanitise (session.name) + ".ldtemplate"), "*.ldtemplate");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this] (const juce::FileChooser& fc)
    {
        auto f = fc.getResult();
        if (f == juce::File()) return;
        f = f.withFileExtension ("ldtemplate");
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
    addTemplateItems (m, ! rec);
    m.addSeparator();
    m.addItem ("Save Session", true, false, [this] { saveSession(); });
    m.addItem ("Save as Template...", true, false, [this] { saveTemplate(); });
    m.addSeparator();
    m.addSubMenu ("Recording Format", format, ! rec);
    m.addItem ("Recover Session Folder...", ! rec, false, [this] { recoverFolder(); });
    if (session.primaryRoot != juce::File())
        m.addItem ("Reveal Session Folder", true, false, [this] { session.primaryRoot.revealToUser(); });
    m.addItem ("Reveal LiveDeck Folder", true, false, [this] { revealLibrary(); });
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

void MainComponent::showAddTracksDialog()
{
    if (engine.isRecording()) return;
    textDialog = std::make_unique<juce::AlertWindow> ("Add Multiple Tracks",
                                                      "Tracks are patched to consecutive inputs that are ticked in Audio Setup.",
                                                      juce::MessageBoxIconType::NoIcon, this);
    const int firstFree = firstUnusedInput (patch, false);
    int freeCount = 0;
    for (int ch = juce::jmax (0, firstFree); ch < patch.numInputs; ++ch) freeCount += isInputActive (patch, ch) ? 1 : 0;
    textDialog->addTextEditor ("count", juce::String (juce::jmax (1, freeCount)), "How many tracks");
    textDialog->addTextEditor ("first", juce::String (firstFree >= 0 ? firstFree + 1 : 1), "Starting at input");
    textDialog->addComboBox ("width", { "Mono", "Stereo pairs" }, "Track width");
    textDialog->addButton ("Add", 1, juce::KeyPress (juce::KeyPress::returnKey));
    textDialog->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    textDialog->enterModalState (true, juce::ModalCallbackFunction::create ([this] (int result)
    {
        const int count = textDialog->getTextEditorContents ("count").getIntValue();
        const int first = textDialog->getTextEditorContents ("first").getIntValue() - 1;
        const bool stereo = textDialog->getComboBoxComponent ("width")->getSelectedItemIndex() == 1;
        juce::MessageManager::callAsync ([this] { textDialog.reset(); });
        if (result != 1) return;
        applyPatch ([this, count, first, stereo] (PatchState& p) { return addTracks (p, count, first, stereo, engine.isRecording()); });
    }), false);
}

// ------------------------------------------------------------------------------------ remote control

void MainComponent::handleRemote (const omr::remote::Command& c)
{
    using omr::remote::Action;
    const auto who = "Remote: " + omr::remote::actionName (c.action);
    auto setTrackFlag = [this, &c, &who] (int which)
    {
        if (c.track < 0 || c.track >= (int) patch.tracks.size()) { engine.log().post (Level::warning, who + ": no track " + juce::String (c.track + 1)); return; }
        const auto& t = patch.tracks[(size_t) c.track];
        const bool current = which == 0 ? t.armed : (which == 1 ? t.mute : t.solo);
        const bool want = c.value < 0 ? ! current : c.value == 1;
        const int row = c.track;
        if (which == 0) applyPatch ([this, row, want] (PatchState& p) { return setArmed (p, row, want, engine.isRecording()); });
        else if (which == 1) applyPatch ([row, want] (PatchState& p) { p.tracks[(size_t) row].mute = want; return juce::Result::ok(); });
        else applyPatch ([row, want] (PatchState& p) { p.tracks[(size_t) row].solo = want; return juce::Result::ok(); });
    };

    switch (c.action)
    {
        case Action::record:
            if (session.primaryRoot == juce::File()) { engine.log().post (Level::warning, who + " ignored: no session folder yet"); return; }
            if (! engine.isRecording()) startRecording();
            break;
        case Action::stop:     stopTransport(); break;
        case Action::play:     if (! engine.isRecording() && ! engine.isPlaying()) startPlayback(); break;
        case Action::playStop:
            if (engine.isRecording()) engine.log().post (Level::info, who + ": recording continues (play/stop never stops a take)");
            else if (engine.isPlaying()) stopTransport();
            else startPlayback();
            break;
        case Action::rtz:      if (! engine.isRecording()) returnToZero(); break;
        case Action::marker:   addMarker (c.text); break;
        case Action::locate:
            if (engine.isRecording()) return;
            cueFrames = (uint64_t) juce::jmax (0.0, c.seconds * engine.sampleRate());
            timeline.setPlayhead (cueFrames);
            if (engine.isPlaying()) startPlayback();
            break;
        case Action::selectTake:
            if (engine.isRecording()) return;
            if (bar.takeCombo.indexOfItemId (c.value) >= 0) bar.takeCombo.setSelectedId (c.value);
            else engine.log().post (Level::warning, who + ": no take " + juce::String (c.value));
            break;
        case Action::nextTake:
        case Action::prevTake:
        {
            if (engine.isRecording()) return;
            const int idx = bar.takeCombo.getSelectedItemIndex() + (c.action == Action::nextTake ? 1 : -1);
            if (idx >= 0 && idx < bar.takeCombo.getNumItems()) bar.takeCombo.setSelectedItemIndex (idx);
            break;
        }
        case Action::armTrack:  setTrackFlag (0); break;
        case Action::muteTrack: setTrackFlag (1); break;
        case Action::soloTrack: setTrackFlag (2); break;
        case Action::armAll:
        {
            const bool want = c.value != 0;
            applyPatch ([this, want] (PatchState& p)
            {
                for (int i = 0; i < (int) p.tracks.size(); ++i)
                    if (auto r = setArmed (p, i, want, engine.isRecording()); r.failed()) return r;
                return juce::Result::ok();
            });
            break;
        }
        case Action::ping:
        case Action::none:
            break;
    }
}

RemoteStatus MainComponent::remoteStatus()
{
    RemoteStatus s;
    s.recording = engine.isRecording();
    s.playing = engine.isPlaying();
    const double rate = juce::jmax (1.0, engine.sampleRate());
    s.seconds = s.recording ? (double) engine.recordedFrames() / rate
              : s.playing   ? (double) engine.playbackPosition() / rate
                            : (double) cueFrames / rate;
    const auto total = (int) s.seconds;
    s.timecode = juce::String::formatted ("%02d:%02d:%02d:%02d", total / 3600, total / 60 % 60, total % 60, (int) ((s.seconds - total) * 30.0));
    s.session = session.name;
    s.take = s.recording ? engine.currentTake() : bar.takeCombo.getSelectedId();
    s.nextTake = session.nextTake;
    s.dropouts = (int) engine.xrunCount() + audio.driverXruns();
    int armed = 0;
    for (const auto& t : patch.tracks) armed += (t.armed && t.input >= 0) ? 1 : 0;
    const double bytesPerSec = rate * (session.bitsPerSample / 8) * juce::jmax (1, armed);
    for (int i = 0; i < 2; ++i)
    {
        const auto root = i == 0 ? session.primaryRoot : session.secondaryRoot;
        if (root == juce::File()) continue;
        const double hours = (double) (root.exists() ? root : root.getParentDirectory()).getBytesFreeOnVolume() / bytesPerSec / 3600.0;
        const auto* t = engine.target (i);
        const bool ok = ! (s.recording && t != nullptr && t->failed.load());
        if (i == 0) { s.diskAHours = hours; s.diskAOk = ok; } else { s.diskBHours = hours; s.diskBOk = ok; }
    }
    for (const auto& t : patch.tracks) s.tracks.push_back ({ t.name, t.armed, t.mute, t.solo });
    return s;
}

void MainComponent::showRemoteSettings()
{
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (new RemoteSettingsComponent (*remote));
    o.dialogTitle = "Remote Control: OSC (Bitfocus Companion) and MIDI";
    o.dialogBackgroundColour = Theme::panel;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.componentToCentreAround = this;
    o.launchAsync();
}

// ------------------------------------------------------------------------------------ extra devices

namespace
{
class ExtraDevicesComponent final : public juce::Component
{
public:
    ExtraDevicesComponent (AudioEngine& a, std::function<void()> done) : audio (a), onDone (std::move (done))
    {
        info.setText ("Record from several interfaces at once (for example Dante and MADI). Each device runs at the primary "
                      "device's sample rate and buffer size, and keeps its own clock: its tracks are recorded bit-exact and the "
                      "drift is shown in the CLOCK chip. For sample-locked files, clock every device from one word clock / PTP source.\n\n"
                      "Inputs and outputs of extra devices are added after the primary's (\"Device: channel\"). Direct outs must be on "
                      "the same device as the track's input. Removing a device in the middle renumbers the ones after it.",
                      juce::dontSendNotification);
        info.setJustificationType (juce::Justification::topLeft);
        info.setFont (Theme::ui (12.5f));
        addAndMakeVisible (info);

        auto names = audio.availableExtraDevices();
        const auto chosen = audio.extraDeviceNames();
        for (const auto& c : chosen) if (! names.contains (c)) names.add (c);   // configured but offline
        for (const auto& n : names)
        {
            auto* t = toggles.add (new juce::ToggleButton (n + (audio.availableExtraDevices().contains (n) ? juce::String() : juce::String ("  (offline)"))));
            t->setToggleState (chosen.contains (n), juce::dontSendNotification);
            t->getProperties().set ("deviceName", n);
            addAndMakeVisible (t);
        }
        if (names.isEmpty())
        {
            empty.setText ("No other devices of this driver type were found.", juce::dontSendNotification);
            addAndMakeVisible (empty);
        }
        ok.onClick = [this]
        {
            juce::StringArray selected;
            for (auto* t : toggles) if (t->getToggleState()) selected.add (t->getProperties()["deviceName"].toString());
            audio.setExtraDevices (selected);
            if (onDone) onDone();
            if (auto* w = findParentComponentOfClass<juce::DialogWindow>()) w->exitModalState (1);
        };
        cancel.onClick = [this] { if (auto* w = findParentComponentOfClass<juce::DialogWindow>()) w->exitModalState (0); };
        addAndMakeVisible (ok);
        addAndMakeVisible (cancel);
        setSize (560, 220 + 28 * juce::jmax (1, toggles.size()));
    }

    void paint (juce::Graphics& g) override { g.fillAll (Theme::panel); }
    void resized() override
    {
        auto r = getLocalBounds().reduced (16);
        info.setBounds (r.removeFromTop (130));
        r.removeFromTop (6);
        for (auto* t : toggles) t->setBounds (r.removeFromTop (26));
        if (empty.isVisible()) empty.setBounds (r.removeFromTop (26));
        auto b = r.removeFromBottom (28);
        cancel.setBounds (b.removeFromRight (90));
        b.removeFromRight (8);
        ok.setBounds (b.removeFromRight (90));
    }

private:
    AudioEngine& audio;
    std::function<void()> onDone;
    juce::Label info, empty;
    juce::OwnedArray<juce::ToggleButton> toggles;
    juce::TextButton ok { "Use These" }, cancel { "Cancel" };
};
}

void MainComponent::showExtraDevices()
{
    if (engine.isRecording()) return;
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (new ExtraDevicesComponent (audio, [this] { driftWarned = {}; saveDeviceState(); }));
    o.dialogTitle = "Additional Audio Devices";
    o.dialogBackgroundColour = Theme::panel;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.componentToCentreAround = this;
    o.launchAsync();
}

// ------------------------------------------------------------------------------------ library, templates, feedback

void MainComponent::revealLibrary()
{
    Library::ensure();
    Library::shows().revealToUser();
}

void MainComponent::addTemplateItems (juce::PopupMenu& m, bool enabled)
{
    juce::PopupMenu sub;
    for (const auto& f : Library::listTemplates())
        sub.addItem (f.getFileNameWithoutExtension(), enabled, false, [this, f] { openTemplateFile (f); });
    if (sub.getNumItems() == 0) sub.addItem ("No templates yet: File > Save as Template...", false, false, nullptr);
    sub.addSeparator();
    sub.addItem ("Show Templates Folder", true, false, [] { Library::ensure(); Library::templates().revealToUser(); });
    m.addSubMenu ("New from Template", sub, true);
}

juce::String MainComponent::systemInfo() const
{
    juce::String s;
    s << "LiveDeck " << juce::JUCEApplication::getInstance()->getApplicationVersion() << "\n"
      << juce::SystemStats::getOperatingSystemName() << (juce::SystemStats::isOperatingSystem64Bit() ? " (64-bit)" : "") << "\n"
      << juce::SystemStats::getCpuModel().trim() << ", " << juce::SystemStats::getNumCpus() << " cores, "
      << juce::roundToInt (juce::SystemStats::getMemorySizeInMegabytes() / 1024.0) << " GB RAM\n"
      << "Audio: " << audio.describeDevice() << "\n"
      << "Tracks: " << (int) patch.tracks.size() << ", format " << session.bitsPerSample << "-bit"
      << (session.secondaryRoot != juce::File() ? ", Disk A + B" : "");
    return s;
}

void MainComponent::showFeedback (int kind)
{
    const auto k = kind == 1 ? feedback::Kind::bug : kind == 2 ? feedback::Kind::general : feedback::Kind::feature;
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (new FeedbackComponent (update::configuredRepository(), systemInfo(), k));
    o.dialogTitle = "Feedback and Feature Requests";
    o.dialogBackgroundColour = Theme::panel;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.componentToCentreAround = this;
    o.launchAsync();
}

// ------------------------------------------------------------------------------------ audio import

struct MainComponent::ImportJob
{
    ImportPlan plan;
    std::vector<std::vector<juce::File>> destinations;   // per source, per output
    std::vector<std::vector<int>> targetIds;             // per source, per output: existing track id, 0 = new track
    std::vector<ImportResult> results;
    juce::File root;
    int take = 0;
    bool newTake = false;
    double rate = 48000.0;
    int bits = 24;
    uint64_t startFrame = 0;
};

void MainComponent::importAudioFiles (const juce::StringArray& paths, uint64_t startFrame, int targetRow)
{
    if (! licensed) return;
    if (engine.isRecording()) { alert ("Import audio", "Stop recording before importing audio.", false); return; }
    if (importRunning) { alert ("Import audio", "An import is already running. Wait for it to finish.", false); return; }

    if (session.primaryRoot == juce::File())
    {
        // No session yet: create one (named after the dropped folder), then import into it.
        const juce::File first (paths[0]);
        auto name = first.isDirectory() ? first.getFileName() : first.getParentDirectory().getFileName();
        if (name.isEmpty()) name = "Show";
        newSession (name, [this, paths, startFrame] { importAudioFiles (paths, startFrame, -1); });
        return;
    }

    auto job = std::make_shared<ImportJob>();
    job->plan = planImport (paths);
    if (job->plan.sources.empty())
    {
        alert ("Import audio", "Nothing to import.\n\n" + job->plan.skipped.joinIntoString ("\n"), true);
        return;
    }

    // Dropped on a track: the first file goes to that track, the next ones to the tracks below it;
    // whatever doesn't fit becomes new tracks. Dropped below the tracks: all new tracks.
    int nextRow = targetRow >= 0 && targetRow < (int) patch.tracks.size() ? targetRow : -1;
    int newTracks = 0;
    for (const auto& src : job->plan.sources)
    {
        std::vector<int> ids;
        for (size_t k = 0; k < src.outputs.size(); ++k)
        {
            if (nextRow >= 0 && nextRow < (int) patch.tracks.size()) ids.push_back (patch.tracks[(size_t) nextRow++].id);
            else { ids.push_back (0); ++newTracks; }
        }
        job->targetIds.push_back (std::move (ids));
    }
    if ((int) patch.tracks.size() + newTracks > kMaxTracks)
    {
        alert ("Import audio", juce::String (newTracks) + " new tracks would exceed the limit of " + juce::String (kMaxTracks) + " tracks.", true);
        return;
    }

    stopPlaybackForEdit();
    job->root = session.primaryRoot;
    job->take = bar.takeCombo.getSelectedId();
    job->newTake = job->take <= 0;
    if (job->newTake) job->take = juce::jmax (session.nextTake, nextFreeTake (session.primaryRoot));
    job->rate = audio.getDeviceManager().getCurrentAudioDevice() != nullptr ? engine.sampleRate() : session.sampleRate;
    if (job->rate <= 0) job->rate = 48000.0;
    job->bits = session.bitsPerSample;
    job->startFrame = startFrame;

    // File names follow the recorded ones (T<row>_<name>_<take>.wav) so the take scanner finds them;
    // the take manifest is what ties each file to its track (and stores where it starts).
    SessionPaths::audioDir (job->root).createDirectory();
    int newRow = (int) patch.tracks.size();
    std::set<juce::String> used;
    for (size_t i = 0; i < job->plan.sources.size(); ++i)
    {
        const auto& src = job->plan.sources[i];
        std::vector<juce::File> d;
        for (size_t k = 0; k < src.outputs.size(); ++k)
        {
            const int id = job->targetIds[i][k];
            const int row = id > 0 ? rowForTrackId (patch, id) : newRow++;
            const auto name = id > 0 ? patch.tracks[(size_t) row].name : src.outputs[k].trackName;
            auto f = SessionPaths::audioDir (job->root).getChildFile (SessionPaths::trackFileName (row, name, job->take, false));
            for (int n = 2; f.exists() || used.count (f.getFileName()) > 0; ++n)
                f = SessionPaths::audioDir (job->root).getChildFile (SessionPaths::trackFileName (row, name + "_" + juce::String (n), job->take, false));
            used.insert (f.getFileName());
            d.push_back (f);
        }
        job->destinations.push_back (std::move (d));
    }

    importRunning = true;
    importCancel = std::make_shared<std::atomic<bool>> (false);
    const auto cancel = importCancel;
    engine.log().post (Level::info, "Importing " + juce::String ((int) job->plan.sources.size()) + " file(s) into take " + juce::String (job->take)
                                        + (targetRow >= 0 ? " on track " + juce::String (targetRow + 1) : juce::String())
                                        + (std::abs (job->rate - job->plan.sources.front().sampleRate) > 0.5 ? " (converting the sample rate)" : ""));
    timeline.setBusyText ("Importing audio...");

    juce::Component::SafePointer<MainComponent> safe (this);
    juce::Thread::launch ([safe, job, cancel]
    {
        const int total = (int) job->plan.sources.size();
        double lastPost = 0;
        for (int i = 0; i < total; ++i)
        {
            if (cancel->load()) break;
            const auto& src = job->plan.sources[(size_t) i];
            auto progress = [&] (double p)
            {
                const double now = juce::Time::getMillisecondCounterHiRes();
                if (now - lastPost < 150) return;
                lastPost = now;
                const auto text = "Importing " + juce::String (i + 1) + " of " + juce::String (total) + ": "
                                  + src.file.getFileName() + "  " + juce::String (juce::roundToInt (p * 100)) + "%";
                juce::MessageManager::callAsync ([safe, text] { if (safe != nullptr) safe->timeline.setBusyText (text); });
            };
            // Files are written from their first sample; where they start in the take lives in the manifest,
            // so they can be moved later without rewriting audio.
            job->results.push_back (importSource (src, job->destinations[(size_t) i], job->rate, job->bits, 0, cancel.get(), progress));
        }
        juce::MessageManager::callAsync ([safe, job] { if (safe != nullptr) safe->finishImport (job); });
    });
}

void MainComponent::finishImport (std::shared_ptr<ImportJob> job)
{
    importRunning = false;
    timeline.setBusyText ({});
    const bool cancelled = importCancel != nullptr && importCancel->load();
    importCancel.reset();

    if (cancelled || engine.isRecording() || session.primaryRoot != job->root)
    {
        // Cancelled (or the session changed): remove whatever was written.
        for (const auto& r : job->results) for (const auto& f : r.written) f.deleteFile();
        engine.log().post (Level::warning, "Audio import cancelled");
        return;
    }

    juce::StringArray problems = job->plan.skipped;
    int filesOk = 0, replaced = 0, added = 0;

    // Existing takes from before manifests: link their files to the current tracks first.
    writeManifestIfMissing (job->root, job->take, patch);
    const auto before = readManifestText (job->root, job->take);
    const auto existing = resolveTakeClips (job->root, job->take, patch);

    std::vector<ManifestEntry> newEntries;
    std::vector<int> createdIds;
    PatchState p = patch;
    for (size_t i = 0; i < job->results.size(); ++i)
    {
        const auto& r = job->results[i];
        const auto& src = job->plan.sources[i];
        if (! r.ok) { problems.add (src.file.getFileName() + ": " + r.error); continue; }
        ++filesOk;
        for (size_t k = 0; k < src.outputs.size() && k < r.written.size(); ++k)
        {
            const auto& out = src.outputs[k];
            const int channels = (int) out.sourceChannels.size();
            int id = job->targetIds[i][k];
            int row = id > 0 ? rowForTrackId (p, id) : -1;
            if (row >= 0)
            {
                // onto an existing track: replaces what that track had in this take (the old file stays on disk)
                if (existing.count (row) > 0) ++replaced;
                setClip (job->root, job->take, { id, row, channels, p.tracks[(size_t) row].name, r.written[k], job->startFrame, out.trackName });
                continue;
            }
            if (addNamedTrack (p, out.trackName, channels == 2, false, &id).failed())
            {
                problems.add (out.trackName + ": track limit reached");
                r.written[k].deleteFile();
                continue;
            }
            createdIds.push_back (id);
            newEntries.push_back ({ id, (int) p.tracks.size() - 1, channels, out.trackName, r.written[k], job->startFrame, out.trackName });
            ++added;
        }
    }

    if (! newEntries.empty()) appendToManifest (job->root, job->take, newEntries);
    if (readManifestText (job->root, job->take) != before)
    {
        undoStack.push_back ({ job->root, job->take, before, "Import Audio", createdIds });
        if (undoStack.size() > 50) undoStack.erase (undoStack.begin());
    }
    if (! createdIds.empty()) applyPatch ([p] (PatchState& s) { s = p; return juce::Result::ok(); });
    if (job->newTake) session.nextTake = job->take + 1;
    saveSession();
    refreshTakes (job->newTake);
    if (! job->newTake) { bar.takeCombo.setSelectedId (job->take, juce::dontSendNotification); clipsChanged (job->take); }
    menuItemsChanged();

    juce::String summary = "Imported " + juce::String (filesOk) + " file(s) into take " + juce::String (job->take) + ": ";
    juce::StringArray parts;
    if (added > 0) parts.add (juce::String (added) + " new track(s)");
    const int onExisting = [&] { int n = 0; for (const auto& v : job->targetIds) for (int id : v) if (id > 0) ++n; return n; }();
    if (onExisting > 0) parts.add (juce::String (onExisting) + " on existing track(s)" + (replaced > 0 ? " (" + juce::String (replaced) + " replaced, Cmd+Z undoes)" : juce::String()));
    engine.log().post (Level::info, summary + parts.joinIntoString (", "));

    if (! problems.isEmpty())
        alert ("Import audio", "Some files were not imported:\n\n" + problems.joinIntoString ("\n"), filesOk == 0);
}

// ------------------------------------------------------------------------------------ clip editing + undo

void MainComponent::pushUndo (int take, const juce::String& label, std::vector<int> createdTrackIds)
{
    undoStack.push_back ({ session.primaryRoot, take, readManifestText (session.primaryRoot, take), label, std::move (createdTrackIds) });
    if (undoStack.size() > 50) undoStack.erase (undoStack.begin());
    menuItemsChanged();
}

void MainComponent::undo()
{
    if (engine.isRecording() || undoStack.empty()) return;
    const auto step = undoStack.back();
    undoStack.pop_back();
    if (step.root != session.primaryRoot) { undoStack.clear(); menuItemsChanged(); return; }   // another session
    stopPlaybackForEdit();
    // No manifest before = nothing was linked yet: an empty one keeps the take's files from being
    // mapped to tracks by file name again.
    restoreManifestText (step.root, step.take, step.manifestBefore.isNotEmpty() ? step.manifestBefore
                                                                                : "{\"take\": " + juce::String (step.take) + ", \"tracks\": []}");
    if (! step.createdTrackIds.empty())
    {
        std::vector<int> rows;
        for (int id : step.createdTrackIds) if (const int r = rowForTrackId (patch, id); r >= 0) rows.push_back (r);
        if (! rows.empty()) applyPatch ([rows] (PatchState& p) { return deleteTracks (p, rows, false); });
    }
    engine.log().post (Level::info, "Undo: " + step.label);
    if (bar.takeCombo.getSelectedId() != step.take) refreshTakes (false);
    clipsChanged (step.take);
    menuItemsChanged();
}

void MainComponent::clipsChanged (int take)
{
    if (session.primaryRoot == juce::File()) return;
    takeLengths[take] = takeLength (resolveTakeClips (session.primaryRoot, take, patch));
    if (bar.takeCombo.getSelectedId() == take) refreshTimeline (false);
}

void MainComponent::moveClipTo (int fromRow, int toRow, uint64_t offset)
{
    if (engine.isRecording() || session.primaryRoot == juce::File()) return;
    const int take = bar.takeCombo.getSelectedId();
    if (take <= 0 || fromRow < 0 || toRow < 0 || fromRow >= (int) patch.tracks.size() || toRow >= (int) patch.tracks.size()) return;
    stopPlaybackForEdit();
    writeManifestIfMissing (session.primaryRoot, take, patch);
    const int fromId = patch.tracks[(size_t) fromRow].id, toId = patch.tracks[(size_t) toRow].id;
    const auto clips = resolveTakeClips (session.primaryRoot, take, patch);

    auto apply = [this, take, fromId, toId, offset, fromRow, toRow] (bool swap)
    {
        pushUndo (take, "Move Clip");
        if (! moveClip (session.primaryRoot, take, fromId, toId, offset, swap)) { undoStack.pop_back(); return; }
        engine.log().post (Level::info, "Clip moved" + (toRow != fromRow ? " from track " + juce::String (fromRow + 1) + " to track " + juce::String (toRow + 1) : juce::String())
                                            + " at " + juce::String ((double) offset / juce::jmax (1.0, engine.sampleRate()), 2) + " s"
                                            + (swap ? " (swapped)" : ""));
        clipsChanged (take);
    };

    if (toRow != fromRow && clips.count (toRow) > 0)
    {
        juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                          .withIconType (juce::MessageBoxIconType::QuestionIcon)
                                          .withTitle ("Track " + juce::String (toRow + 1) + " already has audio")
                                          .withMessage ("\"" + patch.tracks[(size_t) toRow].name + "\" already has audio in this take.\n\n"
                                                        "Replace: the moved clip takes its place (its old file stays in the session folder).\n"
                                                        "Swap: the two clips trade tracks.")
                                          .withButton ("Replace")
                                          .withButton ("Swap")
                                          .withButton ("Cancel")
                                          .withAssociatedComponent (this),
                                      [apply, this] (int result)
                                      {
                                          if (result == 1) apply (false);
                                          else if (result == 2) apply (true);
                                          else refreshTimeline (false);
                                      });
        return;
    }
    apply (false);
}

void MainComponent::showClipMenu (int row, uint64_t frame)
{
    juce::ignoreUnused (frame);
    const int take = bar.takeCombo.getSelectedId();
    if (take <= 0 || row < 0 || row >= (int) patch.tracks.size() || session.primaryRoot == juce::File()) return;
    const auto clips = resolveTakeClips (session.primaryRoot, take, patch);
    const auto it = clips.find (row);
    if (it == clips.end()) return;
    const bool rec = engine.isRecording();
    const int id = patch.tracks[(size_t) row].id;
    const auto file = it->second.file;
    const auto offset = it->second.offset;

    if (selectedClipIds.size() > 1 && selectedClipIds.count (id) > 0)
    {
        // Menu for the whole clip selection.
        juce::int64 earliest = std::numeric_limits<juce::int64>::max();
        for (const auto& [r, c] : clips) if (selectedClipIds.count (patch.tracks[(size_t) r].id)) earliest = juce::jmin (earliest, (juce::int64) c.offset);
        juce::PopupMenu g;
        g.addSectionHeader (juce::String ((int) selectedClipIds.size()) + " clips selected");
        g.addItem ("Move to Start of Take (keep their spacing)", ! rec && earliest > 0, false, [this, earliest] { shiftSelectedClips (-earliest, "Move Clips to Start"); });
        g.addItem ("Move to Playhead (keep their spacing)", ! rec, false, [this, earliest] { shiftSelectedClips ((juce::int64) cueFrames - earliest, "Move Clips to Playhead"); });
        g.addSeparator();
        g.addItem ("Remove " + juce::String ((int) selectedClipIds.size()) + " Clips from Their Tracks", ! rec, false, [this] { removeSelectedClips(); });
        g.addItem ("Deselect", true, false, [this] { clearClipSelection(); });
        g.showMenuAsync (juce::PopupMenu::Options().withMousePosition());
        return;
    }

    juce::PopupMenu m;
    m.addSectionHeader (file.getFileName());
    m.addItem ("Move to Start of Take", ! rec && offset > 0, false, [this, row] { moveClipTo (row, row, 0); });
    m.addItem ("Move to Playhead", ! rec, false, [this, row] { moveClipTo (row, row, cueFrames); });
    m.addSeparator();
    m.addItem ("Remove from Track", ! rec, false, [this, take, id, row]
    {
        stopPlaybackForEdit();
        writeManifestIfMissing (session.primaryRoot, take, patch);
        pushUndo (take, "Remove Clip");
        removeClip (session.primaryRoot, take, id);
        engine.log().post (Level::info, "Clip removed from track " + juce::String (row + 1) + " (the file stays in the session's Audio folder; Cmd+Z undoes)");
        clipsChanged (take);
    });
    m.addItem ("Show File in Finder", true, false, [file] { file.revealToUser(); });
    if (! undoStack.empty())
    {
        m.addSeparator();
        m.addItem ("Undo " + undoStack.back().label, ! rec, false, [this] { undo(); });
    }
    m.showMenuAsync (juce::PopupMenu::Options().withMousePosition());
}

// ------------------------------------------------------------------------------------ accounts + licensing

void MainComponent::licenseGranted (const AccountService::Check& c)
{
    licensed = true;
    lastHeartbeatMs = juce::Time::getMillisecondCounterHiRes();
    if (gate != nullptr) gate->setVisible (false);
    grabKeyboardFocus();
    const auto& t = c.token;
    if (c.status == "offline")
        engine.log().post (Level::warning, "Offline: using the saved license (" + juce::String (license::daysLeft (t, juce::Time::currentTimeMillis()))
                                               + " days left before LiveDeck needs to go online once).");
    else
        engine.log().post (Level::info, "Licensed to " + (t.company.isNotEmpty() ? t.company : t.email) + " (" + t.kind
                                            + (t.expiresAt > 0 ? ", until " + juce::Time (t.expiresAt).formatted ("%d %b %Y") : juce::String()) + ")");
    menuItemsChanged();
}

void MainComponent::heartbeat()
{
    lastHeartbeatMs = juce::Time::getMillisecondCounterHiRes();
    juce::Component::SafePointer<MainComponent> safe (this);
    account->check ([safe] (const AccountService::Check& c)
    {
        if (safe == nullptr) return;
        // Never interrupt a show: problems found while running are reported, and apply at the next launch.
        juce::String warning;
        if (c.status == "no_license") warning = "Your LiveDeck license is no longer active. LiveDeck keeps running now, but will ask for a license next time it starts.";
        else if (c.status == "signed_out") warning = "You've been signed out of LiveDeck. Sign in again before the next launch.";
        else if (c.status == "offline" && c.offlineVerdict != license::Verdict::ok) warning = license::describe (c.offlineVerdict);
        if (warning.isNotEmpty() && warning != safe->lastLicenseWarning)
        {
            safe->lastLicenseWarning = warning;
            safe->engine.log().post (Level::warning, warning);
        }
        safe->menuItemsChanged();
    });
}

void MainComponent::showAccountPanel()
{
    if (account == nullptr) return;
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (new AccountPanel (*account,
                                          [this] { signOutOfAccount(); },
                                          [this] { showRegistration(); },
                                          [this]
                                          {
                                              juce::PopupMenu m;
                                              auto go = [this] (const juce::String& plan)
                                              {
                                                  account->checkout (plan, true, [this] (const juce::var& r)
                                                  {
                                                      if (r.getProperty ("status", {}).toString() == "ok") juce::URL (r["url"].toString()).launchInDefaultBrowser();
                                                      else if (r.getProperty ("status", {}).toString() == "already_licensed") alert ("Buy or renew", "You already have a perpetual license.", false);
                                                      else alert ("Buy or renew", r.getProperty ("message", "Checkout couldn't start.").toString(), true);
                                                  });
                                              };
                                              m.addItem ("Annual license ($50 / year, 10% off for verified non-profits)", [go] { go ("annual"); });
                                              m.addItem ("Perpetual license ($129, 10% off for verified non-profits)", [go] { go ("perpetual"); });
                                              m.showMenuAsync (juce::PopupMenu::Options());
                                          }));
    o.dialogTitle = "LiveDeck Account";
    o.dialogBackgroundColour = Theme::panel;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.componentToCentreAround = this;
    o.launchAsync();
}

void MainComponent::showRegistration()
{
    if (account == nullptr) return;
    auto* form = new RegistrationForm (*account, account->lastCheck().profile, true);
    form->setSize (620, RegistrationForm::preferredHeight());
    juce::Component::SafePointer<MainComponent> safe (this);
    juce::Component::SafePointer<RegistrationForm> f (form);
    auto close = [f] { if (f != nullptr) if (auto* w = f->findParentComponentOfClass<juce::DialogWindow>()) w->exitModalState (0); };
    form->onCancel = close;
    form->onSaved = [safe, close]
    {
        close();
        if (safe != nullptr) { safe->engine.log().post (Level::info, "Registration details saved"); safe->heartbeat(); }
    };
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (form);
    o.dialogTitle = "Registration";
    o.dialogBackgroundColour = Theme::panel;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.componentToCentreAround = this;
    auto* w = o.launchAsync();
    if (w != nullptr) w->setContentComponentSize (640, RegistrationForm::preferredHeight() + 20);
}

void MainComponent::showAdminPanel()
{
    if (account == nullptr) return;
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (new AdminPanel (*account));
    o.dialogTitle = "LiveDeck Admin: users and licenses";
    o.dialogBackgroundColour = Theme::panel;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.componentToCentreAround = this;
    o.launchAsync();
}

void MainComponent::signOutOfAccount()
{
    if (account == nullptr || engine.isRecording()) return;
    juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                      .withIconType (juce::MessageBoxIconType::QuestionIcon)
                                      .withTitle ("Sign out of LiveDeck?")
                                      .withMessage ("This frees this computer's place on your license. You'll need to sign in again to use LiveDeck here.")
                                      .withButton ("Sign Out")
                                      .withButton ("Cancel")
                                      .withAssociatedComponent (this),
                                  [this] (int result)
                                  {
                                      if (result != 1) return;
                                      if (engine.isPlaying()) engine.stopPlayback();
                                      licensed = false;
                                      gate->setVisible (true);
                                      resized();
                                      account->signOut ([this] { gate->start(); menuItemsChanged(); });
                                  });
}

void MainComponent::changePassword()
{
    if (account == nullptr) return;
    textDialog = std::make_unique<juce::AlertWindow> ("Change password",
                                                      "At least " + juce::String (AccountService::kMinPasswordLength) + " characters. "
                                                      "(If you sign in with Google or Microsoft, this adds a password you can also use.)",
                                                      juce::MessageBoxIconType::NoIcon, this);
    textDialog->addTextEditor ("p1", {}, "New password", true);
    textDialog->addTextEditor ("p2", {}, "Repeat new password", true);
    textDialog->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    textDialog->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    textDialog->enterModalState (true, juce::ModalCallbackFunction::create ([this] (int result)
    {
        const auto p1 = textDialog->getTextEditorContents ("p1"), p2 = textDialog->getTextEditorContents ("p2");
        juce::MessageManager::callAsync ([this] { textDialog.reset(); });
        if (result != 1) return;
        if (p1.length() < AccountService::kMinPasswordLength) { alert ("Change password", "Use at least " + juce::String (AccountService::kMinPasswordLength) + " characters.", true); return; }
        if (p1 != p2) { alert ("Change password", "The two passwords don't match.", true); return; }
        account->setNewPassword (p1, [this] (bool ok, juce::String err)
        {
            alert ("Change password", ok ? juce::String ("Your password has been changed.") : err, ! ok);
        });
    }), false);
}

// ------------------------------------------------------------------------------------ clip selection

std::vector<int> MainComponent::selectedClipRows() const
{
    std::vector<int> rows;
    for (int id : selectedClipIds) if (const int r = rowForTrackId (patch, id); r >= 0) rows.push_back (r);
    std::sort (rows.begin(), rows.end());
    return rows;
}

void MainComponent::pushClipSelection()
{
    // keep only tracks that actually have a clip in the take on screen
    const int take = bar.takeCombo.getSelectedId();
    std::set<int> rowsWithClips;
    if (take > 0 && session.primaryRoot != juce::File())
        for (const auto& [row, c] : resolveTakeClips (session.primaryRoot, take, patch)) rowsWithClips.insert (row);
    std::set<int> rows;
    for (auto it = selectedClipIds.begin(); it != selectedClipIds.end();)
    {
        const int r = rowForTrackId (patch, *it);
        if (r < 0 || rowsWithClips.count (r) == 0) it = selectedClipIds.erase (it);
        else { rows.insert (r); ++it; }
    }
    timeline.setClipSelection (rows);
    menuItemsChanged();
}

void MainComponent::selectClip (int row, juce::ModifierKeys mods)
{
    if (row < 0 || row >= (int) patch.tracks.size()) return;
    const int id = patch.tracks[(size_t) row].id;
    if (mods.isShiftDown() || mods.isCommandDown())
    {
        if (selectedClipIds.count (id)) selectedClipIds.erase (id);
        else selectedClipIds.insert (id);
    }
    else selectedClipIds = { id };
    pushClipSelection();
    grabKeyboardFocus();
}

void MainComponent::selectAllClips()
{
    const int take = bar.takeCombo.getSelectedId();
    if (engine.isRecording() || take <= 0 || session.primaryRoot == juce::File()) return;
    selectedClipIds.clear();
    for (const auto& [row, c] : resolveTakeClips (session.primaryRoot, take, patch)) selectedClipIds.insert (patch.tracks[(size_t) row].id);
    pushClipSelection();
    engine.log().post (Level::info, juce::String ((int) selectedClipIds.size()) + " clips selected (Delete removes them, drag one to move them all, Esc clears)");
}

void MainComponent::clearClipSelection()
{
    if (selectedClipIds.empty()) return;
    selectedClipIds.clear();
    pushClipSelection();
}

void MainComponent::removeSelectedClips()
{
    const int take = bar.takeCombo.getSelectedId();
    if (engine.isRecording() || take <= 0 || selectedClipIds.empty()) return;
    stopPlaybackForEdit();
    writeManifestIfMissing (session.primaryRoot, take, patch);
    const std::vector<int> ids (selectedClipIds.begin(), selectedClipIds.end());
    pushUndo (take, ids.size() == 1 ? "Remove Clip" : "Remove Clips");
    if (! removeClips (session.primaryRoot, take, ids)) { undoStack.pop_back(); return; }
    engine.log().post (Level::info, juce::String ((int) ids.size()) + " clip(s) removed from their tracks. The files stay in the session's Audio folder; Cmd+Z brings them back.");
    selectedClipIds.clear();
    clipsChanged (take);
    pushClipSelection();
}

void MainComponent::shiftSelectedClips (juce::int64 delta, const juce::String& label)
{
    const int take = bar.takeCombo.getSelectedId();
    if (engine.isRecording() || take <= 0 || selectedClipIds.empty() || delta == 0) return;
    stopPlaybackForEdit();
    writeManifestIfMissing (session.primaryRoot, take, patch);
    const std::vector<int> ids (selectedClipIds.begin(), selectedClipIds.end());
    pushUndo (take, label);
    if (! shiftClips (session.primaryRoot, take, ids, delta)) { undoStack.pop_back(); return; }
    engine.log().post (Level::info, juce::String ((int) ids.size()) + " clip(s) moved " + (delta >= 0 ? "later" : "earlier") + " by "
                                        + juce::String (std::abs ((double) delta) / juce::jmax (1.0, engine.sampleRate()), 2) + " s");
    clipsChanged (take);
    pushClipSelection();
}

void MainComponent::warnDeadOutputs()
{
    // Tracks whose input or output can't pass audio right now: unticked in Audio Setup, or a channel
    // number the device doesn't have. They show "(off)" in the mixer; this says it in the event log too.
    const auto outRows = tracksWithDeadOutputs (patch);
    const auto inRows = tracksWithDeadInputs (patch);
    juce::String key;
    for (int r : inRows) key << "i" << r;
    for (int r : outRows) key << "o" << r;
    if (key == lastDeadOutputs) return;
    lastDeadOutputs = key;
    if (inRows.empty() && outRows.empty()) return;

    auto list = [this] (const std::vector<int>& rows)
    {
        juce::StringArray names;
        for (int r : rows) names.add (juce::String (r + 1) + " " + patch.tracks[(size_t) r].name);
        return names.joinIntoString (", ", 0, 8) + (rows.size() > 8 ? ", ..." : "");
    };
    int activeIn = 0, activeOut = 0;
    for (int c = 0; c < patch.numInputs; ++c) activeIn += isInputActive (patch, c) ? 1 : 0;
    for (int c = 0; c < patch.numOutputs; ++c) activeOut += isOutputActive (patch, c) ? 1 : 0;
    const auto fix = juce::String (" Use View > Turn On All Inputs and Outputs, or tick them in Audio Setup.");
    if (! inRows.empty())
        engine.log().post (Level::warning, juce::String ((int) inRows.size()) + " track(s) use inputs that are off (no audio, shown as \"(off)\"): "
                                               + list (inRows) + ". " + juce::String (activeIn) + " of " + juce::String (patch.numInputs)
                                               + " inputs are on." + fix);
    if (! outRows.empty())
        engine.log().post (Level::warning, juce::String ((int) outRows.size()) + " track(s) send to outputs that are off or don't exist (shown as \"(off)\"): "
                                               + list (outRows) + ". " + juce::String (activeOut) + " of " + juce::String (patch.numOutputs)
                                               + " outputs are on." + fix);
}
