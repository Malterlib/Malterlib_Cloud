// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Cloud/SecretsManager>

namespace NMib::NCloud
{
	NConcurrency::TCFuture<NFile::CDirectorySyncSend::CSyncResult> fg_UploadSecretFile
		(
			NConcurrency::TCDistributedActor<CSecretsManager> _SecretsManager
		 	, NConcurrency::TCActor<NConcurrency::CActorDistributionManager> _DistributionManager
		 	, CSecretsManager::CSecretID _ID
			, NFile::CDirectorySyncSend::CConfig _Config
			, NConcurrency::CActorSubscription &o_Subscription
		)
	;
}
