// LiveDeck license server (Supabase Edge Function "license").
//
//   POST {action:"check",    machine_id, machine_name, app_version}  -> signed license token or the reason why not
//   POST {action:"release",  machine_id}                              -> frees this computer's seat (app quit / sign out)
//   POST {action:"checkout", plan:"annual"|"perpetual", accept_full_price?} -> Stripe Checkout URL
//   POST {action:"stripe-setup"}            (admin)                   -> registers the Stripe webhook once
//   GET  ?pubkey                                                      -> the public key that verifies tokens
//   GET  ?page=done|cancel                                            -> shown after Stripe Checkout
//
// Rules: a license allows `seats` (2) computers at the same time, and they must share a public IP
// (same network). A computer that stops checking in frees its seat after SEAT_TIMEOUT_MIN minutes.
// Tokens are RSA-signed (RSASSA-PKCS1-v1_5 / SHA-256) and valid offline for OFFLINE_DAYS.

import { createClient } from "npm:@supabase/supabase-js@2.45.4";
import postgres from "npm:postgres@3.4.4";
import Stripe from "npm:stripe@17.4.0";

const SUPABASE_URL = Deno.env.get("SUPABASE_URL")!;
const SERVICE_KEY = Deno.env.get("SUPABASE_SERVICE_ROLE_KEY")!;
const sql = postgres(Deno.env.get("SUPABASE_DB_URL")!, { prepare: false, max: 3 });
const admin = createClient(SUPABASE_URL, SERVICE_KEY, { auth: { persistSession: false } });

const PRICE_CENTS: Record<string, number> = { annual: 5000, perpetual: 12900 };
const NONPROFIT_DISCOUNT_PCT = 10;
const SEAT_TIMEOUT_MIN = 10;
const OFFLINE_DAYS = 14;
const KEY_ID = "license-v1";

const cors = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Headers": "authorization, x-client-info, apikey, content-type",
};
const json = (body: unknown, status = 200) =>
  new Response(JSON.stringify(body), { status, headers: { ...cors, "Content-Type": "application/json" } });
const text = (body: string) => new Response(body, { headers: { ...cors, "Content-Type": "text/plain; charset=utf-8" } });

// ---------------------------------------------------------------- signing

const b64url = (bytes: Uint8Array) =>
  btoa(String.fromCharCode(...bytes)).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
const fromB64 = (s: string) => Uint8Array.from(atob(s), (c) => c.charCodeAt(0));
const toB64 = (buf: ArrayBuffer) => btoa(String.fromCharCode(...new Uint8Array(buf)));

let cachedKey: { priv: CryptoKey; jwk: JsonWebKey } | null = null;

async function signingKey() {
  if (cachedKey) return cachedKey;
  let rows = await sql`select private_pkcs8, public_spki from private.signing_keys where id = ${KEY_ID}`;
  if (rows.length === 0) {
    const kp = await crypto.subtle.generateKey(
      { name: "RSASSA-PKCS1-v1_5", modulusLength: 2048, publicExponent: new Uint8Array([1, 0, 1]), hash: "SHA-256" },
      true, ["sign", "verify"]) as CryptoKeyPair;
    const priv = toB64(await crypto.subtle.exportKey("pkcs8", kp.privateKey));
    const pub = toB64(await crypto.subtle.exportKey("spki", kp.publicKey));
    await sql`insert into private.signing_keys (id, private_pkcs8, public_spki) values (${KEY_ID}, ${priv}, ${pub}) on conflict (id) do nothing`;
    rows = await sql`select private_pkcs8, public_spki from private.signing_keys where id = ${KEY_ID}`;
  }
  const alg = { name: "RSASSA-PKCS1-v1_5", hash: "SHA-256" };
  const priv = await crypto.subtle.importKey("pkcs8", fromB64(rows[0].private_pkcs8), alg, false, ["sign"]);
  const pubKey = await crypto.subtle.importKey("spki", fromB64(rows[0].public_spki), alg, true, ["verify"]);
  cachedKey = { priv, jwk: await crypto.subtle.exportKey("jwk", pubKey) };
  return cachedKey;
}

