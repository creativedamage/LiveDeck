#include "TimelineView.h"

static void setupToolButton (juce::TextButton& b, const juce::String& tip)
{
    b.setTooltip (tip);
    b.setWantsKeyboardFocus (false);
    b.setMouseClickGrabsKeyboardFocus (false);
}

TimelineView::TimelineView()
{
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
    setOpaque (true);

    for (auto* sb : { &hbar, &vbar })
    {
        sb->addListener (this);
        sb->setAutoHide (false);
        addAndMakeVisible (sb);
    }

    setupToolButton (zoomOut, "Zoom out (Cmd + scroll)");
    setupToolButton (zoomIn, "Zoom in (Cmd + scroll)");
    setupToolButton (fit, "Show the whole take");
    setupToolButton (followBtn, "Keep the playhead in view while playing or recording");
    setupToolButton (small, "Small rows");
    setupToolButton (medium, "Medium rows");
    setupToolButton (large, "Large rows");
    followBtn.setClickingTogglesState (true);
    followBtn.setToggleState (true, juce::dontSendNotification);
    followBtn.setColour (juce::TextButton::buttonOnColourId, Theme::green.withAlpha (0.6f));

    zoomOut.onClick = [this] { userZoomed = true; zoomAround (1.6, kHeaderW + laneWidth() / 2); };
    zoomIn.onClick = [this] { userZoomed = true; zoomAround (1.0 / 1.6, kHeaderW + laneWidth() / 2); };
    fit.onClick = [this] { userZoomed = false; zoomToFit(); };
    followBtn.onClick = [this] { follow = followBtn.getToggleState(); followPlayhead(); repaint(); };
    small.onClick = [this] { setRowHeight (34); };
    medium.onClick = [this] { setRowHeight (56); };
    large.onClick = [this] { setRowHeight (92); };

    for (auto* b : { &zoomOut, &zoomIn, &fit, &followBtn, &small, &medium, &large })
        addAndMakeVisible (b);

    renameEditor.setFont (Theme::ui (13.0f, true));
    renameEditor.setJustification (juce::Justification::centredLeft);
    renameEditor.setIndents (4, 2);
    renameEditor.onReturnKey = [this] { commitRename (0); };
    renameEditor.onEscapeKey = [this] { renameRow = -1; renameEditor.setVisible (false); if (onInteraction) onInteraction(); };
    renameEditor.onFocusLost = [this] { if (renameRow >= 0) commitRename (0); };
    renameEditor.onTab = [this] (bool back) { commitRename (back ? -1 : 1); };
    addChildComponent (renameEditor);
}

// ------------------------------------------------------------------------------------ model

void TimelineView::setTracks (const std::vector<omr::TrackState>& tracks, bool rec, const std::set<int>& selectedIds)
{
    rows.resize (tracks.size());
    for (size_t i = 0; i < tracks.size(); ++i)
    {
        auto& r = rows[i];
        const auto& t = tracks[i];
        r.name = t.name;
        r.paletteIndex = t.colour;
        r.colour = Theme::trackColour (t.id - 1, t.colour);   // automatic colour follows the track, not the row
        r.armed = t.armed;
        r.mute = t.mute;
        r.solo = t.solo;
        r.hasInput = t.input >= 0;
        r.stereo = t.stereo;
        r.selected = selectedIds.count (t.id) > 0;
    }
    recordingSession = rec;
    updateScrollBars();
    repaint();
}

void TimelineView::setPeaks (int track, std::shared_ptr<omr::PeakStore> store, uint64_t offset, const juce::String& clipName)
{
    if (track >= 0 && track < (int) rows.size())
    {
        auto& r = rows[(size_t) track];
        r.peaks = std::move (store);
        r.clipOffset = offset;
        r.clipName = clipName;
    }
    repaint();
}

void TimelineView::clearPeaks()
{
    for (auto& r : rows) { r.peaks.reset(); r.clipOffset = 0; r.clipName = {}; }
    repaint();
}

void TimelineView::setSampleRate (double sr) { sampleRate = juce::jmax (1.0, sr); }

void TimelineView::setLength (uint64_t frames)
{
    if (frames == length) return;
    length = frames;
    updateScrollBars();
    repaint();
}

void TimelineView::setMarkers (std::vector<Marker> m) { markers = std::move (m); repaint(); }
void TimelineView::setTitle (const juce::String& t)   { if (t != title) { title = t; repaint(); } }

void TimelineView::setTransport (bool p, bool r)
{
    if (p == playing && r == recording) return;
    playing = p;
    recording = r;
    repaint();
}

void TimelineView::setPlayhead (uint64_t frame)
{
    if (frame == playhead && ! playing && ! recording) return;
    playhead = frame;
    followPlayhead();
    repaint();
}

void TimelineView::setFollow (bool on)
{
    follow = on;
    followBtn.setToggleState (on, juce::dontSendNotification);
    followPlayhead();
}

uint64_t TimelineView::contentLength() const
{
    uint64_t len = length;
    for (const auto& r : rows)
        if (r.peaks != nullptr) len = std::max (len, r.clipOffset + r.peaks->frames());
    return len;
}

