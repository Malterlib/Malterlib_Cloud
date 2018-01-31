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

	TCContinuation<CDirectorySyncReceive::CSyncResult> fg_DownloadSecretFile
		(
			TCDistributedActor<CSecretsManager> const &_SecretsManager
		 	, CSecretsManager::CSecretID &&_ID
		 	, CDirectorySyncReceive::CConfig &&_Config
		)
	{
		TCContinuation<CDirectorySyncReceive::CSyncResult> Continuation;
		DMibCallActor
			(
				_SecretsManager
				, CSecretsManager::f_DownloadFile
				, fg_Move(_ID)
				, g_ActorSubscription > [=]() -> TCContinuation<void>
				{
					// Cleanup?
					return fg_Explicit();
				}
			)
			>  Continuation	/ [=] (TCDistributedActorInterfaceWithID<CDirectorySyncClient> &&_Downloader) mutable
			{
				auto UploadReceive = fg_ConstructActor<NFile::CDirectorySyncReceive>(fg_Move(_Config), fg_Move(_Downloader));

				UploadReceive(&NFile::CDirectorySyncReceive::f_PerformSync) > Continuation;
			}
		;
		return Continuation;
	}
}
