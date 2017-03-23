// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

#include <Mib/Cloud/BackupManager>
#include <Mib/File/RSync>

#include "Malterlib_Cloud_BackupManagerClient.h"
#include "Malterlib_Cloud_BackupManagerClient_Internal.h"

using namespace NMib; 
using namespace NMib::NConcurrency; 
using namespace NMib::NContainer; 
using namespace NMib::NPtr; 
using namespace NMib::NFile; 
using namespace NMib::NStr; 
using namespace NMib::NDataProcessing; 

namespace NMib::NCloud::NPrivate
{
	struct CBackupManagerClient_Instance : public CActor
	{
		using CActorHolder = CSeparateThreadActorHolder;
		
		CBackupManagerClient_Instance
			(
				TCDistributedActor<CBackupManager> const &_BackupManager
				, CBackupManagerBackup::CManifest const &_Manifest
				, CBackupManagerClient::CConfig const &_Config
				, CTrustedActorInfo const &_ActorInfo
				, TCWeakActor<CBackupManagerClient> const &_BackupManagerClient
				, CBackupManager::CBackupKey const &_BackupKey 
			)
		;
		~CBackupManagerClient_Instance();
		
		void f_ManifestChanged(CStr const &_FileName, CBackupManagerClient::CInternal::CUpdateManifestResult const &_ManifestUpdate);
		
	private:
		TCContinuation<void> fp_Destroy() override;
		void fp_StartBackup();
		void fp_BackupNotification(CBackupManagerClient::CNotification &&_Notification);
		void fp_ReportBackupFailed(CStr const &_Error);
		
		void fp_ProcessBackupQueue();

		void fp_NewPendingFile(CStr const &_FileName);
		void fp_FileFinished(CStr const &_FileName);
		void fp_ProcessManifestChange(CStr const &_FileName, CBackupManagerClient::CInternal::CUpdateManifestResult const &_ManifestUpdate);
		
		struct CRunningSyncState : public TCSharedPointerIntrusiveBase<ESharedPointerOption_SupportWeakPointer>
		{
			CRunningSyncState();
			~CRunningSyncState();
			
			TCBinaryStreamFile<> m_File;
			TCUniquePointer<CRSyncServer> m_pRSyncServer;
			COnScopeExitShared m_pOnScopeExit;
			TCSharedPointer<CCanDestroyTracker> m_pCanDestroyTracker;
		};
		
		struct CPendingBackupFile
		{
			CStr const &f_GetFileName() const
			{
				return TCMap<CStr, CPendingBackupFile>::fs_GetKey(*this);
			}
			
			~CPendingBackupFile();
			
			DMibListLinkDS_Link(CPendingBackupFile, m_Link);
			
			TCWeakPointer<CRunningSyncState> m_pRunningState;
			
			CActorSubscription m_RSyncSubscription;
			mint m_SyncSequence = 0; 
			
			bool m_bReschedule = false;
			bool m_bFinished = false;
		};

		TCSharedPointer<CCanDestroyTracker> mp_pCanDestroyTracker = fg_Construct();

		TCDistributedActor<CBackupManager> mp_BackupManager;		
		TCWeakActor<CBackupManagerClient> mp_BackupManagerClient;
		CBackupManagerClient::CConfig mp_Config;
		CTrustedActorInfo mp_ActorInfo;
		
		CBackupManagerBackup::CManifest mp_Manifest;
		
		CBackupManager::CBackupKey mp_BackupKey;
		TCDistributedActorInterface<CBackupManagerBackup> mp_Backup;
		TCSet<CStr> mp_InitialBackupPendingAdded;
		TCSet<CStr> mp_InitialBackupPending;
		
		TCMap<CStr, CPendingBackupFile> mp_PendingFiles;
		DMibListLinkDS_List(CPendingBackupFile, m_Link) mp_PendingFilesQueue;

		TCMap<CStr, CBackupManagerClient::CInternal::CUpdateManifestResult> mp_PendingManifestUpdates;
		
		mint mp_nRunningSyncs = 0;
		mint mp_nMaxRunningSyncs = 8;
		mint mp_SyncSequence = 0;
		
		bool mp_bBackupStarted = false;
		bool mp_bBackupStartFailed = false;
		bool mp_bInitialBackupFinished = false;
	};
}
