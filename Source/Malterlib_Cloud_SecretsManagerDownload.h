// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Cloud/SecretsManager>

namespace NMib::NCloud
{
	NConcurrency::TCContinuation<NFile::CDirectorySyncReceive::CSyncResult> fg_DownloadSecretFile
		(
			NConcurrency::TCDistributedActor<CSecretsManager> const &_SecretsManager
		 	, CSecretsManager::CSecretID &&_ID
		 	, NFile::CDirectorySyncReceive::CConfig &&_Config
		)
	;
}
