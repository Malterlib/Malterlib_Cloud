// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/DistributedActorTrustManager>
#include <Mib/Concurrency/DistributedActorTrustManagerDatabases/JSONDirectory>

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
					.f_Dispatch().f_Timeout(60.0, "Timed out checking permissions for sending version update subscription")
					> [=, VersionID = _Version.f_GetIdentifier(), SubscriptionID = _Subscription.f_GetSubscriptionID()](TCAsyncResult<bool> &&_Result)
					{
						CSubscription const *pSubscription = fp_GetSubscription(_SubscriptionApplication, SubscriptionID);

						if (!pSubscription)
							return;

						if (!_Result)
						{
							CDistributedAppAuditor Auditor(mp_AppState.m_AppActor, pSubscription->m_CallingHostInfo);
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
		fg_Dispatch
			(
				m_DispatchActor
				, [fOnNewVersions = m_fOnNewVersions, NewVersionNotifications = _NewVersionNotifications]() mutable
				{
					return fOnNewVersions(fg_Move(NewVersionNotifications));
				}
			)
			> fg_DiscardResult()
		;
	}

	void CVersionManagerDaemonActor::CServer::fp_UpdateSubscriptionsForChangedPermissions(CPermissionIdentifiers const &_Identity)
	{
		auto fSendForSubscriptions = [&](TCMap<CStr, CSubscription> const &_Subscriptions, CStr const &_Application)
			{
				for (auto &Subscription : _Subscriptions)
				{
					if (Subscription.m_CallingHostInfo.f_GetRealHostID() != _Identity.f_GetHostID())
						continue;
					fp_SendSubscriptionInitial(_Application, Subscription) > fg_DiscardResult();
				}
			}
		;
		fSendForSubscriptions(mp_GlobalVersionSubscriptions, CStr());
		for (auto &Subscriptions : mp_VersionSubscriptions)
			fSendForSubscriptions(Subscriptions, mp_VersionSubscriptions.fs_GetKey(Subscriptions));
	}
	
	TCContinuation<void> CVersionManagerDaemonActor::CServer::fp_SendSubscriptionInitial(CStr const &_ApplicationName, CSubscription const &_Subscription)
	{
		auto fSendInitialForApplication = [&](CStr const &_Application) -> TCContinuation<TCVector<CVersionManager::CNewVersionNotification>>
			{
				TCContinuation<TCVector<CVersionManager::CNewVersionNotification>> Continuation;
				mp_Permissions.f_HasPermission
					(
						"Receive VersionManager version update notification"
						, {"Application/ReadAll", "Application/ListAll", fg_Format("Application/Read/{}", _Application)}
						, _Subscription.m_CallingHostInfo
					)
					.f_Dispatch().f_Timeout(60.0, "Timed out checking permissions for sending version update subscription")
					> Continuation / [=, SubscriptionID = _Subscription.f_GetSubscriptionID()](bool _bHasPermission)
					{
						CSubscription const *pSubscription = fp_GetSubscription(_ApplicationName, SubscriptionID);

						TCVector<CVersionManager::CNewVersionNotification> NewVersionNotifications;
						if (pSubscription && _bHasPermission)
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
						Continuation.f_SetResult(NewVersionNotifications);
					}
				;
				return Continuation;
			}
		;
		TCActorResultVector<TCVector<CVersionManager::CNewVersionNotification>> NewVersionNotificationResults;
		if (_ApplicationName.f_IsEmpty())
		{
			for (auto &Application : mp_Applications)
				fSendInitialForApplication(mp_Applications.fs_GetKey(Application)) > NewVersionNotificationResults.f_AddResult();
		}
		else if (auto *pApplication = mp_Applications.f_FindEqual(_ApplicationName))
			fSendInitialForApplication(mp_Applications.fs_GetKey(pApplication)) > NewVersionNotificationResults.f_AddResult();
		
		TCContinuation<void> Continuation;
		NewVersionNotificationResults.f_GetResults()
			> Continuation / [this, Continuation, _ApplicationName, SubscriptionID = _Subscription.f_GetSubscriptionID()]
			(TCVector<TCAsyncResult<TCVector<CVersionManager::CNewVersionNotification>>> &&_Results)
			{
				CSubscription const *pSubscription = fp_GetSubscription(_ApplicationName, SubscriptionID);

				if (!pSubscription)
					return Continuation.f_SetResult();

				CVersionManager::CNewVersionNotifications NewVersionNotifications;
				NewVersionNotifications.m_bFullResend = true;

				CDistributedAppAuditor Auditor(mp_AppState.m_AppActor, pSubscription->m_CallingHostInfo);

				CStr Errors;

				for (auto &Result : _Results)
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

				Continuation.f_SetResult();
			}
		;

		return Continuation;
	}
	
	auto CVersionManagerDaemonActor::CServer::CVersionManagerImplementation::f_SubscribeToUpdates(CSubscribeToUpdates &&_Params) -> TCContinuation<CSubscribeToUpdates::CResult> 
	{
		auto pThis = m_pThis;
		
		CStr SubscriptionID = fg_RandomID();
		CSubscription *pSubscription;
		if (_Params.m_Application.f_IsEmpty())
			pSubscription = &pThis->mp_GlobalVersionSubscriptions[SubscriptionID];
		else
			pSubscription = &pThis->mp_VersionSubscriptions[_Params.m_Application][SubscriptionID];

		if (!_Params.m_DispatchActor)
			return DMibErrorInstance("m_DispatchActor required");
		if (!_Params.m_fOnNewVersions)
			return DMibErrorInstance("m_fOnNewVersions required");

		auto &Subscription = *pSubscription;
		Subscription.m_DispatchActor = fg_Move(_Params.m_DispatchActor);
		Subscription.m_fOnNewVersions = fg_Move(_Params.m_fOnNewVersions);
		Subscription.m_CallingHostInfo = fg_GetCallingHostInfo();
		Subscription.m_Tags = _Params.m_Tags;
		Subscription.m_Platforms = _Params.m_Platforms;
		Subscription.m_nInitial = _Params.m_nInitial;
			
		CVersionManager::CSubscribeToUpdates::CResult Result;
		Result.m_Subscription = fg_ActorSubscription
			(
				self
				, [pThis, ApplicationName = _Params.m_Application, SubscriptionID]
				{
					if (ApplicationName.f_IsEmpty())
					{
						pThis->mp_GlobalVersionSubscriptions.f_Remove(SubscriptionID);
						return;
					}
					auto *pSubscription = pThis->mp_VersionSubscriptions.f_FindEqual(ApplicationName);
					if (!pSubscription)
						return;
					pSubscription->f_Remove(SubscriptionID);
					if (pSubscription->f_IsEmpty())
						pThis->mp_VersionSubscriptions.f_Remove(pSubscription);
				}
			)
		;
		
		if (!_Params.m_nInitial)
			return fg_Explicit(fg_Move(Result));

		TCContinuation<CSubscribeToUpdates::CResult> Continuation;
		pThis->fp_SendSubscriptionInitial(_Params.m_Application, Subscription)
			> Continuation / [Continuation, DispatchActor = Subscription.m_DispatchActor, Result = fg_Move(Result)] () mutable
			{
				if (DispatchActor.f_IsEmpty())
					return Continuation.f_SetResult(fg_Move(Result));

				// Because versions are dispatched through m_DispatchActor we need to dispatch the result to get correct ordering
				g_Dispatch(DispatchActor) / [Continuation, Result = fg_Move(Result)]() mutable
					{
						Continuation.f_SetResult(fg_Move(Result));
					}
					> fg_DiscardResult()
				;
			}
		;
		return Continuation;
	}
}

