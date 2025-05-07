// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JsonShortcuts>
#include <Mib/Cryptography/RandomID>
#include <Mib/Concurrency/ActorSubscription>

#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud::NAppManager
{
	void CAppManagerActor::fp_ApplicationStartBackup(TCSharedPointer<CApplication> const &_pApplication)
	{
		auto &Application = *_pApplication;
		CBackupManagerClient::CConfig BackupConfig;
		
		auto &ManifestConfig = BackupConfig.m_ManifestConfig;
		
		ManifestConfig.m_Root = Application.f_GetDirectory();
		ManifestConfig.m_IncludeWildcards = Application.m_Settings.m_Backup_IncludeWildcards;
		ManifestConfig.m_ExcludeWildcards = Application.m_Settings.m_Backup_ExcludeWildcards;
		ManifestConfig.m_AddSyncFlagsWildcards = Application.m_Settings.m_Backup_AddSyncFlagsWildcards;
		ManifestConfig.m_RemoveSyncFlagsWildcards = Application.m_Settings.m_Backup_RemoveSyncFlagsWildcards;
		
		BackupConfig.m_BackupIdentifier = Application.m_Name;
		BackupConfig.m_NewBackupInterval = Application.m_Settings.m_Backup_NewBackupInterval;
		BackupConfig.m_LogCategory = Application.m_Name + "/Backup";
		
		Application.m_BackupClient = fg_Construct
			(
				BackupConfig
				, mp_State.m_TrustManager
				, g_ActorFunctor / [_pApplication]
				(
					TCDistributedActorInterfaceWithID<CDistributedAppInterfaceBackup> _BackupInterface
					, NConcurrency::CActorSubscription _ManifestFinished
					, NStr::CStr _BackupRoot
				) -> TCFuture<TCActorSubscriptionWithID<>>
				{
					if (!_BackupInterface)
						co_return DMibErrorInstance("Invalid backup interface");

					if (_pApplication->m_bDeleted)
						co_return {};

					if (!_pApplication->m_Settings.m_bDistributedApp)
						co_return {};

					if (!_pApplication->m_AppInterface || _pApplication->m_LaunchStatus != "Launched")
						co_await _pApplication->m_OnStartedDistributedApp.f_Insert().f_Future();

					if (!_pApplication->m_AppInterface || _pApplication->m_AppInterface->f_InterfaceVersion() < CDistributedAppInterfaceClient::EProtocolVersion_SupportStartBackup)
						co_return {};

					co_await _pApplication->m_AppInterface.f_CallActor(&CDistributedAppInterfaceClient::f_StartBackup)
						(
							fg_Move(_BackupInterface)
							, fg_Move(_ManifestFinished)
							, _BackupRoot
						)
					;

					co_return {};
				}
				, mp_State.m_DistributionManager
			)
		;

		Application.m_BackupClient(&CBackupManagerClient::f_StartBackup) > [ApplicationName = Application.m_Name](TCAsyncResult<void> &&_Result)
			{
				if (!_Result)
					DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Failed to start backup client for '{}' app: {}", ApplicationName, _Result.f_GetExceptionStr());
			}
		;
	}
}