void TimelineView::zoomToFit()
{
    const auto len = contentLength();
    fpp = len > 0 ? juce::jmax (1.0, (double) len / (laneWidth() * 0.97))
                  : sampleRate * 120.0 / laneWidth();   // empty: show two minutes
    viewStart = 0;
    updateScrollBars();
    repaint();
}

void TimelineView::zoomAround (double factor, int x)
{
    const double anchor = xToFrame ((float) x);
    const double maxFpp = juce::jmax ((double) contentLength(), sampleRate * 600.0) / juce::jmax (1, laneWidth()) * 2.0;
    fpp = juce::jlimit (0.5, juce::jmax (1.0, maxFpp), fpp * factor);
    viewStart = juce::jmax (0.0, anchor - (x - kHeaderW) * fpp);
    updateScrollBars();
    repaint();
}

void TimelineView::setRowHeight (int h)
{
    rowH = h;
    updateScrollBars();
    repaint();
}

void TimelineView::followPlayhead()
{
    if (! follow || dragging || ! (playing || recording)) return;
    const float x = frameToX ((double) playhead);
    if (x > (float) kHeaderW + (float) laneWidth() * 0.92f || x < (float) kHeaderW)
    {
        viewStart = juce::jmax (0.0, (double) playhead - laneWidth() * fpp * 0.08);
        updateScrollBars();
    }
}

void TimelineView::updateScrollBars()
{
    const double visible = laneWidth() * fpp;
    const double total = juce::jmax ((double) contentLength() + visible * 0.5, viewStart + visible);
    hbar.setRangeLimits (0.0, total, juce::dontSendNotification);
    hbar.setCurrentRange (viewStart, visible, juce::dontSendNotification);

    const int contentH = (int) rows.size() * rowH;
    const int viewH = rowsArea().getHeight();
    scrollY = juce::jlimit (0, juce::jmax (0, contentH - viewH), scrollY);
    vbar.setRangeLimits (0.0, juce::jmax (contentH, viewH), juce::dontSendNotification);
    vbar.setCurrentRange (scrollY, viewH, juce::dontSendNotification);
}

void TimelineView::scrollBarMoved (juce::ScrollBar* sb, double start)
{
    if (sb == &hbar)
    {
        viewStart = juce::jmax (0.0, start);
        if (playing || recording) setFollow (false); // the user took over the view
    }
    else scrollY = (int) start;
    repaint();
}

// ------------------------------------------------------------------------------------ layout

void TimelineView::resized()
{
    auto tb = toolbarArea().reduced (6, 3);
    followBtn.setBounds (tb.removeFromRight (58));
    tb.removeFromRight (4);
    fit.setBounds (tb.removeFromRight (40));
    tb.removeFromRight (2);
    zoomIn.setBounds (tb.removeFromRight (26));
    tb.removeFromRight (2);
    zoomOut.setBounds (tb.removeFromRight (26));
    tb.removeFromRight (14);
    large.setBounds (tb.removeFromRight (24));
    medium.setBounds (tb.removeFromRight (24));
    small.setBounds (tb.removeFromRight (24));

    const auto ra = rowsArea();
    vbar.setBounds (getWidth() - kScrollW, ra.getY(), kScrollW, ra.getHeight());
    hbar.setBounds (kHeaderW, getHeight() - kScrollW, laneWidth(), kScrollW);
    if (! userZoomed && length == 0) zoomToFit();
    updateScrollBars();
}

juce::Rectangle<int> TimelineView::rowBounds (int i) const
{
    const auto ra = rowsArea();
    return { 0, ra.getY() + i * rowH - scrollY, ra.getWidth(), rowH };
}

int TimelineView::rowAtY (int y) const
{
    const auto ra = rowsArea();
    if (y < ra.getY() || y >= ra.getBottom()) return -1;
    const int i = (y - ra.getY() + scrollY) / rowH;
    return i < (int) rows.size() ? i : -1;
}

juce::Rectangle<int> TimelineView::buttonRect (juce::Rectangle<int> header, int index) const
{
    constexpr int w = 26, h = 16, gap = 3;
    const int x = header.getRight() - 8 - (3 - index) * (w + gap) + gap;
    return { x, header.getCentreY() - h / 2, w, h };
}

TimelineView::HeaderHit TimelineView::hitHeader (juce::Point<int> p, int& row) const
{
    row = rowAtY (p.y);
    if (row < 0 || p.x >= kHeaderW) return HeaderHit::none;
    const auto header = rowBounds (row).withWidth (kHeaderW);
    if (buttonRect (header, 0).expanded (2).contains (p)) return HeaderHit::arm;
    if (buttonRect (header, 1).expanded (2).contains (p)) return HeaderHit::mute;
    if (buttonRect (header, 2).expanded (2).contains (p)) return HeaderHit::solo;
    return HeaderHit::body;
}

// ------------------------------------------------------------------------------------ paint

