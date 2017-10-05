// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

#include "Malterlib_Cloud_App_SecretsManager.h"
#include "Malterlib_Cloud_App_SecretsManager_Server.h"

namespace NMib::NCloud::NSecretsManager
{	
	TCContinuation<CActorSubscription> CSecretsManagerDaemonActor::CServer::CSecretsManagerImplementation::f_DownloadFile(CFileTransferContext &&_TransferContext)
	{
		TCContinuation<CActorSubscription> Continuation;
		return Continuation;
	}

	auto CSecretsManagerDaemonActor::CServer::CSecretsManagerImplementation::f_UploadFile
		(
			TCActorFunctorWithID<TCContinuation<void> (CFileTransferContext &&_TransferContext)> &&_fOnNotification
		)
		-> TCContinuation<TCActorSubscriptionWithID<>>
	{
		TCContinuation<TCActorSubscriptionWithID<>> Continuation;
		return Continuation;
	}
}
