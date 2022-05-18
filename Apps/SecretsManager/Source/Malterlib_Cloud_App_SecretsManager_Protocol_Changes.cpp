// Copyright © 2020 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Concurrency/LogError>

#include "Malterlib_Cloud_App_SecretsManager.h"
#include "Malterlib_Cloud_App_SecretsManager_Server.h"

namespace NMib::NCloud::NSecretsManager
{
	NConcurrency::TCFuture<void> CSecretsManagerDaemonActor::CServer::CChangeSubscription::f_SendChanges(CSecretsManager::CSecretChanges &&_Changes) const
	{
		return m_SubscriptionParams.m_fOnChanges(fg_Move(_Changes)).f_Future();
	}

	void CSecretsManagerDaemonActor::CServer::fp_UpdateSubscriptionsForChangedPermissions(CPermissionIdentifiers const &_Identity)
	{
		auto fSendForSubscriptions = [&](TCMap<CStr, CChangeSubscription> const &_Subscriptions)
			{
				for (auto &Subscription : _Subscriptions)
				{
					if (Subscription.m_CallingHostInfo.f_GetRealHostID() != _Identity.f_GetHostID())
						continue;
					self(&CServer::fp_SendSubscriptionInitial, Subscription.f_GetSubscriptionID()) > fg_DiscardResult();
				}
			}
		;

		fSendForSubscriptions(mp_ChangeSubscriptions);
	}

	bool CSecretsManagerDaemonActor::CServer::fp_SecretMatchesSubscription(CChangeSubscription const &_Subscription, CSecretPropertiesInternal const &_SecretProperties)
	{
		return fs_MatchSecret(_SecretProperties, _Subscription.m_SubscriptionParams.m_SemanticID, _Subscription.m_SubscriptionParams.m_Name, _Subscription.m_SubscriptionParams.m_TagsExclusive);
	}

	void CSecretsManagerDaemonActor::CServer::fp_SecretUpdated(CSecretPropertiesInternal const &_SecretProperties, bool _bRemoved)
	{
		auto SecretID = _SecretProperties.f_GetSecretID();
		auto SecretProperties = _SecretProperties.f_ToSecretProperties();

		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;
		fsp_AddPermissionQueryIndexedByPermission("Read", _SecretProperties.m_SemanticID, _SecretProperties.m_Tags, Permissions);

		auto fSendToSubscription = [this, Permissions, SecretID, SecretProperties, _bRemoved](CStr const &_SubscriptionID) -> TCFuture<void>
			{
				CChangeSubscription const *pSubscription = nullptr;

				auto OnResume = g_OnResume / [&]
					{
						if (f_IsDestroyed())
							DMibError("Shutting down");

						pSubscription = this->mp_ChangeSubscriptions.f_FindEqual(_SubscriptionID);
						if (!pSubscription)
							DMibError("Subscription no longer exists");
					}
				;
				
				auto HasPermissions = co_await mp_Permissions.f_HasPermissions("Send updated secrets in SecretsManager", Permissions, pSubscription->m_CallingHostInfo)
					.f_Timeout(60.0, "Timed out checking permissions for sending secret changes").f_Wrap()
				;

				if (!HasPermissions)
 				{
					CDistributedAppAuditor Auditor(mp_AppState.m_AppActor, pSubscription->m_CallingHostInfo, {});
					Auditor.f_Error("Errors checking permissions for subscription:  {}"_f << HasPermissions.f_GetExceptionStr());
					co_return {};
				}

				for (auto const &bHasPermission : *HasPermissions)
				{
					if (!bHasPermission)
						co_return {}; // Ignore
				}

				CSecretsManager::CSecretChanges Changes;

				Changes.m_bFullResend = false;
				if (_bRemoved)
					Changes.m_Removed[SecretID];
				else
					Changes.m_Changed[SecretID] = SecretProperties;

				pSubscription->m_SubscriptionParams.m_fOnChanges(fg_Move(Changes)) > fg_DiscardResult();

				co_return {};
			}
		;

		for (auto &Subscription : mp_ChangeSubscriptions)
		{
			if (!fp_SecretMatchesSubscription(Subscription, _SecretProperties))
				continue;

			self.f_Invoke(fSendToSubscription, Subscription.f_GetSubscriptionID()) > fg_DiscardResult();
		}
	}

