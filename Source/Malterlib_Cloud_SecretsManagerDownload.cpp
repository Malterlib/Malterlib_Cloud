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

	DMibSuppressUndefinedSanitizerLinux TCFuture<CDirectorySyncReceive::CSyncResult> fg_DownloadSecretFile
		(
			TCDistributedActor<CSecretsManager> _SecretsManager
		 	, CSecretsManager::CSecretID _ID
		 	, CDirectorySyncReceive::CConfig _Config
		)
	{
		TCDistributedActorInterfaceWithID<CDirectorySyncClient> Downloader = co_await _SecretsManager.f_CallActor(&CSecretsManager::f_DownloadFile)
			(
				fg_Move(_ID)
				, g_ActorSubscription / [=]() -> TCFuture<void>
				{
					// Cleanup?
					co_return {};
				}
			)
		;

		if (!Downloader)
			co_return DMibErrorInstance("Invalid downloader");

		auto UploadReceive = fg_ConstructActor<NFile::CDirectorySyncReceive>(fg_Move(_Config), fg_Move(Downloader));

		co_return co_await UploadReceive(&NFile::CDirectorySyncReceive::f_PerformSync);
	}
}
