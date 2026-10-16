#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <array>

namespace Theme
{
    const juce::Colour bg        { 0xff0f1114 };
    const juce::Colour panel     { 0xff181b20 };
    const juce::Colour strip     { 0xff1e2228 };
    const juce::Colour stripAlt  { 0xff22272e };
    const juce::Colour edge      { 0xff2c323a };
    const juce::Colour text      { 0xffd9dee4 };
    const juce::Colour textDim   { 0xff8a939e };
    const juce::Colour red       { 0xffff3b30 };
    const juce::Colour green     { 0xff30d158 };
    const juce::Colour yellow    { 0xffffd60a };
    const juce::Colour orange    { 0xffff9f0a };
    const juce::Colour blue      { 0xff0a84ff };

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
            setColour (juce::ComboBox::backgroundColourId, stripAlt);
            setColour (juce::ComboBox::outlineColourId, edge);
            setColour (juce::ComboBox::textColourId, text);
            setColour (juce::ComboBox::arrowColourId, textDim);
            setColour (juce::PopupMenu::backgroundColourId, panel);
            setColour (juce::PopupMenu::highlightedBackgroundColourId, blue.withAlpha (0.35f));
            setColour (juce::TextButton::buttonColourId, stripAlt);
            setColour (juce::TextButton::textColourOffId, text);
            setColour (juce::TextEditor::backgroundColourId, bg);
            setColour (juce::TextEditor::outlineColourId, edge);
            setColour (juce::Slider::rotarySliderFillColourId, blue);
            setColour (juce::Slider::rotarySliderOutlineColourId, edge);
            setColour (juce::Slider::thumbColourId, text);
            setColour (juce::ToggleButton::tickColourId, green);
            setColour (juce::ToggleButton::textColourId, text);
            setColour (juce::ScrollBar::thumbColourId, edge.brighter (0.3f));
            setColour (juce::ResizableWindow::backgroundColourId, bg);
            setColour (juce::AlertWindow::backgroundColourId, panel);
            setColour (juce::Label::textColourId, text);
        }

        juce::Font getComboBoxFont (juce::ComboBox&) override       { return ui (11.0f); }
        juce::Font getPopupMenuFont() override                      { return ui (13.0f); }
        juce::Font getTextButtonFont (juce::TextButton&, int h) override { return ui (juce::jmin (13.0f, (float) h * 0.55f), true); }
    };
}
