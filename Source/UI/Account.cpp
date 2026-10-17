#include "Account.h"
#include <thread>

using namespace omr;

namespace
{
constexpr int kCallbackPorts[] = { 53682, 53683, 53684 };
std::atomic<int> inFlight { 0 };

juce::String jsonOf (const juce::var& v) { return juce::JSON::toString (v, true); }

juce::var obj (std::initializer_list<std::pair<const char*, juce::var>> kv)
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    for (const auto& [k, v] : kv) o->setProperty (k, v);
    return juce::var (o.get());
}

juce::String appVersion()
{
    if (auto* app = juce::JUCEApplicationBase::getInstance()) return app->getApplicationVersion();
    return "dev";
}
} // namespace

template <typename Fn>
void AccountService::async (Fn&& fn)
{
    ++inFlight;
    juce::Thread::launch ([job = std::forward<Fn> (fn)]() mutable
    {
        job();
        --inFlight;
    });
}

AccountService::AccountService (juce::PropertiesFile& s) : settings (s)
{
    loadSession();
    noteSeen();
}

AccountService::~AccountService()
{
    alive->store (false);
    if (signInCancel) signInCancel->store (true);
    for (int i = 0; i < 100 && inFlight.load() > 0; ++i) juce::Thread::sleep (50);   // let running requests finish
}

// ------------------------------------------------------------------------------------ storage

void AccountService::loadSession()
{
    const auto v = juce::JSON::parse (settings.getValue ("account.session"));
    std::lock_guard<std::mutex> g (lock);
    session.accessToken = v.getProperty ("access_token", {}).toString();
    session.refreshToken = v.getProperty ("refresh_token", {}).toString();
    session.userId = v.getProperty ("user_id", {}).toString();
    session.email = v.getProperty ("email", {}).toString();
    session.expiresAtMs = (juce::int64) (double) v.getProperty ("expires_at_ms", 0.0);
}

void AccountService::storeSession()
{
    Session s;
    {
        std::lock_guard<std::mutex> g (lock);
        s = session;
    }
    auto save = [this, s]
    {
        if (! s.valid()) settings.removeValue ("account.session");
        else settings.setValue ("account.session", jsonOf (obj ({ { "access_token", s.accessToken }, { "refresh_token", s.refreshToken },
                                                                  { "user_id", s.userId }, { "email", s.email },
                                                                  { "expires_at_ms", (double) s.expiresAtMs } })));
        settings.saveIfNeeded();
    };
    if (juce::MessageManager::getInstance()->isThisTheMessageThread()) save();
    else
    {
        auto a = alive;
        juce::MessageManager::callAsync ([a, save] { if (a->load()) save(); });
    }
}

void AccountService::noteSeen()
{
    const auto now = juce::Time::currentTimeMillis();
    if (now > lastSeenMs()) settings.setValue ("account.lastSeen", juce::String (now));
}

juce::int64 AccountService::lastSeenMs() const { return settings.getValue ("account.lastSeen", "0").getLargeIntValue(); }

license::Token AccountService::cachedToken() const { return license::parse (settings.getValue ("account.licenseToken")); }

// ------------------------------------------------------------------------------------ HTTP

AccountService::Http AccountService::request (const juce::String& method, const juce::String& url, const juce::String& body,
                                              const juce::String& bearer, const juce::String& extraHeaders, int timeoutMs)
{
    Http r;
    juce::String headers = "apikey: " + juce::String (kPublishableKey) + "\r\nContent-Type: application/json\r\nAccept: application/json\r\n"
                         + "User-Agent: LiveDeck/" + appVersion() + "\r\n";
    if (bearer.isNotEmpty()) headers << "Authorization: Bearer " << bearer << "\r\n";
    headers << extraHeaders;

    juce::URL u (url);
    if (body.isNotEmpty() || method != "GET") u = u.withPOSTData (body);
    auto options = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                       .withExtraHeaders (headers)
                       .withConnectionTimeoutMs (timeoutMs)
                       .withStatusCode (&r.status)
                       .withHttpRequestCmd (method);
    if (auto stream = u.createInputStream (options))
    {
        r.connected = true;
        r.body = stream->readEntireStreamAsString();
    }
    return r;
}

