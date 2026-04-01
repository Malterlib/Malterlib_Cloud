// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "Malterlib_Cloud_App_BackupManager.h"
#include "Malterlib_Cloud_App_BackupManager_Internal.h"
#include <Mib/Cryptography/Hashes/SHA>

namespace NMib::NCloud::NBackupManager
{
	TCFuture<TCVector<CStr>> CBackupManagerServer::fp_FilterBackupSourcesByPermissions(TCVector<CStr> _Sources)
	{
		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;
		Permissions["//ALL//"] = {{"Backup/ListAll", "Backup/ReadAll"}};
		for (auto &Backup : _Sources)
			Permissions[Backup] = {CPermissionQuery{fg_Format("Backup/Read/{}", Backup)}.f_Description("Access source {} in BackupManager"_f << Backup)};

		auto const HasPermissions = co_await mp_Permissions.f_HasPermissions("List backup sources", Permissions);

		TCVector<CStr> BackupSources;
		bool bListAllAccess = HasPermissions["//ALL//"];

		for (auto &Backup : _Sources)
		{
			auto pHasPermission = HasPermissions.f_FindEqual(Backup);
			if (!bListAllAccess && (!pHasPermission || !*pHasPermission))
				continue;
			BackupSources.f_Insert(Backup);
		}

		co_return fg_Move(BackupSources);
	}

	TCFuture<TCVector<CStr>> CBackupManagerServer::CBackupManagerImplementation::f_ListBackupSources()
	{
		auto pThis = m_pThis;
		auto OnResume = co_await pThis->f_CheckDestroyedOnResume();

		auto Auditor = pThis->mp_AppState.f_Auditor();

		Auditor.f_Info("Listing backup sources");

		auto Sources = co_await (pThis->fp_FilterBackupSourcesByPermissions(pThis->fp_EnumBackupSources()) % "Permission denied listing backup sources" % Auditor);

		Auditor.f_Info(fg_Format("Listed backup sources: {vs,vb}", Sources));

		co_return fg_Move(Sources);
	}

	auto CBackupManagerServer::CBackupManagerImplementation::f_ListBackups(CStr _ForBackupSource) -> TCFuture<TCMap<CStr, CBackupInfo>>
	{
		auto pThis = m_pThis;
		auto OnResume = co_await pThis->f_CheckDestroyedOnResume();

		auto Auditor = pThis->mp_AppState.f_Auditor();

		Auditor.f_Info("Listing backups");

		bool bSingleSource = false;

		TCVector<CStr> BackupSources;

		if (!_ForBackupSource.f_IsEmpty())
		{
			bSingleSource = true;

			if (!CBackupManager::fs_IsValidBackupSource(_ForBackupSource, nullptr, nullptr))
				co_return Auditor.f_Exception({"Invalid backup source format", "(List backups)"});

			BackupSources.f_Insert(_ForBackupSource);
		}
		else
			BackupSources = pThis->fp_EnumBackupSources();

		auto FilteredBackupSources = co_await (pThis->fp_FilterBackupSourcesByPermissions(BackupSources) % "Permission denied listing backups" % Auditor);
		if (bSingleSource)
		{
			if (FilteredBackupSources.f_IsEmpty())
				co_return Auditor.f_AccessDenied("(List backups)", {"Backup/ListAll", "Backup/ReadAll", "Backup/Read/*"});

			auto *pBackupSource = pThis->mp_BackupSources.f_FindEqual(_ForBackupSource);
			if (!pBackupSource)
				co_return Auditor.f_Exception({"No such backup source", "(List backups)"});
		}

		TCFutureMap<CStr, CBackupInfo> BackupInfos;
		for (auto &BackupSourceID : FilteredBackupSources)
		{
			auto *pBackupSource = pThis->mp_BackupSources.f_FindEqual(BackupSourceID);
			DMibCheck(pBackupSource);
			if (!pBackupSource)
				continue;

			(*pBackupSource)(&CBackupSource::f_GetInfo) > BackupInfos[BackupSourceID];
		}

		auto FilteredBackupInfos = co_await (fg_AllDoneWrapped(BackupInfos) % Auditor);

		TCMap<CStr, CBackupInfo> Results;
		for (auto &BackupInfo : FilteredBackupInfos)
		{
			auto &BackupSource = FilteredBackupInfos.fs_GetKey(BackupInfo);
			if (!BackupInfo)
			{
				DMibLogWithCategory
					(
						Mib/Cloud/BackupManager
						, Error
						, "Failed to get backup info for backup source '{}': {}"
						, BackupSource
						, BackupInfo.f_GetExceptionStr()
					)
				;
				continue;
			}
			Results[BackupSource] = fg_Move(*BackupInfo);
		}

		co_return fg_Move(Results);
	}
}
