// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/DistributedActorTrustManager>
#include <Mib/Concurrency/DistributedActorTrustManagerDatabases/JsonDirectory>
#include <Mib/Concurrency/LogError>

#include "Malterlib_Cloud_App_VersionManager.h"
#include "Malterlib_Cloud_App_VersionManager_Server.h"

namespace NMib::NCloud::NVersionManager
{
	auto CVersionManagerDaemonActor::CServer::fp_GetSubscription(CStr const &_ApplicationName, CStr const &_SubscriptionID) const -> CSubscription const *
	{
		CSubscription const *pSubscription = nullptr;
		if (_ApplicationName.f_IsEmpty())
			pSubscription = mp_GlobalVersionSubscriptions.f_FindEqual(_SubscriptionID);
		else
		{
			auto pApplicationSubscriptions = mp_VersionSubscriptions.f_FindEqual(_ApplicationName);
			if (!pApplicationSubscriptions)
				return nullptr;
			pSubscription = pApplicationSubscriptions->f_FindEqual(_SubscriptionID);
		}

		return pSubscription;
	}

	bool CVersionManagerDaemonActor::CServer::fp_VersionMatchesSubscription(CSubscription const &_Subscription, CVersion const &_Version)
	{
		if (!_Subscription.m_Platforms.f_IsEmpty() && !_Subscription.m_Platforms.f_FindEqual(_Version.f_GetIdentifier().m_Platform))
			return false;
		if (!_Subscription.m_Tags.f_IsEmpty())
		{
			bool bAllTagsFound = true;
			for (auto &Tag : _Subscription.m_Tags)
			{
				if (!_Version.m_VersionInfo.m_Tags.f_FindEqual(Tag))
				{
					bAllTagsFound = false;
					break;
				}
			}
			if (!bAllTagsFound)
				return false;
		}
		return true;
	}

	TCFuture<void> CVersionManagerDaemonActor::CServer::fp_NewVersion
		(
			CStr _ApplicationName
			, CVersionManager::CVersionIDAndPlatform _VersionID
			, CVersionManager::CVersionInformation _VersionInfo
			, CStr _OriginID
		)
	{
		TCSharedPointer<CVersionManager::CNewVersionNotifications> pNewVersionNotifications = fg_Construct();
		pNewVersionNotifications->m_OriginID = fg_Move(_OriginID);
		{
			auto &NewVersionNotification = pNewVersionNotifications->m_NewVersions.f_Insert();
			NewVersionNotification.m_Application = _ApplicationName;
			NewVersionNotification.m_VersionIDAndPlatform = _VersionID;
			NewVersionNotification.m_VersionInfo = _VersionInfo;
		}

		fp_NewTagsKnown(_VersionInfo.m_Tags);

		TCVector<CStr> Permissions;
		Permissions.f_Insert("Application/ReadAll");
		Permissions.f_Insert("Application/ListAll");
		Permissions.f_Insert(fg_Format("Application/Read/{}", _ApplicationName));

		auto fSendToSubscription = [this, Permissions, VersionID = _VersionID, pNewVersionNotifications, ApplicationName = _ApplicationName]
			(CStr _SubscriptionID, CCallingHostInfo _CallingHostInfo, CStr _SubscriptionApplication) -> TCFuture<void>
			{
				auto SubscriptionID = _SubscriptionID;
				auto PermissionResult = co_await mp_Permissions.f_HasPermission("Receive VersionManager version update notification", Permissions, _CallingHostInfo)
					.f_Timeout(60.0, "Timed out checking permissions for sending version update subscription").f_Wrap()
				;

				CSubscription const *pSubscription = fp_GetSubscription(_SubscriptionApplication, SubscriptionID);

				if (!pSubscription)
					co_return {};

				CDistributedAppAuditor Auditor(mp_AppState.m_AppActor, pSubscription->m_CallingHostInfo, {});
				if (!PermissionResult)
				{
					Auditor.f_Error("Errors checking permissions for subscription:  {}"_f << PermissionResult.f_GetExceptionStr());
					co_return PermissionResult.f_GetException();
				}
				if (!*PermissionResult)
					co_return {};

				auto pApplication = mp_Applications.f_FindEqual(ApplicationName);
				if (!pApplication)
					co_return {};

				auto pVersion = pApplication->m_Versions.f_FindEqual(VersionID);
				if (!pVersion)
					co_return {};

				if (!fp_VersionMatchesSubscription(*pSubscription, *pVersion))
					co_return {};

				co_await (pSubscription->m_fOnNewVersions(*pNewVersionNotifications) % Auditor);

				co_return {};
			}
		;

		TCFutureVector<void> Results;

		for (auto &Subscription : mp_GlobalVersionSubscriptions)
			self.f_Invoke(fSendToSubscription, Subscription.f_GetSubscriptionID(), Subscription.m_CallingHostInfo, CStr()) > Results;

		auto *pSubscription = mp_VersionSubscriptions.f_FindEqual(_ApplicationName);
		if (pSubscription)
		{
			for (auto &Subscription : *pSubscription)
				self.f_Invoke(fSendToSubscription, Subscription.f_GetSubscriptionID(), Subscription.m_CallingHostInfo, _ApplicationName) > Results;
		}

		co_await fg_AllDone(Results).f_Wrap() > fg_LogError("VersionManager/Subscriptions", "Failed to notify new version to subscriptions");

		co_return {};
	}

