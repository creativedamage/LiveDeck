#pragma once
// Tracks Live style timeline: one row per track (colour header with REC / M / S on the
// left, waveform lane on the right), a time ruler with markers, and a playhead you can
// click or drag anywhere to locate. Zoom with Cmd/Ctrl + scroll or the toolbar.
//
// Everything is painted by this one component (no child widget per row), so 128+ rows
// stay cheap; only visible rows are drawn.

#include "Core/PatchModel.h"
#include "Core/PeakStore.h"
#include "Theme.h"
#include <set>

class TimelineView final : public juce::Component, public juce::FileDragAndDropTarget, private juce::ScrollBar::Listener
{
public:
    struct Marker { uint64_t frame = 0; juce::String label; };

    TimelineView();

    // ---- model (message thread)
    void setTracks (const std::vector<omr::TrackState>& tracks, bool recording, const std::set<int>& selectedIds = {});
    void startRename (int row);                  // inline name editor; Tab / Shift+Tab move to the next / previous track
    // A track's audio in the shown take: waveform, where it starts in the take, and its name.
    void setPeaks (int track, std::shared_ptr<omr::PeakStore> store, uint64_t offset = 0, const juce::String& clipName = {});
    void clearPeaks();
    void setSampleRate (double sr);
    void setLength (uint64_t frames);           // take length (grows while recording)
    void setMarkers (std::vector<Marker> m);
    void setTitle (const juce::String& t);      // "Take 3" / "Recording take 4"
    void setTransport (bool playing, bool recording);
    void setPlayhead (uint64_t frame);
    void setFollow (bool on);
    void zoomToFit();
    void zoomBy (double factor) { userZoomed = true; zoomAround (factor, kHeaderW + laneWidth() / 2); }

    std::function<void (uint64_t frame)> onLocate;      // click / drag-release
    std::function<void (int track, bool)> onArm, onMute, onSolo;
    std::function<void (int track, int paletteIndex)> onColour;
    std::function<void()> onInteraction;                // lets the window take keyboard focus back
    std::function<void (int row, juce::ModifierKeys)> onSelect;   // click on a track header
    std::function<void (int row)> onTrackMenu;                    // right-click on a track header
    std::function<void (int insertBefore)> onMoveSelected;        // header drag-and-drop finished
    std::function<void (int row, const juce::String&)> onRename;
    // Audio files / folders dropped on the timeline; `frame` = where they should start (0 near the left edge).
    // `row` = the track dropped on (its audio is replaced, extra files fill the tracks below), -1 = new tracks.
    std::function<void (const juce::StringArray& files, uint64_t frame, int row)> onFilesDropped;
    // Clip editing: drag a clip by its name bar (or Alt-drag anywhere on it) to move it in time or to another track.
    std::function<void (int fromRow, int toRow, uint64_t newOffset)> onClipMoved;
    std::function<void (int row, uint64_t frameAtClick)> onClipMenu;   // right-click on a clip
    std::function<bool()> canAcceptDrop;                 // false while recording
    void setBusyText (const juce::String& text);         // "Importing 3 of 12..." overlay; empty hides it

    // ---- juce::FileDragAndDropTarget
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray&, int x, int y) override;
    void fileDragMove (const juce::StringArray&, int x, int y) override;
    void fileDragExit (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

    // ---- juce::Component
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    struct Row
    {
        juce::String name;
        juce::Colour colour;
        bool armed = false, mute = false, solo = false, hasInput = false, stereo = false, selected = false;
        int paletteIndex = -1;
        std::shared_ptr<omr::PeakStore> peaks;
        uint64_t clipOffset = 0;
        juce::String clipName;
    };

    void scrollBarMoved (juce::ScrollBar*, double) override;
    void updateScrollBars();
    void zoomAround (double factor, int x);
    void setRowHeight (int h);
    void followPlayhead();
    uint64_t contentLength() const;

    juce::Rectangle<int> toolbarArea() const { return getLocalBounds().removeFromTop (kToolbarH); }
    juce::Rectangle<int> rulerArea() const   { return { kHeaderW, kToolbarH, laneWidth(), kRulerH }; }
    juce::Rectangle<int> rowsArea() const    { return { 0, kToolbarH + kRulerH, getWidth() - kScrollW, getHeight() - kToolbarH - kRulerH - kScrollW }; }
    int laneWidth() const                    { return juce::jmax (1, getWidth() - kHeaderW - kScrollW); }
    float frameToX (double f) const          { return (float) (kHeaderW + (f - viewStart) / fpp); }
    double xToFrame (float x) const          { return viewStart + (x - (float) kHeaderW) * fpp; }
    int rowAtY (int y) const;
    juce::Rectangle<int> rowBounds (int i) const;
    enum class HeaderHit { none, arm, mute, solo, body };
    HeaderHit hitHeader (juce::Point<int> p, int& row) const;
    juce::Rectangle<int> buttonRect (juce::Rectangle<int> header, int index) const;

    void paintRuler (juce::Graphics&);
    void paintRow (juce::Graphics&, int i, juce::Rectangle<int> r);

    static constexpr int kToolbarH = 28, kRulerH = 26, kHeaderW = 200, kScrollW = 10;

    std::vector<Row> rows;
    std::vector<Marker> markers;
    juce::String title;
    double sampleRate = 48000.0;
    uint64_t length = 0, playhead = 0;
    double fpp = 2000.0;        // frames per pixel
    double viewStart = 0.0;     // frame at the left edge of the lanes
    int scrollY = 0;
    int rowH = 56;
    bool playing = false, recording = false, follow = true, recordingSession = false;
    bool dragging = false, userZoomed = false;
    double dragFrame = 0;
    std::vector<float> scratch;

    // header drag-and-drop
    int pressRow = -1, pressY = 0, dropIndex = -1;
    bool reorderArmed = false, reordering = false;
    int dropIndexForY (int y) const;

    // file drop
    bool fileHover = false;
    int fileHoverX = 0, fileHoverY = 0;
    int dropRowForY (int y) const;   // row under y, -1 below the last track

    // clip drag
    juce::Rectangle<float> clipBounds (int row) const;     // the clip's rectangle in this row's lane
    bool hitClipHandle (juce::Point<int> p, int& row) const;
    bool hitClip (juce::Point<int> p, int& row) const;
    struct ClipDrag { int fromRow = -1, toRow = -1, pressX = 0; uint64_t origOffset = 0, newOffset = 0; bool moved = false; } clipDrag;
    uint64_t snapOffset (uint64_t off, int ignoreRow) const;
    void paintClipGhost (juce::Graphics&);
    static constexpr int kClipBarH = 13;
    juce::String busyText;
    uint64_t dropFrameForX (int x) const;
    void paintDropOverlay (juce::Graphics&);

    // inline rename
    struct RenameEditor final : public juce::TextEditor
    {
        std::function<void (bool backwards)> onTab;
        bool keyPressed (const juce::KeyPress& k) override
        {
            if (k.getKeyCode() == juce::KeyPress::tabKey) { if (onTab) onTab (k.getModifiers().isShiftDown()); return true; }
            return juce::TextEditor::keyPressed (k);
        }
    };
    RenameEditor renameEditor;
    int renameRow = -1;
    void commitRename (int moveBy);

    juce::ScrollBar hbar { false }, vbar { true };
    juce::TextButton zoomOut { "-" }, zoomIn { "+" }, fit { "Fit" }, followBtn { "Follow" };
    juce::TextButton small { "S" }, medium { "M" }, large { "L" };
};
