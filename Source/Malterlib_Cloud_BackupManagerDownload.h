// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Cloud/BackupManager>

namespace NMib::NCloud
{
	NConcurrency::TCDispatchedActorCall<NFile::CDirectorySyncReceive::CSyncResult> fg_DownloadBackup
		(
			NConcurrency::TCDistributedActor<CBackupManager> const &_BackupManager
			, NStr::CStr const &_BackupSource
			, CBackupManager::CBackupID const &_DownloadBackupID
			, NTime::CTime const &_PointInTime
			, NFile::CDirectorySyncReceive::CConfig &&_SyncConfig
			, NConcurrency::CActorSubscription &o_Subscription
		)
	;
}

#ifndef DMibPNoShortCuts
using namespace NMib::NCloud;
#endif
