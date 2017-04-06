
#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Cloud/BackupManager>

namespace NMib::NCloud::NBackupManager
{
	class CBackupInstance : public CBackupManagerBackup
	{
	public:
		using CActorHolder = CSeparateThreadActorHolder;
		
		CBackupInstance(CStr const &_Name, CTime const &_StartTime, CStr const &_ID);
		~CBackupInstance();
		
		TCContinuation<CStartBackupResult> f_StartBackup(CManifest const &_Manifest) override;

		TCContinuation<TCActorSubscriptionWithID<>> f_StartRSync
			(
				CStr const &_FileName
				, TCActorFunctorWithID<TCContinuation<CSecureByteVector> (CSecureByteVector &&_Packet)> &&_fRunProtocol
			)
			override
		;

		NConcurrency::TCContinuation<void> f_ManifestChange(NStr::CStr const &_FileName, CManifestChange const &_Change) override;
		TCContinuation<void> f_UploadData(CStr const &_FileName, uint64 _Position, CSecureByteVector &&_Data) override;
		TCContinuation<void> f_InitialBackupFinished() override;
		
	private:
		struct CInternal;
		TCUniquePointer<CInternal> mp_pInternal;
	};
}
