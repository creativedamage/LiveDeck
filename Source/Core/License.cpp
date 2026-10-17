#include "License.h"
#include <juce_cryptography/juce_cryptography.h>

namespace omr::license
{
const char* const kPublicKeyN =
    "zVsjDvUQGZKg-lIWEw772ONyu7jZCaJTE92hTsLDphJb6a5MakpALX4BiLUt6U5iei72YNtnqE2EH4KNBh1OTrgW6nASG8yYx1_YUdsPfaBEhhbngr5h4UMth4cH"
    "gw6AOKa2yAOmFK_pJTNsk_G6l6isoi6wFFm46n4Hk5PsLfIuuIQaEAlK5fc6PVljA8MHnRhGer3qOnB6abriR2S-p8PrRbjF9QCpb9TEXh_xtf93Q8RaaC1k_47K"
    "gWTGsZxf3OrY8M2vI-p73XgUHiaBEBFjlIjY1HJjX50VAtLtj4KdAxeVwW6nxkyfebUFaYNauEXfnNDQpAEwbd6NtHExQQ";
const char* const kPublicKeyE = "AQAB";

// ---------------------------------------------------------------- base64url

juce::String base64UrlEncode (const void* data, size_t size)
{
    return juce::Base64::toBase64 (data, size).replaceCharacter ('+', '-').replaceCharacter ('/', '_').trimCharactersAtEnd ("=");
}

juce::MemoryBlock base64UrlDecode (const juce::String& s)
{
    auto std64 = s.trim().replaceCharacter ('-', '+').replaceCharacter ('_', '/');
    while (std64.length() % 4 != 0) std64 << "=";
    juce::MemoryOutputStream out;
    if (! juce::Base64::convertFromBase64 (out, std64)) return {};
    return out.getMemoryBlock();
}

// ---------------------------------------------------------------- RSA

static juce::BigInteger fromBigEndian (const juce::MemoryBlock& b)
{
    juce::MemoryBlock le (b.getSize());
    for (size_t i = 0; i < b.getSize(); ++i) le[i] = b[b.getSize() - 1 - i];
    juce::BigInteger v;
    v.loadFromMemoryBlock (le);
    return v;
}

bool verifyRsaSha256 (const juce::MemoryBlock& message, const juce::MemoryBlock& signature,
                      const juce::MemoryBlock& modulus, const juce::MemoryBlock& exponent)
{
    // k = modulus length in bytes (ignoring a leading zero byte)
    size_t k = modulus.getSize();
    for (size_t i = 0; i < modulus.getSize() && modulus[i] == 0; ++i) --k;
    if (k < 128 || signature.getSize() != k) return false;

    const auto n = fromBigEndian (modulus);
    auto m = fromBigEndian (signature);
    if (m.compareAbsolute (n) >= 0) return false;
    m.exponentModulo (fromBigEndian (exponent), n);

    // m back to k big-endian bytes
    const auto le = m.toMemoryBlock();
    std::vector<uint8_t> em (k, 0);
    for (size_t i = 0; i < le.getSize() && i < k; ++i) em[k - 1 - i] = (uint8_t) le[i];

    // EMSA-PKCS1-v1_5: 00 01 FF..FF 00 || DigestInfo(SHA-256) || H
    static const uint8_t prefix[] = { 0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20 };
    const juce::SHA256 sha (message.getData(), message.getSize());
    const auto h = sha.getRawData();
    const size_t tLen = sizeof (prefix) + 32;
    if (k < tLen + 11) return false;

    std::vector<uint8_t> expected (k, 0xff);
    expected[0] = 0x00;
    expected[1] = 0x01;
    expected[k - tLen - 1] = 0x00;
    std::memcpy (expected.data() + k - tLen, prefix, sizeof (prefix));
    std::memcpy (expected.data() + k - 32, h.getData(), 32);

    uint8_t diff = 0;   // constant time compare
    for (size_t i = 0; i < k; ++i) diff |= (uint8_t) (em[i] ^ expected[i]);
    return diff == 0;
}

// ---------------------------------------------------------------- tokens

Token parse (const juce::String& token, const juce::String& nB64, const juce::String& eB64)
{
    Token t;
    t.raw = token.trim();
    const auto body = t.raw.upToFirstOccurrenceOf (".", false, false);
    const auto sig = t.raw.fromFirstOccurrenceOf (".", false, false);
    if (body.isEmpty() || sig.isEmpty()) return t;

    const auto msg = body.toUTF8();
    t.signatureOk = verifyRsaSha256 (juce::MemoryBlock (msg.getAddress(), msg.sizeInBytes() - 1), base64UrlDecode (sig),
                                     base64UrlDecode (nB64), base64UrlDecode (eB64));
    const auto json = base64UrlDecode (body);
    const auto v = juce::JSON::parse (json.toString());
    auto str = [&v] (const char* k) { return v.getProperty (k, {}).toString(); };
    auto num = [&v] (const char* k) { const auto x = v.getProperty (k, {}); return x.isVoid() || x.isUndefined() ? (juce::int64) 0 : (juce::int64) (double) x; };
    t.userId = str ("sub");
    t.email = str ("email");
    t.name = str ("name");
    t.company = str ("company");
    t.licenseId = str ("license_id");
    t.kind = str ("kind");
    t.machineId = str ("machine_id");
    t.seats = (int) num ("seats");
    t.expiresAt = num ("expires_at");
    t.issuedAt = num ("issued_at");
    t.validUntil = num ("valid_until");
    t.admin = (bool) v.getProperty ("admin", false);
    return t;
}

Verdict evaluate (const Token& t, const juce::String& machine, juce::int64 nowMs, juce::int64 lastSeenMs)
{
    if (! t.signatureOk) return Verdict::badSignature;
    if (t.machineId != machine) return Verdict::wrongMachine;
    // The clock went back more than a day from what this computer has already seen: don't trust it.
    if (nowMs < lastSeenMs - 86400000LL || nowMs < t.issuedAt - 86400000LL) return Verdict::clockRolledBack;
    if (t.expiresAt > 0 && nowMs >= t.expiresAt) return Verdict::licenseExpired;
    if (nowMs >= t.validUntil) return Verdict::offlineGraceOver;
    return Verdict::ok;
}

juce::String describe (Verdict v)
{
    switch (v)
    {
        case Verdict::ok:               return "License valid";
        case Verdict::badSignature:     return "The saved license is damaged. Connect to the internet to verify it again.";
        case Verdict::wrongMachine:     return "The saved license belongs to another computer.";
        case Verdict::offlineGraceOver: return "LiveDeck has been offline for more than 14 days. Connect to the internet once to verify your license.";
        case Verdict::licenseExpired:   return "Your license has expired.";
        case Verdict::clockRolledBack:  return "This computer's clock looks wrong. Set the correct date and time, or connect to the internet.";
    }
    return {};
}

int daysLeft (const Token& t, juce::int64 nowMs)
{
    return (int) juce::jmax<juce::int64> (0, (t.validUntil - nowMs) / 86400000LL);
}

// ---------------------------------------------------------------- machine

juce::String machineId()
{
    auto raw = juce::SystemStats::getUniqueDeviceID();
    if (raw.isEmpty())
        raw = juce::SystemStats::getComputerName() + "|" + juce::SystemStats::getLogonName() + "|" + juce::SystemStats::getCpuModel();
    return juce::SHA256 (("livedeck-machine|" + raw).toUTF8()).toHexString().substring (0, 40);
}

juce::String machineName()
{
    auto n = juce::SystemStats::getComputerName().trim();
    return n.isNotEmpty() ? n : juce::SystemStats::getOperatingSystemName();
}

// ---------------------------------------------------------------- PKCE

juce::String makeCodeVerifier (juce::Random& rng)
{
    uint8_t bytes[48];
    for (auto& b : bytes) b = (uint8_t) rng.nextInt (256);
    return base64UrlEncode (bytes, sizeof (bytes));   // 64 chars, all unreserved
}

juce::String codeChallengeS256 (const juce::String& verifier)
{
    const auto utf8 = verifier.toUTF8();
    const auto h = juce::SHA256 (utf8.getAddress(), utf8.sizeInBytes() - 1).getRawData();
    return base64UrlEncode (h.getData(), h.getSize());
}

juce::String formatPrice (int cents)
{
    return "$" + juce::String (cents / 100) + "." + juce::String (cents % 100).paddedLeft ('0', 2);
}
} // namespace omr::license
