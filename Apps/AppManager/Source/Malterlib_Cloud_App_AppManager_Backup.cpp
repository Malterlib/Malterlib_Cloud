// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Cryptography/RandomID>
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
		
		try
		{
			Application.m_BackupClient = fg_Construct(BackupConfig, mp_State.m_TrustManager);
		}
		catch (CException const &_Exception)
		{
			DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Failed to initialize backup client for '{}' app: {}", Application.m_Name, _Exception);
		}
	}
}
