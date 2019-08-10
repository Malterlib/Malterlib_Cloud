// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Cloud/BackupManager>

namespace NMib::NCloud
{
	NConcurrency::TCFuture<NFile::CDirectorySyncReceive::CSyncResult> fg_DownloadBackup
		(
			NConcurrency::TCDistributedActor<CBackupManager> _BackupManager
			, NStr::CStr _BackupSource
			, NTime::CTime _PointInTime
			, NFile::CDirectorySyncReceive::CConfig _SyncConfig
			, NConcurrency::CActorSubscription &o_Subscription
		)
	;
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NCloud;
#endif
