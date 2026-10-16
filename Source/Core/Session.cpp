#include "Session.h"

#if JUCE_MAC || JUCE_BSD
 #include <sys/mount.h>
 #include <sys/param.h>
#elif JUCE_LINUX
 #include <sys/statfs.h>
 #include <linux/magic.h>
#elif JUCE_WINDOWS
 #include <windows.h>
#endif

namespace omr
{
namespace SessionPaths
{
    juce::File audioDir (const juce::File& r)    { return r.getChildFile ("Audio"); }
    juce::File recoveryDir (const juce::File& r) { return r.getChildFile ("Recovery"); }
    juce::File logsDir (const juce::File& r)     { return r.getChildFile ("Logs"); }
    juce::File markersDir (const juce::File& r)  { return r.getChildFile ("Markers"); }
    juce::File journalFile (const juce::File& r) { return recoveryDir (r).getChildFile ("journal.ndjson"); }
    juce::File takeManifest (const juce::File& r, int take) { return audioDir (r).getChildFile (".take_" + juce::String (take).paddedLeft ('0', 3) + ".json"); }
    juce::File sessionFile (const juce::File& r, const juce::String& name) { return r.getChildFile (sanitise (name) + ".livedeck"); }

    bool createTree (const juce::File& r)
    {
        bool ok = r.createDirectory() && audioDir (r).createDirectory() && recoveryDir (r).createDirectory()
               && logsDir (r).createDirectory() && markersDir (r).createDirectory();
       #if JUCE_MAC
        r.getChildFile (".metadata_never_index").create(); // keep Spotlight off the session while recording
       #endif
        return ok;
    }

    juce::String sanitise (const juce::String& s)
    {
        auto out = s.trim().replaceCharacters (" /\\:*?\"<>|", "__________").retainCharacters (
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-.()");
        return out.isEmpty() ? juce::String ("Untitled") : out.substring (0, 64);
    }

    juce::String trackFileName (int trackIndex, const juce::String& trackName, int take, bool secondary)
    {
        return "T" + juce::String (trackIndex + 1).paddedLeft ('0', 2) + "_" + sanitise (trackName) + "_"
             + juce::String (take).paddedLeft ('0', 3) + (secondary ? "_B" : "") + ".wav";
    }
}

namespace Library
{
    juce::File root()      { return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("LiveDeck"); }
    juce::File shows()     { return root().getChildFile ("Shows"); }
    juce::File templates() { return root().getChildFile ("Templates"); }

    bool ensure()
    {
        const bool ok = shows().createDirectory() && templates().createDirectory();
        const auto readme = root().getChildFile ("About this folder.txt");
        if (ok && ! readme.exists())
            readme.replaceWithText ("LiveDeck library\n"
                                    "\n"
                                    "Shows       New sessions are created here (one folder per show: audio, markers, logs).\n"
                                    "Templates   Track layouts saved with File > Save as Template. They appear under File > New from Template.\n"
                                    "\n"
                                    "You can record anywhere else too (File > New Session > Choose Folder...), for example on an external SSD.\n",
                                    false, false, "\n");
        return shows().isDirectory() && templates().isDirectory();
    }

