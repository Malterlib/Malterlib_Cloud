// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

#include "Malterlib_Cloud_BackupManagerClient.h"
#include "Malterlib_Cloud_BackupManagerClient_Internal.h"

namespace NMib::NCloud
{
	NConcurrency::TCContinuation<NConcurrency::CActorSubscription> CBackupManagerClient::f_SubscribeNotifications
		(
			ENotification _ToSubscribeTo 
			, NConcurrency::TCActorFunctor<NConcurrency::TCContinuation<void> (NConcurrency::CHostInfo const &_RemoteHost, CNotification &&_Notification)> &&_fOnFinished
		)
	{
		return {};
	}

	void CBackupManagerClient::fp_OnNotification(NConcurrency::CHostInfo const &_RemoteHost, CNotification &&_Notification)
	{
	}
}
