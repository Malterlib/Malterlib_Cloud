// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Cryptography/RandomID>
#include <Mib/Concurrency/ActorSubscription>

#include "Malterlib_Cloud_BackupManagerClient.h"
#include "Malterlib_Cloud_BackupManagerClient_Internal.h"

namespace NMib::NCloud
{
	NConcurrency::TCContinuation<NConcurrency::CActorSubscription> CBackupManagerClient::f_SubscribeNotifications
		(
			ENotification _ToSubscribeTo
			, NConcurrency::TCActorFunctor<NConcurrency::TCContinuation<void> (NConcurrency::CHostInfo const &_RemoteHost, CNotification &&_Notification)> &&_fOnNotification
		)
	{
		auto &Internal = *mp_pInternal;
		CStr SubscriptionID = NCryptography::fg_RandomID();
		
		auto &Subscription = Internal.m_NotificationSubscriptions[SubscriptionID];
		
		Subscription.m_Notifications = _ToSubscribeTo;
		Subscription.m_fOnNotification = fg_Move(_fOnNotification);

		NConcurrency::CHostInfo DummyHostInfo;

		for (auto &Notification : Internal.m_LastNotification)
		{
			if (!(Subscription.m_Notifications & Notification.m_Notification.f_GetTypeID()))
				continue;
			Subscription.m_fOnNotification(Notification.m_RemoteHost, fg_TempCopy(Notification.m_Notification)) > fg_DiscardResult();
		}

		return g_Explicit = g_ActorSubscription > [this, SubscriptionID]() -> TCContinuation<void>
			{
				auto &Internal = *mp_pInternal;

				auto *pSubscription = Internal.m_NotificationSubscriptions.f_FindEqual(SubscriptionID);
				if (!pSubscription)
					return fg_Explicit();
				
				auto Continuation = pSubscription->m_fOnNotification.f_Destroy();

				Internal.m_NotificationSubscriptions.f_Remove(SubscriptionID);
				
				return Continuation;
			}
		;
	}

	void CBackupManagerClient::fp_OnNotification(NConcurrency::CHostInfo const &_RemoteHost, CNotification &&_Notification)
	{
		auto &Internal = *mp_pInternal;

		auto &Notification = Internal.m_LastNotification[_Notification.f_GetTypeID()];
		Notification.m_Notification = fg_Move(_Notification);
		Notification.m_RemoteHost = _RemoteHost;

		for (auto &Subscription : Internal.m_NotificationSubscriptions)
		{
			if (!(Subscription.m_Notifications & Notification.m_Notification.f_GetTypeID()))
				continue;
			Subscription.m_fOnNotification(_RemoteHost, fg_TempCopy(Notification.m_Notification)) > fg_DiscardResult();
		}
		
		if (Notification.m_Notification.f_GetTypeID() == ENotification_InitialFinished)
		{
			if (!Internal.m_bInitialFinished)
			{
				Internal.m_bInitialFinished = true;
				
				for (auto &Subscription : Internal.m_OnInitialFinishedSubscriptions)
					Subscription() > fg_DiscardResult();
			}
		}
		else if (Notification.m_Notification.f_GetTypeID() == ENotification_Quiescent)
			Internal.m_LastNotification.f_Remove(ENotification_Unquiescent);
		else if (Notification.m_Notification.f_GetTypeID() == ENotification_Unquiescent)
			Internal.m_LastNotification.f_Remove(ENotification_Quiescent);
	}

	void CBackupManagerClient::CInternal::f_ReportBackupError(CStr const &_Error, bool _bFatal)
	{
		DMibLogCategoryStr(m_Config.m_LogCategory);
		DMibLog(Error, "{}", _Error);
		m_pThis->fp_OnNotification({}, CBackupManagerClient::CNotification_BackupError{_Error, _bFatal});
	}

	TCContinuation<TCActorSubscriptionWithID<>> CBackupManagerClient::CInternal::CDistributedAppInterfaceBackupImplementation::f_SubscribeInitialFinished
		(
			TCActorFunctorWithID<TCContinuation<void> ()> &&_fOnInitialFinished
		)
	{
		auto &Internal = *m_pThis->mp_pInternal;
		CStr SubscriptionID = NCryptography::fg_RandomID();
		
		auto &fOnFinished = *Internal.m_OnInitialFinishedSubscriptions(SubscriptionID, fg_Move(_fOnInitialFinished));
		
		if (Internal.m_bInitialFinished)
			fOnFinished() > fg_DiscardResult();
		
		return g_Explicit = g_ActorSubscription > [pThis = m_pThis, SubscriptionID]() -> TCContinuation<void>
			{
				auto &Internal = *pThis->mp_pInternal;

				auto *pSubscription = Internal.m_OnInitialFinishedSubscriptions.f_FindEqual(SubscriptionID);
				if (!pSubscription)
					return fg_Explicit();
				
				auto Continuation = pSubscription->f_Destroy();

				Internal.m_OnInitialFinishedSubscriptions.f_Remove(SubscriptionID);
				
				return Continuation;
			}
		;
	}
	
	TCContinuation<TCActorSubscriptionWithID<>> CBackupManagerClient::CInternal::CDistributedAppInterfaceBackupImplementation::f_SubscribeBackupStopped
		(
			TCActorFunctorWithID<TCContinuation<void> ()> &&_fOnStopped
		)
	{
		auto &Internal = *m_pThis->mp_pInternal;
		CStr SubscriptionID = NCryptography::fg_RandomID();
		
		auto &fOnStopped = *Internal.m_OnBackupStoppedSubscriptions(SubscriptionID, fg_Move(_fOnStopped));
		
		if (Internal.m_bStopped)
			fOnStopped() > fg_DiscardResult();
		
		return g_Explicit = g_ActorSubscription > [pThis = m_pThis, SubscriptionID]() -> TCContinuation<void>
			{
				auto &Internal = *pThis->mp_pInternal;

				auto *pSubscription = Internal.m_OnBackupStoppedSubscriptions.f_FindEqual(SubscriptionID);
				if (!pSubscription)
					return fg_Explicit();
				
				auto Continuation = pSubscription->f_Destroy();

				Internal.m_OnBackupStoppedSubscriptions.f_Remove(SubscriptionID);
				
				return Continuation;
			}
		;
	}
}
