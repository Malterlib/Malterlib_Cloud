// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Cloud/BackupManager>
#include <Mib/Storage/Reference>

namespace NMib::NCloud
{
	auto fg_DownloadBackup
		(
			NConcurrency::TCDistributedActor<CBackupManager> _BackupManager
			, NStr::CStr _BackupSource
			, NTime::CTime _PointInTime
			, NFile::CDirectorySyncReceive::CConfig _SyncConfig
			, NStorage::NReference::TCReference<NConcurrency::CActorSubscription> o_Subscription // NOLINT(malterlib-parameter-naming): it can be written through
		)
		-> NConcurrency::TCFuture<NFile::CDirectorySyncReceive::CSyncResult>
	;
}

#ifndef DMibPNoShortCuts
using namespace NMib::NCloud;
#endif
