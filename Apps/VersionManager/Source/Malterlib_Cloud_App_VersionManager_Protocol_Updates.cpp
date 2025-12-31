// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/DistributedActorTrustManager>
#include <Mib/Concurrency/DistributedActorTrustManagerDatabases/JsonDirectory>

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
	
	void CVersionManagerDaemonActor::CServer::fp_NewVersion(CStr const &_ApplicationName, CVersion const &_Version)
	{
		TCSharedPointer<CVersionManager::CNewVersionNotifications> pNewVersionNotifications = fg_Construct();
		{
			auto &NewVersionNotification = pNewVersionNotifications->m_NewVersions.f_Insert();
			NewVersionNotification.m_Application = _ApplicationName;
			NewVersionNotification.m_VersionIDAndPlatform = _Version.f_GetIdentifier();
			NewVersionNotification.m_VersionInfo = _Version.m_VersionInfo;
		}
		
		fp_NewTagsKnown(_Version.m_VersionInfo.m_Tags);

		TCVector<CStr> Permissions;
		Permissions.f_Insert("Application/ReadAll");
		Permissions.f_Insert("Application/ListAll");
		Permissions.f_Insert(fg_Format("Application/Read/{}", _ApplicationName));
		
		auto fSendToSubscription = [&](CSubscription const &_Subscription, CStr const &_SubscriptionApplication)
			{
				mp_Permissions.f_HasPermission("Receive VersionManager version update notification", Permissions, _Subscription.m_CallingHostInfo)
					.f_Timeout(60.0, "Timed out checking permissions for sending version update subscription")
					> [=, this, VersionID = _Version.f_GetIdentifier(), SubscriptionID = _Subscription.f_GetSubscriptionID()](TCAsyncResult<bool> &&_Result)
					{
						CSubscription const *pSubscription = fp_GetSubscription(_SubscriptionApplication, SubscriptionID);

						if (!pSubscription)
							return;

						if (!_Result)
						{
							CDistributedAppAuditor Auditor(mp_AppState.m_AppActor, pSubscription->m_CallingHostInfo, {});
							Auditor.f_Error("Errors checking permissions for subscription:  {}"_f << _Result.f_GetExceptionStr());
							return;
						}
						if (!*_Result)
							return;

						auto pApplication = mp_Applications.f_FindEqual(_ApplicationName);
						if (!pApplication)
							return;

						auto pVersion = pApplication->m_Versions.f_FindEqual(VersionID);
						if (!pVersion)
							return;

						if (!fp_VersionMatchesSubscription(*pSubscription, *pVersion))
							return;

						pSubscription->f_SendVersions(*pNewVersionNotifications);
					}
				;
			}
		;

		for (auto &Subscription : mp_GlobalVersionSubscriptions)
			fSendToSubscription(Subscription, {});
		auto *pSubscription = mp_VersionSubscriptions.f_FindEqual(_ApplicationName);
		if (pSubscription)
		{
			for (auto &Subscription : *pSubscription)
				fSendToSubscription(Subscription, _ApplicationName);
		}
	}
	
	void CVersionManagerDaemonActor::CServer::CSubscription::f_SendVersions(CVersionManager::CNewVersionNotifications const &_NewVersionNotifications) const
	{
		m_fOnNewVersions(_NewVersionNotifications).f_DiscardResult();
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
					mint nToSend = pSubscription->m_nInitial;
					mint nVersions = pApplication->m_VersionsByTime.f_GetLen();
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

		pSubscription->f_SendVersions(NewVersionNotifications);

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

