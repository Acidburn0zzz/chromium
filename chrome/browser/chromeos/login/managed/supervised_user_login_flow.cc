// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/chromeos/login/managed/supervised_user_login_flow.h"

#include "base/base64.h"
#include "base/logging.h"
#include "base/metrics/histogram.h"
#include "base/prefs/pref_registry_simple.h"
#include "base/prefs/pref_service.h"
#include "base/values.h"
#include "chrome/browser/chromeos/login/login_display_host_impl.h"
#include "chrome/browser/chromeos/login/login_utils.h"
#include "chrome/browser/chromeos/login/managed/locally_managed_user_constants.h"
#include "chrome/browser/chromeos/login/managed/locally_managed_user_creation_screen.h"
#include "chrome/browser/chromeos/login/managed/supervised_user_authentication.h"
#include "chrome/browser/chromeos/login/supervised_user_manager.h"
#include "chrome/browser/chromeos/login/user_manager.h"
#include "chrome/browser/chromeos/login/wizard_controller.h"
#include "content/public/browser/browser_thread.h"

using content::BrowserThread;

namespace chromeos {

SupervisedUserLoginFlow::SupervisedUserLoginFlow(
    const std::string& user_id)
    : ExtendedUserFlow(user_id),
      data_loaded_(false),
      weak_factory_(this) {
}

SupervisedUserLoginFlow::~SupervisedUserLoginFlow() {}

bool SupervisedUserLoginFlow::CanLockScreen() {
  return true;
}

bool SupervisedUserLoginFlow::ShouldLaunchBrowser() {
  return data_loaded_;
}

bool SupervisedUserLoginFlow::ShouldSkipPostLoginScreens() {
  return true;
}

bool SupervisedUserLoginFlow::HandleLoginFailure(
    const LoginFailure& failure) {
  return false;
}

bool SupervisedUserLoginFlow::HandlePasswordChangeDetected() {
  return false;
}

void SupervisedUserLoginFlow::HandleOAuthTokenStatusChange(
    User::OAuthTokenStatus status) {
}

void SupervisedUserLoginFlow::OnSyncSetupDataLoaded(
    const std::string& token) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  ConfigureSync(token);
}

void SupervisedUserLoginFlow::ConfigureSync(const std::string& token) {
  data_loaded_ = true;

  // TODO(antrim): add error handling (no token loaded).
  // See also: http://crbug.com/312751
  UserManager::Get()->GetSupervisedUserManager()->ConfigureSyncWithToken(
      profile_, token);
  SupervisedUserAuthentication* auth =
      UserManager::Get()->GetSupervisedUserManager()->GetAuthentication();

  if (auth->HasScheduledPasswordUpdate(user_id())) {
    auth->LoadPasswordUpdateData(
        user_id(),
        base::Bind(&SupervisedUserLoginFlow::OnPasswordChangeDataLoaded,
                   weak_factory_.GetWeakPtr()),
        base::Bind(&SupervisedUserLoginFlow::OnPasswordChangeDataLoadFailed,
                   weak_factory_.GetWeakPtr()));
    return;
  }
  Finish();
}

void SupervisedUserLoginFlow::HandleLoginSuccess(
    const UserContext& login_context) {
  context_.CopyFrom(login_context);
}

void SupervisedUserLoginFlow::OnPasswordChangeDataLoaded(
    const base::DictionaryValue* password_data) {
  // Edge case, when manager has signed in and already updated the password.
  SupervisedUserAuthentication* auth =
      UserManager::Get()->GetSupervisedUserManager()->GetAuthentication();
  if (!auth->NeedPasswordChange(user_id(), password_data)) {
    VLOG(1) << "Password already changed for " << user_id();
    auth->ClearScheduledPasswordUpdate(user_id());
    Finish();
    return;
  }

  // Two cases now - we can currently have either old-style password, or new
  // password.
  std::string base64_signature;
  std::string signature;
  std::string password;
  int revision = 0;
  int schema = 0;
  bool success = password_data->GetStringWithoutPathExpansion(
      kPasswordSignatureKey, &base64_signature);
  success &= password_data->GetIntegerWithoutPathExpansion(kPasswordRevision,
                                                           &revision);
  success &=
      password_data->GetIntegerWithoutPathExpansion(kSchemaVersion, &schema);
  success &= password_data->GetStringWithoutPathExpansion(kEncryptedPassword,
                                                          &password);
  if (!success) {
    LOG(ERROR) << "Incomplete data for password change";

    UMA_HISTOGRAM_ENUMERATION(
        "ManagedUsers.ChromeOS.PasswordChange",
        SupervisedUserAuthentication::PASSWORD_CHANGE_FAILED_INCOMPLETE_DATA,
        SupervisedUserAuthentication::PASSWORD_CHANGE_RESULT_MAX_VALUE);
    Finish();
    return;
  }
  base::Base64Decode(base64_signature, &signature);
  scoped_ptr<base::DictionaryValue> data_copy(password_data->DeepCopy());
  cryptohome::KeyDefinition key(password,
                                kCryptohomeManagedUserKeyLabel,
                                kCryptohomeManagedUserKeyPrivileges);

  authenticator_ = new ExtendedAuthenticator(this);
  SupervisedUserAuthentication::Schema current_schema =
      auth->GetPasswordSchema(user_id());

  key.revision = revision;

  if (SupervisedUserAuthentication::SCHEMA_PLAIN == current_schema) {
    // We need to add new key, and block old one. As we don't actually have
    // signature key, use Migrate privilege instead of AuthorizedUpdate.
    key.privileges = kCryptohomeManagedUserIncompleteKeyPrivileges;

    VLOG(1) << "Adding new schema key";
    DCHECK_EQ(context_.key_label, std::string());
    authenticator_->AddKey(context_,
                           key,
                           false /* no key exists */,
                           base::Bind(&SupervisedUserLoginFlow::OnNewKeyAdded,
                                      weak_factory_.GetWeakPtr(),
                                      Passed(&data_copy)));
  } else if (SupervisedUserAuthentication::SCHEMA_PLAIN == current_schema) {
    VLOG(1) << "Updating the key";

    if (auth->HasIncompleteKey(user_id())) {
      // We need to use Migrate instead of Authorized Update privilege.
      key.privileges = kCryptohomeManagedUserIncompleteKeyPrivileges;
    }

    // Just update the key.
    DCHECK_EQ(context_.key_label, kCryptohomeManagedUserKeyLabel);
    authenticator_->UpdateKeyAuthorized(
        context_,
        key,
        signature,
        base::Bind(&SupervisedUserLoginFlow::OnPasswordUpdated,
                   weak_factory_.GetWeakPtr(),
                   Passed(&data_copy)));
  } else {
    NOTREACHED() << "Unsupported password schema";
  }
}