async function signToken(payload: Record<string, unknown>) {
  const { priv } = await signingKey();
  const body = b64url(new TextEncoder().encode(JSON.stringify(payload)));
  const sig = new Uint8Array(await crypto.subtle.sign("RSASSA-PKCS1-v1_5", priv, new TextEncoder().encode(body)));
  return body + "." + b64url(sig);
}

// ---------------------------------------------------------------- helpers

function clientIp(req: Request) {
  const fwd = req.headers.get("x-forwarded-for") ?? "";
  return (fwd.split(",")[0] || req.headers.get("cf-connecting-ip") || req.headers.get("x-real-ip") || "").trim();
}

async function userFrom(req: Request) {
  const token = (req.headers.get("Authorization") ?? "").replace(/^Bearer\s+/i, "");
  if (!token) return null;
  const { data, error } = await admin.auth.getUser(token);
  return error ? null : data.user;
}

function pricing(profile: Record<string, any>) {
  const verified = profile.org_type === "nonprofit" && profile.nonprofit_status === "verified";
  const pct = verified ? NONPROFIT_DISCOUNT_PCT : 0;
  const price = (k: string) => Math.round(PRICE_CENTS[k] * (100 - pct) / 100);
  return {
    currency: "usd",
    annual_cents: price("annual"),
    perpetual_cents: price("perpetual"),
    list_annual_cents: PRICE_CENTS.annual,
    list_perpetual_cents: PRICE_CENTS.perpetual,
    discount_pct: pct,
    tax_exempt: verified,
    nonprofit_status: profile.nonprofit_status,
  };
}

async function activeLicense(userId: string) {
  const rows = await sql`
    select * from public.licenses
    where user_id = ${userId} and status = 'active' and starts_at <= now()
      and (expires_at is null or expires_at > now())
    order by (expires_at is null) desc, expires_at desc
    limit 1`;
  return rows[0] ?? null;
}

const publicProfile = (p: Record<string, any>) => ({
  email: p.email, full_name: p.full_name, company_name: p.company_name,
  address_line1: p.address_line1, address_line2: p.address_line2, city: p.city, region: p.region,
  postal_code: p.postal_code, country: p.country, phone: p.phone, org_type: p.org_type, tax_id: p.tax_id,
  nonprofit_status: p.nonprofit_status, is_admin: p.is_admin, registered: p.registered_at !== null,
});

// ---------------------------------------------------------------- actions

async function check(req: Request, user: { id: string }, body: Record<string, any>) {
  const machineId = String(body.machine_id ?? "").slice(0, 128);
  const machineName = String(body.machine_name ?? "").slice(0, 128);
  const appVersion = String(body.app_version ?? "").slice(0, 32);
  if (machineId.length < 8) return json({ status: "error", message: "missing machine id" }, 400);

  const [profile] = await sql`select * from public.profiles where id = ${user.id}`;
  if (!profile) return json({ status: "error", message: "no profile" }, 404);
  if (!profile.registered_at) return json({ status: "needs_registration", profile: publicProfile(profile) });

  const lic = await activeLicense(user.id);
  if (!lic) return json({ status: "no_license", profile: publicProfile(profile), pricing: pricing(profile) });

  const ip = clientIp(req);
  const others = await sql`
    select machine_name, public_ip, last_seen from public.activations
    where license_id = ${lic.id} and machine_id <> ${machineId}
      and last_seen > now() - make_interval(mins => ${SEAT_TIMEOUT_MIN})
    order by last_seen desc`;
  const names = others.map((o) => o.machine_name || "another computer");
  if (others.length >= lic.seats)
    return json({ status: "seats_full", seats: lic.seats, machines: names, profile: publicProfile(profile) });
  if (others.some((o) => o.public_ip && ip && o.public_ip !== ip))
    return json({ status: "different_network", seats: lic.seats, machines: names, profile: publicProfile(profile) });

  await sql`
    insert into public.activations (license_id, user_id, machine_id, machine_name, public_ip, app_version)
    values (${lic.id}, ${user.id}, ${machineId}, ${machineName}, ${ip}, ${appVersion})
    on conflict (license_id, machine_id) do update
      set last_seen = now(), machine_name = excluded.machine_name, public_ip = excluded.public_ip, app_version = excluded.app_version`;

  const now = Date.now();
  const expires = lic.expires_at ? new Date(lic.expires_at).getTime() : null;
  const validUntil = Math.min(now + OFFLINE_DAYS * 86400e3, expires ?? Number.MAX_SAFE_INTEGER);
  const token = await signToken({
    v: 1, kid: KEY_ID, sub: user.id, email: profile.email, name: profile.full_name, company: profile.company_name,
    license_id: lic.id, kind: lic.kind, seats: lic.seats, expires_at: expires, machine_id: machineId,
    issued_at: now, valid_until: validUntil, admin: profile.is_admin,
  });
  return json({
    status: "ok", token, profile: publicProfile(profile),
    license: { id: lic.id, kind: lic.kind, seats: lic.seats, expires_at: lic.expires_at, source: lic.source },
    machines_in_use: names.length + 1,
  });
}

