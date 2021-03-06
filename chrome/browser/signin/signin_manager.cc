// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/signin/signin_manager.h"

#include <string>
#include <vector>

#include "base/prefs/pref_service.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "chrome/browser/chrome_notification_types.h"
#include "chrome/browser/profiles/profile_io_data.h"
#include "chrome/browser/signin/local_auth.h"
#include "chrome/browser/signin/profile_oauth2_token_service_factory.h"
#include "chrome/browser/signin/signin_account_id_helper.h"
#include "chrome/common/pref_names.h"
#include "chrome/common/profile_management_switches.h"
#include "components/signin/core/browser/profile_oauth2_token_service.h"
#include "components/signin/core/browser/signin_client.h"
#include "components/signin/core/browser/signin_internals_util.h"
#include "components/signin/core/browser/signin_manager_cookie_helper.h"
#include "content/public/browser/notification_service.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/common/child_process_host.h"
#include "google_apis/gaia/gaia_auth_util.h"
#include "google_apis/gaia/gaia_urls.h"
#include "net/base/escape.h"
#include "third_party/icu/source/i18n/unicode/regex.h"

using namespace signin_internals_util;

using content::ChildProcessHost;
using content::RenderProcessHost;

namespace {

const char kChromiumSyncService[] = "service=chromiumsync";

}  // namespace

// Under the covers, we use a dummy chrome-extension ID to serve the purposes
// outlined in the .h file comment for this string.
const char SigninManager::kChromeSigninEffectiveSite[] =
    "chrome-extension://acfccoigjajmmgbhpfbjnpckhjjegnih";

// static
bool SigninManager::IsWebBasedSigninFlowURL(const GURL& url) {
  GURL effective(kChromeSigninEffectiveSite);
  if (url.SchemeIs(effective.scheme().c_str()) &&
      url.host() == effective.host()) {
    return true;
  }

  GURL service_login(GaiaUrls::GetInstance()->service_login_url());
  if (url.GetOrigin() != service_login.GetOrigin())
    return false;

  // Any login UI URLs with signin=chromiumsync should be considered a web
  // URL (relies on GAIA keeping the "service=chromiumsync" query string
  // fragment present even when embedding inside a "continue" parameter).
  return net::UnescapeURLComponent(
      url.query(), net::UnescapeRule::URL_SPECIAL_CHARS)
          .find(kChromiumSyncService) != std::string::npos;
}

SigninManager::SigninManager(SigninClient* client)
    : SigninManagerBase(client),
      profile_(NULL),
      prohibit_signout_(false),
      type_(SIGNIN_TYPE_NONE),
      weak_pointer_factory_(this),
      signin_host_id_(ChildProcessHost::kInvalidUniqueID),
      client_(client) {}

void SigninManager::SetSigninProcess(int process_id) {
  if (process_id == signin_host_id_)
    return;
  DLOG_IF(WARNING,
          signin_host_id_ != ChildProcessHost::kInvalidUniqueID)
      << "Replacing in-use signin process.";
  signin_host_id_ = process_id;
  RenderProcessHost* host = RenderProcessHost::FromID(process_id);
  DCHECK(host);
  host->AddObserver(this);
  signin_hosts_observed_.insert(host);
}

void SigninManager::ClearSigninProcess() {
  signin_host_id_ = ChildProcessHost::kInvalidUniqueID;
}

bool SigninManager::IsSigninProcess(int process_id) const {
  return process_id == signin_host_id_;
}

bool SigninManager::HasSigninProcess() const {
  return signin_host_id_ != ChildProcessHost::kInvalidUniqueID;
}

void SigninManager::AddMergeSessionObserver(
    MergeSessionHelper::Observer* observer) {
  if (merge_session_helper_)
    merge_session_helper_->AddObserver(observer);
}

void SigninManager::RemoveMergeSessionObserver(
    MergeSessionHelper::Observer* observer) {
  if (merge_session_helper_)
    merge_session_helper_->RemoveObserver(observer);
}

SigninManager::~SigninManager() {
  std::set<RenderProcessHost*>::iterator i;
  for (i = signin_hosts_observed_.begin();
       i != signin_hosts_observed_.end();
       ++i) {
    (*i)->RemoveObserver(this);
  }
}

