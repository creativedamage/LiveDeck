#include "Updater.h"
#include <juce_cryptography/juce_cryptography.h>

#if JUCE_MAC || JUCE_LINUX || JUCE_BSD
 #include <sys/wait.h>
 #include <unistd.h>
#endif

namespace omr::update
{
// ------------------------------------------------------------------------------------ versions

Version Version::parse (juce::String s)
{
    Version v;
    s = s.trim();
    if (s.startsWithIgnoreCase ("v")) s = s.substring (1);
    s = s.upToFirstOccurrenceOf ("-", false, false).upToFirstOccurrenceOf ("+", false, false);
    juce::StringArray parts;
    parts.addTokens (s, ".", "");
    if (parts.isEmpty() || parts.size() > 3) return v;
    for (const auto& p : parts)
        if (p.isEmpty() || ! p.containsOnly ("0123456789")) return v;
    v.major = parts[0].getIntValue();
    v.minor = parts.size() > 1 ? parts[1].getIntValue() : 0;
    v.patch = parts.size() > 2 ? parts[2].getIntValue() : 0;
    v.valid = true;
    return v;
}

bool Version::operator< (const Version& o) const
{
    if (major != o.major) return major < o.major;
    if (minor != o.minor) return minor < o.minor;
    return patch < o.patch;
}

// ------------------------------------------------------------------------------------ release JSON

ReleaseInfo parseRelease (const juce::var& json)
{
    ReleaseInfo r;
    if (! json.isObject()) return r;
    if ((bool) json.getProperty ("draft", false) || (bool) json.getProperty ("prerelease", false)) return r;

    r.tag = json.getProperty ("tag_name", {}).toString();
    r.version = Version::parse (r.tag);
    r.name = json.getProperty ("name", {}).toString();
    r.notes = json.getProperty ("body", {}).toString();
    r.pageUrl = json.getProperty ("html_url", {}).toString();

    if (auto* assets = json.getProperty ("assets", {}).getArray())
    {
        for (const auto& a : *assets)
        {
            const auto name = a.getProperty ("name", {}).toString();
            const auto url = a.getProperty ("browser_download_url", {}).toString();
            if (name.endsWithIgnoreCase ("-mac.zip")) { r.zipUrl = url; r.zipSize = (juce::int64) a.getProperty ("size", 0); }
            else if (name.endsWithIgnoreCase ("-mac.zip.sha256")) r.shaUrl = url;
            else if (name.endsWithIgnoreCase (".dmg")) r.dmgUrl = url;
        }
    }
    r.valid = r.version.valid && r.zipUrl.startsWith ("https://") && r.shaUrl.startsWith ("https://");
    return r;
}

juce::String configuredRepository()
{
   #ifdef LIVEDECK_GITHUB_REPO
    return LIVEDECK_GITHUB_REPO;
   #else
    return {};
   #endif
}

// ------------------------------------------------------------------------------------ network

static std::unique_ptr<juce::InputStream> openUrl (const juce::String& url, int& status, bool api)
{
    juce::String headers = "User-Agent: LiveDeck-Updater\r\n";
    if (api) headers << "Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
    return juce::URL (url).createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                                  .withExtraHeaders (headers)
                                                  .withConnectionTimeoutMs (15000)
                                                  .withNumRedirectsToFollow (8)
                                                  .withStatusCode (&status));
}

juce::Result fetchLatestRelease (const juce::String& repo, ReleaseInfo& out)
{
    if (repo.isEmpty()) return juce::Result::fail ("This build has no GitHub repository configured, so it cannot check for updates.");
    int status = 0;
    auto in = openUrl ("https://api.github.com/repos/" + repo + "/releases/latest", status, true);
    if (in == nullptr) return juce::Result::fail ("Could not reach GitHub. Check the internet connection.");
    const auto body = in->readEntireStreamAsString();
    if (status == 404) return juce::Result::fail ("No releases have been published yet.");
    if (status == 403) return juce::Result::fail ("GitHub is rate-limiting update checks right now. Try again later.");
    if (status != 200) return juce::Result::fail ("GitHub answered with HTTP " + juce::String (status) + ".");

    juce::var json;
    if (auto r = juce::JSON::parse (body, json); r.failed()) return juce::Result::fail ("Unexpected answer from GitHub.");
    out = parseRelease (json);
    if (! out.valid) return juce::Result::fail ("The latest release (" + out.tag + ") has no macOS update package.");
    return juce::Result::ok();
}