async function release(user: { id: string }, body: Record<string, any>) {
  await sql`delete from public.activations where user_id = ${user.id} and machine_id = ${String(body.machine_id ?? "")}`;
  return json({ status: "ok" });
}

// The Stripe secret key, from the Edge Function secret STRIPE_SECRET_KEY (or "Stripe" / "STRIPE").
const stripeKey = () => Deno.env.get("STRIPE_SECRET_KEY") ?? Deno.env.get("Stripe") ?? Deno.env.get("STRIPE") ?? Deno.env.get("stripe");

function stripe() {
  const key = stripeKey();
  return key ? new Stripe(key, { apiVersion: "2024-06-20", httpClient: Stripe.createFetchHttpClient() }) : null;
}

async function checkout(user: { id: string }, body: Record<string, any>) {
  const plan = String(body.plan ?? "");
  if (!(plan in PRICE_CENTS)) return json({ status: "error", message: "unknown plan" }, 400);
  const s = stripe();
  if (!s) return json({ status: "error", message: "Online payment isn't set up yet. Please contact us for a license." }, 503);

  const [profile] = await sql`select * from public.profiles where id = ${user.id}`;
  if (!profile?.registered_at) return json({ status: "needs_registration" }, 400);
  if (profile.org_type === "nonprofit" && profile.nonprofit_status === "pending" && !body.accept_full_price)
    return json({ status: "nonprofit_pending" });
  const lic = await activeLicense(user.id);
  if (lic && lic.kind === "perpetual") return json({ status: "already_licensed" });

  const price = pricing(profile);
  const amount = plan === "annual" ? price.annual_cents : price.perpetual_cents;
  const address = {
    line1: profile.address_line1 ?? undefined, line2: profile.address_line2 ?? undefined, city: profile.city ?? undefined,
    state: profile.region ?? undefined, postal_code: profile.postal_code ?? undefined, country: profile.country ?? undefined,
  };
  const customerFields = {
    email: profile.email, name: profile.company_name || profile.full_name, phone: profile.phone ?? undefined, address,
    tax_exempt: (price.tax_exempt ? "exempt" : "none") as Stripe.CustomerUpdateParams.TaxExempt,
    metadata: { user_id: user.id, contact: profile.full_name ?? "", tax_id: profile.tax_id ?? "" },
  };
  let customerId = profile.stripe_customer_id as string | null;
  if (customerId) await s.customers.update(customerId, customerFields);
  else {
    customerId = (await s.customers.create(customerFields)).id;
    await sql`update public.profiles set stripe_customer_id = ${customerId} where id = ${user.id}`;
  }

  const metadata = {
    user_id: user.id, kind: plan, price_cents: String(amount), discount_pct: String(price.discount_pct),
    tax_exempt: String(price.tax_exempt),
  };
  const product = {
    name: plan === "annual" ? "LiveDeck annual license" : "LiveDeck perpetual license",
    description: "2 computers at a time on the same network" + (price.discount_pct ? ` (non-profit ${price.discount_pct}% off)` : ""),
  };
  const done = `${SUPABASE_URL}/functions/v1/license?page=done`;
  const cancel = `${SUPABASE_URL}/functions/v1/license?page=cancel`;
  const tax = Deno.env.get("STRIPE_TAX") === "on";
  const session = await s.checkout.sessions.create({
    mode: plan === "annual" ? "subscription" : "payment",
    customer: customerId,
    client_reference_id: user.id,
    line_items: [{
      quantity: 1,
      price_data: {
        currency: "usd", unit_amount: amount, product_data: product,
        ...(plan === "annual" ? { recurring: { interval: "year" as const } } : {}),
      },
    }],
    metadata,
    ...(plan === "annual" ? { subscription_data: { metadata } } : { payment_intent_data: { metadata } }),
    ...(tax ? { automatic_tax: { enabled: true }, customer_update: { address: "auto" as const, name: "auto" as const } } : {}),
    success_url: done,
    cancel_url: cancel,
  });
  return json({ status: "ok", url: session.url, amount_cents: amount, discount_pct: price.discount_pct });
}