    juce::Array<juce::File> listTemplates()
    {
        auto files = templates().findChildFiles (juce::File::findFiles, false, "*.ldtemplate");
        files.sort();
        return files;
    }
}

juce::var sessionToVar (const SessionSettings& s, const PatchState& p)
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty ("format", "livedeck-session");
    o->setProperty ("version", 1);
    o->setProperty ("name", s.name);
    o->setProperty ("primaryRoot", s.primaryRoot.getFullPathName());
    o->setProperty ("secondaryRoot", s.secondaryRoot.getFullPathName());
    o->setProperty ("bitsPerSample", s.bitsPerSample);
    o->setProperty ("ringSeconds", s.ringSeconds);
    o->setProperty ("nextTake", s.nextTake);
    o->setProperty ("deviceType", s.deviceType);
    o->setProperty ("deviceName", s.deviceName);
    o->setProperty ("sampleRate", s.sampleRate);
    o->setProperty ("bufferSize", s.bufferSize);
    o->setProperty ("minFreeHours", s.minFreeHours);
    o->setProperty ("patch", patchToVar (p));
    return juce::var (o.get());
}

void sessionFromVar (const juce::var& v, SessionSettings& s, PatchState& p)
{
    s.name = v.getProperty ("name", "Untitled").toString();
    const auto pr = v.getProperty ("primaryRoot", {}).toString();
    const auto sr = v.getProperty ("secondaryRoot", {}).toString();
    s.primaryRoot = juce::File::isAbsolutePath (pr) ? juce::File (pr) : juce::File();
    s.secondaryRoot = juce::File::isAbsolutePath (sr) ? juce::File (sr) : juce::File();
    s.bitsPerSample = (int) v.getProperty ("bitsPerSample", 24);
    s.ringSeconds = (double) v.getProperty ("ringSeconds", 20.0);
    s.nextTake = (int) v.getProperty ("nextTake", 1);
    s.deviceType = v.getProperty ("deviceType", {}).toString();
    s.deviceName = v.getProperty ("deviceName", {}).toString();
    s.sampleRate = (double) v.getProperty ("sampleRate", 48000.0);
    s.bufferSize = (int) v.getProperty ("bufferSize", 256);
    s.minFreeHours = (double) v.getProperty ("minFreeHours", 0.5);
    p = patchFromVar (v.getProperty ("patch", {}));
}

static juce::Result writeAtomically (const juce::File& file, const juce::String& text)
{
    juce::TemporaryFile tmp (file);
    if (! tmp.getFile().replaceWithText (text, false, false, "\n")) return juce::Result::fail ("Cannot write " + file.getFullPathName());
    if (! tmp.overwriteTargetFileWithTemporary()) return juce::Result::fail ("Cannot replace " + file.getFullPathName());
    return juce::Result::ok();
}

juce::Result saveSession (const juce::File& file, const SessionSettings& s, const PatchState& p)
{
    return writeAtomically (file, juce::JSON::toString (sessionToVar (s, p)));
}

juce::Result saveTemplate (const juce::File& file, const SessionSettings& s, const PatchState& p)
{
    auto t = s;
    t.primaryRoot = juce::File();
    t.secondaryRoot = juce::File();
    t.nextTake = 1;
    auto v = sessionToVar (t, p);
    v.getDynamicObject()->setProperty ("format", "livedeck-template");
    return writeAtomically (file, juce::JSON::toString (v));
}

juce::Result loadSession (const juce::File& file, SessionSettings& s, PatchState& p)
{
    juce::var v;
    const auto r = juce::JSON::parse (file.loadFileAsString(), v);
    if (r.failed()) return juce::Result::fail ("Not a valid session file: " + r.getErrorMessage());
    const auto fmt = v.getProperty ("format", {}).toString();
    if (fmt != "livedeck-session" && fmt != "livedeck-template" && fmt != "omniroute-session" && fmt != "omniroute-template") return juce::Result::fail ("Not a LiveDeck session or template");
    sessionFromVar (v, s, p);
    return juce::Result::ok();
}

juce::String filesystemType (const juce::File& f)
{
   #if JUCE_MAC || JUCE_BSD
    struct statfs st {};
    if (statfs (f.getFullPathName().toRawUTF8(), &st) == 0) return st.f_fstypename;
   #elif JUCE_LINUX
    struct statfs st {};
    if (statfs (f.getFullPathName().toRawUTF8(), &st) == 0)
    {
        switch ((unsigned long) st.f_type)
        {
            case MSDOS_SUPER_MAGIC: return "msdos";
            case 0x2011BAB0UL:      return "exfat";
            case 0x5346544EUL:      return "ntfs";
            case NFS_SUPER_MAGIC:   return "nfs";
            case 0xFF534D42UL:      return "cifs";
            case 0xFE534D42UL:      return "smb2";
            default:                return "local";
        }
    }
   #elif JUCE_WINDOWS
    wchar_t fsName[MAX_PATH + 1] = {};
    const auto rootPath = f.getFullPathName().substring (0, 3);
    if (GetVolumeInformationW (rootPath.toWideCharPointer(), nullptr, 0, nullptr, nullptr, nullptr, fsName, MAX_PATH))
        return juce::String (fsName).toLowerCase();
   #endif
    juce::ignoreUnused (f);
    return "unknown";
}

TargetCheck validateTarget (const juce::File& root, double sampleRate, int bitsPerSample, int armedTracks, double minFreeHours)
{
    TargetCheck c;
    if (root == juce::File()) { c.message = "No folder selected"; return c; }
    if (! root.exists() && ! root.createDirectory()) { c.message = "Cannot create folder " + root.getFullPathName(); return c; }

    const auto probe = root.getChildFile (".omr_write_probe");
    if (! probe.replaceWithText ("ok")) { c.message = "Folder is not writable"; return c; }
    probe.deleteFile();

    c.filesystem = filesystemType (root);
    const double bytesPerSec = sampleRate * (bitsPerSample / 8) * juce::jmax (1, armedTracks);
    c.hoursAvailable = (double) root.getBytesFreeOnVolume() * 0.95 / bytesPerSec / 3600.0;

    const auto fs = c.filesystem.toLowerCase();
    if (fs == "msdos" || fs == "vfat" || fs == "fat32" || fs == "fat")
    {
        c.message = "FAT32 limits files to 4 GB (about 8 h per 24-bit 48 kHz track). Reformat as exFAT, APFS or NTFS.";
        return c;
    }
    if (c.hoursAvailable < minFreeHours)
    {
        c.message = "Only " + juce::String (juce::roundToInt (c.hoursAvailable * 60.0)) + " minutes of space for " + juce::String (armedTracks) + " tracks";
        return c;
    }

    c.ok = true;
    const bool network = fs.contains ("smb") || fs.contains ("nfs") || fs.contains ("cifs") || fs.contains ("afp") || fs.contains ("webdav");
    if (network) c.message = "Network volume: use it as the secondary target only, and run the disk benchmark.";
    else if (fs == "exfat") c.message = "exFAT has no journal: a power cut can damage the volume. Prefer APFS/NTFS for the primary target.";
    return c;
}
} // namespace omr
