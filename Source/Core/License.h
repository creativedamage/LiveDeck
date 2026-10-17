#pragma once
// Offline side of LiveDeck licensing: verifying the license server's signed tokens, the
// offline grace rules, this computer's id, and the PKCE helpers for browser sign-in.
// No networking here (see UI/Account.*), so all of it is unit-tested.
//
// Token format (issued by server/supabase/functions/license):
//   base64url(JSON payload) "." base64url(RSASSA-PKCS1-v1_5 / SHA-256 signature over the first part)

#include <juce_core/juce_core.h>

namespace omr::license
{
// The license server's public key (RSA-2048, from GET .../functions/v1/license?pubkey).
extern const char* const kPublicKeyN;   // base64url modulus
extern const char* const kPublicKeyE;   // base64url exponent

struct Token
{
    bool signatureOk = false;
    juce::String userId, email, name, company, licenseId, kind, machineId;
    int seats = 0;
    juce::int64 expiresAt = 0;     // ms since epoch, 0 = perpetual
    juce::int64 issuedAt = 0;      // ms
    juce::int64 validUntil = 0;    // ms: offline grace end (never after expiresAt)
    bool admin = false;
    juce::String raw;
};

enum class Verdict { ok, badSignature, wrongMachine, offlineGraceOver, licenseExpired, clockRolledBack };

// Parses and verifies a token against the given public key (defaults: LiveDeck's key).
Token parse (const juce::String& token, const juce::String& nB64 = kPublicKeyN, const juce::String& eB64 = kPublicKeyE);

// Can LiveDeck run on a cached token right now (no server contact)?
// `lastSeenMs` = the latest time LiveDeck has seen on this computer (clock rollback guard).
Verdict evaluate (const Token& t, const juce::String& machineId, juce::int64 nowMs, juce::int64 lastSeenMs);
juce::String describe (Verdict v);
int daysLeft (const Token& t, juce::int64 nowMs);   // whole days of offline grace left

// RSASSA-PKCS1-v1_5 with SHA-256 (what WebCrypto's "RSASSA-PKCS1-v1_5" produces).
bool verifyRsaSha256 (const juce::MemoryBlock& message, const juce::MemoryBlock& signature,
                      const juce::MemoryBlock& modulusBigEndian, const juce::MemoryBlock& exponentBigEndian);

// base64url without padding
juce::String base64UrlEncode (const void* data, size_t size);
juce::MemoryBlock base64UrlDecode (const juce::String& s);

// A stable, anonymous id for this computer (hash of the OS device id; never the raw hardware ids).
juce::String machineId();
juce::String machineName();   // "Wayne's MacBook Pro"

// PKCE (RFC 7636) for the browser sign-in.
juce::String makeCodeVerifier (juce::Random& rng);
juce::String codeChallengeS256 (const juce::String& verifier);

// "$50.00"
juce::String formatPrice (int cents);
} // namespace omr::license
