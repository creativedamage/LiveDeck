#include "FeedbackDialog.h"

using namespace omr::feedback;

FeedbackComponent::FeedbackComponent (juce::String repository, juce::String systemInfo, Kind initial)
    : repo (std::move (repository)), sysInfo (std::move (systemInfo))
{
    for (auto* c : std::initializer_list<juce::Component*> { &heading, &kindLabel, &titleLabel, &descLabel, &stepsLabel, &note, &status, &kindBox,
                                                             &title, &description, &steps, &sysView, &includeSys, &openButton, &copyButton,
                                                             &browseButton, &cancelButton })
        addAndMakeVisible (c);

    heading.setText ("Tell us what LiveDeck should do next", juce::dontSendNotification);
    heading.setFont (Theme::ui (16.0f, true));

    kindBox.addItem ("Feature request", 1);
    kindBox.addItem ("Bug report", 2);
    kindBox.addItem ("General feedback", 3);
    kindBox.setSelectedId (initial == Kind::bug ? 2 : initial == Kind::general ? 3 : 1, juce::dontSendNotification);
    kindBox.onChange = [this] { kindChanged(); };

    title.setTextToShowWhenEmpty ("One line summary, e.g. \"Ripple delete on the timeline\"", Theme::textDim);
    title.setInputRestrictions (200);
    for (auto* e : { &description, &steps })
    {
        e->setMultiLine (true, true);
        e->setReturnKeyStartsNewLine (true);
        e->setScrollbarsShown (true);
    }

    sysView.setMultiLine (true);
    sysView.setReadOnly (true);
    sysView.setFont (Theme::mono (11.0f));
    sysView.setText (sysInfo);
    sysView.setColour (juce::TextEditor::textColourId, Theme::textDim);
    includeSys.setToggleState (true, juce::dontSendNotification);
    includeSys.onClick = [this] { sysView.setEnabled (includeSys.getToggleState()); sysView.setAlpha (includeSys.getToggleState() ? 1.0f : 0.4f); };

    note.setText ("Opens github.com in your browser with everything filled in. Review it there and click \"Create\" "
                  "(a free GitHub account is needed). Nothing is sent from LiveDeck itself.",
                  juce::dontSendNotification);
    note.setColour (juce::Label::textColourId, Theme::textDim);
    note.setFont (Theme::ui (12.0f));
    note.setJustificationType (juce::Justification::topLeft);
    status.setColour (juce::Label::textColourId, Theme::orange);

    openButton.setColour (juce::TextButton::buttonColourId, Theme::blue);
    openButton.onClick = [this] { submit(); };
    copyButton.onClick = [this]
    {
        const auto r = report();
        juce::SystemClipboard::copyTextToClipboard (issueTitle (r) + "\n\n" + issueBody (r));
        status.setText ("Copied: paste it into an email or a new GitHub issue.", juce::dontSendNotification);
    };
    browseButton.onClick = [this] { if (repo.isNotEmpty()) juce::URL (issuesUrl (repo, kind())).launchInDefaultBrowser(); };
    cancelButton.onClick = [this] { close(); };

    if (repo.isEmpty())
    {
        openButton.setEnabled (false);
        browseButton.setEnabled (false);
        status.setText ("This build has no GitHub repository configured: use Copy Text instead.", juce::dontSendNotification);
    }
    kindChanged();
    setSize (620, 600);
    juce::Timer::callAfterDelay (150, [safe = juce::Component::SafePointer<FeedbackComponent> (this)]
    {
        if (safe != nullptr && safe->isShowing()) safe->title.grabKeyboardFocus();
    });
}

Kind FeedbackComponent::kind() const
{
    switch (kindBox.getSelectedId())
    {
        case 2: return Kind::bug;
        case 3: return Kind::general;
        default: return Kind::feature;
    }
}

void FeedbackComponent::kindChanged()
{
    const auto k = kind();
    descLabel.setText (k == Kind::bug ? "What happened? What did you expect?" : k == Kind::feature ? "What would you like LiveDeck to do?" : "Your feedback",
                       juce::dontSendNotification);
    stepsLabel.setText (k == Kind::bug ? "Steps to reproduce (optional)" : "How would it help at a show? (optional)", juce::dontSendNotification);
    const bool showSteps = k != Kind::general;
    stepsLabel.setVisible (showSteps);
    steps.setVisible (showSteps);
    browseButton.setButtonText (k == Kind::bug ? "See Known Bugs" : "See Existing Requests");
    resized();
}

Report FeedbackComponent::report() const
{
    Report r;
    r.kind = kind();
    r.title = title.getText();
    r.description = description.getText();
    r.steps = steps.isVisible() ? steps.getText() : juce::String();
    r.includeSystemInfo = includeSys.getToggleState();
    r.systemInfo = sysInfo;
    return r;
}

void FeedbackComponent::submit()
{
    if (title.getText().trim().isEmpty() && description.getText().trim().isEmpty())
    {
        status.setText ("Add a title or a description first.", juce::dontSendNotification);
        title.grabKeyboardFocus();
        return;
    }
    const auto r = report();
    const auto url = newIssueUrl (repo, r);
    if (! juce::URL (url).launchInDefaultBrowser())
    {
        juce::SystemClipboard::copyTextToClipboard (issueTitle (r) + "\n\n" + issueBody (r));
        status.setText ("Could not open the browser. The text is on the clipboard: paste it at " + issuesUrl (repo), juce::dontSendNotification);
        return;
    }
    close();
}

void FeedbackComponent::close()
{
    if (auto* w = findParentComponentOfClass<juce::DialogWindow>()) w->exitModalState (0);
}

void FeedbackComponent::paint (juce::Graphics& g)
{
    g.fillAll (Theme::panel);
}

void FeedbackComponent::resized()
{
    auto r = getLocalBounds().reduced (18, 14);
    heading.setBounds (r.removeFromTop (26));
    r.removeFromTop (8);

    auto line = [&] (int h) { auto x = r.removeFromTop (h); r.removeFromTop (6); return x; };
    { auto x = line (26); kindLabel.setBounds (x.removeFromLeft (60)); kindBox.setBounds (x.removeFromLeft (200)); }
    { auto x = line (26); titleLabel.setBounds (x.removeFromLeft (60)); title.setBounds (x); }

    auto buttons = r.removeFromBottom (30);
    r.removeFromBottom (6);
    status.setBounds (r.removeFromBottom (22));
    note.setBounds (r.removeFromBottom (36));
    r.removeFromBottom (4);
    sysView.setBounds (r.removeFromBottom (78));
    includeSys.setBounds (r.removeFromBottom (24));
    r.removeFromBottom (4);

    const bool showSteps = steps.isVisible();
    const int stepsH = showSteps ? juce::jmax (60, r.getHeight() / 3) : 0;
    descLabel.setBounds (r.removeFromTop (20));
    description.setBounds (r.removeFromTop (r.getHeight() - (showSteps ? stepsH + 28 : 0)));
    if (showSteps)
    {
        r.removeFromTop (6);
        stepsLabel.setBounds (r.removeFromTop (20));
        steps.setBounds (r);
    }

    openButton.setBounds (buttons.removeFromRight (150));
    buttons.removeFromRight (8);
    cancelButton.setBounds (buttons.removeFromRight (80));
    browseButton.setBounds (buttons.removeFromLeft (170));
    buttons.removeFromLeft (8);
    copyButton.setBounds (buttons.removeFromLeft (100));
}