bool AccountService::refreshSession()
{
    juce::String refresh;
    {
        std::lock_guard<std::mutex> g (lock);
        refresh = session.refreshToken;
    }
    if (refresh.isEmpty()) return false;
    const auto r = request ("POST", juce::String (kSupabaseUrl) + "/auth/v1/token?grant_type=refresh_token",
                            jsonOf (obj ({ { "refresh_token", refresh } })), {});
    if (! r.connected) return false;
    const auto v = juce::JSON::parse (r.body);
    if (r.status != 200 || v.getProperty ("access_token", {}).toString().isEmpty())
    {
        if (r.status == 400 || r.status == 401)   // refresh token revoked / expired: signed out
        {
            std::lock_guard<std::mutex> g (lock);
            session = {};
        }
        storeSession();
        return false;
    }
    {
        std::lock_guard<std::mutex> g (lock);
        session.accessToken = v["access_token"].toString();
        session.refreshToken = v.getProperty ("refresh_token", session.refreshToken).toString();
        session.expiresAtMs = juce::Time::currentTimeMillis() + (juce::int64) (int) v.getProperty ("expires_in", 3600) * 1000;
        if (auto user = v.getProperty ("user", {}); user.isObject())
        {
            session.userId = user["id"].toString();
            session.email = user["email"].toString();
        }
    }
    storeSession();
    return true;
}

bool AccountService::ensureFreshToken()
{
    bool needs;
    {
        std::lock_guard<std::mutex> g (lock);
        if (! session.valid()) return false;
        needs = session.accessToken.isEmpty() || juce::Time::currentTimeMillis() > session.expiresAtMs - 120000;
    }
    return ! needs || refreshSession();
}

// ------------------------------------------------------------------------------------ sign in

// ---- one-shot local web server that receives ".../callback?code=..." from the browser (OAuth, email links)

namespace
{
struct Loopback
{
    juce::StreamingSocket server;
    int port = 0;

    bool open()
    {
        for (int p : kCallbackPorts)
            if (server.createListener (p, "127.0.0.1")) { port = p; return true; }
        return false;
    }
    juce::String redirect() const { return "http://127.0.0.1:" + juce::String (port) + "/callback"; }

    // Waits for the browser; returns the PKCE auth code (empty on timeout / cancel / error).
    juce::String waitForCode (const std::atomic<bool>& cancel, juce::uint32 timeoutMs, juce::String& error)
    {
        const auto deadline = juce::Time::getMillisecondCounter() + timeoutMs;
        while (! cancel.load() && juce::Time::getMillisecondCounter() < deadline)
        {
            if (server.waitUntilReady (true, 300) != 1) continue;
            std::unique_ptr<juce::StreamingSocket> client (server.waitForNextConnection());
            if (client == nullptr) continue;
            char buf[8192] = {};
            int n = 0;
            if (client->waitUntilReady (true, 3000) == 1) n = client->read (buf, (int) sizeof (buf) - 1, false);
            const juce::String req (juce::CharPointer_UTF8 (buf), (size_t) juce::jmax (0, n));
            const auto target = req.upToFirstOccurrenceOf ("\r\n", false, false).fromFirstOccurrenceOf (" ", false, false)
                                   .upToLastOccurrenceOf (" ", false, false);
            if (! target.startsWith ("/callback"))
            {
                const juce::String nf = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                client->write (nf.toRawUTF8(), (int) nf.getNumBytesAsUTF8());
                continue;
            }
            juce::String code;
            juce::URL parsed ("http://127.0.0.1" + target);
            const auto names = parsed.getParameterNames();
            const auto values = parsed.getParameterValues();
            for (int i = 0; i < names.size(); ++i)
            {
                if (names[i] == "code") code = values[i];
                if (names[i] == "error_description" || (names[i] == "error" && error.isEmpty())) error = values[i];
            }
            const auto style = juce::String ("font-family:-apple-system,Helvetica,sans-serif;background:#121418;color:#e6e9ee;text-align:center;padding-top:15vh");
            const juce::String page = code.isNotEmpty()
                ? "<html><body style=\"" + style + "\"><h2>You're signed in to LiveDeck</h2><p>You can close this tab and go back to the app.</p></body></html>"
                : "<html><body style=\"" + style + "\"><h2>That didn't work</h2><p>" + juce::XmlElement::createTextElement (error)->getText()
                  + "</p><p>Go back to LiveDeck and try again.</p></body></html>";
            const auto resp = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " + juce::String ((int) page.getNumBytesAsUTF8())
                            + "\r\nConnection: close\r\n\r\n" + page;
            client->write (resp.toRawUTF8(), (int) resp.getNumBytesAsUTF8());
            if (code.isNotEmpty() || error.isNotEmpty()) return code;
        }
        return {};
    }
};

juce::String authError (const juce::String& body, int status)
{
    const auto v = juce::JSON::parse (body);
    for (auto key : { "error_description", "msg", "message", "error" })
        if (auto m = v.getProperty (key, {}).toString(); m.isNotEmpty())
        {
            if (m == "Invalid login credentials") return "Wrong email or password.";
            if (m.containsIgnoreCase ("Email not confirmed")) return "Please confirm your email first: open the link we emailed you.";
            if (m.containsIgnoreCase ("User already registered")) return "There's already an account with this email. Sign in instead, or use Forgot Password.";
            return m;
        }
    return status == 0 ? juce::String ("Couldn't reach the LiveDeck account server.") : "Error " + juce::String (status);
}
} // namespace

