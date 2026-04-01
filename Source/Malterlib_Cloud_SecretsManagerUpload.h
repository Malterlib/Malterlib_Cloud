// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <Mib/Core/Core>
#include <Mib/Cloud/SecretsManager>
#include <Mib/Storage/Reference>

namespace NMib::NCloud
{
	NConcurrency::TCFuture<NFile::CDirectorySyncSend::CSyncResult> fg_UploadSecretFile
		(
			NConcurrency::TCDistributedActor<CSecretsManager> _SecretsManager
			, NConcurrency::TCActor<NConcurrency::CActorDistributionManager> _DistributionManager
			, CSecretsManager::CSecretID _ID
			, NFile::CDirectorySyncSend::CConfig _Config
			, NStorage::NReference::TCReference<NConcurrency::CActorSubscription> o_Subscription
		)
	;
}
