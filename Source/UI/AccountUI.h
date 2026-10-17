#pragma once
// Account screens: the license gate shown at launch, the registration form, the account window
// and the admin panel.

#include "Account.h"
#include "Theme.h"

// Company / address form (registration and later edits).
class RegistrationForm final : public juce::Component
{
public:
    RegistrationForm (AccountService& account, const juce::var& profile, bool showCancel);
    std::function<void()> onSaved, onCancel;
    void resized() override;
    void paint (juce::Graphics&) override;
    static int preferredHeight() { return 560; }

private:
    void save();
    void updateNonprofit();
    juce::String countryCode() const;

    AccountService& account;
    struct Field { std::unique_ptr<juce::Label> label; std::unique_ptr<juce::TextEditor> editor; bool required = false; };
    Field fullName, company, phone, addr1, addr2, city, region, postal, taxId;
    juce::Label countryLabel { {}, "Country *" }, orgLabel { {}, "Organization type *" }, emailLabel, note, status;
    juce::ComboBox country, orgType;
    juce::TextEditor countryOther;
    juce::TextButton saveButton { "Save and Continue" }, cancelButton { "Cancel" };
    Field* fields[9] = { &fullName, &company, &phone, &addr1, &addr2, &city, &region, &postal, &taxId };
};

// Covers the main window until this computer is licensed.
class LicenseGate final : public juce::Component, private juce::Timer
{
public:
    explicit LicenseGate (AccountService& account);
    ~LicenseGate() override;

    std::function<void (const AccountService::Check&)> onLicensed;   // ok, or a valid offline license
    std::function<void()> onOpenAdmin;

    void start();                                   // runs the license check
    void showResult (const AccountService::Check&);
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    enum class Page { checking, signIn, signUp, forgot, checkEmail, newPassword, waitingBrowser, registration, purchase, waitingPayment, blocked };
    void showSignIn (const juce::String& message);
    void showSignUp (const juce::String& message = {});
    void showForgot (const juce::String& message = {});
    void showCheckEmail (bool reset, const juce::String& email);
    void showNewPassword (const juce::String& message = {});
    void signInWithProvider (AccountService::Provider p);
    juce::TextEditor* addField (const juce::String& id, const juce::String& hint, bool password);
    juce::TextButton* addButton (const juce::String& id, const juce::String& text, bool primary, std::function<void()> onClick);
    void addStatus (const juce::String& message);
    juce::TextEditor* field (const juce::String& id) const;
    void timerCallback() override;
    void setPage (Page p, const juce::String& title, const juce::String& body);
    void clearContent();
    void buy (const juce::String& plan, bool acceptFull);
    void addFooter (bool withAdmin);
    template <typename T> T* own (T* c) { content.add (c); return c; }

    AccountService& account;
    Page page = Page::checking;
    juce::String title, body;
    AccountService::Check lastCheck;
    juce::OwnedArray<juce::Component> content;
    std::unique_ptr<RegistrationForm> form;
    juce::Rectangle<int> card, planRects[2];
    int pollCount = 0;
};

// Account > Account & License...
class AccountPanel final : public juce::Component
{
public:
    AccountPanel (AccountService& account, std::function<void()> onSignOut, std::function<void()> onEditRegistration,
                  std::function<void()> onBuy);
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    AccountService& account;
    juce::TextButton refresh { "Check Now" }, edit { "Edit Registration..." }, buy { "Buy or Renew..." }, signOut { "Sign Out of This Computer" };
    juce::String text;
    void rebuildText();
};

// Account > Admin Panel... (admins only; the server enforces it too)
class AdminPanel final : public juce::Component, private juce::ListBoxModel
{
public:
    explicit AdminPanel (AccountService& account);
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    int getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics&, int w, int h, bool selected) override;
    void selectedRowsChanged (int lastRowSelected) override;

    void reload();
    void applyFilter (const juce::String& keepId = {});
    void showUser (int index);
    void patchLicense (const juce::String& id, const juce::var& fields, const juce::String& what);
    juce::var selectedUser() const;

    AccountService& account;
    juce::var users;                     // profiles with embedded licenses + activations
    juce::Array<int> visible;            // indices into users after the search filter
    juce::TextEditor search, details;
    juce::ListBox list { "users", this };
    juce::TextButton reloadButton { "Refresh" }, stripeButton { "Set Up Stripe Webhook" };
    juce::Label npLabel { {}, "Non-profit status" }, status;
    juce::ComboBox npStatus;
    juce::TextButton npSave { "Save Status" };

    juce::Label licTitle { {}, "Licenses" }, newTitle { {}, "Create a license" };
    juce::ComboBox licenseChoice, newKind;
    juce::TextButton revoke { "Revoke" }, reactivate { "Reactivate" }, extend { "Extend 1 Year" };
    juce::TextEditor newSeats, newExpires, newNotes;
    juce::TextButton create { "Create License" };
    juce::Label machinesTitle { {}, "Computers using a license" };
    juce::ComboBox machineChoice;
    juce::TextButton releaseMachine { "Release Computer" };
};
