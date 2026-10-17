#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <array>

namespace Theme
{
    // Flat palette: solid colours, no gradients, bevels or shadows.
    const juce::Colour bg        { 0xff121418 };
    const juce::Colour panel     { 0xff1a1d22 };
    const juce::Colour strip     { 0xff20242a };
    const juce::Colour stripAlt  { 0xff252a31 };
    const juce::Colour edge      { 0xff2e343c };
    const juce::Colour control   { 0xff2b3038 };   // buttons, combo boxes
    const juce::Colour text      { 0xffe6e9ee };
    const juce::Colour textDim   { 0xff8b94a0 };
    const juce::Colour red       { 0xffeb4d4b };
    const juce::Colour green     { 0xff27ae60 };
    const juce::Colour yellow    { 0xfff2c94c };
    const juce::Colour orange    { 0xfff2994a };
    const juce::Colour blue      { 0xff2f80ed };
    constexpr float radius = 3.0f;               // one corner radius everywhere

    // Track colours (Tracks Live style: saturated, readable on dark)
    inline const std::array<juce::Colour, 12>& trackPalette()
    {
        static const std::array<juce::Colour, 12> p { {
            juce::Colour (0xffff8a1f), juce::Colour (0xff1fb5a8), juce::Colour (0xffe63ec8), juce::Colour (0xff3d8bff),
            juce::Colour (0xff8bd12c), juce::Colour (0xffffc21a), juce::Colour (0xffb05cff), juce::Colour (0xffff4f5e),
            juce::Colour (0xff22c3e6), juce::Colour (0xff4fd68b), juce::Colour (0xffff7ab8), juce::Colour (0xffa8a8b8) } };
        return p;
    }
    inline juce::Colour trackColour (int trackIndex, int paletteIndex)
    {
        const auto& p = trackPalette();
        const int i = paletteIndex >= 0 ? paletteIndex : trackIndex;
        return p[(size_t) (((i % 12) + 12) % 12)];
    }