void SigninManager::InitTokenService() {
  ProfileOAuth2TokenService* token_service =
      ProfileOAuth2TokenServiceFactory::GetForProfile(profile_);
  const std::string& account_id = GetAuthenticatedUsername();
  if (token_service && !account_id.empty())
    token_service->LoadCredentials(account_id);
}

std::string SigninManager::SigninTypeToString(
    SigninManager::SigninType type) {
  switch (type) {
    case SIGNIN_TYPE_NONE:
      return "No Signin";
    case SIGNIN_TYPE_WITH_REFRESH_TOKEN:
      return "Signin with refresh token";
  }

  NOTREACHED();
  return std::string();
}

bool SigninManager::PrepareForSignin(SigninType type,
                                     const std::string& username,
                                     const std::string& password) {
  DCHECK(possibly_invalid_username_.empty() ||
         possibly_invalid_username_ == username);
  DCHECK(!username.empty());

  if (!IsAllowedUsername(username)) {
    // Account is not allowed by admin policy.
    HandleAuthError(GoogleServiceAuthError(
        GoogleServiceAuthError::ACCOUNT_DISABLED));
    return false;
  }

  // This attempt is either 1) the user trying to establish initial sync, or
  // 2) trying to refresh credentials for an existing username.  If it is 2, we
  // need to try again, but take care to leave state around tracking that the
  // user has successfully signed in once before with this username, so that on
  // restart we don't think sync setup has never completed.
  ClearTransientSigninData();
  type_ = type;
  possibly_invalid_username_.assign(username);
  password_.assign(password);
  NotifyDiagnosticsObservers(SIGNIN_TYPE, SigninTypeToString(type));
  return true;
}

void SigninManager::StartSignInWithRefreshToken(
    const std::string& refresh_token,
    const std::string& username,
    const std::string& password,
    const OAuthTokenFetchedCallback& callback) {
  DCHECK(GetAuthenticatedUsername().empty() ||
         gaia::AreEmailsSame(username, GetAuthenticatedUsername()));

  if (!PrepareForSignin(SIGNIN_TYPE_WITH_REFRESH_TOKEN, username, password))
    return;

  // Store our callback and token.
  temp_refresh_token_ = refresh_token;
  possibly_invalid_username_ = username;

  NotifyDiagnosticsObservers(GET_USER_INFO_STATUS, "Successful");

  if (!callback.is_null() && !temp_refresh_token_.empty()) {
    callback.Run(temp_refresh_token_);
  } else {
    // No oauth token or callback, so just complete our pending signin.
    CompletePendingSignin();
  }
}

void SigninManager::CopyCredentialsFrom(const SigninManager& source) {
  DCHECK_NE(this, &source);
  possibly_invalid_username_ = source.possibly_invalid_username_;
  temp_refresh_token_ = source.temp_refresh_token_;
}

void SigninManager::ClearTransientSigninData() {
  DCHECK(IsInitialized());

  possibly_invalid_username_.clear();
  password_.clear();
  type_ = SIGNIN_TYPE_NONE;
  temp_refresh_token_.clear();
}

void SigninManager::HandleAuthError(const GoogleServiceAuthError& error) {
  ClearTransientSigninData();

  // TODO(blundell): Eliminate this notification send once crbug.com/333997 is
  // fixed.
  content::NotificationService::current()->Notify(
      chrome::NOTIFICATION_GOOGLE_SIGNIN_FAILED,
      content::Source<Profile>(profile_),
      content::Details<const GoogleServiceAuthError>(&error));

  FOR_EACH_OBSERVER(Observer, observer_list_, GoogleSigninFailed(error));
}

