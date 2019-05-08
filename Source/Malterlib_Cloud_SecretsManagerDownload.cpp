// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

#include "Malterlib_Cloud_SecretsManagerUpload.h"
#include <Mib/Concurrency/ActorSubscription>

namespace NMib::NCloud
{
	using namespace NConcurrency;
	using namespace NFile;
	using namespace NStr;

	TCFuture<CDirectorySyncReceive::CSyncResult> fg_DownloadSecretFile
		(
			TCDistributedActor<CSecretsManager> const &_SecretsManager
		 	, CSecretsManager::CSecretID &&_ID
		 	, CDirectorySyncReceive::CConfig &&_Config
		)
	{
		TCPromise<CDirectorySyncReceive::CSyncResult> Promise;
		_SecretsManager.f_CallActor(&CSecretsManager::f_DownloadFile)
			(
				fg_Move(_ID)
				, g_ActorSubscription / [=]() -> TCFuture<void>
				{
					// Cleanup?
					return fg_Explicit();
				}
			)
			>  Promise	/ [=] (TCDistributedActorInterfaceWithID<CDirectorySyncClient> &&_Downloader) mutable
			{
				auto UploadReceive = fg_ConstructActor<NFile::CDirectorySyncReceive>(fg_Move(_Config), fg_Move(_Downloader));

				UploadReceive(&NFile::CDirectorySyncReceive::f_PerformSync) > Promise;
			}
		;
		return Promise.f_MoveFuture();
	}
}