juce::Result downloadText (const juce::String& url, juce::String& out)
{
    int status = 0;
    auto in = openUrl (url, status, false);
    if (in == nullptr || status != 200) return juce::Result::fail ("Download failed (HTTP " + juce::String (status) + ")");
    out = in->readEntireStreamAsString();
    return juce::Result::ok();
}

juce::Result downloadFile (const juce::String& url, const juce::File& dest, const std::function<bool (double)>& onProgress)
{
    int status = 0;
    auto in = openUrl (url, status, false);
    if (in == nullptr || status != 200) return juce::Result::fail ("Download failed (HTTP " + juce::String (status) + ")");

    dest.deleteFile();
    juce::FileOutputStream os (dest);
    if (! os.openedOk()) return juce::Result::fail ("Cannot write " + dest.getFullPathName());

    const auto total = in->getTotalLength();
    juce::HeapBlock<char> buf (65536);
    juce::int64 done = 0;
    for (;;)
    {
        const int n = in->read (buf.get(), 65536);
        if (n <= 0) break;
        if (! os.write (buf.get(), (size_t) n)) return juce::Result::fail ("Disk full while downloading the update");
        done += n;
        if (onProgress && ! onProgress (total > 0 ? (double) done / (double) total : -1.0))
            return juce::Result::fail ("Cancelled");
    }
    os.flush();
    if (total > 0 && done != total) return juce::Result::fail ("Download was interrupted");
    return juce::Result::ok();
}

// ------------------------------------------------------------------------------------ checksums

juce::String parseSha256File (const juce::String& text)
{
    const auto token = text.trim().upToFirstOccurrenceOf (" ", false, false).trim().toLowerCase();
    return token.length() == 64 && token.containsOnly ("0123456789abcdef") ? token : juce::String();
}

juce::String sha256OfFile (const juce::File& f)
{
    return juce::SHA256 (f).toHexString().toLowerCase();
}

// ------------------------------------------------------------------------------------ install

static juce::String shellQuote (const juce::String& s)
{
    return "'" + s.replace ("'", "'\\''") + "'";
}

juce::String makeInstallScript (const juce::File& currentApp, const juce::File& newApp, int pid, const juce::File& logFile)
{
    juce::String s;
    s << "#!/bin/bash\n"
      << "# LiveDeck updater: waits for LiveDeck to quit, swaps the app bundle, relaunches it.\n"
      << "PID=" << pid << "\n"
      << "OLD=" << shellQuote (currentApp.getFullPathName()) << "\n"
      << "NEW=" << shellQuote (newApp.getFullPathName()) << "\n"
      << "STAGE=" << shellQuote (newApp.getParentDirectory().getFullPathName()) << "\n"
      << "LOG=" << shellQuote (logFile.getFullPathName()) << "\n"
      << "BACKUP=\"$OLD.previous\"\n"
      << "exec >>\"$LOG\" 2>&1\n"
      << "echo \"$(date) update: waiting for LiveDeck ($PID) to quit\"\n"
      << "for i in $(seq 1 1200); do kill -0 \"$PID\" 2>/dev/null || break; sleep 0.1; done\n"
      << "if kill -0 \"$PID\" 2>/dev/null; then echo \"LiveDeck did not quit; update cancelled\"; exit 1; fi\n"
      << "rm -rf \"$BACKUP\"\n"
      << "if ! mv \"$OLD\" \"$BACKUP\"; then echo \"cannot move the old app aside; update cancelled\"; open \"$OLD\"; exit 1; fi\n"
      << "COPY=\"/usr/bin/ditto\"; [ -x \"$COPY\" ] || COPY=\"cp -R\"\n"
      << "if $COPY \"$NEW\" \"$OLD\"; then\n"
      << "  /usr/bin/xattr -dr com.apple.quarantine \"$OLD\" 2>/dev/null\n"
      << "  rm -rf \"$BACKUP\"\n"
      << "  echo \"update installed\"\n"
      << "else\n"
      << "  echo \"copy failed; restoring the previous version\"\n"
      << "  rm -rf \"$OLD\"; mv \"$BACKUP\" \"$OLD\"\n"
      << "fi\n"
      << "rm -rf \"$STAGE\"\n"
      << "open \"$OLD\"\n";
    return s;
}

juce::File currentAppBundle()
{
   #if JUCE_MAC
    const auto f = juce::File::getSpecialLocation (juce::File::currentApplicationFile);
    return f.hasFileExtension ("app") ? f : juce::File();
   #else
    return {};
   #endif
}