bool AccountService::adoptSession (const juce::var& v)
{
    if (v.getProperty ("access_token", {}).toString().isEmpty()) return false;
    {
        std::lock_guard<std::mutex> g (lock);
        session.accessToken = v["access_token"].toString();
        session.refreshToken = v["refresh_token"].toString();
        session.expiresAtMs = juce::Time::currentTimeMillis() + (juce::int64) (int) v.getProperty ("expires_in", 3600) * 1000;
        session.userId = v["user"]["id"].toString();
        session.email = v["user"]["email"].toString();
    }
    storeSession();
    return true;
}

bool AccountService::exchangeCode (const juce::String& code, const juce::String& verifier, juce::String& error)
{
    const auto r = request ("POST", juce::String (kSupabaseUrl) + "/auth/v1/token?grant_type=pkce",
                            jsonOf (obj ({ { "auth_code", code }, { "code_verifier", verifier } })), {});
    if (r.connected && r.status == 200 && adoptSession (juce::JSON::parse (r.body))) return true;
    error = authError (r.body, r.connected ? r.status : 0);
    return false;
}

void AccountService::signIn (Provider provider, std::function<void (bool, juce::String)> done)
{
    cancelSignIn();
    signInCancel = std::make_shared<std::atomic<bool>> (false);
    auto cancel = signInCancel;
    auto a = alive;
    auto finish = [a, done] (bool ok, juce::String err)
    {
        juce::MessageManager::callAsync ([a, done, ok, err] { if (a->load() && done) done (ok, err); });
    };

    async ([this, provider, cancel, finish]
    {
        juce::Random rng;
        rng.setSeedRandomly();
        const auto verifier = license::makeCodeVerifier (rng);
        Loopback lb;
        if (! lb.open()) { finish (false, "Couldn't start the sign-in listener (ports 53682-53684 are busy)."); return; }

        auto authUrl = juce::String (kSupabaseUrl) + "/auth/v1/authorize?provider=" + (provider == Provider::google ? "google" : "azure")
                     + "&redirect_to=" + juce::URL::addEscapeChars (lb.redirect(), true)
                     + "&code_challenge=" + license::codeChallengeS256 (verifier) + "&code_challenge_method=s256";
        if (provider == Provider::microsoft) authUrl << "&scopes=" << juce::URL::addEscapeChars ("openid email profile", true);
        juce::MessageManager::callAsync ([authUrl] { juce::URL (authUrl).launchInDefaultBrowser(); });

        juce::String error;
        const auto code = lb.waitForCode (*cancel, 5u * 60u * 1000u, error);
        if (cancel->load()) { finish (false, "Sign-in cancelled."); return; }
        if (code.isEmpty()) { finish (false, error.isNotEmpty() ? error : juce::String ("Sign-in timed out.")); return; }
        if (! exchangeCode (code, verifier, error)) { finish (false, "Sign-in failed: " + error); return; }
        finish (true, {});
    });
}

