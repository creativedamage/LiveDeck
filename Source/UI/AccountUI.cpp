#include "AccountUI.h"

using namespace omr;

namespace
{
const juce::StringArray kCountries {
    "US|United States", "CA|Canada", "GB|United Kingdom", "IE|Ireland", "AU|Australia", "NZ|New Zealand", "DE|Germany", "FR|France",
    "NL|Netherlands", "BE|Belgium", "LU|Luxembourg", "CH|Switzerland", "AT|Austria", "IT|Italy", "ES|Spain", "PT|Portugal",
    "SE|Sweden", "NO|Norway", "DK|Denmark", "FI|Finland", "IS|Iceland", "PL|Poland", "CZ|Czechia", "HU|Hungary", "GR|Greece",
    "EE|Estonia", "LV|Latvia", "LT|Lithuania", "SI|Slovenia", "HR|Croatia", "RO|Romania", "BG|Bulgaria", "MX|Mexico", "BR|Brazil",
    "AR|Argentina", "CL|Chile", "CO|Colombia", "PE|Peru", "JP|Japan", "KR|South Korea", "SG|Singapore", "HK|Hong Kong",
    "TW|Taiwan", "IN|India", "PH|Philippines", "MY|Malaysia", "TH|Thailand", "ID|Indonesia", "AE|United Arab Emirates",
    "IL|Israel", "ZA|South Africa", "NG|Nigeria", "KE|Kenya"
};

juce::var obj (std::initializer_list<std::pair<const char*, juce::var>> kv)
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    for (const auto& [k, v] : kv) o->setProperty (k, v);
    return juce::var (o.get());
}

juce::String str (const juce::var& v, const char* key) { return v.getProperty (key, {}).toString(); }

juce::String niceDate (const juce::var& isoOrMs)
{
    if (isoOrMs.isVoid() || isoOrMs.isUndefined() || isoOrMs.toString().isEmpty()) return {};
    juce::Time t = isoOrMs.isString() ? juce::Time::fromISO8601 (isoOrMs.toString()) : juce::Time ((juce::int64) (double) isoOrMs);
    return t.formatted ("%d %b %Y");
}

// Flat primary / secondary buttons.
void stylePrimary (juce::TextButton& b)
{
    b.setColour (juce::TextButton::buttonColourId, Theme::blue);
    b.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
}

// The LiveDeck mark: six rounded bars (same shape as the app icon).
void drawMark (juce::Graphics& g, juce::Rectangle<float> r)
{
    static const float heights[] = { 0.36f, 0.70f, 1.0f, 0.52f, 0.80f, 0.40f };
    const float w = r.getWidth() / 11.0f;
    g.setColour (Theme::text);
    for (int i = 0; i < 6; ++i)
    {
        const float h = r.getHeight() * heights[i];
        g.fillRoundedRectangle (r.getX() + w * (float) (i * 2), r.getCentreY() - h * 0.5f, w, h, w * 0.5f);
    }
}
} // namespace

// ====================================================================================== RegistrationForm