	TCFuture<void> CVersionManagerDaemonActor::CServer::fp_UpdateSubscriptionsForChangedPermissions(CPermissionIdentifiers _Identity)
	{
		TCFutureVector<void> Results;

		auto fSendForSubscriptions = [&](TCMap<CStr, CSubscription> const &_Subscriptions, CStr const &_Application)
			{
				for (auto &Subscription : _Subscriptions)
				{
					if (Subscription.m_CallingHostInfo.f_GetRealHostID() != _Identity.f_GetHostID())
						continue;
					fp_SendSubscriptionInitial(_Application, &Subscription) > Results;
				}
			}
		;
		fSendForSubscriptions(mp_GlobalVersionSubscriptions, CStr());
		for (auto &Subscriptions : mp_VersionSubscriptions)
			fSendForSubscriptions(Subscriptions, mp_VersionSubscriptions.fs_GetKey(Subscriptions));

		co_await fg_AllDone(Results).f_Wrap() > fg_LogError("VersionManager/Subscriptions", "Falied to send initial subscription when changing permissions");

		co_return {};
	}

	TCFuture<void> CVersionManagerDaemonActor::CServer::fp_SendSubscriptionInitial(CStr _ApplicationName, CSubscription const *_pSubscription)
	{
		auto fSendInitialForApplication =
			[this, CallingHostInfo = _pSubscription->m_CallingHostInfo, SubscriptionID = _pSubscription->f_GetSubscriptionID(), ApplicationName = _ApplicationName]
			(CStr _Application) -> TCFuture<TCVector<CVersionManager::CNewVersionNotification>>
			{
				auto bHasPermissions = co_await mp_Permissions.f_HasPermission
					(
						"Receive VersionManager version update notification"
						, {"Application/ReadAll", "Application/ListAll", fg_Format("Application/Read/{}", _Application)}
						, CallingHostInfo
					)
					.f_Timeout(60.0, "Timed out checking permissions for sending version update subscription")
				;

				CSubscription const *pSubscription = fp_GetSubscription(ApplicationName, SubscriptionID);

				TCVector<CVersionManager::CNewVersionNotification> NewVersionNotifications;
				if (pSubscription && bHasPermissions)
				{
					auto *pApplication = mp_Applications.f_FindEqual(_Application);
					umint nToSend = pSubscription->m_nInitial;
					umint nVersions = pApplication->m_VersionsByTime.f_GetLen();
					(void)nVersions;
					decltype(pApplication->m_VersionsByTime.f_GetIterator()) iVersion;
					for (iVersion.f_StartBackward(pApplication->m_VersionsByTime); iVersion && nToSend; --iVersion)
					{
						auto &Version = *iVersion;
						if (!fp_VersionMatchesSubscription(*pSubscription, Version))
							continue;
						auto &NewVersionNotification = NewVersionNotifications.f_Insert();
						NewVersionNotification.m_Application = pApplication->f_GetName();
						NewVersionNotification.m_VersionIDAndPlatform = Version.f_GetIdentifier();
						NewVersionNotification.m_VersionInfo = Version.m_VersionInfo;
						--nToSend;
					}
				}

				co_return fg_Move(NewVersionNotifications);
			}
		;

		TCFutureVector<TCVector<CVersionManager::CNewVersionNotification>> NewVersionNotificationResults;
		if (_ApplicationName.f_IsEmpty())
		{
			for (auto &Application : mp_Applications)
				self.f_Invoke(fSendInitialForApplication, mp_Applications.fs_GetKey(Application)) > NewVersionNotificationResults;
		}
		else if (auto *pApplication = mp_Applications.f_FindEqual(_ApplicationName))
			self.f_Invoke(fSendInitialForApplication, mp_Applications.fs_GetKey(pApplication)) > NewVersionNotificationResults;

		auto Results = co_await fg_AllDoneWrapped(NewVersionNotificationResults);

		CSubscription const *pSubscription = fp_GetSubscription(_ApplicationName, _pSubscription->f_GetSubscriptionID());

		if (!pSubscription)
			co_return {};

		CVersionManager::CNewVersionNotifications NewVersionNotifications;
		NewVersionNotifications.m_bFullResend = true;
		NewVersionNotifications.m_OriginID = fg_RandomID();

		CDistributedAppAuditor Auditor(mp_AppState.m_AppActor, pSubscription->m_CallingHostInfo, {});

		CStr Errors;

		for (auto &Result : Results)
		{
			if (!Result)
			{
				Errors += "{}\n"_f << Result.f_GetExceptionStr();
				continue;
			}
			NewVersionNotifications.m_NewVersions.f_InsertLast(*Result);
		}

		if (!Errors.f_IsEmpty())
			Auditor.f_Error("Errors checking permissions for subscription:\n\n{}"_f << Errors);

		co_await (pSubscription->m_fOnNewVersions(NewVersionNotifications) % Auditor);

		co_return {};
	}