static juce::String formatTime (double secs, double step)
{
    const int total = (int) secs;
    const int h = total / 3600, m = (total / 60) % 60, s = total % 60;
    juce::String out = h > 0 ? juce::String (h) + ":" + juce::String (m).paddedLeft ('0', 2) : juce::String (m);
    out << ":" << juce::String (s).paddedLeft ('0', 2);
    if (step < 1.0) out << "." << juce::String ((int) ((secs - total) * 10.0));
    return out;
}

void TimelineView::paintRuler (juce::Graphics& g)
{
    const auto r = rulerArea();
    g.setColour (Theme::panel);
    g.fillRect (r);
    g.setColour (Theme::edge);
    g.drawHorizontalLine (r.getBottom() - 1, (float) r.getX(), (float) r.getRight());

    static const double steps[] = { 0.1, 0.25, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600 };
    double step = steps[std::size (steps) - 1];
    for (double s : steps) if (s * sampleRate / fpp >= 80.0) { step = s; break; }

    const double firstSec = std::floor (viewStart / sampleRate / step) * step;
    const double lastSec = xToFrame ((float) r.getRight()) / sampleRate;
    g.setFont (Theme::mono (10.5f));
    for (double s = firstSec; s <= lastSec; s += step)
    {
        const float x = frameToX (s * sampleRate);
        if (x < (float) r.getX()) continue;
        g.setColour (Theme::textDim);
        g.drawVerticalLine ((int) x, (float) r.getBottom() - 8.0f, (float) r.getBottom());
        g.drawText (formatTime (s, step), juce::Rectangle<float> (x + 3.0f, (float) r.getY() + 2.0f, 70.0f, 14.0f), juce::Justification::centredLeft);
        for (int k = 1; k < 4; ++k) // minor ticks
        {
            const float mx = frameToX ((s + step * k / 4.0) * sampleRate);
            g.drawVerticalLine ((int) mx, (float) r.getBottom() - 4.0f, (float) r.getBottom());
        }
    }

    // markers: yellow flag + label
    for (const auto& m : markers)
    {
        const float x = frameToX ((double) m.frame);
        if (x < (float) r.getX() - 80.0f || x > (float) r.getRight()) continue;
        juce::Path flag;
        flag.addTriangle (x, (float) r.getBottom() - 12.0f, x + 6.0f, (float) r.getBottom() - 6.0f, x, (float) r.getBottom());
        g.setColour (Theme::yellow);
        g.fillPath (flag);
        g.setFont (Theme::ui (10.5f, true));
        g.drawText (m.label, juce::Rectangle<float> (x + 8.0f, (float) r.getBottom() - 13.0f, 120.0f, 12.0f), juce::Justification::centredLeft);
    }
}

