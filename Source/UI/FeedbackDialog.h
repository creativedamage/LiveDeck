#pragma once
// Help > Send Feedback / Request a Feature: a small form that opens a pre-filled
// GitHub "New issue" page in the browser (nothing is sent from the app itself).

#include "Core/Feedback.h"
#include "Theme.h"

class FeedbackComponent final : public juce::Component
{
public:
    FeedbackComponent (juce::String repository, juce::String systemInfo, omr::feedback::Kind initial);
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    omr::feedback::Report report() const;
    omr::feedback::Kind kind() const;
    void kindChanged();
    void submit();
    void close();

    const juce::String repo, sysInfo;
    juce::Label heading, kindLabel { {}, "Type" }, titleLabel { {}, "Title" }, descLabel, stepsLabel, note, status;
    juce::ComboBox kindBox;
    juce::TextEditor title, description, steps, sysView;
    juce::ToggleButton includeSys { "Include app version and system info" };
    juce::TextButton openButton { "Open in GitHub..." }, copyButton { "Copy Text" }, browseButton { "See Existing Requests" }, cancelButton { "Cancel" };
};