void AccountService::signInWithPassword (const juce::String& email, const juce::String& password, std::function<void (bool, juce::String)> done)
{
    cancelSignIn();
    auto a = alive;
    async ([this, a, email, password, done]
    {
        const auto r = request ("POST", juce::String (kSupabaseUrl) + "/auth/v1/token?grant_type=password",
                                jsonOf (obj ({ { "email", email.trim() }, { "password", password } })), {});
        const bool ok = r.connected && r.status == 200 && adoptSession (juce::JSON::parse (r.body));
        const auto err = ok ? juce::String() : authError (r.body, r.connected ? r.status : 0);
        juce::MessageManager::callAsync ([a, done, ok, err] { if (a->load() && done) done (ok, err); });
    });
}

void AccountService::signUp (const juce::String& email, const juce::String& password,
                             std::function<void (EmailStep, juce::String)> progress)
{
    cancelSignIn();
    signInCancel = std::make_shared<std::atomic<bool>> (false);
    auto cancel = signInCancel;
    auto a = alive;
    auto tell = [a, progress] (EmailStep st, juce::String msg)
    {
        juce::MessageManager::callAsync ([a, progress, st, msg] { if (a->load() && progress) progress (st, msg); });
    };

    async ([this, email, password, cancel, tell]
    {
        juce::Random rng;
        rng.setSeedRandomly();
        const auto verifier = license::makeCodeVerifier (rng);
        Loopback lb;
        const bool listening = lb.open();
        auto url = juce::String (kSupabaseUrl) + "/auth/v1/signup";
        if (listening) url << "?redirect_to=" << juce::URL::addEscapeChars (lb.redirect(), true);
        const auto r = request ("POST", url, jsonOf (obj ({ { "email", email.trim() }, { "password", password },
                                                            { "code_challenge", license::codeChallengeS256 (verifier) },
                                                            { "code_challenge_method", "s256" } })), {});
        const auto v = juce::JSON::parse (r.body);
        if (! r.connected || r.status >= 300) { tell (EmailStep::failed, authError (r.body, r.connected ? r.status : 0)); return; }
        if (adoptSession (v)) { tell (EmailStep::signedIn, {}); return; }   // email confirmation turned off

        // Supabase answers an already-registered address with a user that has no identities (it never reveals accounts).
        if (auto* ids = v.getProperty ("identities", {}).getArray(); ids != nullptr && ids->isEmpty())
        {
            tell (EmailStep::failed, "There's already an account with this email. Sign in instead, or use Forgot Password.");
            return;
        }
        tell (EmailStep::emailSent, {});
        if (! listening) return;

        // The confirmation link comes back to LiveDeck (if it's still open) and signs the user in.
        juce::String error;
        const auto code = lb.waitForCode (*cancel, 60u * 60u * 1000u, error);
        if (cancel->load() || code.isEmpty()) return;
        if (exchangeCode (code, verifier, error)) tell (EmailStep::signedIn, {});
        else tell (EmailStep::failed, error);
    });
}

void AccountService::sendPasswordReset (const juce::String& email, std::function<void (EmailStep, juce::String)> progress)
{
    cancelSignIn();
    signInCancel = std::make_shared<std::atomic<bool>> (false);
    auto cancel = signInCancel;
    auto a = alive;
    auto tell = [a, progress] (EmailStep st, juce::String msg)
    {
        juce::MessageManager::callAsync ([a, progress, st, msg] { if (a->load() && progress) progress (st, msg); });
    };

    async ([this, email, cancel, tell]
    {
        juce::Random rng;
        rng.setSeedRandomly();
        const auto verifier = license::makeCodeVerifier (rng);
        Loopback lb;
        if (! lb.open()) { tell (EmailStep::failed, "Couldn't start the sign-in listener (ports 53682-53684 are busy)."); return; }
        const auto r = request ("POST", juce::String (kSupabaseUrl) + "/auth/v1/recover?redirect_to=" + juce::URL::addEscapeChars (lb.redirect(), true),
                                jsonOf (obj ({ { "email", email.trim() }, { "code_challenge", license::codeChallengeS256 (verifier) },
                                               { "code_challenge_method", "s256" } })), {});
        if (! r.connected || r.status >= 300) { tell (EmailStep::failed, authError (r.body, r.connected ? r.status : 0)); return; }
        tell (EmailStep::emailSent, {});

        juce::String error;
        const auto code = lb.waitForCode (*cancel, 60u * 60u * 1000u, error);
        if (cancel->load() || code.isEmpty()) { if (error.isNotEmpty()) tell (EmailStep::failed, error); return; }
        if (exchangeCode (code, verifier, error)) tell (EmailStep::signedIn, {});
        else tell (EmailStep::failed, error);
    });
}

