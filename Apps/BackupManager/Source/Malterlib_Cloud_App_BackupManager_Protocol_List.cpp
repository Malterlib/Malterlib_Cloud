
#include "Malterlib_Cloud_App_BackupManager.h"
#include "Malterlib_Cloud_App_BackupManager_Internal.h"
#include <Mib/Cryptography/Hashes/SHA>

namespace NMib::NCloud::NBackupManager
{
	TCContinuation<TCVector<CStr>> CBackupManagerServer::fp_FilterBackupSourcesByPermissions(TCVector<CStr> const &_Sources)
	{
		TCContinuation<TCVector<CStr>> Continuation;
		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;
		Permissions["//ALL//"] = {{"Backup/ListAll", "Backup/ReadAll"}};
		for (auto &Backup : _Sources)
			Permissions[Backup] = {CPermissionQuery{fg_Format("Backup/Read/{}", Backup)}.f_Description("Access source {} in BackupManager"_f << Backup)};

		mp_Permissions.f_HasPermissions("List backup sources", Permissions) > Continuation / [Continuation, _Sources](NContainer::TCMap<NStr::CStr, bool> const &_HasPermissions)
			{
				TCVector<CStr> BackupSources;
				bool bListAllAccess = _HasPermissions["//ALL//"];

				for (auto &Backup : _Sources)
				{
					auto pHasPermission = _HasPermissions.f_FindEqual(Backup);
					if (!bListAllAccess && (!pHasPermission || !*pHasPermission))
						continue;
					BackupSources.f_Insert(Backup);
				}

				Continuation.f_SetResult(fg_Move(BackupSources));
			}
		;
		return Continuation;
	}

	TCContinuation<TCVector<CStr>> CBackupManagerServer::CBackupManagerImplementation::f_ListBackupSources()
	{
		auto pThis = m_pThis;
		if (pThis->f_IsDestroyed())
			return DMibErrorInstance("Shutting down");
			
		auto Auditor = pThis->mp_AppState.f_Auditor();
		NConcurrency::TCContinuation<TCVector<CStr>> Continuation;

		Auditor.f_Info("Listing backup sources");
		
		pThis->fp_FilterBackupSourcesByPermissions(pThis->fp_EnumBackupSources()) > Continuation / [=](TCVector<CStr> &&_Sources)
			{
				Auditor.f_Info(fg_Format("Listed backup sources: {vs,vb}", _Sources));
				Continuation.f_SetResult(fg_Move(_Sources));
			}
		;

		return Continuation;
	}

	auto CBackupManagerServer::CBackupManagerImplementation::f_ListBackups(CStr const &_ForBackupSource) -> TCContinuation<TCMap<CStr, CBackupInfo>>
	{
		auto pThis = m_pThis;
		if (pThis->f_IsDestroyed())
			return DMibErrorInstance("Shutting down");

		auto Auditor = pThis->mp_AppState.f_Auditor();

		Auditor.f_Info("Listing backups");

		bool bSingleSource = false;

		TCVector<CStr> BackupSources;

		if (!_ForBackupSource.f_IsEmpty())
		{
			bSingleSource = true;

			if (!CBackupManager::fs_IsValidBackupSource(_ForBackupSource, nullptr, nullptr))
				return Auditor.f_Exception({"Invalid backup source format", "(List backups)"});

			BackupSources.f_Insert(_ForBackupSource);
		}
		else
			BackupSources = pThis->fp_EnumBackupSources();

		NConcurrency::TCContinuation<TCMap<CStr, CBackupInfo>> Continuation;

		pThis->fp_FilterBackupSourcesByPermissions(BackupSources) > Continuation / [=](TCVector<CStr> &&_BackupSources)
			{
				if (bSingleSource)
				{
					if (_BackupSources.f_IsEmpty())
						return Continuation.f_SetException(Auditor.f_AccessDenied("(List backups)"));

					auto *pBackupSource = pThis->mp_BackupSources.f_FindEqual(_ForBackupSource);
					if (!pBackupSource)
						return Continuation.f_SetException(Auditor.f_Exception({"No such backup source", "(List backups)"}));
				}

				TCActorResultMap<CStr, CBackupInfo> BackupInfos;
				for (auto &BackupSourceID : _BackupSources)
				{
					auto *pBackupSource = pThis->mp_BackupSources.f_FindEqual(BackupSourceID);
					DMibCheck(pBackupSource);
					if (!pBackupSource)
						continue;

					(*pBackupSource)(&CBackupSource::f_GetInfo) > BackupInfos.f_AddResult(BackupSourceID);
				}

				BackupInfos.f_GetResults() > Continuation % Auditor / [=](TCMap<CStr, TCAsyncResult<CBackupInfo>> &&_BackupInfos)
					{
						TCMap<CStr, CBackupInfo> Results;
						for (auto &BackupInfo : _BackupInfos)
						{
							auto &BackupSource =_BackupInfos.fs_GetKey(BackupInfo);
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

						Continuation.f_SetResult(fg_Move(Results));
					}
				;
			}
		;

		return Continuation;
	}
}