void SigninManager::SignOut() {
  DCHECK(IsInitialized());

  if (GetAuthenticatedUsername().empty()) {
    if (AuthInProgress()) {
      // If the user is in the process of signing in, then treat a call to
      // SignOut as a cancellation request.
      GoogleServiceAuthError error(GoogleServiceAuthError::REQUEST_CANCELED);
      HandleAuthError(error);
    } else {
      // Clean up our transient data and exit if we aren't signed in.
      // This avoids a perf regression from clearing out the TokenDB if
      // SignOut() is invoked on startup to clean up any incomplete previous
      // signin attempts.
      ClearTransientSigninData();
    }
    return;
  }

  if (prohibit_signout_) {
    DVLOG(1) << "Ignoring attempt to sign out while signout is prohibited";
    return;
  }

  ClearTransientSigninData();

  const std::string& username = GetAuthenticatedUsername();
  clear_authenticated_username();
  profile_->GetPrefs()->ClearPref(prefs::kGoogleServicesUsername);

  // Erase (now) stale information from AboutSigninInternals.
  NotifyDiagnosticsObservers(USERNAME, "");

  // Revoke all tokens before sending signed_out notification, because there
  // may be components that don't listen for token service events when the
  // profile is not connected to an account.
  ProfileOAuth2TokenService* token_service =
      ProfileOAuth2TokenServiceFactory::GetForProfile(profile_);
  LOG(WARNING) << "Revoking refresh token on server. Reason: sign out, "
               << "IsSigninAllowed: " << IsSigninAllowed();
  token_service->RevokeAllCredentials();

  // TODO(blundell): Eliminate this notification send once crbug.com/333997 is
  // fixed.
  GoogleServiceSignoutDetails details(username);
  content::NotificationService::current()->Notify(
      chrome::NOTIFICATION_GOOGLE_SIGNED_OUT,
      content::Source<Profile>(profile_),
      content::Details<const GoogleServiceSignoutDetails>(&details));

  FOR_EACH_OBSERVER(Observer, observer_list_, GoogleSignedOut(username));
}

void SigninManager::Initialize(Profile* profile, PrefService* local_state) {
  profile_ = profile;
  SigninManagerBase::Initialize(profile, local_state);

  // local_state can be null during unit tests.
  if (local_state) {
    local_state_pref_registrar_.Init(local_state);
    local_state_pref_registrar_.Add(
        prefs::kGoogleServicesUsernamePattern,
        base::Bind(&SigninManager::OnGoogleServicesUsernamePatternChanged,
                   weak_pointer_factory_.GetWeakPtr()));
  }
  signin_allowed_.Init(prefs::kSigninAllowed, profile_->GetPrefs(),
      base::Bind(&SigninManager::OnSigninAllowedPrefChanged,
                 base::Unretained(this)));

  std::string user = profile_->GetPrefs()->GetString(
      prefs::kGoogleServicesUsername);
  if ((!user.empty() && !IsAllowedUsername(user)) || !IsSigninAllowed()) {
    // User is signed in, but the username is invalid - the administrator must
    // have changed the policy since the last signin, so sign out the user.
    SignOut();
  }

  InitTokenService();
  account_id_helper_.reset(new SigninAccountIdHelper(profile_, this));
}

void SigninManager::Shutdown() {
  if (merge_session_helper_)
    merge_session_helper_->CancelAll();

  local_state_pref_registrar_.RemoveAll();
  account_id_helper_.reset();
  SigninManagerBase::Shutdown();
}

void SigninManager::OnGoogleServicesUsernamePatternChanged() {
  if (!GetAuthenticatedUsername().empty() &&
      !IsAllowedUsername(GetAuthenticatedUsername())) {
    // Signed in user is invalid according to the current policy so sign
    // the user out.
    SignOut();
  }
}

bool SigninManager::IsSigninAllowed() const {
  return signin_allowed_.GetValue();
}

void SigninManager::OnSigninAllowedPrefChanged() {
  if (!IsSigninAllowed())
    SignOut();
}

// static
bool SigninManager::IsUsernameAllowedByPolicy(const std::string& username,
                                              const std::string& policy) {
  if (policy.empty())
    return true;

  // Patterns like "*@foo.com" are not accepted by our regex engine (since they
  // are not valid regular expressions - they should instead be ".*@foo.com").
  // For convenience, detect these patterns and insert a "." character at the
  // front.
  base::string16 pattern = base::UTF8ToUTF16(policy);
  if (pattern[0] == L'*')
    pattern.insert(pattern.begin(), L'.');

  // See if the username matches the policy-provided pattern.
  UErrorCode status = U_ZERO_ERROR;
  const icu::UnicodeString icu_pattern(pattern.data(), pattern.length());
  icu::RegexMatcher matcher(icu_pattern, UREGEX_CASE_INSENSITIVE, status);
  if (!U_SUCCESS(status)) {
    LOG(ERROR) << "Invalid login regex: " << pattern << ", status: " << status;
    // If an invalid pattern is provided, then prohibit *all* logins (better to
    // break signin than to quietly allow users to sign in).
    return false;
  }
  base::string16 username16 = base::UTF8ToUTF16(username);
  icu::UnicodeString icu_input(username16.data(), username16.length());
  matcher.reset(icu_input);
  status = U_ZERO_ERROR;
  UBool match = matcher.matches(status);
  DCHECK(U_SUCCESS(status));
  return !!match;  // !! == convert from UBool to bool.
}

