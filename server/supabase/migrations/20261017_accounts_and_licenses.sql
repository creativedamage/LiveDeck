-- LiveDeck accounts, licenses and computer activations (Supabase project "livedeck").
-- This is the schema as applied to the live project, in one file.

create schema if not exists private;
create extension if not exists pg_net;

-- Emails that become administrators when they first sign in.
create table private.admin_emails (email text primary key);
insert into private.admin_emails (email) values ('wayne.goodman.productions@gmail.com'), ('waynegoodman@outlook.com');

-- Server-only material: the license signing key (created by the license function on first use) and settings.
create table private.signing_keys (id text primary key, private_pkcs8 text not null, public_spki text not null,
                                   created_at timestamptz not null default now());
create table private.settings (key text primary key, value text not null, updated_at timestamptz not null default now());

create table public.profiles (
  id uuid primary key references auth.users (id) on delete cascade,
  email text not null,
  full_name text, company_name text, address_line1 text, address_line2 text, city text, region text,
  postal_code text, country text, phone text,
  org_type text not null default 'business' check (org_type in ('business', 'nonprofit')),
  tax_id text,
  nonprofit_status text not null default 'none' check (nonprofit_status in ('none', 'pending', 'verified', 'rejected')),
  stripe_customer_id text,
  is_admin boolean not null default false,
  registered_at timestamptz,
  created_at timestamptz not null default now(),
  updated_at timestamptz not null default now()
);

create table public.licenses (
  id uuid primary key default gen_random_uuid(),
  user_id uuid not null references public.profiles (id) on delete cascade,
  kind text not null check (kind in ('annual', 'perpetual')),
  status text not null default 'active' check (status in ('active', 'revoked')),
  seats integer not null default 2 check (seats between 1 and 50),
  starts_at timestamptz not null default now(),
  expires_at timestamptz,
  source text not null default 'admin' check (source in ('admin', 'stripe')),
  price_cents integer, discount_pct integer not null default 0, tax_exempt boolean not null default false,
  stripe_session_id text unique, stripe_subscription_id text, notes text,
  created_by uuid,   -- admin who created it (deliberately not a foreign key: keeps profiles->licenses unambiguous for the API)
  created_at timestamptz not null default now(), updated_at timestamptz not null default now(),
  check (kind = 'perpetual' or expires_at is not null)
);
create index licenses_user_idx on public.licenses (user_id);
create index licenses_subscription_idx on public.licenses (stripe_subscription_id);

create table public.activations (
  id uuid primary key default gen_random_uuid(),
  license_id uuid not null references public.licenses (id) on delete cascade,
  user_id uuid not null references public.profiles (id) on delete cascade,
  machine_id text not null, machine_name text, public_ip text, app_version text,
  first_seen timestamptz not null default now(), last_seen timestamptz not null default now(),
  unique (license_id, machine_id)
);
create index activations_user_idx on public.activations (user_id);

-- Admin check used by the row-level security policies (kept out of the public API).
create or replace function private.is_admin() returns boolean language sql stable security definer set search_path = '' as $$
  select coalesce((select p.is_admin from public.profiles p where p.id = (select auth.uid())), false);
$$;
grant usage on schema private to authenticated;
revoke all on function private.is_admin() from public, anon;
grant execute on function private.is_admin() to authenticated;

-- New sign-in -> profile (admin if the email is on the admin list).
create or replace function private.handle_new_user() returns trigger language plpgsql security definer set search_path = '' as $$
begin
  insert into public.profiles (id, email, full_name, is_admin)
  values (new.id, coalesce(new.email, ''),
          coalesce(new.raw_user_meta_data ->> 'full_name', new.raw_user_meta_data ->> 'name'),
          exists (select 1 from private.admin_emails a where lower(a.email) = lower(new.email)))
  on conflict (id) do nothing;
  return new;
end;
$$;
create trigger on_auth_user_created after insert on auth.users for each row execute function private.handle_new_user();

-- Users edit their own registration; admin, verification and billing fields are protected.
create or replace function private.protect_profile() returns trigger language plpgsql security definer set search_path = '' as $$
declare
  uid uuid := (select auth.uid());
  caller_is_admin boolean := coalesce((select p.is_admin from public.profiles p where p.id = uid), false);
begin
  new.updated_at := now();
  if uid is null or (select auth.role()) = 'service_role' or caller_is_admin then return new; end if;
  new.id := old.id; new.email := old.email; new.is_admin := old.is_admin;
  new.stripe_customer_id := old.stripe_customer_id; new.created_at := old.created_at;
  if new.org_type = 'business' then new.nonprofit_status := 'none';
  elsif old.org_type is distinct from 'nonprofit' or new.tax_id is distinct from old.tax_id or old.nonprofit_status = 'none' then
    new.nonprofit_status := 'pending';
  else new.nonprofit_status := old.nonprofit_status;
  end if;
  if old.registered_at is not null then new.registered_at := old.registered_at;
  elsif coalesce(trim(new.full_name), '') <> '' and coalesce(trim(new.company_name), '') <> ''
     and coalesce(trim(new.address_line1), '') <> '' and coalesce(trim(new.city), '') <> ''
     and coalesce(trim(new.postal_code), '') <> '' and coalesce(trim(new.country), '') <> ''
     and (new.org_type = 'business' or coalesce(trim(new.tax_id), '') <> '') then new.registered_at := now();
  else new.registered_at := null;
  end if;
  return new;
end;
$$;
create trigger profiles_protect before update on public.profiles for each row execute function private.protect_profile();

create or replace function private.touch_updated_at() returns trigger language plpgsql set search_path = '' as $$
begin new.updated_at := now(); return new; end;
$$;
create trigger licenses_touch before update on public.licenses for each row execute function private.touch_updated_at();

alter table private.admin_emails enable row level security;
alter table private.signing_keys enable row level security;
alter table private.settings enable row level security;
alter table public.profiles enable row level security;
alter table public.licenses enable row level security;
alter table public.activations enable row level security;

create policy "profiles: own or admin can read" on public.profiles for select to authenticated
  using (id = (select auth.uid()) or (select private.is_admin()));
create policy "profiles: own or admin can update" on public.profiles for update to authenticated
  using (id = (select auth.uid()) or (select private.is_admin())) with check (id = (select auth.uid()) or (select private.is_admin()));
create policy "licenses: own or admin can read" on public.licenses for select to authenticated
  using (user_id = (select auth.uid()) or (select private.is_admin()));
create policy "licenses: admin can create" on public.licenses for insert to authenticated with check ((select private.is_admin()));
create policy "licenses: admin can change" on public.licenses for update to authenticated
  using ((select private.is_admin())) with check ((select private.is_admin()));
create policy "licenses: admin can delete" on public.licenses for delete to authenticated using ((select private.is_admin()));
create policy "activations: own or admin can read" on public.activations for select to authenticated
  using (user_id = (select auth.uid()) or (select private.is_admin()));
create policy "activations: own or admin can release" on public.activations for delete to authenticated
  using (user_id = (select auth.uid()) or (select private.is_admin()));
