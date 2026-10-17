#pragma once
// LiveDeck accounts and licenses (client side).
//
//   Sign-in:   Google or Microsoft through Supabase Auth, in the user's browser, using PKCE and a
//              one-shot local callback (http://127.0.0.1:53682/callback). LiveDeck never sees a password.
//   License:   at every launch (and every 5 minutes while running) the license server checks the
//              account's license, the 2-computer limit and the same-network rule, and returns a signed
//              token. The token lets LiveDeck start offline for up to 14 days.
//   Admin:     admins manage users, non-profit verification, licenses and computers through the same
//              API (row-level security only lets admins do that).
//
// Network calls run on background threads; every callback arrives on the message thread.

#include "Core/License.h"
#include <juce_events/juce_events.h>
#include <juce_data_structures/juce_data_structures.h>

class AccountService
{
public:
    static constexpr const char* kSupabaseUrl = "https://wtrrobcvdbxlwnyjtuts.supabase.co";
    static constexpr const char* kPublishableKey = "sb_publishable_7KjKRllZCermIwFNQAhZ9w_6Ny-E-Rl";
    static constexpr int kHeartbeatMinutes = 5;

    enum class Provider { google, microsoft };

    struct Session
    {
        juce::String accessToken, refreshToken, userId, email;
        juce::int64 expiresAtMs = 0;
        bool valid() const { return refreshToken.isNotEmpty(); }
    };

    // Result of a license check. status: ok | needs_registration | no_license | seats_full |
    // different_network | signed_out | offline | error
    struct Check
    {
        juce::String status, message;
        juce::var profile, license, pricing;
        juce::StringArray machines;
        omr::license::Token token;          // ok: fresh token; offline: the cached one
        omr::license::Verdict offlineVerdict = omr::license::Verdict::badSignature;
        bool isAdmin() const { return (bool) profile.getProperty ("is_admin", false) || token.admin; }
    };

    explicit AccountService (juce::PropertiesFile& settings);
    ~AccountService();

    bool isSignedIn() const { return session.valid(); }
    juce::String email() const { return session.email; }
    const Check& lastCheck() const { return last; }

    void signIn (Provider, std::function<void (bool ok, juce::String error)> done);

    // Email + password accounts (Supabase Auth). Confirmation and reset links come back to LiveDeck
    // through the same local callback as Google / Microsoft, so clicking them while LiveDeck is open
    // signs the user in.
    enum class EmailStep { emailSent, signedIn, failed };
    static constexpr int kMinPasswordLength = 8;
    void signInWithPassword (const juce::String& email, const juce::String& password, std::function<void (bool ok, juce::String error)> done);
    void signUp (const juce::String& email, const juce::String& password, std::function<void (EmailStep, juce::String message)> progress);
    void sendPasswordReset (const juce::String& email, std::function<void (EmailStep, juce::String message)> progress);
    void setNewPassword (const juce::String& password, std::function<void (bool ok, juce::String error)> done);
    void cancelSignIn();
    void signOut (std::function<void()> done);

    void check (std::function<void (const Check&)> done);
    void release (bool waitForIt);   // frees this computer's seat (quit / sign out)

    void saveProfile (const juce::var& fields, std::function<void (bool ok, juce::String error)> done);
    void checkout (const juce::String& plan, bool acceptFullPrice, std::function<void (const juce::var& result)> done);

    // Admin + general API access (PostgREST under /rest/v1, the license function).
    void rest (const juce::String& method, const juce::String& pathAndQuery, const juce::var& body,
               std::function<void (int status, const juce::var& result)> done);
    void callLicenseFunction (const juce::var& body, std::function<void (int status, const juce::var& result)> done);

    // Cached token for offline starts.
    omr::license::Token cachedToken() const;
    juce::int64 lastSeenMs() const;

private:
    struct Http { int status = 0; juce::String body; bool connected = false; };
    static Http request (const juce::String& method, const juce::String& url, const juce::String& body,
                         const juce::String& bearer, const juce::String& extraHeaders = {}, int timeoutMs = 20000);
    bool adoptSession (const juce::var& tokenResponse);   // background thread
    bool exchangeCode (const juce::String& code, const juce::String& verifier, juce::String& error);
    bool ensureFreshToken();          // background thread
    bool refreshSession();            // background thread
    void storeSession();
    void loadSession();
    void noteSeen();
    Check runCheck();                 // background thread
    template <typename Fn> void async (Fn&& fn);

    juce::PropertiesFile& settings;
    Session session;
    Check last;
    mutable std::mutex lock;          // guards `session`
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);
    std::shared_ptr<std::atomic<bool>> signInCancel;
};