	TCFuture<void> CSecretsManagerDaemonActor::CServer::fp_SendSubscriptionInitial(CStr const &_SubscriptionID)
	{
		TCPromise<void> Promise;

		CChangeSubscription const *pSubscription = nullptr;

		auto OnResume = g_OnResume / [&]
			{
				if (f_IsDestroyed())
					DMibError("Shutting down");

				pSubscription = mp_ChangeSubscriptions.f_FindEqual(_SubscriptionID);
				if (!pSubscription)
					DMibError("Subscription no longer exists");
			}
		;

		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;

		fsp_AddPermissionsForMatchingSecrets
			(
				mp_Database.m_Secrets
				, pSubscription->m_SubscriptionParams.m_SemanticID
				, pSubscription->m_SubscriptionParams.m_Name
				, pSubscription->m_SubscriptionParams.m_TagsExclusive
				, Permissions
			)
		;

		CDistributedAppAuditor Auditor(mp_AppState.m_AppActor, pSubscription->m_CallingHostInfo, {});

		auto HasPermissions = co_await
			(
				mp_Permissions.f_HasPermissions("Send initial secrets in change subscription in SecretsManager", Permissions, pSubscription->m_CallingHostInfo)
				% "Permission denied sending initial secrets"
				% Auditor
			)
		;

		TCSet<CSecretsManager::CSecretID> IDs;

		CSecretsManager::CSecretChanges SecretChanges;
		SecretChanges.m_bFullResend = true;

		for (auto const &SecretProperties : mp_Database.m_Secrets)
		{
			if
				(
					!fs_MatchSecret
					(
						SecretProperties
						, pSubscription->m_SubscriptionParams.m_SemanticID
						, pSubscription->m_SubscriptionParams.m_Name
						, pSubscription->m_SubscriptionParams.m_TagsExclusive
					)
				)
			{
				continue;
			}

			auto const &ID = mp_Database.m_Secrets.fs_GetKey(SecretProperties);
			auto *pHasPermission = HasPermissions.f_FindEqual(CStr::fs_ToStr(ID));
			if (!pHasPermission || !*pHasPermission)
				continue;

			SecretChanges.m_Changed[ID] = SecretProperties.f_ToSecretProperties();
		}

		co_await pSubscription->f_SendChanges(fg_Move(SecretChanges)).f_Wrap() > fg_LogError("Mib/Cloud/SecretsManager", "Failed to send changes to subscription");

		co_return {};
	}

	TCFuture<TCActorSubscriptionWithID<>> CSecretsManagerDaemonActor::CServer::CSecretsManagerImplementation::f_SubscribeToChanges(CSubscribeToChanges &&_Params)
	{
		auto &This = *m_pThis;
		auto Auditor = This.mp_AppState.f_Auditor();

		if (!_Params.m_fOnChanges)
			co_return DMibErrorInstance("m_fOnChanges required");

		if (_Params.m_SemanticID && !CSecretsManager::fs_IsValidSemanticIDWildcard(*_Params.m_SemanticID))
			co_return Auditor.f_Exception(fg_Format("Malformed semantic ID: '{}'", *_Params.m_SemanticID));

		if (_Params.m_Name && !CSecretsManager::fs_IsValidNameWildcard(*_Params.m_Name))
			co_return Auditor.f_Exception(fg_Format("Malformed name: '{}'", *_Params.m_Name));

		for (auto const &Tag : _Params.m_TagsExclusive)
		{
			if (!CSecretsManager::fs_IsValidTag(Tag))
				co_return Auditor.f_Exception(fg_Format("Malformed Tag: '{}'", Tag));
		}

		auto OnResume = g_OnResume / [&]
			{
				if (f_IsDestroyed())
					DMibError("Shutting down");
			}
		;

		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;

		Permissions["Command"] = {{"SecretsManager/CommandAll", "SecretsManager/Command/SubscribeToChanges"}};

		auto HasPermissions = co_await
			(
			 	This.mp_Permissions.f_HasPermissions("Subscribe to changes from SecretsManager", Permissions)
			 	% "Permission denied subscribing to changes"
			 	% Auditor
			)
		;

		if (!HasPermissions["Command"])
			co_return Auditor.f_AccessDenied("(SubscribeToChanges, command)", Permissions["Command"]);

		CStr SubscriptionID = fg_RandomID(This.mp_ChangeSubscriptions);
		auto pSubscription = &This.mp_ChangeSubscriptions[SubscriptionID];

		auto OnResume2 = g_OnResume / [&]
			{
				pSubscription = This.mp_ChangeSubscriptions.f_FindEqual(SubscriptionID);
				if (!pSubscription)
					DMibError("Subscription no longer exists");
			}
		;

		pSubscription->m_SubscriptionParams = fg_Move(_Params);
		pSubscription->m_CallingHostInfo = fg_GetCallingHostInfo();

		CActorSubscription SubscriptionHandle = g_ActorSubscription / [pThis = &This, SubscriptionID]() -> TCFuture<void>
			{
				pThis->mp_ChangeSubscriptions.f_Remove(SubscriptionID);
				co_return {};
			}
		;

		co_await (This.self(&CServer::fp_SendSubscriptionInitial, SubscriptionID) % "Error sending initial subsciption" % Auditor);

		Auditor.f_Info
			(
				"Subscribed to secrets matching: SemanticID: {} Tags: {vs}"_f
				<< pSubscription->m_SubscriptionParams.m_SemanticID
				<< pSubscription->m_SubscriptionParams.m_TagsExclusive
			)
		;

		co_return fg_Move(SubscriptionHandle);
	}
}