void SupervisedUserLoginFlow::OnNewKeyAdded(
    scoped_ptr<base::DictionaryValue> password_data) {
  VLOG(1) << "New key added";
  SupervisedUserAuthentication* auth =
      UserManager::Get()->GetSupervisedUserManager()->GetAuthentication();
  auth->StorePasswordData(user_id(), *password_data.get());
  auth->MarkKeyIncomplete(user_id());
  // TODO (antrim): use RemoveKey to remove existing key once Will lands it.
  OnOldKeyRemoved();
}

void SupervisedUserLoginFlow::OnOldKeyRemoved() {
  UMA_HISTOGRAM_ENUMERATION(
      "ManagedUsers.ChromeOS.PasswordChange",
      SupervisedUserAuthentication::PASSWORD_CHANGED_IN_USER_SESSION,
      SupervisedUserAuthentication::PASSWORD_CHANGE_RESULT_MAX_VALUE);
  Finish();
}

void SupervisedUserLoginFlow::OnPasswordChangeDataLoadFailed() {
  LOG(ERROR) << "Could not load data for password change";

  UMA_HISTOGRAM_ENUMERATION(
      "ManagedUsers.ChromeOS.PasswordChange",
      SupervisedUserAuthentication::PASSWORD_CHANGE_FAILED_LOADING_DATA,
      SupervisedUserAuthentication::PASSWORD_CHANGE_RESULT_MAX_VALUE);
  Finish();
}

void SupervisedUserLoginFlow::OnAuthenticationFailure(
    ExtendedAuthenticator::AuthState state) {
  LOG(ERROR) << "Authentication error during password change";

  UMA_HISTOGRAM_ENUMERATION(
      "ManagedUsers.ChromeOS.PasswordChange",
      SupervisedUserAuthentication::
          PASSWORD_CHANGE_FAILED_AUTHENTICATION_FAILURE,
      SupervisedUserAuthentication::PASSWORD_CHANGE_RESULT_MAX_VALUE);
  Finish();
}

void SupervisedUserLoginFlow::OnPasswordUpdated(
    scoped_ptr<base::DictionaryValue> password_data) {
  VLOG(1) << "Updated password for supervised user";

  SupervisedUserAuthentication* auth =
      UserManager::Get()->GetSupervisedUserManager()->GetAuthentication();

  // Incomplete state is not there in password_data, carry it from old state.
  bool was_incomplete = auth->HasIncompleteKey(user_id());
  auth->StorePasswordData(user_id(), *password_data.get());
  if (was_incomplete)
    auth->MarkKeyIncomplete(user_id());

  UMA_HISTOGRAM_ENUMERATION(
      "ManagedUsers.ChromeOS.PasswordChange",
      SupervisedUserAuthentication::PASSWORD_CHANGED_IN_USER_SESSION,
      SupervisedUserAuthentication::PASSWORD_CHANGE_RESULT_MAX_VALUE);
  Finish();
}

void SupervisedUserLoginFlow::Finish() {
  LoginUtils::Get()->DoBrowserLaunch(profile_, host());
  profile_ = NULL;
  UnregisterFlowSoon();
}

void SupervisedUserLoginFlow::LaunchExtraSteps(
    Profile* profile) {
  profile_ = profile;
  UserManager::Get()->GetSupervisedUserManager()->LoadSupervisedUserToken(
      profile,
      base::Bind(
           &SupervisedUserLoginFlow::OnSyncSetupDataLoaded,
           weak_factory_.GetWeakPtr()));
}

}  // namespace chromeos