void AccountService::setNewPassword (const juce::String& password, std::function<void (bool, juce::String)> done)
{
    auto a = alive;
    async ([this, a, password, done]
    {
        bool ok = false;
        juce::String err = "Your sign-in expired. Please start again.";
        if (ensureFreshToken())
        {
            juce::String bearer;
            {
                std::lock_guard<std::mutex> g (lock);
                bearer = session.accessToken;
            }
            const auto r = request ("PUT", juce::String (kSupabaseUrl) + "/auth/v1/user", jsonOf (obj ({ { "password", password } })), bearer);
            ok = r.connected && r.status == 200;
            if (! ok) err = authError (r.body, r.connected ? r.status : 0);
        }
        juce::MessageManager::callAsync ([a, done, ok, err] { if (a->load() && done) done (ok, ok ? juce::String() : err); });
    });
}

void AccountService::cancelSignIn()
{
    if (signInCancel) signInCancel->store (true);
}

void AccountService::signOut (std::function<void()> done)
{
    auto a = alive;
    async ([this, a, done]
    {
        const auto machine = license::machineId();
        if (ensureFreshToken())
        {
            juce::String bearer;
            {
                std::lock_guard<std::mutex> g (lock);
                bearer = session.accessToken;
            }
            request ("POST", juce::String (kSupabaseUrl) + "/functions/v1/license",
                     jsonOf (obj ({ { "action", "release" }, { "machine_id", machine } })), bearer, {}, 8000);
            request ("POST", juce::String (kSupabaseUrl) + "/auth/v1/logout", "{}", bearer, {}, 8000);
        }
        {
            std::lock_guard<std::mutex> g (lock);
            session = {};
        }
        storeSession();
        juce::MessageManager::callAsync ([this, a, done]
        {
            if (! a->load()) return;
            settings.removeValue ("account.licenseToken");
            settings.saveIfNeeded();
            last = {};
            if (done) done();
        });
    });
}

// ------------------------------------------------------------------------------------ license check

AccountService::Check AccountService::runCheck()
{
    Check c;
    const auto offline = [this, &c] (const juce::String& why)
    {
        c.status = "offline";
        c.message = why;
        c.token = cachedToken();
        c.offlineVerdict = license::evaluate (c.token, license::machineId(), juce::Time::currentTimeMillis(), lastSeenMs());
        return c;
    };

    if (! isSignedIn()) { c.status = "signed_out"; return c; }
    if (! ensureFreshToken())
    {
        if (! isSignedIn()) { c.status = "signed_out"; c.message = "Your sign-in has expired. Please sign in again."; return c; }
        return offline ("Can't reach the LiveDeck account server.");
    }

    for (int attempt = 0; attempt < 2; ++attempt)
    {
        juce::String bearer;
        {
            std::lock_guard<std::mutex> g (lock);
            bearer = session.accessToken;
        }
        const auto r = request ("POST", juce::String (kSupabaseUrl) + "/functions/v1/license",
                                jsonOf (obj ({ { "action", "check" }, { "machine_id", license::machineId() },
                                               { "machine_name", license::machineName() }, { "app_version", appVersion() } })),
                                bearer, {}, 15000);
        if (! r.connected) return offline ("Can't reach the LiveDeck license server.");
        if (r.status == 401 && attempt == 0 && refreshSession()) continue;
        if (r.status == 401) { c.status = "signed_out"; c.message = "Please sign in again."; return c; }

        const auto v = juce::JSON::parse (r.body);
        c.status = v.getProperty ("status", "error").toString();
        c.message = v.getProperty ("message", {}).toString();
        c.profile = v.getProperty ("profile", {});
        c.license = v.getProperty ("license", {});
        c.pricing = v.getProperty ("pricing", {});
        if (auto* m = v.getProperty ("machines", {}).getArray())
            for (const auto& x : *m) c.machines.add (x.toString());
        if (c.status == "ok")
        {
            c.token = license::parse (v.getProperty ("token", {}).toString());
            if (! c.token.signatureOk || c.token.machineId != license::machineId())
            {
                c.status = "error";
                c.message = "The license server's answer couldn't be verified.";
            }
        }
        if (c.status == "error" && r.status >= 500) return offline (c.message.isNotEmpty() ? c.message : "The license server had a problem.");
        return c;
    }
    return c;
}

