#include "Feedback.h"

namespace omr::feedback
{
juce::String kindName (Kind k)
{
    switch (k)
    {
        case Kind::feature: return "Feature request";
        case Kind::bug:     return "Bug report";
        case Kind::general: break;
    }
    return "Feedback";
}

juce::String label (Kind k)
{
    switch (k)
    {
        case Kind::feature: return "enhancement";
        case Kind::bug:     return "bug";
        case Kind::general: break;
    }
    return "feedback";
}

static juce::String prefix (Kind k)
{
    switch (k)
    {
        case Kind::feature: return "[Feature] ";
        case Kind::bug:     return "[Bug] ";
        case Kind::general: break;
    }
    return "[Feedback] ";
}

juce::String issueTitle (const Report& r)
{
    auto t = r.title.trim().replaceCharacters ("\r\n", "  ");
    if (t.isEmpty()) t = kindName (r.kind);
    return prefix (r.kind) + t.substring (0, 200);
}

static juce::String bodyWith (const Report& r, const juce::String& description, const juce::String& steps, bool shortened)
{
    juce::String b;
    switch (r.kind)
    {
        case Kind::feature: b << "### What would you like LiveDeck to do?\n\n"; break;
        case Kind::bug:     b << "### What happened?\n\n"; break;
        case Kind::general: b << "### Feedback\n\n"; break;
    }
    b << (description.trim().isNotEmpty() ? description.trim() : juce::String ("_(no description)_")) << "\n\n";

    if (steps.trim().isNotEmpty())
        b << (r.kind == Kind::bug ? "### Steps to reproduce\n\n" : "### How would it help at a show?\n\n") << steps.trim() << "\n\n";

    if (shortened) b << "_(Shortened to fit the link: please paste the rest here.)_\n\n";

    if (r.includeSystemInfo && r.systemInfo.isNotEmpty())
        b << "<details><summary>System</summary>\n\n```\n" << r.systemInfo.trim() << "\n```\n</details>\n\n";

    b << "_Sent from LiveDeck (" << kindName (r.kind) << ")._";
    return b;
}

juce::String issueBody (const Report& r) { return bodyWith (r, r.description, r.steps, false); }

static juce::String buildUrl (const juce::String& repo, const Report& r, const juce::String& body)
{
    return juce::URL ("https://github.com/" + repo + "/issues/new")
        .withParameter ("title", issueTitle (r))
        .withParameter ("labels", label (r.kind))
        .withParameter ("body", body)
        .toString (true);
}

juce::String newIssueUrl (const juce::String& repo, const Report& r, int maxLength)
{
    auto url = buildUrl (repo, r, issueBody (r));
    if (url.length() <= maxLength) return url;

    // Shorten the longest text first, keeping whole lines where possible.
    juce::String d = r.description, s = r.steps;
    for (int guard = 0; guard < 64 && url.length() > maxLength; ++guard)
    {
        auto& longer = d.length() >= s.length() ? d : s;
        if (longer.isEmpty()) break;
        longer = longer.substring (0, (int) (longer.length() * 0.8)).trimEnd() + " ...";
        url = buildUrl (repo, r, bodyWith (r, d, s, true));
    }
    return url;
}

juce::String issuesUrl (const juce::String& repo, Kind k)
{
    return juce::URL ("https://github.com/" + repo + "/issues")
        .withParameter ("q", "is:issue is:open label:" + label (k))
        .toString (true);
}

juce::String issuesUrl (const juce::String& repo) { return "https://github.com/" + repo + "/issues"; }
} // namespace omr::feedback