void TimelineView::paintRow (juce::Graphics& g, int i, juce::Rectangle<int> r)
{
    const auto& row = rows[(size_t) i];
    auto header = r.withWidth (kHeaderW);
    auto lane = r.withTrimmedLeft (kHeaderW);

    // ---- header: track colour block with name and REC / M / S
    g.setColour (row.colour.withMultipliedBrightness (row.mute ? 0.45f : 0.85f));
    g.fillRect (header.reduced (0, 1));
    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.fillRect (header.getRight() - 2, header.getY(), 2, header.getHeight());

    if (row.selected)
    {
        g.setColour (juce::Colours::white.withAlpha (0.22f));
        g.fillRect (header.reduced (0, 1));
        g.setColour (juce::Colours::white);
        g.drawRect (header.reduced (1, 2), 2);
        g.setColour (juce::Colours::white.withAlpha (0.05f));
        g.fillRect (lane);
    }

    const auto textArea = header.withTrimmedLeft (8).withTrimmedRight (3 * 29 + 10);
    g.setColour (juce::Colours::black.withAlpha (0.85f));
    if (rowH >= 48)
    {
        g.setFont (Theme::mono (10.0f));
        g.drawText (juce::String (i + 1) + (row.stereo ? "   ST" : ""), textArea.withHeight (16).translated (0, 4), juce::Justification::centredLeft);
        g.setFont (Theme::ui (13.0f, true));
        g.drawText (row.name, textArea.withTrimmedTop (18).withHeight (18), juce::Justification::centredLeft, true);
    }
    else
    {
        g.setFont (Theme::ui (12.0f, true));
        g.drawText (juce::String (i + 1) + (row.stereo ? " ST " : "  ") + row.name, textArea, juce::Justification::centredLeft, true);
    }

    auto drawButton = [&] (int idx, const juce::String& label, bool on, juce::Colour activeColour)
    {
        const auto b = buttonRect (header, idx).toFloat();
        g.setColour (on ? activeColour : juce::Colours::black.withAlpha (0.35f));
        g.fillRoundedRectangle (b, Theme::radius);
        g.setColour (on ? juce::Colours::black : Theme::textDim);
        g.setFont (Theme::ui (10.0f, true));
        g.drawText (label, b, juce::Justification::centred);
    };
    drawButton (0, "REC", row.armed, Theme::red);
    drawButton (1, "M", row.mute, Theme::yellow);
    drawButton (2, "S", row.solo, Theme::green);

    // ---- lane
    g.setColour ((i & 1) ? juce::Colour (0xff15181c) : juce::Colour (0xff181b20));
    g.fillRect (lane);
    g.setColour (juce::Colours::black.withAlpha (0.5f));
    g.drawHorizontalLine (r.getBottom() - 1, (float) lane.getX(), (float) lane.getRight());

    if (row.peaks == nullptr) return;
    const double off = (double) row.clipOffset;
    const double frames = (double) row.peaks->frames();
    const float x0 = juce::jmax ((float) lane.getX(), frameToX (off));
    const float x1 = juce::jmin ((float) lane.getRight(), frameToX (off + frames));
    if (x1 <= x0) return;

    const bool live = recording && recordingSession;
    const bool beingMoved = clipDrag.moved && clipDrag.fromRow == i;
    auto clip = juce::Rectangle<float> (x0, (float) lane.getY() + 2.0f, x1 - x0, (float) lane.getHeight() - 5.0f);
    g.setColour (row.colour.withAlpha (row.mute ? 0.10f : 0.22f));
    g.fillRect (clip);
    g.setColour (live && row.armed ? Theme::red : row.colour.withAlpha (0.8f));
    g.fillRect (clip.withHeight (2.0f));

    // Name bar: drag it to move the clip (time and/or track).
    if (rowH >= 40 && ! live)
    {
        const auto bar = clip.withHeight ((float) kClipBarH);
        g.setColour (row.colour.withAlpha (0.55f));
        g.fillRect (bar);
        g.setColour (juce::Colours::black.withAlpha (0.85f));
        g.setFont (Theme::ui (10.0f, true));
        g.drawText (row.clipName, bar.reduced (4.0f, 0.0f), juce::Justification::centredLeft, true);
        clip = clip.withTrimmedTop ((float) kClipBarH);
    }
    const int px0 = (int) x0, w = (int) x1 - px0;
    if (w <= 0) return;
    scratch.resize ((size_t) w);
    const auto waveColour = row.colour.interpolatedWith (juce::Colours::white, 0.6f).withAlpha (beingMoved ? 0.25f : (row.mute ? 0.35f : 0.92f));

    // Stereo files draw two lanes (L on top, R below); mono draws one.
    const int nch = row.peaks->numChannels();
    for (int c = 0; c < nch; ++c)
    {
        auto chLane = nch == 1 ? clip : (c == 0 ? clip.withHeight (clip.getHeight() * 0.5f)
                                                : clip.withTrimmedTop (clip.getHeight() * 0.5f));
        row.peaks->render (xToFrame ((float) px0) - off, fpp, w, scratch.data(), nch == 1 ? -1 : c);

        const float mid = chLane.getCentreY() + (nch == 1 ? 1.0f : 0.0f);
        const float half = chLane.getHeight() * 0.5f - (nch == 1 ? 3.0f : 1.5f);
        juce::RectangleList<float> bars;
        bars.ensureStorageAllocated (w);
        for (int k = 0; k < w; ++k)
        {
            const float v = scratch[(size_t) k];
            if (v <= 0.0f) continue;
            const float hh = juce::jmax (0.5f, v * half);
            bars.addWithoutMerging ({ (float) (px0 + k), mid - hh, 1.0f, hh * 2.0f });
        }
        g.setColour (waveColour);
        g.fillRectList (bars);

        if (nch == 2)
        {
            if (c == 1)
            {
                g.setColour (juce::Colours::black.withAlpha (0.45f));
                g.fillRect (chLane.getX(), chLane.getY() - 0.5f, chLane.getWidth(), 1.0f);
            }
            if (rowH >= 48)
            {
                g.setColour (row.colour.withAlpha (0.9f));
                g.setFont (Theme::ui (9.5f, true));
                g.drawText (c == 0 ? "L" : "R", juce::Rectangle<float> (juce::jmax (chLane.getX(), (float) kHeaderW) + 3.0f, chLane.getY() + 1.0f, 10.0f, 11.0f),
                            juce::Justification::centredLeft);
            }
        }
    }
}

