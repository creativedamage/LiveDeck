#pragma once
// Peak + RMS meter drawing shared by the channel strips and the meter bridge.
// Values are linear gain; the UI applies a fast attack / slow release ballistic.

#include "Theme.h"
#include <juce_audio_basics/juce_audio_basics.h>

struct MeterState
{
    float peak = 0, rms = 0, hold = 0;
    int holdFrames = 0;
    bool clip = false;

    void push (float newPeak, float newRms, bool newClip)
    {
        peak = std::max (newPeak, peak * 0.80f);          // ~30 fps release
        rms = std::max (newRms, rms * 0.85f);
        if (newPeak >= hold) { hold = newPeak; holdFrames = 45; }
        else if (--holdFrames <= 0) hold *= 0.92f;
        clip = newClip;
    }
};

namespace MeterDraw
{
    inline float toY (float gain, float h)
    {
        const float db = juce::Decibels::gainToDecibels (gain, -60.0f);
        return h * (1.0f - (db + 60.0f) / 60.0f);
    }

    inline juce::Colour colourFor (float gain)
    {
        const float db = juce::Decibels::gainToDecibels (gain, -100.0f);
        return db > -6.0f ? Theme::red : (db > -18.0f ? Theme::yellow : Theme::green);
    }

    inline void draw (juce::Graphics& g, juce::Rectangle<float> r, const MeterState& m, bool showClip = true)
    {
        g.setColour (Theme::bg.darker (0.3f));
        g.fillRect (r);
        auto clipBox = showClip ? r.removeFromTop (5.0f) : juce::Rectangle<float>();
        if (showClip)
        {
            g.setColour (m.clip ? Theme::red : juce::Colour (0xff2a1414));
            g.fillRect (clipBox.reduced (0.5f, 0));
            r.removeFromTop (1.0f);
        }
        const float h = r.getHeight();
        const float yPeak = toY (m.peak, h), yRms = toY (m.rms, h), yHold = toY (m.hold, h);

        // Flat meter: solid zones (green below -18 dBFS, yellow to -6, red above). Peak = light bar, RMS = solid core.
        const float y18 = r.getY() + toY (juce::Decibels::decibelsToGain (-18.0f), h);
        const float y6 = r.getY() + toY (juce::Decibels::decibelsToGain (-6.0f), h);
        auto zones = [&] (juce::Rectangle<float> bar, float alpha)
        {
            const auto lo = bar.withTop (juce::jmax (bar.getY(), y18));
            const auto mid = bar.withTop (juce::jmax (bar.getY(), y6)).withBottom (juce::jmin (bar.getBottom(), y18));
            const auto hi = bar.withBottom (juce::jmin (bar.getBottom(), y6));
            g.setColour (Theme::green.withAlpha (alpha));  if (! lo.isEmpty()) g.fillRect (lo);
            g.setColour (Theme::yellow.withAlpha (alpha)); if (mid.getHeight() > 0) g.fillRect (mid);
            g.setColour (Theme::red.withAlpha (alpha));    if (hi.getHeight() > 0) g.fillRect (hi);
        };
        zones (r.withTop (r.getY() + yPeak), 0.45f);
        zones (r.withTop (r.getY() + yRms).reduced (r.getWidth() * 0.18f, 0), 1.0f);
        // peak hold line
        if (m.hold > 0.001f)
        {
            g.setColour (colourFor (m.hold));
            g.fillRect (r.getX(), r.getY() + yHold, r.getWidth(), 1.5f);
        }
        // -18 dBFS reference tick
        g.setColour (juce::Colours::white.withAlpha (0.12f));
        g.fillRect (r.getX(), r.getY() + toY (juce::Decibels::decibelsToGain (-18.0f), h), r.getWidth(), 1.0f);
    }
}
