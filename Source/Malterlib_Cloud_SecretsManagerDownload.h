// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <Mib/Core/Core>
#include <Mib/Cloud/SecretsManager>

namespace NMib::NCloud
{
	NConcurrency::TCFuture<NFile::CDirectorySyncReceive::CSyncResult> fg_DownloadSecretFile
		(
			NConcurrency::TCDistributedActor<CSecretsManager> _SecretsManager
			, CSecretsManager::CSecretID _ID
			, NFile::CDirectorySyncReceive::CConfig _Config
		)
	;
}