void TimelineView::paint (juce::Graphics& g)
{
    g.fillAll (Theme::bg);

    // toolbar
    const auto tb = toolbarArea();
    g.setColour (Theme::panel);
    g.fillRect (tb);
    g.setColour (recording ? Theme::red : (playing ? Theme::green : Theme::text));
    g.setFont (Theme::ui (12.0f, true));
    const double secs = (double) contentLength() / sampleRate;
    g.drawText (title + (secs > 0 ? "   " + formatTime (secs, 1.0) : juce::String()), tb.reduced (10, 0).withWidth (400), juce::Justification::centredLeft);
    g.setColour (Theme::textDim);
    g.setFont (Theme::ui (11.0f));
    g.drawText ("Click or drag to locate.  Cmd+scroll zooms.  Space plays/stops.",
                tb.withTrimmedLeft (420).withTrimmedRight (330), juce::Justification::centredRight, true);

    // corner above the headers
    const auto corner = juce::Rectangle<int> (0, kToolbarH, kHeaderW, kRulerH);
    g.setColour (Theme::panel);
    g.fillRect (corner);
    g.setColour (Theme::textDim);
    g.setFont (Theme::ui (11.0f, true));
    g.drawText ("TRACKS  (" + juce::String ((int) rows.size()) + ")", corner.reduced (10, 0), juce::Justification::centredLeft);

    paintRuler (g);

    // rows
    {
        juce::Graphics::ScopedSaveState s (g);
        const auto ra = rowsArea();
        g.reduceClipRegion (ra);
        const int first = juce::jmax (0, scrollY / rowH);
        const int last = juce::jmin ((int) rows.size() - 1, (scrollY + ra.getHeight()) / rowH);
        for (int i = first; i <= last; ++i) paintRow (g, i, rowBounds (i));
        if (rows.empty())
        {
            g.setColour (Theme::textDim);
            g.setFont (Theme::ui (13.0f));
            g.drawText ("No tracks yet: click + Track, use Tracks > Add Multiple Tracks..., or drop WAV files here", ra, juce::Justification::centred);
        }

        // drag-and-drop insertion line
        if (reordering && dropIndex >= 0)
        {
            const int y = ra.getY() + dropIndex * rowH - scrollY;
            g.setColour (Theme::blue);
            g.fillRect (0, y - 2, ra.getWidth(), 4);
        }

        // marker lines across the lanes
        g.reduceClipRegion (ra.withTrimmedLeft (kHeaderW));
        for (const auto& m : markers)
        {
            const float x = frameToX ((double) m.frame);
            g.setColour (Theme::yellow.withAlpha (0.45f));
            const float dash[] = { 4.0f, 4.0f };
            g.drawDashedLine (juce::Line<float> (x, (float) ra.getY(), x, (float) ra.getBottom()), dash, 2, 1.0f);
        }
    }

    paintClipGhost (g);

    // playhead across ruler + lanes
    const float px = frameToX (dragging ? dragFrame : (double) playhead);
    if (px >= (float) kHeaderW && px <= (float) (kHeaderW + laneWidth()))
    {
        const auto col = recording ? Theme::red : (playing ? Theme::green : juce::Colours::white);
        g.setColour (col);
        g.fillRect (px - 0.75f, (float) kToolbarH, 1.5f, (float) (rowsArea().getBottom() - kToolbarH));
        juce::Path tri;
        tri.addTriangle (px - 6.0f, (float) kToolbarH, px + 6.0f, (float) kToolbarH, px, (float) kToolbarH + 9.0f);
        g.fillPath (tri);
    }

    g.setColour (Theme::edge);
    g.drawHorizontalLine (getHeight() - kScrollW - 1, 0.0f, (float) getWidth());

    paintDropOverlay (g);
}

void TimelineView::paintClipGhost (juce::Graphics& g)
{
    if (clipDrag.fromRow < 0 || ! clipDrag.moved) return;
    const auto& src = rows[(size_t) clipDrag.fromRow];
    if (src.peaks == nullptr) return;
    const auto ra = rowsArea();
    juce::Graphics::ScopedSaveState s (g);
    g.reduceClipRegion (ra.withTrimmedLeft (kHeaderW));
    const auto lane = rowBounds (clipDrag.toRow).withTrimmedLeft (kHeaderW);
    const float x0 = frameToX ((double) clipDrag.newOffset);
    const float x1 = frameToX ((double) (clipDrag.newOffset + src.peaks->frames()));
    const auto ghost = juce::Rectangle<float> (x0, (float) lane.getY() + 2.0f, x1 - x0, (float) lane.getHeight() - 5.0f);
    g.setColour (src.colour.withAlpha (0.35f));
    g.fillRect (ghost);
    g.setColour (juce::Colours::white);
    g.drawRect (ghost, 1.5f);
    g.fillRect (x0 - 1.0f, (float) ra.getY(), 2.0f, (float) ra.getHeight());

    juce::String label = formatTime ((double) clipDrag.newOffset / sampleRate, 0.1);
    if (clipDrag.toRow != clipDrag.fromRow)
    {
        const auto& dst = rows[(size_t) clipDrag.toRow];
        label << "  ->  " << dst.name << (dst.peaks != nullptr ? "  (has audio)" : "");
    }
    g.setFont (Theme::ui (11.0f, true));
    const auto tb = juce::Rectangle<float> (juce::jmax ((float) kHeaderW + 2.0f, x0 + 4.0f), (float) lane.getY() + 4.0f, 320.0f, 16.0f);
    g.setColour (juce::Colours::black.withAlpha (0.7f));
    g.fillRoundedRectangle (tb.withWidth (juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), label) + 10.0f), 3.0f);
    g.setColour (juce::Colours::white);
    g.drawText (label, tb.translated (5.0f, 0.0f), juce::Justification::centredLeft, false);
}

// ------------------------------------------------------------------------------------ file drop

static bool isAudioOrFolder (const juce::String& path)
{
    const juce::File f (path);
    return f.isDirectory() || f.hasFileExtension ("wav;wave;bwf;aif;aiff;aifc");
}