void AccountService::check (std::function<void (const Check&)> done)
{
    auto a = alive;
    async ([this, a, done]
    {
        auto c = runCheck();
        juce::MessageManager::callAsync ([this, a, done, c]
        {
            if (! a->load()) return;
            if (c.status == "ok")
            {
                settings.setValue ("account.licenseToken", c.token.raw);
                settings.saveIfNeeded();
            }
            if (c.status == "ok" || c.status == "offline") noteSeen();
            last = c;
            if (done) done (c);
        });
    });
}

void AccountService::release (bool waitForIt)
{
    if (! isSignedIn()) return;
    auto work = [this]
    {
        if (! ensureFreshToken()) return;
        juce::String bearer;
        {
            std::lock_guard<std::mutex> g (lock);
            bearer = session.accessToken;
        }
        request ("POST", juce::String (kSupabaseUrl) + "/functions/v1/license",
                 jsonOf (obj ({ { "action", "release" }, { "machine_id", license::machineId() } })), bearer, {}, 4000);
    };
    if (! waitForIt) { async (work); return; }
    std::atomic<bool> finished { false };
    async ([work, &finished] { work(); finished = true; });
    for (int i = 0; i < 100 && ! finished.load(); ++i) juce::Thread::sleep (50);   // at most ~5 s at quit
    while (! finished.load()) juce::Thread::sleep (50);   // `finished` lives on this stack: don't leave before it is set
}

// ------------------------------------------------------------------------------------ profile, checkout, admin

void AccountService::rest (const juce::String& method, const juce::String& pathAndQuery, const juce::var& body,
                           std::function<void (int, const juce::var&)> done)
{
    auto a = alive;
    async ([this, a, method, pathAndQuery, body, done]
    {
        int status = 0;
        juce::var result;
        if (ensureFreshToken())
        {
            juce::String bearer;
            {
                std::lock_guard<std::mutex> g (lock);
                bearer = session.accessToken;
            }
            const auto r = request (method, juce::String (kSupabaseUrl) + "/rest/v1/" + pathAndQuery,
                                    body.isVoid() ? juce::String() : jsonOf (body), bearer, "Prefer: return=representation\r\n");
            status = r.connected ? r.status : 0;
            result = juce::JSON::parse (r.body);
        }
        juce::MessageManager::callAsync ([a, done, status, result] { if (a->load() && done) done (status, result); });
    });
}

void AccountService::callLicenseFunction (const juce::var& body, std::function<void (int, const juce::var&)> done)
{
    auto a = alive;
    async ([this, a, body, done]
    {
        int status = 0;
        juce::var result;
        if (ensureFreshToken())
        {
            juce::String bearer;
            {
                std::lock_guard<std::mutex> g (lock);
                bearer = session.accessToken;
            }
            const auto r = request ("POST", juce::String (kSupabaseUrl) + "/functions/v1/license", jsonOf (body), bearer, {}, 30000);
            status = r.connected ? r.status : 0;
            result = juce::JSON::parse (r.body);
        }
        juce::MessageManager::callAsync ([a, done, status, result] { if (a->load() && done) done (status, result); });
    });
}

void AccountService::saveProfile (const juce::var& fields, std::function<void (bool, juce::String)> done)
{
    juce::String uid;
    {
        std::lock_guard<std::mutex> g (lock);
        uid = session.userId;
    }
    rest ("PATCH", "profiles?id=eq." + uid, fields, [done] (int status, const juce::var& result)
    {
        if (status >= 200 && status < 300 && result.isArray() && result.size() == 1) { done (true, {}); return; }
        done (false, status == 0 ? juce::String ("Couldn't reach the account server.")
                                 : "Couldn't save: " + result.getProperty ("message", "error " + juce::String (status)).toString());
    });
}

void AccountService::checkout (const juce::String& plan, bool acceptFullPrice, std::function<void (const juce::var&)> done)
{
    callLicenseFunction (obj ({ { "action", "checkout" }, { "plan", plan }, { "accept_full_price", acceptFullPrice } }),
                         [done] (int status, const juce::var& v)
    {
        if (status == 0) done (obj ({ { "status", "error" }, { "message", "Couldn't reach the license server." } }));
        else done (v);
    });
}
