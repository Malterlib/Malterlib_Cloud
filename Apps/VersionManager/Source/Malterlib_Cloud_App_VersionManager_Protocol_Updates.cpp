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
	void CVersionManagerDaemonActor::CServer::CSubscription::f_SendVersion(CVersionManager::CNewVersionNotification const &_NewVersionNotification) const
	{
		fg_Dispatch
			(
				m_DispatchActor
				, [fOnNewVersion = m_fOnNewVersion, NewVersionNotification = _NewVersionNotification]() mutable
				{
					return fOnNewVersion(fg_Move(NewVersionNotification));
				}
			)
			> fg_DiscardResult()
		;
	}

	void CVersionManagerDaemonActor::CServer::fp_UpdateSubscriptionsForChangedPermissions(CStr const &_HostID)
	{
		auto fSendForSubscriptions = [&](TCMap<CStr, CSubscription> const &_Subscriptions, CStr const &_Application)
			{
				for (auto &Subscription : _Subscriptions)
				{
					if (Subscription.m_HostID != _HostID)
						continue;
					fp_SendSubscriptionInitial(_Application, Subscription, true);
				}
			}
		;
		fSendForSubscriptions(mp_GlobalVersionSubscriptions, CStr());
		for (auto &Subscriptions : mp_VersionSubscriptions)
			fSendForSubscriptions(Subscriptions, mp_VersionSubscriptions.fs_GetKey(Subscriptions));
	}
	
	void CVersionManagerDaemonActor::CServer::fp_SendSubscriptionInitial(CStr const &_Application, CSubscription const &_Subscription, bool _bPermissionsChanged)
	{
		TCVector<CStr> SharedPermissions;
		SharedPermissions.f_Insert("Application/ReadAll");
		SharedPermissions.f_Insert("Application/ListAll");

		if (_bPermissionsChanged)
		{
			fg_Dispatch
				(
					_Subscription.m_DispatchActor
					, fg_TempCopy(_Subscription.m_fOnPermissionsChanged)
				)
				> fg_DiscardResult()
			;
		}
		
		auto fSendInitialForApplication = [&](CApplication const &_Application)
			{
				if 
					(
						!mp_Permissions.f_HostHasAnyPermission(_Subscription.m_HostID, SharedPermissions) 
						&& !mp_Permissions.f_HostHasPermission(_Subscription.m_HostID, fg_Format("Application/Read/{}", _Application.f_GetName()))
					)
				{
					return;
				}

				CVersionManager::CNewVersionNotification NewVersionNotification;
				NewVersionNotification.m_Application = _Application.f_GetName();
				
				mint nToSend = _Subscription.m_nInitial;
				decltype(_Application.m_VersionsByTime.f_GetIterator()) iVersion;
				for (iVersion.f_StartBackward(_Application.m_VersionsByTime); iVersion && nToSend; --iVersion, --nToSend)
				{
					auto &Version = *iVersion;
					NewVersionNotification.m_VersionID = Version.f_GetIdentifier();
					NewVersionNotification.m_VersionInfo = Version.m_VersionInfo;
					_Subscription.f_SendVersion(NewVersionNotification);
				}
			}
		;
		
		if (_Application.f_IsEmpty())
		{
			for (auto &Application : mp_Applications)
				fSendInitialForApplication(Application);
		}
		else if (auto *pApplication = mp_Applications.f_FindEqual(_Application))
			fSendInitialForApplication(*pApplication);
	}
	
	auto CVersionManagerDaemonActor::CServer::fp_Protocol_SubscribeToUpdates(CCallingHostInfo const &_CallingHostInfo, CVersionManager::CSubscribeToUpdates &&_Params)
		-> TCContinuation<CVersionManager::CSubscribeToUpdates::CResult> 
	{
		CStr SubscriptionID = fg_RandomID();
		CSubscription *pSubscription;
		if (_Params.m_Application.f_IsEmpty())
			pSubscription = &mp_GlobalVersionSubscriptions[SubscriptionID];
		else
			pSubscription = &mp_VersionSubscriptions[_Params.m_Application][SubscriptionID];
			
		auto &Subscription = *pSubscription;
		Subscription.m_DispatchActor = fg_Move(_Params.m_DispatchActor);
		Subscription.m_fOnPermissionsChanged = fg_Move(_Params.m_fOnPermissionsChanged);
		Subscription.m_fOnNewVersion = fg_Move(_Params.m_fOnNewVersion);
		Subscription.m_HostID = _CallingHostInfo.f_GetRealHostID();
		Subscription.m_nInitial = _Params.m_nInitial;
			
		CVersionManager::CSubscribeToUpdates::CResult Result;
		Result.m_Subscription = fg_ActorSubscription
			(
				self
				, [this, ApplicationName = _Params.m_Application, SubscriptionID]
				{
					if (ApplicationName.f_IsEmpty())
					{
						mp_GlobalVersionSubscriptions.f_Remove(SubscriptionID);
						return;
					}
					auto *pSubscription = mp_VersionSubscriptions.f_FindEqual(ApplicationName);
					if (!pSubscription)
						return;
					pSubscription->f_Remove(SubscriptionID);
					if (pSubscription->f_IsEmpty())
						mp_VersionSubscriptions.f_Remove(pSubscription);
				}
			)
		;
		
		if (!_Params.m_nInitial)
			return fg_Explicit(fg_Move(Result));

		fp_SendSubscriptionInitial(_Params.m_Application, Subscription, false);
			
		return fg_Explicit(fg_Move(Result));
	}
}