bool TimelineView::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& f : files) if (isAudioOrFolder (f)) return true;
    return false;
}

uint64_t TimelineView::dropFrameForX (int x) const
{
    // Within 12 px of the start (or over the track headers) snaps to zero, keeping the take aligned.
    if (x < kHeaderW || (float) x - frameToX (0.0) < 12.0f) return 0;
    return (uint64_t) juce::jmax (0.0, xToFrame ((float) x));
}

int TimelineView::dropRowForY (int y) const
{
    const auto ra = rowsArea();
    if (y < ra.getY() || y >= ra.getBottom()) return -1;
    const int r = (y - ra.getY() + scrollY) / rowH;
    return r >= 0 && r < (int) rows.size() ? r : -1;
}

void TimelineView::fileDragEnter (const juce::StringArray&, int x, int y) { fileHover = true; fileHoverX = x; fileHoverY = y; repaint(); }
void TimelineView::fileDragMove (const juce::StringArray&, int x, int y)  { fileHoverX = x; fileHoverY = y; repaint(); }
void TimelineView::fileDragExit (const juce::StringArray&)              { fileHover = false; repaint(); }

void TimelineView::filesDropped (const juce::StringArray& files, int x, int y)
{
    fileHover = false;
    repaint();
    juce::StringArray audio;
    for (const auto& f : files) if (isAudioOrFolder (f)) audio.add (f);
    if (! audio.isEmpty() && onFilesDropped) onFilesDropped (audio, dropFrameForX (x), dropRowForY (y));
}

void TimelineView::setBusyText (const juce::String& text)
{
    if (busyText == text) return;
    busyText = text;
    repaint();
}

void TimelineView::paintDropOverlay (juce::Graphics& g)
{
    const auto ra = rowsArea();
    if (fileHover)
    {
        const bool ok = ! canAcceptDrop || canAcceptDrop();
        const auto col = ok ? Theme::blue : Theme::red;
        g.setColour (col.withAlpha (0.10f));
        g.fillRect (ra);
        g.setColour (col);
        g.drawRect (ra, 2);

        juce::String msg = "Recording: stop before importing audio";
        if (ok)
        {
            const auto frame = dropFrameForX (fileHoverX);
            const float x = frameToX ((double) frame);
            const int row = dropRowForY (fileHoverY);
            const auto at = formatTime ((double) frame / sampleRate, 0.1);
            if (row >= 0)
            {
                const auto rb = rowBounds (row);
                g.setColour (col.withAlpha (0.22f));
                g.fillRect (rb);
                g.setColour (col);
                g.drawRect (rb, 2);
                g.fillRect (x - 1.0f, (float) rb.getY(), 2.0f, (float) rb.getHeight());
                const auto& r = rows[(size_t) row];
                msg = "Drop onto " + r.name + " at " + at + (r.peaks != nullptr ? "  (replaces its audio in this take)" : "");
            }
            else
            {
                g.fillRect (x - 1.0f, (float) ra.getY(), 2.0f, (float) ra.getHeight());
                msg = "Drop to add as new tracks, starting at " + at;
            }
        }
        g.setFont (Theme::ui (14.0f, true));
        const auto box = juce::Rectangle<int> (juce::jmin (ra.getWidth() - 20, 620), 40)
                             .withCentre ({ ra.getCentreX(), juce::jlimit (ra.getY() + 30, ra.getBottom() - 30,
                                                                         dropRowForY (fileHoverY) >= 0 ? rowBounds (dropRowForY (fileHoverY)).getBottom() + 30
                                                                                                      : fileHoverY - 50) });
        g.setColour (Theme::panel.withAlpha (0.92f));
        g.fillRoundedRectangle (box.toFloat(), Theme::radius);
        g.setColour (Theme::text);
        g.drawText (msg, box, juce::Justification::centred);
    }
    else if (busyText.isNotEmpty())
    {
        const auto box = juce::Rectangle<int> (0, 0, juce::jmin (ra.getWidth() - 20, 520), 34)
                             .withCentre ({ ra.getCentreX(), ra.getY() + 30 });
        g.setColour (Theme::panel.withAlpha (0.95f));
        g.fillRoundedRectangle (box.toFloat(), Theme::radius);
        g.setColour (Theme::blue);
        g.fillRect (box.withWidth (3));
        g.setColour (Theme::text);
        g.setFont (Theme::ui (13.0f));
        g.drawText (busyText, box.reduced (12, 0), juce::Justification::centred, true);
    }
}

// ------------------------------------------------------------------------------------ mouse

juce::Rectangle<float> TimelineView::clipBounds (int row) const
{
    if (row < 0 || row >= (int) rows.size() || rows[(size_t) row].peaks == nullptr) return {};
    const auto& r = rows[(size_t) row];
    const auto lane = rowBounds (row).withTrimmedLeft (kHeaderW);
    const float x0 = juce::jmax ((float) lane.getX(), frameToX ((double) r.clipOffset));
    const float x1 = juce::jmin ((float) lane.getRight(), frameToX ((double) (r.clipOffset + r.peaks->frames())));
    if (x1 <= x0) return {};
    return { x0, (float) lane.getY() + 2.0f, x1 - x0, (float) lane.getHeight() - 5.0f };
}