#if JUCE_MAC
static int run (const juce::StringArray& args, juce::String* output = nullptr)
{
    juce::ChildProcess p;
    if (! p.start (args)) return -1;
    const auto text = p.readAllProcessOutput();
    if (output != nullptr) *output = text;
    p.waitForProcessToFinish (60000);
    return (int) p.getExitCode();
}

static juce::String plistValue (const juce::File& plist, const juce::String& key)
{
    auto xml = juce::XmlDocument::parse (plist);
    if (xml == nullptr) return {};
    if (auto* dict = xml->getChildByName ("dict"))
        for (int i = 0; i + 1 < dict->getNumChildElements(); ++i)
            if (dict->getChildElement (i)->hasTagName ("key") && dict->getChildElement (i)->getAllSubText() == key)
                return dict->getChildElement (i + 1)->getAllSubText();
    return {};
}
#endif

juce::Result prepareInstall (const juce::File& zip, const juce::File& currentApp, const Version& expected, juce::File& scriptOut)
{
   #if JUCE_MAC
    if (! currentApp.isDirectory()) return juce::Result::fail ("LiveDeck is not running from an app bundle.");
    if (! currentApp.getParentDirectory().hasWriteAccess())
        return juce::Result::fail ("No permission to replace " + currentApp.getFullPathName() + ". Install the new version from the .dmg instead.");

    const auto stage = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("LiveDeck-update-" + expected.toString());
    stage.deleteRecursively();
    if (! stage.createDirectory()) return juce::Result::fail ("Cannot create " + stage.getFullPathName());

    if (run ({ "/usr/bin/ditto", "-x", "-k", zip.getFullPathName(), stage.getFullPathName() }) != 0)
        return juce::Result::fail ("The update package could not be unpacked.");

    const auto apps = stage.findChildFiles (juce::File::findDirectories, false, "*.app");
    if (apps.size() != 1) return juce::Result::fail ("The update package does not contain exactly one app.");
    const auto newApp = apps.getFirst();

    const auto newPlist = newApp.getChildFile ("Contents/Info.plist");
    const auto version = Version::parse (plistValue (newPlist, "CFBundleShortVersionString"));
    if (! (version == expected))
        return juce::Result::fail ("The downloaded app reports version " + version.toString() + ", expected " + expected.toString() + ".");
    const auto newId = plistValue (newPlist, "CFBundleIdentifier");
    const auto oldId = plistValue (currentApp.getChildFile ("Contents/Info.plist"), "CFBundleIdentifier");
    if (newId.isEmpty() || newId != oldId)
        return juce::Result::fail ("The downloaded app is not LiveDeck (" + newId + ").");

    juce::String out;
    if (run ({ "/usr/bin/codesign", "--verify", "--deep", "--strict", newApp.getFullPathName() }, &out) != 0)
        return juce::Result::fail ("The downloaded app's code signature is invalid: " + out.trim());

    const auto log = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Library/Logs/LiveDeck-update.log");
    log.getParentDirectory().createDirectory();
    scriptOut = stage.getChildFile ("install.sh");
    if (! scriptOut.replaceWithText (makeInstallScript (currentApp, newApp, (int) getpid(), log), false, false, "\n"))
        return juce::Result::fail ("Cannot write the install script.");
    scriptOut.setExecutePermission (true);
    return juce::Result::ok();
   #else
    juce::ignoreUnused (zip, currentApp, expected, scriptOut);
    return juce::Result::fail ("Automatic install is only available on macOS. Download the new version from the release page.");
   #endif
}

juce::Result launchDetached (const juce::File& script)
{
   #if JUCE_MAC || JUCE_LINUX || JUCE_BSD
    const std::string path = script.getFullPathName().toStdString();
    const pid_t child = fork();
    if (child < 0) return juce::Result::fail ("Could not start the installer.");
    if (child == 0)
    {
        // first child: new session, fork again so the installer is re-parented and outlives LiveDeck
        setsid();
        if (fork() == 0)
        {
            execl ("/bin/bash", "bash", path.c_str(), (char*) nullptr);
            _exit (127);
        }
        _exit (0);
    }
    int st = 0;
    waitpid (child, &st, 0);
    return juce::Result::ok();
   #else
    juce::ignoreUnused (script);
    return juce::Result::fail ("Automatic install is not supported on this platform.");
   #endif
}
} // namespace omr::update
