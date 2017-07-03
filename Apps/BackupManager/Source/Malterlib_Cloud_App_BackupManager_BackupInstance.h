
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
		
		TCContinuation<TCActorSubscriptionWithID<>> f_StartManifestRSync
			(
				TCActorFunctorWithID<TCContinuation<CSecureByteVector> (CSecureByteVector &&_Packet)> &&_fRunProtocol
				, uint64 _ManifestSize
			)
			override
		;

		TCContinuation<CStartBackupResult> f_StartBackup() override;

		TCContinuation<TCActorSubscriptionWithID<>> f_StartRSync
			(
				CStr const &_FileName
				, TCActorFunctorWithID<TCContinuation<CSecureByteVector> (CSecureByteVector &&_Packet)> &&_fRunProtocol
			)
			override
		;

		TCContinuation<void> f_ManifestChange(NStr::CStr const &_FileName, CManifestChange const &_Change) override;
		TCContinuation<void> f_AppendData(CStr const &_FileName, uint64 _Position, CSecureByteVector &&_Data, CManifestChange_ChangeAppend const &_ChangeAppend) override;
		TCContinuation<void> f_InitialBackupFinished() override;
		
	private:
		struct CInternal;
		TCUniquePointer<CInternal> mp_pInternal;
	};
}