bool TimelineView::hitClip (juce::Point<int> p, int& row) const
{
    if (p.x < kHeaderW || recording) return false;
    row = dropRowForY (p.y);
    return row >= 0 && clipBounds (row).contains (p.toFloat());
}

bool TimelineView::hitClipHandle (juce::Point<int> p, int& row) const
{
    if (rowH < 40 || ! hitClip (p, row)) return false;
    return (float) p.y < clipBounds (row).getY() + (float) kClipBarH;
}

uint64_t TimelineView::snapOffset (uint64_t off, int ignoreRow) const
{
    const auto px = [this] (uint64_t f) { return frameToX ((double) f); };
    if (px (off) - px (0) < 10.0f) return 0;                                         // start of the take
    if (std::abs (px (off) - px (clipDrag.origOffset)) < 6.0f) return clipDrag.origOffset;   // where it was
    for (int i = 0; i < (int) rows.size(); ++i)                                       // other clips' starts / ends
    {
        if (i == ignoreRow || rows[(size_t) i].peaks == nullptr) continue;
        for (uint64_t edge : { rows[(size_t) i].clipOffset, rows[(size_t) i].clipOffset + rows[(size_t) i].peaks->frames() })
            if (std::abs (px (off) - px (edge)) < 6.0f) return edge;
    }
    for (const auto& m : markers)
        if (std::abs (px (off) - px (m.frame)) < 6.0f) return m.frame;
    return off;
}

void TimelineView::mouseMove (const juce::MouseEvent& e)
{
    int row = -1;
    if (hitClipHandle (e.getPosition(), row) || (e.mods.isAltDown() && hitClip (e.getPosition(), row)))
    {
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
        return;
    }
    const bool inLanes = e.x >= kHeaderW && e.y >= kToolbarH && e.y < rowsArea().getBottom();
    setMouseCursor (inLanes ? juce::MouseCursor::IBeamCursor : juce::MouseCursor::NormalCursor);
}

void TimelineView::mouseDown (const juce::MouseEvent& e)
{
    if (renameRow >= 0) commitRename (0);
    if (onInteraction) onInteraction();
    const auto p = e.getPosition();
    if (p.y < kToolbarH) return;

    if (p.x < kHeaderW)
    {
        int row = -1;
        const auto hit = hitHeader (p, row);
        if (row < 0) return;
        const auto& r = rows[(size_t) row];
        if (e.mods.isPopupMenu())
        {
            if (onTrackMenu) onTrackMenu (row);
            return;
        }
        if (hit == HeaderHit::arm && onArm)   { onArm (row, ! r.armed); return; }
        if (hit == HeaderHit::mute && onMute) { onMute (row, ! r.mute); return; }
        if (hit == HeaderHit::solo && onSolo) { onSolo (row, ! r.solo); return; }

        // body: select (Shift = range, Cmd = toggle) and arm a possible drag-and-drop
        if (onSelect) onSelect (row, e.mods);
        pressRow = row;
        pressY = p.y;
        reorderArmed = true;
        reordering = false;
        return;
    }

    // Clips: right-click for the clip menu; drag the name bar (or Alt-drag the clip) to move it.
    int clipRow = -1;
    if (e.mods.isPopupMenu())
    {
        if (hitClip (p, clipRow) && onClipMenu) onClipMenu (clipRow, (uint64_t) juce::jmax (0.0, xToFrame ((float) p.x)));
        return;
    }
    if (hitClipHandle (p, clipRow) || (e.mods.isAltDown() && hitClip (p, clipRow)))
    {
        clipDrag = {};
        clipDrag.fromRow = clipDrag.toRow = clipRow;
        clipDrag.pressX = p.x;
        clipDrag.origOffset = clipDrag.newOffset = rows[(size_t) clipRow].clipOffset;
        return;
    }

    if (p.y < rowsArea().getBottom() && p.x < kHeaderW + laneWidth())
    {
        dragging = true;
        dragFrame = juce::jmax (0.0, xToFrame ((float) p.x));
        repaint();
    }
}

int TimelineView::dropIndexForY (int y) const
{
    const auto ra = rowsArea();
    const int idx = (int) std::lround ((double) (y - ra.getY() + scrollY) / (double) rowH);
    return juce::jlimit (0, (int) rows.size(), idx);
}