async function stripeSetup(user: { id: string }) {
  const [profile] = await sql`select is_admin from public.profiles where id = ${user.id}`;
  if (!profile?.is_admin) return json({ status: "error", message: "admins only" }, 403);
  const s = stripe();
  if (!s) return json({ status: "error", message: "No Stripe secret key found (add an Edge Function secret named STRIPE_SECRET_KEY)" }, 503);
  const [existing] = await sql`select value from private.settings where key = 'stripe_webhook_id'`;
  if (existing) return json({ status: "ok", message: "Stripe webhook already registered" });
  const hook = await s.webhookEndpoints.create({
    url: `${SUPABASE_URL}/functions/v1/stripe-webhook`,
    enabled_events: ["checkout.session.completed", "invoice.paid", "customer.subscription.deleted"],
    description: "LiveDeck licenses",
  });
  await sql`insert into private.settings (key, value) values ('stripe_webhook_id', ${hook.id}), ('stripe_webhook_secret', ${hook.secret!})
            on conflict (key) do update set value = excluded.value, updated_at = now()`;
  return json({ status: "ok", message: "Stripe webhook registered" });
}

// ---------------------------------------------------------------- entry

Deno.serve(async (req) => {
  if (req.method === "OPTIONS") return new Response("ok", { headers: cors });
  const url = new URL(req.url);
  try {
    if (req.method === "GET") {
      if (url.searchParams.has("pubkey")) {
        const { jwk } = await signingKey();
        return json({ kid: KEY_ID, alg: "RS256-PKCS1", n: jwk.n, e: jwk.e });
      }
      const page = url.searchParams.get("page");
      if (page === "done") return text("Payment received - thank you!\n\nYou can close this tab and go back to LiveDeck: your license activates in a few seconds.");
      if (page === "cancel") return text("Checkout cancelled. Nothing was charged.\n\nYou can close this tab and go back to LiveDeck.");
      return text("LiveDeck license server");
    }

    const user = await userFrom(req);
    if (!user) return json({ status: "signed_out", message: "Please sign in again." }, 401);
    const body = await req.json().catch(() => ({}));
    switch (body.action) {
      case "check": return await check(req, user, body);
      case "release": return await release(user, body);
      case "checkout": return await checkout(user, body);
      case "stripe-setup": return await stripeSetup(user);
      default: return json({ status: "error", message: "unknown action" }, 400);
    }
  } catch (e) {
    console.error(e);
    return json({ status: "error", message: String((e as Error).message ?? e) }, 500);
  }
});
