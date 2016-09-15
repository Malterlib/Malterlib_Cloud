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
	void CVersionManagerDaemonActor::CServer::fp_NewVersion
		(
			CStr const &_ApplicationName
			, CVersionManager::CVersionIdentifier const &_VersionID
			, CVersionManager::CVersionInformation const &_VersionInfo
		)
	{
		CVersionManager::CNewVersionNotifications NewVersionNotifications;
		auto &NewVersionNotification = NewVersionNotifications.m_NewVersions.f_Insert();
		NewVersionNotification.m_Application = _ApplicationName;
		NewVersionNotification.m_VersionID = _VersionID;
		NewVersionNotification.m_VersionInfo = _VersionInfo;
		
		fp_NewTagsKnown(_VersionInfo.m_Tags);
		
		TCVector<CStr> Permissions;
		Permissions.f_Insert("Application/ReadAll");
		Permissions.f_Insert("Application/ListAll");
		Permissions.f_Insert(fg_Format("Application/Read/{}", _ApplicationName));
		
		auto fSendToSubscription = [&](CSubscription const &_Subscription)
			{
				if (!mp_Permissions.f_HostHasAnyPermission(_Subscription.m_HostID, Permissions))
					return;
				_Subscription.f_SendVersions(NewVersionNotifications);
			}
		;
		for (auto &Subscription : mp_GlobalVersionSubscriptions)
			fSendToSubscription(Subscription);
		auto *pSubscription = mp_VersionSubscriptions.f_FindEqual(_ApplicationName);
		if (pSubscription)
		{
			for (auto &Subscription : *pSubscription)
				fSendToSubscription(Subscription);
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

		CVersionManager::CNewVersionNotifications NewVersionNotifications;

		NewVersionNotifications.m_bFullResend = true; 
		
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
				
				mint nToSend = _Subscription.m_nInitial;
				decltype(_Application.m_VersionsByTime.f_GetIterator()) iVersion;
				for (iVersion.f_StartBackward(_Application.m_VersionsByTime); iVersion && nToSend; --iVersion, --nToSend)
				{
					auto &Version = *iVersion;
					auto &NewVersionNotification = NewVersionNotifications.m_NewVersions.f_Insert();
					NewVersionNotification.m_Application = _Application.f_GetName();
					NewVersionNotification.m_VersionID = Version.f_GetIdentifier();
					NewVersionNotification.m_VersionInfo = Version.m_VersionInfo;
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
		
		_Subscription.f_SendVersions(NewVersionNotifications);
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
		Subscription.m_fOnNewVersions = fg_Move(_Params.m_fOnNewVersions);
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