RegistrationForm::RegistrationForm (AccountService& a, const juce::var& profile, bool showCancel) : account (a)
{
    auto make = [this, &profile] (Field& f, const juce::String& label, const char* key, bool required, const juce::String& hint = {})
    {
        f.required = required;
        f.label = std::make_unique<juce::Label> (juce::String(), label + (required ? " *" : ""));
        f.label->setColour (juce::Label::textColourId, Theme::textDim);
        f.label->setFont (Theme::ui (12.0f));
        f.editor = std::make_unique<juce::TextEditor>();
        f.editor->setText (str (profile, key), juce::dontSendNotification);
        f.editor->setTextToShowWhenEmpty (hint, Theme::textDim.withAlpha (0.6f));
        f.editor->setIndents (8, 0);
        f.editor->setJustification (juce::Justification::centredLeft);
        addAndMakeVisible (*f.label);
        addAndMakeVisible (*f.editor);
    };
    make (fullName, "Your name", "full_name", true, "First and last name");
    make (company, "Company or organization", "company_name", true);
    make (phone, "Phone", "phone", false);
    make (addr1, "Address", "address_line1", true, "Street address");
    make (addr2, "Address line 2", "address_line2", false, "Suite, unit, floor (optional)");
    make (city, "City", "city", true);
    make (region, "State / region", "region", false, "e.g. TX");
    make (postal, "ZIP / postal code", "postal_code", true);
    make (taxId, "Tax ID / EIN", "tax_id", false, "Your non-profit's EIN or tax-exemption number");

    emailLabel.setText ("Signed in as " + str (profile, "email"), juce::dontSendNotification);
    emailLabel.setColour (juce::Label::textColourId, Theme::textDim);
    addAndMakeVisible (emailLabel);

    for (auto* l : { &countryLabel, &orgLabel })
    {
        l->setColour (juce::Label::textColourId, Theme::textDim);
        l->setFont (Theme::ui (12.0f));
        addAndMakeVisible (*l);
    }
    for (int i = 0; i < kCountries.size(); ++i) country.addItem (kCountries[i].fromFirstOccurrenceOf ("|", false, false), i + 1);
    country.addItem ("Other (enter 2-letter code)", 1000);
    const auto cc = str (profile, "country").toUpperCase();
    int sel = 1;
    for (int i = 0; i < kCountries.size(); ++i) if (kCountries[i].startsWith (cc + "|")) sel = i + 1;
    if (cc.isNotEmpty() && sel == 1 && cc != "US") { sel = 1000; countryOther.setText (cc); }
    country.setSelectedId (sel, juce::dontSendNotification);
    country.onChange = [this] { countryOther.setVisible (country.getSelectedId() == 1000); };
    countryOther.setInputRestrictions (2, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz");
    countryOther.setTextToShowWhenEmpty ("XX", Theme::textDim);
    countryOther.setVisible (sel == 1000);
    addAndMakeVisible (country);
    addChildComponent (countryOther);

    orgType.addItem ("Business", 1);
    orgType.addItem ("Non-profit / charity", 2);
    orgType.setSelectedId (str (profile, "org_type") == "nonprofit" ? 2 : 1, juce::dontSendNotification);
    orgType.onChange = [this] { updateNonprofit(); };
    addAndMakeVisible (orgType);

    note.setFont (Theme::ui (12.0f));
    note.setColour (juce::Label::textColourId, Theme::textDim);
    note.setJustificationType (juce::Justification::topLeft);
    addAndMakeVisible (note);
    status.setColour (juce::Label::textColourId, Theme::orange);
    addAndMakeVisible (status);

    stylePrimary (saveButton);
    saveButton.onClick = [this] { save(); };
    addAndMakeVisible (saveButton);
    cancelButton.onClick = [this] { if (onCancel) onCancel(); };
    addChildComponent (cancelButton);
    cancelButton.setVisible (showCancel);
    if (showCancel) saveButton.setButtonText ("Save");

    const auto np = str (profile, "nonprofit_status");
    if (np == "verified") note.setText ("Non-profit verified: 10% off and tax exempt.", juce::dontSendNotification);
    updateNonprofit();
}

void RegistrationForm::updateNonprofit()
{
    const bool np = orgType.getSelectedId() == 2;
    taxId.required = np;
    taxId.label->setText (np ? "Tax ID / EIN *" : "Tax ID / EIN", juce::dontSendNotification);
    taxId.label->setVisible (np);
    taxId.editor->setVisible (np);
    if (np && ! note.getText().startsWith ("Non-profit verified"))
        note.setText ("Non-profits get 10% off and are not charged sales tax. We verify your non-profit status (usually within one "
                      "business day) before the discount and exemption apply.", juce::dontSendNotification);
    else if (! np)
        note.setText ({}, juce::dontSendNotification);
    resized();
}

juce::String RegistrationForm::countryCode() const
{
    if (country.getSelectedId() == 1000) return countryOther.getText().trim().toUpperCase();
    const int i = country.getSelectedId() - 1;
    return i >= 0 && i < kCountries.size() ? kCountries[i].upToFirstOccurrenceOf ("|", false, false) : juce::String ("US");
}

void RegistrationForm::save()
{
    bool missing = false;
    for (auto* f : fields)
    {
        const bool empty = f->required && f->editor->isVisible() && f->editor->getText().trim().isEmpty();
        f->editor->setColour (juce::TextEditor::outlineColourId, empty ? Theme::red : Theme::edge);
        f->editor->repaint();
        missing |= empty;
    }
    if (countryCode().length() != 2) missing = true;
    if (missing) { status.setText ("Please fill in the fields marked *.", juce::dontSendNotification); return; }

    const bool np = orgType.getSelectedId() == 2;
    auto fieldsVar = obj ({
        { "full_name", fullName.editor->getText().trim() }, { "company_name", company.editor->getText().trim() },
        { "phone", phone.editor->getText().trim() }, { "address_line1", addr1.editor->getText().trim() },
        { "address_line2", addr2.editor->getText().trim() }, { "city", city.editor->getText().trim() },
        { "region", region.editor->getText().trim() }, { "postal_code", postal.editor->getText().trim() },
        { "country", countryCode() }, { "org_type", np ? "nonprofit" : "business" },
        { "tax_id", np ? taxId.editor->getText().trim() : juce::String() } });
    saveButton.setEnabled (false);
    status.setText ("Saving...", juce::dontSendNotification);
    juce::Component::SafePointer<RegistrationForm> safe (this);
    account.saveProfile (fieldsVar, [safe] (bool ok, juce::String err)
    {
        if (safe == nullptr) return;
        safe->saveButton.setEnabled (true);
        if (! ok) { safe->status.setText (err, juce::dontSendNotification); return; }
        safe->status.setText ({}, juce::dontSendNotification);
        if (safe->onSaved) safe->onSaved();
    });
}

void RegistrationForm::paint (juce::Graphics&) {}

void RegistrationForm::resized()
{
    auto r = getLocalBounds();
    emailLabel.setBounds (r.removeFromTop (22));
    r.removeFromTop (6);
    const int gap = 12, rowH = 50;
    auto place = [] (Field& f, juce::Rectangle<int> a)
    {
        f.label->setBounds (a.removeFromTop (18));
        f.editor->setBounds (a.removeFromTop (28));
    };
    auto row = [&] { auto x = r.removeFromTop (rowH); r.removeFromTop (4); return x; };
    { auto x = row(); place (fullName, x.removeFromLeft ((x.getWidth() - gap) / 2)); x.removeFromLeft (gap); place (phone, x); }
    place (company, row());
    place (addr1, row());
    place (addr2, row());
    { auto x = row(); const int w = (x.getWidth() - 2 * gap) / 3;
      place (city, x.removeFromLeft (w)); x.removeFromLeft (gap); place (region, x.removeFromLeft (w)); x.removeFromLeft (gap); place (postal, x); }
    { auto x = row();
      auto c = x.removeFromLeft ((x.getWidth() - gap) / 2); x.removeFromLeft (gap);
      countryLabel.setBounds (c.removeFromTop (18));
      auto cc = c.removeFromTop (28);
      if (countryOther.isVisible()) { countryOther.setBounds (cc.removeFromRight (50)); cc.removeFromRight (6); }
      country.setBounds (cc);
      orgLabel.setBounds (x.removeFromTop (18));
      orgType.setBounds (x.removeFromTop (28)); }
    if (taxId.editor->isVisible()) place (taxId, row());
    note.setBounds (r.removeFromTop (note.getText().isEmpty() ? 0 : 36));
    auto buttons = r.removeFromBottom (34);
    status.setBounds (r.removeFromBottom (22));
    saveButton.setBounds (buttons.removeFromRight (170));
    buttons.removeFromRight (8);
    if (cancelButton.isVisible()) cancelButton.setBounds (buttons.removeFromRight (90));
}

// ====================================================================================== LicenseGate

LicenseGate::LicenseGate (AccountService& a) : account (a)
{
    setWantsKeyboardFocus (true);
}

LicenseGate::~LicenseGate()
{
    stopTimer();
    account.cancelSignIn();
}

void LicenseGate::clearContent()
{
    // Pages are rebuilt from inside their own button / return-key callbacks, so the old components
    // are only hidden here and deleted on the next message-loop turn (never while their callback runs).
    std::vector<std::unique_ptr<juce::Component>> old;
    if (form != nullptr) { removeChildComponent (form.get()); old.push_back (std::move (form)); }
    while (! content.isEmpty())
    {
        auto* c = content.removeAndReturn (content.size() - 1);
        removeChildComponent (c);
        old.emplace_back (c);
    }
    if (old.empty()) return;
    auto bin = std::make_shared<std::vector<std::unique_ptr<juce::Component>>> (std::move (old));
    juce::MessageManager::callAsync ([bin] { bin->clear(); });
}

void LicenseGate::setPage (Page p, const juce::String& t, const juce::String& b)
{
    page = p;
    title = t;
    body = b;
    resized();
    repaint();
}

void LicenseGate::start()
{
    stopTimer();
    clearContent();
    setPage (Page::checking, "Checking your license...", {});
    juce::Component::SafePointer<LicenseGate> safe (this);
    account.check ([safe] (const AccountService::Check& c) { if (safe != nullptr) safe->showResult (c); });
}

void LicenseGate::addFooter (bool withAdmin)
{
    juce::Component::SafePointer<LicenseGate> safe (this);
    auto* who = own (new juce::Label ({}, account.isSignedIn() ? "Signed in as " + account.email() : juce::String()));
    who->setColour (juce::Label::textColourId, Theme::textDim);
    who->setComponentID ("footer-label");
    auto* out = own (new juce::TextButton ("Sign Out"));
    out->setComponentID ("footer-signout");
    out->onClick = [safe]
    {
        if (safe == nullptr) return;
        safe->clearContent();
        safe->setPage (Page::checking, "Signing out...", {});
        safe->account.signOut ([safe] { if (safe != nullptr) safe->start(); });
    };
    if (withAdmin)
    {
        auto* adm = own (new juce::TextButton ("Admin Panel..."));
        adm->setComponentID ("footer-admin");
        adm->onClick = [safe] { if (safe != nullptr && safe->onOpenAdmin) safe->onOpenAdmin(); };
    }
    for (auto* c : { (juce::Component*) who, (juce::Component*) out }) addAndMakeVisible (c);
    if (withAdmin) addAndMakeVisible (content.getLast());
}

void LicenseGate::showResult (const AccountService::Check& c)
{
    lastCheck = c;
    stopTimer();
    clearContent();
    juce::Component::SafePointer<LicenseGate> safe (this);
    const bool admin = c.isAdmin();

    if (c.status == "ok" || (c.status == "offline" && c.offlineVerdict == license::Verdict::ok))
    {
        if (onLicensed) onLicensed (c);
        return;
    }

    if (c.status == "signed_out")
    {
        showSignIn (c.message);
        return;
    }

    if (c.status == "needs_registration")
    {
        setPage (Page::registration, "Tell us about your organization",
                 "We need these details for your license and invoices.");
        form = std::make_unique<RegistrationForm> (account, c.profile, false);
        form->onSaved = [safe] { if (safe != nullptr) safe->start(); };
        addAndMakeVisible (*form);
        addFooter (admin);
        resized();
        return;
    }

    if (c.status == "no_license")
    {
        const auto& pr = c.pricing;
        const int discount = (int) pr.getProperty ("discount_pct", 0);
        const auto np = str (pr, "nonprofit_status");
        juce::String sub = "Each license runs LiveDeck on 2 computers at a time on the same network.";
        if (discount > 0) sub << "\nNon-profit pricing: " << discount << "% off, no sales tax.";
        else if (np == "pending") sub << "\nYour non-profit status is being verified. The 10% discount and tax exemption apply once it's approved.";
        else if (np == "rejected") sub << "\nWe couldn't verify your non-profit status. Contact us if this is a mistake.";
        if (admin) sub << "\nAdmin: you can also create a license for yourself in the Admin Panel.";
        setPage (Page::purchase, "Choose your LiveDeck license", sub);

        auto addPlan = [&] (const juce::String& id, const juce::String& name, int cents, int listCents, const juce::String& per, const juce::String& blurb)
        {
            auto* l = own (new juce::Label ({}, name));
            l->setComponentID ("plan-name-" + id);
            l->setFont (Theme::ui (15.0f, true));
            auto* p = own (new juce::Label ({}, license::formatPrice (cents) + per));
            p->setComponentID ("plan-price-" + id);
            p->setFont (Theme::ui (20.0f, true));
            auto* d = own (new juce::Label ({}, (listCents != cents ? "Regular price " + license::formatPrice (listCents) + ". " : juce::String()) + blurb));
            d->setComponentID ("plan-blurb-" + id);
            d->setColour (juce::Label::textColourId, Theme::textDim);
            d->setJustificationType (juce::Justification::topLeft);
            d->setMinimumHorizontalScale (1.0f);
            p->setMinimumHorizontalScale (1.0f);
            auto* b = own (new juce::TextButton ("Buy " + name));
            b->setComponentID ("plan-buy-" + id);
            stylePrimary (*b);
            b->onClick = [safe, id] { if (safe != nullptr) safe->buy (id, false); };
        };
        addPlan ("annual", "Annual", (int) pr.getProperty ("annual_cents", 5000), (int) pr.getProperty ("list_annual_cents", 5000), " / year",
                 "Renews every year. Includes every update while it's active.");
        addPlan ("perpetual", "Perpetual", (int) pr.getProperty ("perpetual_cents", 12900), (int) pr.getProperty ("list_perpetual_cents", 12900), "",
                 "Pay once, use it for good. Includes all updates.");
        auto* again = own (new juce::TextButton ("I Already Have a License - Check Again"));
        again->setComponentID ("secondary-0");
        again->onClick = [safe] { if (safe != nullptr) safe->start(); };
        auto* edit = own (new juce::TextButton ("Edit Registration"));
        edit->setComponentID ("secondary-1");
        edit->onClick = [safe]
        {
            if (safe == nullptr) return;
            auto c2 = safe->lastCheck;
            c2.status = "needs_registration";
            safe->showResult (c2);
        };
        for (auto* x : content) addAndMakeVisible (x);
        addFooter (admin);
        resized();
        return;
    }

    if (c.status == "seats_full" || c.status == "different_network")
    {
        const auto list = c.machines.joinIntoString (", ");
        if (c.status == "seats_full")
            setPage (Page::blocked, "Your license is in use on 2 computers",
                     "LiveDeck is open on: " + list + ".\n\nQuit LiveDeck on one of them, then try again. A computer that goes offline "
                     "frees its place after 10 minutes.");
        else
            setPage (Page::blocked, "Your other computer is on a different network",
                     "LiveDeck is open on " + list + ", which is connected to a different network. A license runs on 2 computers at a "
                     "time only when both are on the same network (for example, the same venue or office).");
        auto* retry = own (new juce::TextButton ("Try Again"));
        retry->setComponentID ("primary-0");
        stylePrimary (*retry);
        retry->onClick = [safe] { if (safe != nullptr) safe->start(); };
        addAndMakeVisible (retry);
        addFooter (admin);
        resized();
        return;
    }

    // offline without a usable cached license, or an error
    const bool offline = c.status == "offline";
    setPage (Page::blocked, offline ? "Can't verify your license" : "Something went wrong",
             offline ? (c.token.signatureOk ? license::describe (c.offlineVerdict)
                                            : "LiveDeck needs to reach the internet once to activate this computer.\n" + c.message)
                     : (c.message.isNotEmpty() ? c.message : juce::String ("The license server returned an error.")));
    auto* retry = own (new juce::TextButton ("Try Again"));
    retry->setComponentID ("primary-0");
    stylePrimary (*retry);
    retry->onClick = [safe] { if (safe != nullptr) safe->start(); };
    addAndMakeVisible (retry);
    if (account.isSignedIn()) addFooter (admin);
    resized();
}

// ---- sign-in pages

juce::TextEditor* LicenseGate::addField (const juce::String& id, const juce::String& hint, bool password)
{
    auto* e = own (new juce::TextEditor());
    e->setComponentID ("field-" + id);
    e->setTextToShowWhenEmpty (hint, Theme::textDim.withAlpha (0.7f));
    e->setIndents (10, 0);
    e->setJustification (juce::Justification::centredLeft);
    e->setFont (Theme::ui (14.0f));
    if (password) e->setPasswordCharacter ((juce::juce_wchar) 0x2022);
    addAndMakeVisible (e);
    return e;
}

juce::TextButton* LicenseGate::addButton (const juce::String& id, const juce::String& text, bool primary, std::function<void()> onClick)
{
    auto* b = own (new juce::TextButton (text));
    b->setComponentID (id);
    if (primary) stylePrimary (*b);
    if (id.startsWith ("rowbtn-"))
    {
        b->setColour (juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        b->setColour (juce::TextButton::textColourOffId, Theme::blue.brighter (0.3f));
    }
    b->onClick = std::move (onClick);
    addAndMakeVisible (b);
    return b;
}

void LicenseGate::addStatus (const juce::String& message)
{
    if (message.isEmpty()) return;
    auto* l = own (new juce::Label ({}, message));
    l->setComponentID ("status");
    l->setColour (juce::Label::textColourId, Theme::orange);
    l->setFont (Theme::ui (13.0f));
    l->setJustificationType (juce::Justification::topLeft);
    l->setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (l);
}

juce::TextEditor* LicenseGate::field (const juce::String& id) const
{
    for (auto* c : content)
        if (c->getComponentID() == "field-" + id) return dynamic_cast<juce::TextEditor*> (c);
    return nullptr;
}

void LicenseGate::showSignIn (const juce::String& message)
{
    stopTimer();
    clearContent();
    setPage (Page::signIn, "Sign in to LiveDeck", "Your license follows your account, on up to 2 computers.");
    juce::Component::SafePointer<LicenseGate> safe (this);
    auto* email = addField ("email", "Email", false);
    auto* pw = addField ("password", "Password", true);
    addStatus (message);
    auto signIn = [safe]
    {
        if (safe == nullptr) return;
        const auto e = safe->field ("email")->getText().trim();
        const auto p = safe->field ("password")->getText();
        if (! e.containsChar ('@') || p.isEmpty()) { safe->showSignIn ("Enter your email and password."); return; }
        safe->clearContent();
        safe->setPage (Page::checking, "Signing in...", {});
        safe->account.signInWithPassword (e, p, [safe, e] (bool ok, juce::String err)
        {
            if (safe == nullptr) return;
            if (ok) { safe->start(); return; }
            safe->showSignIn (err);
            if (auto* f = safe->field ("email")) f->setText (e);
        });
    };
    email->onReturnKey = [pw] { pw->grabKeyboardFocus(); };
    pw->onReturnKey = signIn;
    addButton ("primary-signin", "Sign In", true, signIn);
    addButton ("rowbtn-create", "Create an Account", false, [safe] { if (safe != nullptr) safe->showSignUp(); });
    addButton ("rowbtn-forgot", "Forgot Password?", false, [safe] { if (safe != nullptr) safe->showForgot(); });
    auto* div = own (new juce::Label ({}, "or"));
    div->setComponentID ("divider");
    div->setColour (juce::Label::textColourId, Theme::textDim);
    div->setJustificationType (juce::Justification::centred);
    addAndMakeVisible (div);
    addButton ("alt-google", "Continue with Google", false, [safe] { if (safe != nullptr) safe->signInWithProvider (AccountService::Provider::google); });
    addButton ("alt-microsoft", "Continue with Microsoft", false, [safe] { if (safe != nullptr) safe->signInWithProvider (AccountService::Provider::microsoft); });
    resized();
    juce::MessageManager::callAsync ([email] { if (email->isShowing()) email->grabKeyboardFocus(); });
}

void LicenseGate::signInWithProvider (AccountService::Provider p)
{
    juce::Component::SafePointer<LicenseGate> safe (this);
    clearContent();
    setPage (Page::waitingBrowser, "Finish signing in in your browser",
             "A sign-in page has opened in your web browser. When it says you're signed in, come back here.");
    addButton ("primary-cancel", "Cancel", false, [safe] { if (safe != nullptr) safe->account.cancelSignIn(); });
    resized();
    account.signIn (p, [safe] (bool ok, juce::String err)
    {
        if (safe == nullptr) return;
        if (ok) safe->start();
        else safe->showSignIn (err == "Sign-in cancelled." ? juce::String() : err);
    });
}

void LicenseGate::showSignUp (const juce::String& message)
{
    clearContent();
    setPage (Page::signUp, "Create your LiveDeck account",
             "Use your work email. We'll send a link to confirm it. Passwords need at least "
             + juce::String (AccountService::kMinPasswordLength) + " characters.");
    juce::Component::SafePointer<LicenseGate> safe (this);
    auto* email = addField ("email", "Email", false);
    auto* pw = addField ("password", "Password", true);
    auto* pw2 = addField ("password2", "Repeat password", true);
    addStatus (message);
    auto create = [safe]
    {
        if (safe == nullptr) return;
        const auto e = safe->field ("email")->getText().trim();
        const auto p = safe->field ("password")->getText();
        auto fail = [safe, e] (const juce::String& m) { safe->showSignUp (m); safe->field ("email")->setText (e); };
        if (! e.containsChar ('@') || ! e.fromLastOccurrenceOf ("@", false, false).containsChar ('.')) { fail ("Enter a valid email address."); return; }
        if (p.length() < AccountService::kMinPasswordLength) { fail ("Use at least " + juce::String (AccountService::kMinPasswordLength) + " characters for the password."); return; }
        if (p != safe->field ("password2")->getText()) { fail ("The two passwords don't match."); return; }
        safe->clearContent();
        safe->setPage (Page::checking, "Creating your account...", {});
        safe->account.signUp (e, p, [safe, e] (AccountService::EmailStep st, juce::String msg)
        {
            if (safe == nullptr) return;
            if (st == AccountService::EmailStep::signedIn) safe->start();
            else if (st == AccountService::EmailStep::emailSent) safe->showCheckEmail (false, e);
            else { safe->showSignUp (msg); if (auto* f = safe->field ("email")) f->setText (e); }
        });
    };
    email->onReturnKey = [pw] { pw->grabKeyboardFocus(); };
    pw->onReturnKey = [pw2] { pw2->grabKeyboardFocus(); };
    pw2->onReturnKey = create;
    addButton ("primary-create", "Create Account", true, create);
    addButton ("rowbtn-back", "I Already Have an Account", false, [safe] { if (safe != nullptr) safe->showSignIn ({}); });
    resized();
    juce::MessageManager::callAsync ([email] { if (email->isShowing()) email->grabKeyboardFocus(); });
}

void LicenseGate::showForgot (const juce::String& message)
{
    clearContent();
    setPage (Page::forgot, "Reset your password", "We'll email you a link. Open it on this computer while LiveDeck is open.");
    juce::Component::SafePointer<LicenseGate> safe (this);
    auto* email = addField ("email", "Email", false);
    addStatus (message);
    auto send = [safe]
    {
        if (safe == nullptr) return;
        const auto e = safe->field ("email")->getText().trim();
        if (! e.containsChar ('@')) { safe->showForgot ("Enter your email address."); return; }
        safe->clearContent();
        safe->setPage (Page::checking, "Sending...", {});
        safe->account.sendPasswordReset (e, [safe, e] (AccountService::EmailStep st, juce::String msg)
        {
            if (safe == nullptr) return;
            if (st == AccountService::EmailStep::emailSent) safe->showCheckEmail (true, e);
            else if (st == AccountService::EmailStep::signedIn) safe->showNewPassword();
            else safe->showForgot (msg);
        });
    };
    email->onReturnKey = send;
    addButton ("primary-send", "Email Me a Reset Link", true, send);
    addButton ("rowbtn-back", "Back to Sign In", false, [safe] { if (safe != nullptr) { safe->account.cancelSignIn(); safe->showSignIn ({}); } });
    resized();
    juce::MessageManager::callAsync ([email] { if (email->isShowing()) email->grabKeyboardFocus(); });
}

void LicenseGate::showCheckEmail (bool reset, const juce::String& email)
{
    clearContent();
    setPage (Page::checkEmail, "Check your email",
             reset ? "We sent a password reset link to " + email + ". Open it on this computer and keep LiveDeck open: "
                     "you'll be asked for a new password here."
                   : "We sent a confirmation link to " + email + ". Open it on this computer and keep LiveDeck open: "
                     "you'll be signed in automatically. (If LiveDeck was closed, just sign in with your email and password.)");
    juce::Component::SafePointer<LicenseGate> safe (this);
    addButton ("rowbtn-back", "Back to Sign In", false, [safe] { if (safe != nullptr) { safe->account.cancelSignIn(); safe->showSignIn ({}); } });
    resized();
}

void LicenseGate::showNewPassword (const juce::String& message)
{
    clearContent();
    setPage (Page::newPassword, "Choose a new password",
             "At least " + juce::String (AccountService::kMinPasswordLength) + " characters.");
    juce::Component::SafePointer<LicenseGate> safe (this);
    auto* pw = addField ("password", "New password", true);
    auto* pw2 = addField ("password2", "Repeat new password", true);
    addStatus (message);
    auto save = [safe]
    {
        if (safe == nullptr) return;
        const auto p = safe->field ("password")->getText();
        if (p.length() < AccountService::kMinPasswordLength) { safe->showNewPassword ("Use at least " + juce::String (AccountService::kMinPasswordLength) + " characters."); return; }
        if (p != safe->field ("password2")->getText()) { safe->showNewPassword ("The two passwords don't match."); return; }
        safe->clearContent();
        safe->setPage (Page::checking, "Saving your new password...", {});
        safe->account.setNewPassword (p, [safe] (bool ok, juce::String err)
        {
            if (safe == nullptr) return;
            if (ok) safe->start();
            else safe->showNewPassword (err);
        });
    };
    pw->onReturnKey = [pw2] { pw2->grabKeyboardFocus(); };
    pw2->onReturnKey = save;
    addButton ("primary-save", "Save Password", true, save);
    resized();
    juce::MessageManager::callAsync ([pw] { if (pw->isShowing()) pw->grabKeyboardFocus(); });
}

void LicenseGate::buy (const juce::String& plan, bool acceptFull)
{
    juce::Component::SafePointer<LicenseGate> safe (this);
    clearContent();
    setPage (Page::checking, "Opening checkout...", {});
    account.checkout (plan, acceptFull, [safe, plan] (const juce::var& r)
    {
        if (safe == nullptr) return;
        const auto st = str (r, "status");
        if (st == "ok")
        {
            juce::URL (str (r, "url")).launchInDefaultBrowser();
            safe->clearContent();
            safe->setPage (Page::waitingPayment, "Complete your purchase in the browser",
                           "Stripe's secure checkout has opened in your browser. LiveDeck unlocks automatically once the payment goes through.");
            auto* now = safe->own (new juce::TextButton ("Check Now"));
            now->setComponentID ("primary-0");
            stylePrimary (*now);
            now->onClick = [safe] { if (safe != nullptr) safe->start(); };
            auto* back = safe->own (new juce::TextButton ("Back to Plans"));
            back->setComponentID ("primary-1");
            back->onClick = [safe] { if (safe != nullptr) safe->start(); };
            for (auto* x : safe->content) safe->addAndMakeVisible (x);
            safe->resized();
            safe->pollCount = 0;
            safe->startTimer (5000);
            return;
        }
        if (st == "nonprofit_pending")
        {
            safe->clearContent();
            safe->setPage (Page::blocked, "Your non-profit status is still being verified",
                           "Once it's approved you'll get 10% off and no sales tax. You can wait, or buy now at the regular price.");
            auto* full = safe->own (new juce::TextButton ("Buy Now at the Regular Price"));
            full->setComponentID ("primary-0");
            stylePrimary (*full);
            full->onClick = [safe, plan] { if (safe != nullptr) safe->buy (plan, true); };
            auto* wait = safe->own (new juce::TextButton ("I'll Wait"));
            wait->setComponentID ("primary-1");
            wait->onClick = [safe] { if (safe != nullptr) safe->start(); };
            for (auto* x : safe->content) safe->addAndMakeVisible (x);
            safe->resized();
            return;
        }
        if (st == "already_licensed") { safe->start(); return; }
        AccountService::Check err = safe->lastCheck;
        err.status = "error";
        err.message = str (r, "message").isNotEmpty() ? str (r, "message") : juce::String ("Checkout couldn't start.");
        safe->showResult (err);
    });
}

void LicenseGate::timerCallback()
{
    // Waiting for Stripe: re-check quietly every 5 s for up to 20 minutes.
    if (++pollCount > 240) { stopTimer(); return; }
    juce::Component::SafePointer<LicenseGate> safe (this);
    account.check ([safe] (const AccountService::Check& c)
    {
        if (safe != nullptr && safe->page == Page::waitingPayment && c.status == "ok") safe->showResult (c);
    });
}

void LicenseGate::paint (juce::Graphics& g)
{
    g.fillAll (Theme::bg);
    g.setColour (Theme::panel);
    g.fillRect (card);

    auto r = card.reduced (36, 30);
    drawMark (g, r.removeFromTop (34).removeFromLeft (40).toFloat());
    r.removeFromTop (4);
    g.setColour (Theme::textDim);
    g.setFont (Theme::ui (12.0f, true));
    g.drawText ("LIVEDECK", r.removeFromTop (18), juce::Justification::centredLeft);
    r.removeFromTop (14);
    g.setColour (Theme::text);
    g.setFont (Theme::ui (22.0f, true));
    g.drawText (title, r.removeFromTop (30), juce::Justification::centredLeft, true);
    r.removeFromTop (8);
    if (body.isNotEmpty())
    {
        g.setColour (Theme::textDim);
        g.setFont (Theme::ui (14.0f));
        juce::AttributedString as;
        as.append (body, Theme::ui (14.0f), Theme::textDim);
        as.setWordWrap (juce::AttributedString::byWord);
        juce::TextLayout tl;
        tl.createLayout (as, (float) r.getWidth());
        tl.draw (g, r.removeFromTop ((int) tl.getHeight() + 2).toFloat());
    }

    if (page == Page::purchase)
        for (const auto& pr : planRects)
        {
            g.setColour (Theme::strip);
            g.fillRoundedRectangle (pr.toFloat(), Theme::radius);
        }
}

void LicenseGate::resized()
{
    const int w = juce::jmin (getWidth() - 40, page == Page::registration ? 640 : 600);
    int h = page == Page::checking ? 220 : 330;
    if (page == Page::signIn) h = 510;
    if (page == Page::signUp) h = 430;
    if (page == Page::forgot || page == Page::newPassword) h = 400;
    if (page == Page::checkEmail) h = 320;
    if (page == Page::registration) h = RegistrationForm::preferredHeight() + 200;
    if (page == Page::purchase) h = 560;
    card = juce::Rectangle<int> (w, juce::jmin (h, getHeight() - 40)).withCentre (getLocalBounds().getCentre());

    auto r = card.reduced (36, 30);
    r.removeFromTop (34 + 4 + 18 + 14 + 30 + 8);
    // body text height (same layout as paint)
    if (body.isNotEmpty())
    {
        juce::AttributedString as;
        as.append (body, Theme::ui (14.0f), Theme::textDim);
        as.setWordWrap (juce::AttributedString::byWord);
        juce::TextLayout tl;
        tl.createLayout (as, (float) r.getWidth());
        r.removeFromTop ((int) tl.getHeight() + 18);
    }

    auto footer = r.removeFromBottom (30);
    for (auto* c : content)
    {
        const auto id = c->getComponentID();
        if (id == "footer-label") c->setBounds (footer.removeFromLeft (footer.getWidth() - 230));
        if (id == "footer-admin") { c->setBounds (footer.removeFromRight (120)); footer.removeFromRight (6); }
        if (id == "footer-signout") { c->setBounds (footer.removeFromRight (90)); footer.removeFromRight (6); }
    }
    r.removeFromBottom (10);

    if (form != nullptr) { form->setBounds (r); return; }

    if (page == Page::purchase)
    {
        auto plans = r.removeFromTop (210);
        const int pw = (plans.getWidth() - 16) / 2;
        planRects[0] = plans.removeFromLeft (pw);
        plans.removeFromLeft (16);
        planRects[1] = plans;
        auto layoutPlan = [this] (const juce::String& id, juce::Rectangle<int> a)
        {
            a = a.reduced (18, 16);
            for (auto* c : content)
            {
                const auto cid = c->getComponentID();
                if (cid == "plan-name-" + id) c->setBounds (a.getX(), a.getY(), a.getWidth(), 22);
                if (cid == "plan-price-" + id) c->setBounds (a.getX(), a.getY() + 26, a.getWidth(), 30);
                if (cid == "plan-blurb-" + id) c->setBounds (a.getX(), a.getY() + 58, a.getWidth(), 64);
                if (cid == "plan-buy-" + id) c->setBounds (a.getX(), a.getBottom() - 34, a.getWidth(), 34);
            }
        };
        layoutPlan ("annual", planRects[0]);
        layoutPlan ("perpetual", planRects[1]);
        r.removeFromTop (22);
    }

    // Flow layout for the sign-in style pages: fields, status, primary buttons, a row of link buttons,
    // the "or" divider and alternative sign-in buttons, top to bottom.
    const int colW = juce::jmin (r.getWidth(), 340);
    int y = r.getY();
    int rowX = -1;
    auto endRow = [&] { if (rowX >= 0) { y += 34; rowX = -1; } };
    for (auto* c : content)
    {
        const auto id = c->getComponentID();
        if (id.startsWith ("footer-") || id.startsWith ("plan-")) continue;
        if (id.startsWith ("secondary-"))
        {
            if (id == "secondary-0") c->setBounds (r.getX(), r.getY() + 14, 290, 30);
            if (id == "secondary-1") c->setBounds (r.getX() + 300, r.getY() + 14, 160, 30);
            continue;
        }
        if (id.startsWith ("rowbtn-"))
        {
            if (rowX < 0) rowX = r.getX() - 8;
            const int bw = juce::roundToInt (juce::GlyphArrangement::getStringWidth (Theme::ui (13.0f, true), dynamic_cast<juce::Button*> (c)->getButtonText())) + 18;
            c->setBounds (rowX, y, bw, 28);
            rowX += bw + 4;
            continue;
        }
        endRow();
        if (id.startsWith ("field-")) { c->setBounds (r.getX(), y, colW, 36); y += 44; }
        else if (id == "status")      { c->setBounds (r.getX(), y - 4, colW + 120, 36); y += 34; }
        else if (id.startsWith ("primary-") || id.startsWith ("alt-")) { c->setBounds (r.getX(), y, colW, 36); y += 44; }
        else if (id == "divider")     { c->setBounds (r.getX(), y + 2, colW, 20); y += 28; }
    }
}

// ====================================================================================== AccountPanel

AccountPanel::AccountPanel (AccountService& a, std::function<void()> onSignOut, std::function<void()> onEdit, std::function<void()> onBuy)
    : account (a)
{
    for (auto* b : { &refresh, &edit, &buy, &signOut }) addAndMakeVisible (b);
    juce::Component::SafePointer<AccountPanel> safe (this);
    refresh.onClick = [safe]
    {
        if (safe == nullptr) return;
        safe->text = "Checking...";
        safe->repaint();
        safe->account.check ([safe] (const AccountService::Check&) { if (safe != nullptr) { safe->rebuildText(); safe->repaint(); } });
    };
    edit.onClick = [onEdit] { if (onEdit) onEdit(); };
    buy.onClick = [onBuy] { if (onBuy) onBuy(); };
    signOut.setColour (juce::TextButton::buttonColourId, Theme::red.withAlpha (0.85f));
    signOut.onClick = [onSignOut] { if (onSignOut) onSignOut(); };
    rebuildText();
    setSize (560, 380);
}

void AccountPanel::rebuildText()
{
    const auto& c = account.lastCheck();
    const auto& p = c.profile;
    juce::String t;
    t << str (p, "full_name") << "\n" << str (p, "company_name") << "\n" << account.email() << "\n";
    const auto addr = juce::StringArray { str (p, "address_line1"), str (p, "address_line2"),
                                          (str (p, "city") + " " + str (p, "region") + " " + str (p, "postal_code")).trim(), str (p, "country") };
    for (const auto& a : addr) if (a.trim().isNotEmpty()) t << a << "\n";
    t << "\n";
    if (str (p, "org_type") == "nonprofit") t << "Non-profit status: " << str (p, "nonprofit_status") << "\n";

    if (c.status == "ok" || c.status == "offline")
    {
        const auto& tok = c.token;
        t << "License: " << (tok.kind == "perpetual" ? "Perpetual" : "Annual")
          << (tok.expiresAt > 0 ? ", renews or expires " + niceDate ((double) tok.expiresAt) : juce::String()) << "\n";
        t << "Computers: up to " << tok.seats << " at a time on the same network\n";
        t << "This computer: " << license::machineName() << "\n";
        if (c.status == "offline") t << "\nOffline: " << license::daysLeft (tok, juce::Time::currentTimeMillis()) << " days left before LiveDeck needs to go online.\n";
        else t << "Verified online just now.\n";
    }
    else t << "License: " << (c.status.isNotEmpty() ? c.status : juce::String ("not checked yet")) << "\n";
    text = t;
}

void AccountPanel::paint (juce::Graphics& g)
{
    g.fillAll (Theme::panel);
    g.setColour (Theme::text);
    g.setFont (Theme::ui (14.0f));
    g.drawMultiLineText (text, 20, 34, getWidth() - 40);
}

void AccountPanel::resized()
{
    auto r = getLocalBounds().reduced (20).removeFromBottom (34);
    signOut.setBounds (r.removeFromRight (200));
    r.removeFromRight (8);
    buy.setBounds (r.removeFromLeft (130));
    r.removeFromLeft (8);
    edit.setBounds (r.removeFromLeft (150));
    r.removeFromLeft (8);
    refresh.setBounds (r.removeFromLeft (100));
}

// ====================================================================================== AdminPanel

AdminPanel::AdminPanel (AccountService& a) : account (a)
{
    for (auto* c : std::initializer_list<juce::Component*> { &search, &details, &list, &reloadButton, &stripeButton, &npLabel, &status, &npStatus,
                                                             &npSave, &licTitle, &newTitle, &licenseChoice, &newKind, &revoke, &reactivate,
                                                             &extend, &newSeats, &newExpires, &newNotes, &create, &machinesTitle,
                                                             &machineChoice, &releaseMachine })
        addAndMakeVisible (c);

    for (auto* e : { &search, &newSeats, &newExpires, &newNotes }) { e->setJustification (juce::Justification::centredLeft); e->setIndents (8, 0); }
    search.setTextToShowWhenEmpty ("Search name, company or email", Theme::textDim);
    search.onTextChange = [this] { applyFilter(); };
    details.setMultiLine (true);
    details.setReadOnly (true);
    details.setFont (Theme::ui (13.0f));
    list.setRowHeight (40);
    list.setColour (juce::ListBox::backgroundColourId, Theme::bg);
    status.setColour (juce::Label::textColourId, Theme::textDim);
    for (auto* l : { &npLabel, &licTitle, &newTitle, &machinesTitle }) l->setFont (Theme::ui (12.0f, true));

    npStatus.addItem ("none", 1);
    npStatus.addItem ("pending", 2);
    npStatus.addItem ("verified", 3);
    npStatus.addItem ("rejected", 4);
    newKind.addItem ("Annual", 1);
    newKind.addItem ("Perpetual", 2);
    newKind.setSelectedId (1);
    newKind.onChange = [this] { newExpires.setEnabled (newKind.getSelectedId() == 1); };
    newSeats.setText ("2");
    newSeats.setInputRestrictions (2, "0123456789");
    newExpires.setText ((juce::Time::getCurrentTime() + juce::RelativeTime::days (365)).formatted ("%Y-%m-%d"));
    newExpires.setTextToShowWhenEmpty ("YYYY-MM-DD", Theme::textDim);
    newNotes.setTextToShowWhenEmpty ("Notes (invoice number, reason...)", Theme::textDim);
    stylePrimary (create);

    reloadButton.onClick = [this] { reload(); };
    stripeButton.onClick = [this]
    {
        status.setText ("Registering the Stripe webhook...", juce::dontSendNotification);
        juce::Component::SafePointer<AdminPanel> safe (this);
        account.callLicenseFunction (obj ({ { "action", "stripe-setup" } }), [safe] (int, const juce::var& r)
        {
            if (safe != nullptr) safe->status.setText (str (r, "message").isNotEmpty() ? str (r, "message") : juce::String ("Stripe setup failed."), juce::dontSendNotification);
        });
    };
    npSave.onClick = [this]
    {
        const auto u = selectedUser();
        if (! u.isObject()) return;
        juce::Component::SafePointer<AdminPanel> safe (this);
        account.rest ("PATCH", "profiles?id=eq." + str (u, "id"), obj ({ { "nonprofit_status", npStatus.getText() } }),
                      [safe] (int st, const juce::var&)
        {
            if (safe == nullptr) return;
            safe->status.setText (st >= 200 && st < 300 ? "Non-profit status saved." : "Couldn't save (error " + juce::String (st) + ").", juce::dontSendNotification);
            safe->reload();
        });
    };
    revoke.onClick = [this] { patchLicense (licenseChoice.getItemText (licenseChoice.getSelectedItemIndex()).fromLastOccurrenceOf ("#", false, false), obj ({ { "status", "revoked" } }), "revoked"); };
    reactivate.onClick = [this] { patchLicense (licenseChoice.getItemText (licenseChoice.getSelectedItemIndex()).fromLastOccurrenceOf ("#", false, false), obj ({ { "status", "active" } }), "reactivated"); };
    extend.onClick = [this]
    {
        const auto id = licenseChoice.getItemText (licenseChoice.getSelectedItemIndex()).fromLastOccurrenceOf ("#", false, false);
        const auto u = selectedUser();
        if (auto* lics = u.getProperty ("licenses", {}).getArray())
            for (const auto& l : *lics)
                if (str (l, "id") == id)
                {
                    auto base = str (l, "expires_at").isNotEmpty() ? juce::Time::fromISO8601 (str (l, "expires_at")) : juce::Time::getCurrentTime();
                    if (base < juce::Time::getCurrentTime()) base = juce::Time::getCurrentTime();
                    patchLicense (id, obj ({ { "expires_at", (base + juce::RelativeTime::days (365)).toISO8601 (true) }, { "status", "active" } }), "extended by a year");
                }
    };
    create.onClick = [this]
    {
        const auto u = selectedUser();
        if (! u.isObject()) { status.setText ("Pick a user first.", juce::dontSendNotification); return; }
        const bool annual = newKind.getSelectedId() == 1;
        juce::var expires;
        if (annual)
        {
            const auto t = juce::Time::fromISO8601 (newExpires.getText().trim() + "T23:59:59Z");
            if (t.toMilliseconds() <= 0) { status.setText ("Enter the expiry date as YYYY-MM-DD.", juce::dontSendNotification); return; }
            expires = t.toISO8601 (true);
        }
        const bool np = str (u, "org_type") == "nonprofit" && str (u, "nonprofit_status") == "verified";
        auto body = obj ({ { "user_id", str (u, "id") }, { "kind", annual ? "annual" : "perpetual" },
                           { "seats", juce::jlimit (1, 50, newSeats.getText().getIntValue()) }, { "expires_at", expires },
                           { "source", "admin" }, { "discount_pct", np ? 10 : 0 }, { "tax_exempt", np },
                           { "price_cents", juce::roundToInt ((annual ? 5000 : 12900) * (np ? 0.9 : 1.0)) },
                           { "notes", newNotes.getText().trim() } });
        juce::Component::SafePointer<AdminPanel> safe (this);
        account.rest ("POST", "licenses", body, [safe] (int st, const juce::var& r)
        {
            if (safe == nullptr) return;
            safe->status.setText (st >= 200 && st < 300 ? "License created." : "Couldn't create the license: " + str (r, "message"), juce::dontSendNotification);
            safe->reload();
        });
    };
    releaseMachine.onClick = [this]
    {
        const auto id = machineChoice.getItemText (machineChoice.getSelectedItemIndex()).fromLastOccurrenceOf ("#", false, false);
        if (id.isEmpty()) return;
        juce::Component::SafePointer<AdminPanel> safe (this);
        account.rest ("DELETE", "activations?id=eq." + id, {}, [safe] (int st, const juce::var&)
        {
            if (safe == nullptr) return;
            safe->status.setText (st >= 200 && st < 300 ? "Computer released." : "Couldn't release it.", juce::dontSendNotification);
            safe->reload();
        });
    };

    setSize (1000, 680);
    reload();
}

void AdminPanel::reload()
{
    status.setText ("Loading...", juce::dontSendNotification);
    const auto keepId = str (selectedUser(), "id");
    juce::Component::SafePointer<AdminPanel> safe (this);
    account.rest ("GET", "profiles?select=*,licenses(*),activations(*)&order=created_at.desc", {}, [safe, keepId] (int st, const juce::var& r)
    {
        if (safe == nullptr) return;
        if (st != 200 || ! r.isArray()) { safe->status.setText ("Couldn't load users (error " + juce::String (st) + ").", juce::dontSendNotification); return; }
        safe->users = r;
        safe->applyFilter (keepId);
        safe->status.setText (juce::String (r.size()) + " users. Users appear here after they sign in to LiveDeck once.", juce::dontSendNotification);
    });
}

void AdminPanel::applyFilter (const juce::String& keepId)
{
    const auto keep = keepId.isNotEmpty() ? keepId : str (selectedUser(), "id");
    visible.clear();
    const auto q = search.getText().trim().toLowerCase();
    for (int i = 0; i < users.size(); ++i)
    {
        const auto& u = users[i];
        const auto hay = (str (u, "email") + " " + str (u, "full_name") + " " + str (u, "company_name")).toLowerCase();
        if (q.isEmpty() || hay.contains (q)) visible.add (i);
    }
    list.updateContent();
    list.deselectAllRows();
    for (int row = 0; row < visible.size(); ++row)
        if (str (users[visible[row]], "id") == keep) { list.selectRow (row); showUser (row); list.repaint(); return; }
    showUser (-1);
    list.repaint();
}

int AdminPanel::getNumRows() { return visible.size(); }

juce::var AdminPanel::selectedUser() const
{
    const int row = list.getSelectedRow();
    return row >= 0 && row < visible.size() ? users[visible[row]] : juce::var();
}

void AdminPanel::paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected)
{
    if (row < 0 || row >= visible.size()) return;
    const auto& u = users[visible[row]];
    if (selected) { g.setColour (Theme::blue.withAlpha (0.30f)); g.fillRect (0, 0, w, h); }
    g.setColour (Theme::text);
    g.setFont (Theme::ui (13.0f, true));
    const auto name = str (u, "company_name").isNotEmpty() ? str (u, "company_name") : str (u, "email");
    g.drawText (name, 10, 3, w - 20, 18, juce::Justification::centredLeft, true);
    int active = 0;
    if (auto* l = u.getProperty ("licenses", {}).getArray())
        for (const auto& x : *l)
            if (str (x, "status") == "active" && (str (x, "expires_at").isEmpty() || juce::Time::fromISO8601 (str (x, "expires_at")) > juce::Time::getCurrentTime()))
                ++active;
    g.setColour (Theme::textDim);
    g.setFont (Theme::ui (11.0f));
    juce::String sub = str (u, "email");
    if (active > 0) sub << "  -  licensed";
    if (str (u, "nonprofit_status") == "pending") sub << "  -  non-profit to verify";
    if (! (bool) u.getProperty ("registered_at", juce::var()).toString().isNotEmpty()) sub << "  -  not registered";
    g.drawText (sub, 10, 21, w - 20, 16, juce::Justification::centredLeft, true);
    g.setColour (Theme::edge);
    g.fillRect (0, h - 1, w, 1);
}

void AdminPanel::selectedRowsChanged (int row) { showUser (row); }

void AdminPanel::showUser (int row)
{
    licenseChoice.clear (juce::dontSendNotification);
    machineChoice.clear (juce::dontSendNotification);
    if (row < 0 || row >= visible.size()) { details.setText ("Select a user."); return; }
    const auto& u = users[visible[row]];
    juce::String t;
    t << str (u, "full_name") << "\n" << str (u, "company_name") << "\n" << str (u, "email") << "\n" << str (u, "phone") << "\n"
      << str (u, "address_line1") << " " << str (u, "address_line2") << "\n"
      << str (u, "city") << " " << str (u, "region") << " " << str (u, "postal_code") << " " << str (u, "country") << "\n\n"
      << "Organization: " << str (u, "org_type") << (str (u, "tax_id").isNotEmpty() ? "   Tax ID: " + str (u, "tax_id") : juce::String()) << "\n"
      << "Registered: " << (str (u, "registered_at").isNotEmpty() ? niceDate (u["registered_at"]) : juce::String ("not yet")) << "\n"
      << "Admin: " << ((bool) u.getProperty ("is_admin", false) ? "yes" : "no") << "\n";
    details.setText (t);
    const auto np = str (u, "nonprofit_status");
    npStatus.setSelectedId (np == "pending" ? 2 : np == "verified" ? 3 : np == "rejected" ? 4 : 1, juce::dontSendNotification);

    int id = 1;
    if (auto* l = u.getProperty ("licenses", {}).getArray())
        for (const auto& x : *l)
            licenseChoice.addItem ((str (x, "kind") == "perpetual" ? "Perpetual" : "Annual") + juce::String (", ") + str (x, "status")
                                   + (str (x, "expires_at").isNotEmpty() ? ", until " + niceDate (x["expires_at"]) : juce::String())
                                   + ", " + str (x, "seats") + " seats, " + str (x, "source") + "   #" + str (x, "id"), id++);
    if (licenseChoice.getNumItems() > 0) licenseChoice.setSelectedItemIndex (0, juce::dontSendNotification);
    else licenseChoice.setTextWhenNoChoicesAvailable ("No licenses yet");

    id = 1;
    if (auto* acts = u.getProperty ("activations", {}).getArray())
        for (const auto& x : *acts)
        {
            const auto seen = juce::Time::fromISO8601 (str (x, "last_seen"));
            const bool live = (juce::Time::getCurrentTime() - seen).inMinutes() < 10;
            machineChoice.addItem (str (x, "machine_name") + (live ? "  (in use)" : "  (last seen " + niceDate (x["last_seen"]) + ")")
                                   + "  " + str (x, "public_ip") + "  v" + str (x, "app_version") + "   #" + str (x, "id"), id++);
        }
    if (machineChoice.getNumItems() > 0) machineChoice.setSelectedItemIndex (0, juce::dontSendNotification);
    else machineChoice.setTextWhenNoChoicesAvailable ("No computers yet");
}

void AdminPanel::patchLicense (const juce::String& id, const juce::var& fields, const juce::String& what)
{
    if (id.isEmpty()) return;
    juce::Component::SafePointer<AdminPanel> safe (this);
    account.rest ("PATCH", "licenses?id=eq." + id, fields, [safe, what] (int st, const juce::var& r)
    {
        if (safe == nullptr) return;
        safe->status.setText (st >= 200 && st < 300 ? "License " + what + "." : "Couldn't change the license: " + str (r, "message"), juce::dontSendNotification);
        safe->reload();
    });
}

void AdminPanel::paint (juce::Graphics& g)
{
    g.fillAll (Theme::panel);
}

void AdminPanel::resized()
{
    auto r = getLocalBounds().reduced (16);
    auto top = r.removeFromTop (30);
    stripeButton.setBounds (top.removeFromRight (190));
    top.removeFromRight (8);
    reloadButton.setBounds (top.removeFromRight (90));
    top.removeFromRight (8);
    search.setBounds (top.removeFromLeft (320));
    r.removeFromTop (6);
    status.setBounds (r.removeFromBottom (22));
    r.removeFromBottom (6);

    list.setBounds (r.removeFromLeft (320));
    r.removeFromLeft (16);
    details.setBounds (r.removeFromTop (200));
    r.removeFromTop (10);
    auto row = [&r] (int h) { auto x = r.removeFromTop (h); r.removeFromTop (6); return x; };
    { auto x = row (28); npLabel.setBounds (x.removeFromLeft (130)); npStatus.setBounds (x.removeFromLeft (140)); x.removeFromLeft (8); npSave.setBounds (x.removeFromLeft (110)); }
    r.removeFromTop (6);
    licTitle.setBounds (row (18));
    licenseChoice.setBounds (row (28));
    { auto x = row (28); revoke.setBounds (x.removeFromLeft (90)); x.removeFromLeft (8); reactivate.setBounds (x.removeFromLeft (110)); x.removeFromLeft (8); extend.setBounds (x.removeFromLeft (130)); }
    r.removeFromTop (6);
    newTitle.setBounds (row (18));
    { auto x = row (28); newKind.setBounds (x.removeFromLeft (120)); x.removeFromLeft (8); newSeats.setBounds (x.removeFromLeft (50)); x.removeFromLeft (8);
      newExpires.setBounds (x.removeFromLeft (120)); x.removeFromLeft (8); create.setBounds (x.removeFromRight (140)); x.removeFromRight (8); newNotes.setBounds (x); }
    r.removeFromTop (6);
    machinesTitle.setBounds (row (18));
    { auto x = row (28); releaseMachine.setBounds (x.removeFromRight (150)); x.removeFromRight (8); machineChoice.setBounds (x); }
}
