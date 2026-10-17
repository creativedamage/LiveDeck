// Stripe -> LiveDeck licenses (Supabase Edge Function "stripe-webhook", no JWT: Stripe signs each event).
//   checkout.session.completed  -> creates the license (annual: until the subscription period ends + 3 days)
//   invoice.paid (renewal)      -> extends the annual license to the new period end + 3 days
//   customer.subscription.deleted -> nothing to do: the license simply runs to its end date

import postgres from "npm:postgres@3.4.4";
import Stripe from "npm:stripe@17.4.0";

const sql = postgres(Deno.env.get("SUPABASE_DB_URL")!, { prepare: false, max: 2 });
const GRACE_DAYS = 3;

const stripeKey = () => Deno.env.get("STRIPE_SECRET_KEY") ?? Deno.env.get("Stripe") ?? Deno.env.get("STRIPE") ?? Deno.env.get("stripe");

function stripe() {
  return new Stripe(stripeKey()!, { apiVersion: "2024-06-20", httpClient: Stripe.createFetchHttpClient() });
}

async function webhookSecret() {
  const env = Deno.env.get("STRIPE_WEBHOOK_SECRET");
  if (env) return env;
  const [row] = await sql`select value from private.settings where key = 'stripe_webhook_secret'`;
  return row?.value as string | undefined;
}

Deno.serve(async (req) => {
  const secret = await webhookSecret();
  if (!secret || !stripeKey()) return new Response("not configured", { status: 503 });
  const s = stripe();
  const body = await req.text();
  let event: Stripe.Event;
  try {
    event = await s.webhooks.constructEventAsync(body, req.headers.get("Stripe-Signature") ?? "", secret,
                                                 undefined, Stripe.createSubtleCryptoProvider());
  } catch (e) {
    return new Response(`bad signature: ${(e as Error).message}`, { status: 400 });
  }

  try {
    if (event.type === "checkout.session.completed") {
      const cs = event.data.object as Stripe.Checkout.Session;
      if (cs.payment_status !== "paid" && cs.payment_status !== "no_payment_required") return new Response("not paid yet");
      const m = cs.metadata ?? {};
      const kind = m.kind === "annual" ? "annual" : "perpetual";
      let expires: Date | null = null;
      let subscriptionId: string | null = null;
      if (kind === "annual") {
        subscriptionId = typeof cs.subscription === "string" ? cs.subscription : cs.subscription?.id ?? null;
        const end = subscriptionId ? (await s.subscriptions.retrieve(subscriptionId)).current_period_end : null;
        expires = new Date(((end ?? Math.floor(Date.now() / 1000) + 365 * 86400) + GRACE_DAYS * 86400) * 1000);
      }
      await sql`
        insert into public.licenses (user_id, kind, seats, expires_at, source, price_cents, discount_pct, tax_exempt,
                                     stripe_session_id, stripe_subscription_id, notes)
        values (${m.user_id}, ${kind}, 2, ${expires}, 'stripe', ${Number(m.price_cents ?? cs.amount_total ?? 0)},
                ${Number(m.discount_pct ?? 0)}, ${m.tax_exempt === "true"}, ${cs.id}, ${subscriptionId},
                ${"Stripe " + (cs.amount_total != null ? "$" + (cs.amount_total / 100).toFixed(2) : "")})
        on conflict (stripe_session_id) do nothing`;
    } else if (event.type === "invoice.paid") {
      const inv = event.data.object as Stripe.Invoice;
      const sub = typeof inv.subscription === "string" ? inv.subscription : inv.subscription?.id;
      const end = inv.lines?.data?.[0]?.period?.end;
      if (sub && end && inv.billing_reason !== "subscription_create")
        await sql`update public.licenses set expires_at = ${new Date((end + GRACE_DAYS * 86400) * 1000)}, status = 'active'
                  where stripe_subscription_id = ${sub}`;
    }
  } catch (e) {
    console.error(e);
    return new Response(`error: ${(e as Error).message}`, { status: 500 });   // Stripe retries
  }
  return new Response("ok");
});