bool SigninManager::IsAllowedUsername(const std::string& username) const {
  const PrefService* local_state = local_state_pref_registrar_.prefs();
  if (!local_state)
    return true;  // In a unit test with no local state - all names are allowed.

  std::string pattern = local_state->GetString(
      prefs::kGoogleServicesUsernamePattern);
  return IsUsernameAllowedByPolicy(username, pattern);
}

bool SigninManager::AuthInProgress() const {
  return !possibly_invalid_username_.empty();
}

const std::string& SigninManager::GetUsernameForAuthInProgress() const {
  return possibly_invalid_username_;
}

void SigninManager::DisableOneClickSignIn(Profile* profile) {
  PrefService* pref_service = profile->GetPrefs();
  pref_service->SetBoolean(prefs::kReverseAutologinEnabled, false);
}

void SigninManager::CompletePendingSignin() {
  DCHECK(!possibly_invalid_username_.empty());
  OnSignedIn(possibly_invalid_username_);

  ProfileOAuth2TokenService* token_service =
      ProfileOAuth2TokenServiceFactory::GetForProfile(profile_);

  // If inline sign in is enabled, but new profile management is not, perform a
  // merge session now to push the user's credentials into the cookie jar.
  bool do_merge_session_in_signin_manager =
      !switches::IsEnableWebBasedSignin() &&
      !switches::IsNewProfileManagement();

  if (do_merge_session_in_signin_manager) {
    merge_session_helper_.reset(new MergeSessionHelper(
        token_service, profile_->GetRequestContext(), NULL));
  }

  DCHECK(!temp_refresh_token_.empty());
  DCHECK(!GetAuthenticatedUsername().empty());
  token_service->UpdateCredentials(GetAuthenticatedUsername(),
                                   temp_refresh_token_);
  temp_refresh_token_.clear();

  if (do_merge_session_in_signin_manager)
    merge_session_helper_->LogIn(GetAuthenticatedUsername());
}

void SigninManager::OnExternalSigninCompleted(const std::string& username) {
  OnSignedIn(username);
}

void SigninManager::OnSignedIn(const std::string& username) {
  SetAuthenticatedUsername(username);
  possibly_invalid_username_.clear();

  // TODO(blundell): Eliminate this notification send once crbug.com/333997 is
  // fixed.
  GoogleServiceSigninSuccessDetails details(GetAuthenticatedUsername(),
                                            password_);
  content::NotificationService::current()->Notify(
      chrome::NOTIFICATION_GOOGLE_SIGNIN_SUCCESSFUL,
      content::Source<Profile>(profile_),
      content::Details<const GoogleServiceSigninSuccessDetails>(&details));

  FOR_EACH_OBSERVER(Observer, observer_list_,
                    GoogleSigninSucceeded(GetAuthenticatedUsername(),
                                          password_));

#if !defined(OS_ANDROID)
  // Don't store password hash except for users of new profile features.
  if (switches::IsNewProfileManagement())
    chrome::SetLocalAuthCredentials(profile_, password_);
#endif

  password_.clear();  // Don't need it anymore.
  DisableOneClickSignIn(profile_);  // Don't ever offer again.
}

void SigninManager::RenderProcessHostDestroyed(RenderProcessHost* host) {
  // It's possible we're listening to a "stale" renderer because it was replaced
  // with a new process by process-per-site. In either case, stop observing it,
  // but only reset signin_host_id_ tracking if this was from the current signin
  // process.
  signin_hosts_observed_.erase(host);
  if (signin_host_id_ == host->GetID())
    signin_host_id_ = ChildProcessHost::kInvalidUniqueID;
}

void SigninManager::ProhibitSignout(bool prohibit_signout) {
  prohibit_signout_ = prohibit_signout;
}

bool SigninManager::IsSignoutProhibited() const {
  return prohibit_signout_;
}
