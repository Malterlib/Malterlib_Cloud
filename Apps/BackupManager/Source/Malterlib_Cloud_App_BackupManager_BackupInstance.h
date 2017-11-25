
#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Cloud/BackupManager>

#include "Malterlib_Cloud_App_BackupManager_BackupSource.h"

namespace NMib::NCloud::NBackupManager
{
	class CBackupInstance : public CBackupManagerBackup
	{
	public:
		using CActorHolder = CSeparateThreadActorHolder;
		
		CBackupInstance(CStr const &_Name, CTime const &_StartTime, CStr const &_ID, CStr const &_RootDirectory, bool _bForceNew, TCActor<CBackupSource> const &_BackupSource);
		~CBackupInstance();
		
		TCContinuation<TCActorSubscriptionWithID<>> f_StartManifestRSync(FRunRSyncProtocol &&_fRunProtocol, uint64 _ManifestSize, CHashDigest_SHA256 const &_ExpectedDigest) override;
		TCContinuation<CStartBackupResult> f_StartBackup() override;
		TCContinuation<TCActorSubscriptionWithID<>> f_StartRSync(CStr const &_FileName, CManifestFile const &_ManifestFile, FRunRSyncProtocol &&_fRunProtocol) override;

		TCContinuation<void> f_ManifestChange(NStr::CStr const &_FileName, CManifestChange const &_Change) override;
		TCContinuation<void> f_AppendData(CStr const &_FileName, CAppendData &&_Data) override;
		TCContinuation<CInitialBackupFinishedResult> f_InitialBackupFinished(EInitialBackupFinishedFlag _FinishedFlags) override;

	private:
		TCContinuation<void> fp_Destroy() override;

		struct CInternal;
		TCUniquePointer<CInternal> mp_pInternal;
	};
}
