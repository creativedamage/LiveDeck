#pragma once
// One track: name, input dropdown, output dropdown, trim, meter, arm / mute / solo.
// The strip never touches engine state; it reports intent through the callbacks and
// MainComponent applies it through the patch model (which enforces the record locks).

#include "Core/PatchModel.h"
#include "Meter.h"

class ChannelStrip final : public juce::Component
{
public:
    static constexpr int kWidth = 78;

    explicit ChannelStrip (int trackIndex);

    // Only channels ticked in Audio Setup are offered (stereo pairs need both channels active).
    void setChannelLists (const juce::StringArray& inputs, const juce::StringArray& outputs,
                          const std::vector<bool>& inputActive = {}, const std::vector<bool>& outputActive = {});
    void showTrack (const omr::TrackState& t, bool mirror, bool recording);
    void pushMeter (float peak, float rms, bool clip);

    // -1 = none; `stereo` = the pair (n, n + 1). The input choice sets the track's width.
    std::function<void (int track, int input, bool stereo)> onInput;
    std::function<void (int track, int output, bool stereo)> onOutput;
    std::function<void (int track, float db)> onTrim;
    std::function<void (int track, bool)> onArm, onMute, onSolo;
    std::function<void (int track, const juce::String&)> onRename;
    std::function<void (int track)> onClearClip;
    std::function<void (int track, bool backwards)> onRenameTab;           // Tab / Shift+Tab while renaming
    std::function<void (int track, juce::ModifierKeys)> onSelect;          // click on the strip
    std::function<void (int track)> onTrackMenu;                           // right-click
    std::function<void (int track, int xInParent)> onDragMove;             // drag-to-reorder in progress
    std::function<void (int track, bool drop)> onDragEnd;

    void setSelected (bool s) { if (s != selected) { selected = s; repaint(); } }
    void startRename() { name.showEditor(); }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    // Label whose editor sends Tab / Shift+Tab to the next / previous strip instead of moving focus.
    struct TabLabel final : public juce::Label, private juce::KeyListener
    {
        std::function<void (bool backwards)> onTab;
        using juce::Label::keyPressed;
        void editorShown (juce::TextEditor* ed) override { ed->addKeyListener (this); }
        bool keyPressed (const juce::KeyPress& k, juce::Component*) override
        {
            if (k.getKeyCode() != juce::KeyPress::tabKey) return false;
            hideEditor (false);   // commits (onTextChange fires)
            if (onTab) onTab (k.getModifiers().isShiftDown());
            return true;
        }
    };

    const int track;
    TabLabel name;
    bool selected = false, dragArmed = false, dragging = false;
    juce::ComboBox input, output;
    juce::Slider trim;
    std::unique_ptr<juce::TextButton> arm, mute, solo;
    MeterState meter;
    juce::Rectangle<int> meterArea;
    bool armedState = false, recordingState = false, stereoState = false;
    juce::Colour colour;
    juce::Rectangle<int> meterRepaintArea() const { return meterArea.expanded (2); }
};

// Compact meters for every track on one canvas: the "is everything alive?" view.
class MeterBridge final : public juce::Component
{
public:
    void setTracks (const std::vector<omr::TrackState>& tracks);
    void push (int track, float peak, float rms, bool clip);
    void paint (juce::Graphics&) override;
    std::function<void (int track)> onClick;
    void mouseDown (const juce::MouseEvent&) override;

private:
    std::vector<MeterState> meters;
    std::vector<bool> armed;
    juce::Rectangle<float> cell (int i) const;
    int perRow() const;
};