    inline juce::Font mono (float h) { return juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), h, juce::Font::plain)); }
    inline juce::Font ui (float h, bool bold = false) { return juce::Font (juce::FontOptions (h, bold ? juce::Font::bold : juce::Font::plain)); }

    class LookAndFeel final : public juce::LookAndFeel_V4
    {
    public:
        LookAndFeel()
            : juce::LookAndFeel_V4 (juce::LookAndFeel_V4::ColourScheme {
                  bg, panel, strip, edge, text, stripAlt, text, blue, text })
        {
            setColour (juce::ComboBox::backgroundColourId, control);
            setColour (juce::ComboBox::outlineColourId, juce::Colours::transparentBlack);
            setColour (juce::ComboBox::textColourId, text);
            setColour (juce::ComboBox::arrowColourId, textDim);
            setColour (juce::PopupMenu::backgroundColourId, panel);
            setColour (juce::PopupMenu::highlightedBackgroundColourId, blue);
            setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::white);
            setColour (juce::TextButton::buttonColourId, control);
            setColour (juce::TextButton::buttonOnColourId, blue);
            setColour (juce::TextButton::textColourOffId, text);
            setColour (juce::TextButton::textColourOnId, juce::Colours::white);
            setColour (juce::TextEditor::backgroundColourId, bg);
            setColour (juce::TextEditor::outlineColourId, edge);
            setColour (juce::TextEditor::focusedOutlineColourId, blue);
            setColour (juce::TextEditor::highlightColourId, blue.withAlpha (0.4f));
            setColour (juce::CaretComponent::caretColourId, text);
            setColour (juce::Slider::rotarySliderFillColourId, blue);
            setColour (juce::Slider::rotarySliderOutlineColourId, edge);
            setColour (juce::Slider::thumbColourId, text);
            setColour (juce::ToggleButton::tickColourId, juce::Colours::white);
            setColour (juce::ToggleButton::tickDisabledColourId, textDim);
            setColour (juce::ToggleButton::textColourId, text);
            setColour (juce::ScrollBar::thumbColourId, edge.brighter (0.25f));
            setColour (juce::ResizableWindow::backgroundColourId, bg);
            setColour (juce::AlertWindow::backgroundColourId, panel);
            setColour (juce::AlertWindow::textColourId, text);
            setColour (juce::AlertWindow::outlineColourId, edge);
            setColour (juce::Label::textColourId, text);
            setColour (juce::ListBox::backgroundColourId, bg);
            setColour (juce::ListBox::outlineColourId, juce::Colours::transparentBlack);
            setColour (juce::TooltipWindow::backgroundColourId, control);
            setColour (juce::TooltipWindow::outlineColourId, juce::Colours::transparentBlack);
            setColour (juce::TooltipWindow::textColourId, text);
        }

        juce::Font getComboBoxFont (juce::ComboBox&) override       { return ui (12.0f); }
        juce::Font getPopupMenuFont() override                      { return ui (13.0f); }
        juce::Font getTextButtonFont (juce::TextButton&, int h) override { return ui (juce::jmin (13.0f, (float) h * 0.5f), true); }
        juce::Font getAlertWindowTitleFont() override               { return ui (16.0f, true); }
        juce::Font getAlertWindowMessageFont() override             { return ui (14.0f); }
        juce::Font getAlertWindowFont() override                    { return ui (13.0f); }

        // ---- buttons: one solid fill, lighter on hover, darker when pressed. No outline, no gradient.
        void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& base, bool over, bool down) override
        {
            auto c = b.getToggleState() ? b.findColour (juce::TextButton::buttonOnColourId) : base;
            if (! b.isEnabled()) c = c.withMultipliedAlpha (0.45f);
            else if (down) c = c.darker (0.15f);
            else if (over) c = c.brighter (0.12f);
            g.setColour (c);
            g.fillRoundedRectangle (b.getLocalBounds().toFloat().reduced (0.5f), radius);
        }

        // ---- combo boxes: flat field with a thin chevron
        void drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box) override
        {
            const auto r = juce::Rectangle<float> (0, 0, (float) w, (float) h);
            g.setColour (box.findColour (juce::ComboBox::backgroundColourId).withMultipliedAlpha (box.isEnabled() ? 1.0f : 0.5f));
            g.fillRoundedRectangle (r.reduced (0.5f), radius);
            if (box.hasKeyboardFocus (true)) { g.setColour (blue); g.drawRoundedRectangle (r.reduced (0.5f), radius, 1.0f); }
            juce::Path chevron;
            const float cx = (float) w - 12.0f, cy = (float) h * 0.5f;
            chevron.startNewSubPath (cx - 4.0f, cy - 2.0f);
            chevron.lineTo (cx, cy + 2.0f);
            chevron.lineTo (cx + 4.0f, cy - 2.0f);
            g.setColour (box.findColour (juce::ComboBox::arrowColourId).withMultipliedAlpha (box.isEnabled() ? 1.0f : 0.5f));
            g.strokePath (chevron, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
        {
            label.setBounds (2, 1, box.getWidth() - 22, box.getHeight() - 2);
            label.setFont (getComboBoxFont (box));
        }

        // ---- text fields: flat fill, 1 px line only (accent when focused)
        void fillTextEditorBackground (juce::Graphics& g, int w, int h, juce::TextEditor& e) override
        {
            g.setColour (e.findColour (juce::TextEditor::backgroundColourId));
            g.fillRoundedRectangle (juce::Rectangle<float> (0, 0, (float) w, (float) h), radius);
        }

        void drawTextEditorOutline (juce::Graphics& g, int w, int h, juce::TextEditor& e) override
        {
            if (! e.isEnabled() || e.isReadOnly()) return;
            const bool focused = e.hasKeyboardFocus (true);
            g.setColour (focused ? e.findColour (juce::TextEditor::focusedOutlineColourId) : e.findColour (juce::TextEditor::outlineColourId));
            g.drawRoundedRectangle (juce::Rectangle<float> (0, 0, (float) w, (float) h).reduced (0.5f), radius, 1.0f);
        }

        // ---- check boxes: filled square with a white tick when on
        void drawTickBox (juce::Graphics& g, juce::Component& c, float x, float y, float w, float h, bool ticked, bool enabled, bool, bool) override
        {
            const auto r = juce::Rectangle<float> (x, y, w, h).reduced (1.0f);
            g.setColour (ticked ? blue : control);
            if (! enabled) g.setOpacity (0.5f);
            g.fillRoundedRectangle (r, radius);
            if (ticked)
            {
                juce::Path tick;
                tick.startNewSubPath (r.getX() + r.getWidth() * 0.22f, r.getCentreY());
                tick.lineTo (r.getX() + r.getWidth() * 0.43f, r.getBottom() - r.getHeight() * 0.25f);
                tick.lineTo (r.getRight() - r.getWidth() * 0.2f, r.getY() + r.getHeight() * 0.25f);
                g.setColour (c.findColour (juce::ToggleButton::tickColourId));
                g.strokePath (tick, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }
        }

        // ---- rotary knobs: flat track + value arc and a dot
        void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float a0, float a1, juce::Slider& s) override
        {
            const auto bounds = juce::Rectangle<int> (x, y, w, h).toFloat().reduced (3.0f);
            const float rad = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
            const float angle = a0 + pos * (a1 - a0);
            const float lw = juce::jmax (2.0f, rad * 0.18f);
            const float arcR = rad - lw * 0.5f;
            juce::Path track, value;
            track.addCentredArc (bounds.getCentreX(), bounds.getCentreY(), arcR, arcR, 0.0f, a0, a1, true);
            g.setColour (s.findColour (juce::Slider::rotarySliderOutlineColourId));
            g.strokePath (track, juce::PathStrokeType (lw, juce::PathStrokeType::curved, juce::PathStrokeType::butt));
            const float mid = a0 + (a1 - a0) * 0.5f;   // trim knobs: the arc grows from 12 o'clock
            value.addCentredArc (bounds.getCentreX(), bounds.getCentreY(), arcR, arcR, 0.0f, juce::jmin (mid, angle), juce::jmax (mid, angle), true);
            g.setColour (s.findColour (juce::Slider::rotarySliderFillColourId));
            g.strokePath (value, juce::PathStrokeType (lw, juce::PathStrokeType::curved, juce::PathStrokeType::butt));
            const auto dot = bounds.getCentre().getPointOnCircumference (arcR - lw * 1.6f, angle);
            g.setColour (s.findColour (juce::Slider::thumbColourId));
            g.fillEllipse (juce::Rectangle<float> (lw * 1.2f, lw * 1.2f).withCentre (dot));
        }

        // ---- scroll bars: a thin pill, no track
        void drawScrollbar (juce::Graphics& g, juce::ScrollBar& bar, int x, int y, int w, int h, bool vertical, int thumbPos, int thumbSize, bool over, bool down) override
        {
            juce::Rectangle<float> thumb = vertical ? juce::Rectangle<float> ((float) x + (float) w * 0.25f, (float) thumbPos, (float) w * 0.5f, (float) thumbSize)
                                                    : juce::Rectangle<float> ((float) thumbPos, (float) y + (float) h * 0.25f, (float) thumbSize, (float) h * 0.5f);
            auto c = bar.findColour (juce::ScrollBar::thumbColourId);
            if (over || down) c = c.brighter (0.25f);
            g.setColour (c);
            g.fillRoundedRectangle (thumb, juce::jmin (thumb.getWidth(), thumb.getHeight()) * 0.5f);
        }

        void drawPopupMenuBackground (juce::Graphics& g, int w, int h) override
        {
            g.fillAll (findColour (juce::PopupMenu::backgroundColourId));
            g.setColour (edge);
            g.drawRect (0, 0, w, h, 1);
        }

        // ---- alerts: flat panel with an accent bar; no icon bubble
        void drawAlertBox (juce::Graphics& g, juce::AlertWindow& alert, const juce::Rectangle<int>& textArea, juce::TextLayout& layout) override
        {
            g.fillAll (alert.findColour (juce::AlertWindow::backgroundColourId));
            const auto type = alert.getAlertType();
            const auto accent = type == juce::MessageBoxIconType::WarningIcon ? orange
                              : type == juce::MessageBoxIconType::QuestionIcon ? blue
                              : type == juce::MessageBoxIconType::InfoIcon ? blue : edge;
            g.setColour (accent);
            g.fillRect (0, 0, alert.getWidth(), 3);
            g.setColour (alert.findColour (juce::AlertWindow::textColourId));
            layout.draw (g, juce::Rectangle<int> (textArea.getX() - 20, textArea.getY(), textArea.getWidth() + 20, textArea.getHeight()).toFloat());
            g.setColour (edge);
            g.drawRect (alert.getLocalBounds(), 1);
        }
        int getAlertWindowButtonHeight() override { return 32; }

        void drawTooltip (juce::Graphics& g, const juce::String& tip, int w, int h) override
        {
            g.fillAll (findColour (juce::TooltipWindow::backgroundColourId));
            g.setColour (findColour (juce::TooltipWindow::textColourId));
            g.setFont (ui (12.0f));
            g.drawFittedText (tip, juce::Rectangle<int> (w, h).reduced (8, 4), juce::Justification::centredLeft, 4);
        }
    };
}
