# Accounts and licensing

LiveDeck requires an account and a license. This page is for the LiveDeck owner (you): what's already set up, and the steps left to finish.

## How it works

- **Sign in:** with **email and password**, or with **Google** or **Microsoft** in the browser (Supabase Auth, PKCE).
  - New email accounts confirm their address through an emailed link. If LiveDeck is open when the link is clicked, the user is signed in automatically.
  - **Forgot Password?** emails a reset link. Opening it while LiveDeck is open asks for a new password in the app.
  - Signed-in users can change their password under **Account > Change Password**.
- **Registration:** the first sign-in asks for name, company, address and country, and the organization type. Non-profits also give their tax ID / EIN.
- **Prices:** annual is **$50/year** and perpetual is **$129**. Verified non-profits get **10% off** ($45 and $116.10) and are marked **tax exempt** in Stripe. You verify non-profits in the Admin Panel.
- **Payments:** these go through Stripe Checkout in the browser. Once paid, the license is created automatically. Annual licenses renew through a Stripe subscription.
- **Two computers per license, on the same network:**
  - When two computers run LiveDeck at the same time, both must reach the internet from the same public IP address.
  - A computer that quits frees its place right away. One that crashes or goes offline frees it after 10 minutes.
- **Every launch** checks the license. The server returns a signed token (RSA-2048), and LiveDeck can start offline on that token for **14 days**.
- **During a show** LiveDeck never locks. Problems found while it's running are logged, and they apply at the next launch.
- **Admin:**
  - Accounts on the admin list (`wayne.goodman.productions@gmail.com`, `waynegoodman@outlook.com`) see **Account > Admin Panel**.
  - From there you can list users, verify non-profits, create, revoke or extend licenses, release computers, and register the Stripe webhook.
  - Users appear in the list after their first sign-in.

## What's already set up

- Supabase project **livedeck** (`wtrrobcvdbxlwnyjtuts`, us-east-1, in your "Sunday's" org).
- **Database:** profiles, licenses and activations, with row-level security so users only see their own data and only admins can create licenses.
  - The schema is in `server/supabase/migrations/`.
- **Edge Functions**, with their code in `server/supabase/functions/`:
  - `license`: license check, release, checkout and Stripe setup.
  - `stripe-webhook`: turns payments into licenses.
- **License signing key:** generated on the server. The private key never leaves Supabase, and the public key is built into LiveDeck (`Source/Core/License.cpp`).

## Steps left (about 30 minutes, in the Supabase, email service, Google, Microsoft and Stripe dashboards)

Email and password sign-in works as soon as step 1 is done (plus step 2 before real customers sign up). Google and Microsoft are optional extras.

### 1. Allow LiveDeck's sign-in callback

Supabase dashboard > **livedeck** > Authentication > **URL Configuration**.

- **Site URL:** `http://127.0.0.1:53682`
- **Redirect URLs:** add these three:
  - `http://127.0.0.1:53682/callback`
  - `http://127.0.0.1:53683/callback`
  - `http://127.0.0.1:53684/callback`

### 2. Email sign-up emails (needed for email and password accounts)

Supabase's built-in email sender only delivers to members of your Supabase team, a few emails an hour. That's enough for testing, not for customers. Set up your own sender:

1. Create an account with an email service: [Resend](https://resend.com), Postmark or SendGrid all work. Verify your domain there (e.g. `livedeck.app` or your company domain).
2. In Supabase, go to Authentication > **Emails > SMTP Settings**. Turn on custom SMTP and enter the service's host, port, user and password.
   - **Sender email:** e.g. `accounts@yourdomain.com`
   - **Sender name:** "LiveDeck"
3. Optional: under Authentication > **Emails > Templates**, reword the "Confirm signup" and "Reset password" emails. Keep `{{ .ConfirmationURL }}` as the link.
4. Authentication > **Providers > Email** is on by default. Keep **Confirm email** turned on.

### 3. Google sign-in

1. Go to [Google Cloud Console](https://console.cloud.google.com/) > APIs & Services. Set up the **OAuth consent screen**: External, app name "LiveDeck", your support email.
2. Credentials > **Create credentials > OAuth client ID** > **Web application**.
   - **Authorized redirect URI:** `https://wtrrobcvdbxlwnyjtuts.supabase.co/auth/v1/callback`
3. In Supabase, go to Authentication > **Providers > Google**. Turn it on and paste the **Client ID** and **Client secret**.

### 4. Microsoft sign-in

1. Go to [Azure portal](https://portal.azure.com/) > **App registrations > New registration**.
   - Name: LiveDeck.
   - Supported accounts: **any organizational directory and personal Microsoft accounts**.
   - Redirect URI (Web): `https://wtrrobcvdbxlwnyjtuts.supabase.co/auth/v1/callback`
2. **Certificates & secrets > New client secret.** Copy the **Value**.
3. In Supabase, go to Authentication > **Providers > Azure**. Turn it on and fill in:
   - **Application (client) ID**
   - The **secret value**
   - **Azure Tenant URL:** `https://login.microsoftonline.com/common`

### 5. Stripe

1. **Roll the secret key that was shared in chat:** Stripe > Developers > API keys > secret key > Roll key.
2. Add the new key to Supabase: Edge Functions > **Secrets**. Name it `STRIPE_SECRET_KEY` and paste the new `sk_live_...` value.
3. Optional, to have Stripe calculate sales tax:
   - Turn on **Stripe Tax** in Stripe and add your tax registrations.
   - Add a Supabase secret `STRIPE_TAX` with the value `on`.
   - Without it, no tax is added. Non-profit customers are marked tax exempt either way.
4. Register the webhook. Open LiveDeck, sign in, then go to **Account > Admin Panel > Set Up Stripe Webhook**.
   - This creates the webhook in Stripe and stores its signing secret on the server.

### 6. Give yourself a license

Sign in to LiveDeck (it shows the purchase page) and click **Admin Panel**. Select yourself, choose **Perpetual**, and click **Create License**. Then click **I Already Have a License - Check Again**.

## Development builds

`cmake -DLIVEDECK_LICENSING=OFF ...` builds LiveDeck without the sign-in and license check, for development only. Release builds always have it on.