void TimelineView::mouseDrag (const juce::MouseEvent& e)
{
    if (clipDrag.fromRow >= 0)
    {
        const auto ra = rowsArea();
        if (e.y < ra.getY() + 10) scrollY -= rowH / 4;
        else if (e.y > ra.getBottom() - 10) scrollY += rowH / 4;
        updateScrollBars();
        const double raw = (double) clipDrag.origOffset + (e.x - clipDrag.pressX) * fpp;
        clipDrag.newOffset = snapOffset ((uint64_t) juce::jmax (0.0, raw), clipDrag.fromRow);
        const int over = dropRowForY (juce::jlimit (ra.getY(), ra.getBottom() - 1, e.y));
        clipDrag.toRow = over >= 0 ? over : (e.y < ra.getY() ? (scrollY / rowH) : (int) rows.size() - 1);
        clipDrag.toRow = juce::jlimit (0, (int) rows.size() - 1, clipDrag.toRow);
        if (std::abs (e.x - clipDrag.pressX) > 3 || clipDrag.toRow != clipDrag.fromRow) clipDrag.moved = true;
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
        repaint();
        return;
    }
    if (reorderArmed)
    {
        if (! reordering && std::abs (e.y - pressY) > 5) reordering = true;
        if (reordering)
        {
            // auto-scroll when dragging near the top / bottom edge
            const auto ra = rowsArea();
            if (e.y < ra.getY() + 20) scrollY -= rowH / 3;
            else if (e.y > ra.getBottom() - 20) scrollY += rowH / 3;
            updateScrollBars();
            dropIndex = dropIndexForY (e.y);
            setMouseCursor (juce::MouseCursor::DraggingHandCursor);
            repaint();
        }
        return;
    }
    if (! dragging) return;
    dragFrame = juce::jlimit (0.0, juce::jmax (0.0, (double) contentLength()), xToFrame ((float) e.x));
    repaint();
}

void TimelineView::mouseUp (const juce::MouseEvent&)
{
    if (clipDrag.fromRow >= 0)
    {
        const auto d = clipDrag;
        clipDrag = {};
        setMouseCursor (juce::MouseCursor::NormalCursor);
        repaint();
        if (d.moved && (d.toRow != d.fromRow || d.newOffset != d.origOffset) && onClipMoved) onClipMoved (d.fromRow, d.toRow, d.newOffset);
        return;
    }
    if (reorderArmed)
    {
        const bool drop = reordering && dropIndex >= 0;
        const int target = dropIndex;
        reorderArmed = reordering = false;
        dropIndex = -1;
        setMouseCursor (juce::MouseCursor::NormalCursor);
        repaint();
        if (drop && onMoveSelected) onMoveSelected (target);
        return;
    }
    if (! dragging) return;
    dragging = false;
    playhead = (uint64_t) dragFrame;
    if (onLocate) onLocate (playhead);
    repaint();
}

void TimelineView::mouseDoubleClick (const juce::MouseEvent& e)
{
    int row = -1;
    if (e.x < kHeaderW && hitHeader (e.getPosition(), row) == HeaderHit::body && row >= 0) startRename (row);
}

void TimelineView::startRename (int row)
{
    if (row < 0 || row >= (int) rows.size()) return;
    // scroll the row into view
    const int viewH = rowsArea().getHeight();
    if (row * rowH < scrollY) scrollY = row * rowH;
    else if ((row + 1) * rowH > scrollY + viewH) scrollY = (row + 1) * rowH - viewH;
    updateScrollBars();
    repaint();

    renameRow = row;
    const auto header = rowBounds (row).withWidth (kHeaderW);
    renameEditor.setBounds (header.withTrimmedLeft (6).withTrimmedRight (3 * 29 + 12).withSizeKeepingCentre (kHeaderW - 6 - 3 * 29 - 12, 24));
    renameEditor.setText (rows[(size_t) row].name, juce::dontSendNotification);
    renameEditor.setVisible (true);
    renameEditor.grabKeyboardFocus();
    renameEditor.selectAll();
}

void TimelineView::commitRename (int moveBy)
{
    if (renameRow < 0) return;
    const int row = renameRow;
    const auto text = renameEditor.getText().trim();
    const int next = row + moveBy;

    if (moveBy != 0 && next >= 0 && next < (int) rows.size())
    {
        // Tab: keep the editor visible and focused, just move it to the next track
        if (onRename && text != rows[(size_t) row].name) onRename (row, text);
        startRename (next);
        return;
    }

    renameRow = -1;
    renameEditor.setVisible (false);
    if (onRename && text != rows[(size_t) row].name) onRename (row, text);
    if (onInteraction) onInteraction();
}

void TimelineView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (e.mods.isCommandDown() || e.mods.isCtrlDown())
    {
        userZoomed = true;
        const float d = std::abs (w.deltaY) > std::abs (w.deltaX) ? w.deltaY : w.deltaX;
        zoomAround (d > 0 ? 1.0 / 1.25 : 1.25, juce::jmax (kHeaderW, e.x));
        return;
    }
    const bool horizontal = e.mods.isShiftDown() || std::abs (w.deltaX) > std::abs (w.deltaY);
    if (horizontal)
    {
        const float d = std::abs (w.deltaX) > std::abs (w.deltaY) ? w.deltaX : w.deltaY;
        viewStart = juce::jmax (0.0, viewStart - (double) d * (double) laneWidth() * fpp * 0.5);
        if (playing || recording) setFollow (false);
    }
    else
    {
        scrollY -= (int) (w.deltaY * (float) rowH * 4.0f);
    }
    updateScrollBars();
    repaint();
}
