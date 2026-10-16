#pragma once
// Self-update from GitHub Releases.
//
//   1. fetchLatestRelease()   GET api.github.com/repos/<owner>/<repo>/releases/latest
//   2. downloadFile()         the release's  LiveDeck-<ver>-mac.zip  (+ .sha256)
//   3. sha256 check           the zip must match the checksum published with the release
//   4. prepareInstall()       unzip with ditto, verify the code signature and bundle version,
//                             write a small script that waits for LiveDeck to quit, swaps the
//                             app (keeping a backup until the copy succeeds) and relaunches it
//
// Network and filesystem work runs on a background thread; nothing here touches audio.

#include <juce_core/juce_core.h>

namespace omr::update
{
struct Version
{
    int major = 0, minor = 0, patch = 0;
    bool valid = false;

    static Version parse (juce::String s);   // "v0.3.1", "0.3", "0.3.1-beta" -> 0.3.1
    juce::String toString() const { return juce::String (major) + "." + juce::String (minor) + "." + juce::String (patch); }
    bool operator< (const Version& o) const;
    bool operator== (const Version& o) const { return major == o.major && minor == o.minor && patch == o.patch; }
};

struct ReleaseInfo
{
    bool valid = false;
    Version version;
    juce::String tag, name, notes, pageUrl, zipUrl, shaUrl, dmgUrl;
    juce::int64 zipSize = 0;
};

// Parses the GitHub "latest release" JSON. Drafts and pre-releases are ignored.
ReleaseInfo parseRelease (const juce::var& json);

// "owner/repo" baked in at build time (empty when the build had no GitHub remote).
juce::String configuredRepository();

// Blocking network calls (background thread only).
juce::Result fetchLatestRelease (const juce::String& repo, ReleaseInfo& out);
juce::Result downloadFile (const juce::String& url, const juce::File& dest,
                           const std::function<bool (double progress)>& onProgress); // return false to cancel
juce::Result downloadText (const juce::String& url, juce::String& out);

// Checksums: the .sha256 asset is "<hex>  <filename>" (shasum format) or just "<hex>".
juce::String parseSha256File (const juce::String& text);
juce::String sha256OfFile (const juce::File& f);

// The swap-and-relaunch script, as text (exposed for tests).
juce::String makeInstallScript (const juce::File& currentApp, const juce::File& newApp, int pid, const juce::File& logFile);

// macOS: unzip, verify signature + version, write the script. On success `scriptOut` is ready to run.
juce::Result prepareInstall (const juce::File& zip, const juce::File& currentApp, const Version& expected, juce::File& scriptOut);

// Starts the script detached (it outlives this process), so the caller can quit right after.
juce::Result launchDetached (const juce::File& script);

// The running app's bundle (LiveDeck.app) on macOS, or an empty File elsewhere.
juce::File currentAppBundle();
} // namespace omr::update
