#pragma once
// Feedback, feature requests and bug reports go to the project's GitHub Issues.
// LiveDeck never sends anything itself: it opens a pre-filled "New issue" page in the
// browser, and the user reviews it and presses Submit there (a free GitHub account is needed).

#include <juce_core/juce_core.h>

namespace omr::feedback
{
enum class Kind { feature, bug, general };

struct Report
{
    Kind kind = Kind::feature;
    juce::String title;
    juce::String description;     // what they want / what happened
    juce::String steps;           // bug: steps to reproduce; feature: how it would help
    bool includeSystemInfo = true;
    juce::String systemInfo;      // filled by the app: version, macOS, audio device...
};

juce::String kindName (Kind);                  // "Feature request" / "Bug report" / "Feedback"
juce::String label (Kind);                     // GitHub label: "enhancement" / "bug" / "feedback"
juce::String issueTitle (const Report&);       // "[Feature] Ripple delete"
juce::String issueBody (const Report&);        // Markdown

// https://github.com/<repo>/issues/new?title=...&body=...&labels=...
// The body is shortened (with a note) so the link stays under browser / GitHub limits.
juce::String newIssueUrl (const juce::String& repo, const Report&, int maxLength = 7000);
juce::String issuesUrl (const juce::String& repo, Kind);   // open issues with that label
juce::String issuesUrl (const juce::String& repo);         // all open issues
} // namespace omr::feedback