	auto CVersionManagerDaemonActor::CServer::CVersionManagerImplementation::f_SubscribeToUpdates(CSubscribeToUpdates _Params) -> TCFuture<CSubscribeToUpdates::CResult>
	{
		auto pThis = m_pThis;

		CStr SubscriptionID;
		CSubscription *pSubscription;
		if (_Params.m_Application.f_IsEmpty())
		{
			SubscriptionID = fg_RandomID(pThis->mp_GlobalVersionSubscriptions);
			pSubscription = &pThis->mp_GlobalVersionSubscriptions[SubscriptionID];
		}
		else
		{
			SubscriptionID = fg_RandomID(pThis->mp_VersionSubscriptions[_Params.m_Application]);
			pSubscription = &pThis->mp_VersionSubscriptions[_Params.m_Application][SubscriptionID];
		}

		if (!_Params.m_fOnNewVersions)
			co_return DMibErrorInstance("m_fOnNewVersions required");

		auto &Subscription = *pSubscription;
		Subscription.m_fOnNewVersions = fg_Move(_Params.m_fOnNewVersions);
		Subscription.m_CallingHostInfo = fg_GetCallingHostInfo();
		Subscription.m_Tags = _Params.m_Tags;
		Subscription.m_Platforms = _Params.m_Platforms;
		Subscription.m_nInitial = _Params.m_nInitial;

		CVersionManager::CSubscribeToUpdates::CResult Result;
		Result.m_Subscription = g_ActorSubscription / [pThis, ApplicationName = _Params.m_Application, SubscriptionID]() -> TCFuture<void>
			{
				if (ApplicationName.f_IsEmpty())
				{
					pThis->mp_GlobalVersionSubscriptions.f_Remove(SubscriptionID);
					co_return {};
				}

				auto *pSubscription = pThis->mp_VersionSubscriptions.f_FindEqual(ApplicationName);
				if (!pSubscription)
					co_return {};

				pSubscription->f_Remove(SubscriptionID);
				if (pSubscription->f_IsEmpty())
					pThis->mp_VersionSubscriptions.f_Remove(pSubscription);

				co_return {};
			}
		;

		if (!_Params.m_nInitial)
			co_return fg_Move(Result);

		auto ApplicationName = _Params.m_Application;
		co_await pThis->fp_SendSubscriptionInitial(_Params.m_Application, &Subscription);

		{
			CSubscription const *pSubscription = pThis->fp_GetSubscription(ApplicationName, SubscriptionID);

			if (!pSubscription)
				co_return fg_Move(Result);

			auto DispatchActor = pSubscription->m_fOnNewVersions.f_GetActor();

			// Because versions are dispatched through m_DispatchActor we need to dispatch the result to get correct ordering
			if (DispatchActor)
				co_await fg_ContinueRunningOnActor(fg_Move(DispatchActor));
		}

		co_return fg_Move(Result);
	}
}

