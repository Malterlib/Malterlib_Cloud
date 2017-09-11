
#include "Malterlib_Cloud_App_BackupManager.h"
#include "Malterlib_Cloud_App_BackupManager_Internal.h"
#include <Mib/Cryptography/Hashes/SHA>

namespace NMib::NCloud::NBackupManager
{
	namespace
	{
		auto g_fFindBackupSources = []() -> TCVector<CStr>
			{
				CStr FindPath = CFile::fs_GetProgramDirectory() + "/Backups";
				CFile::CFindFilesOptions FindOptions(FindPath + "/*_*", false);
				FindOptions.m_AttribMask = EFileAttrib_Directory;
				auto FoundFiles = CFile::fs_FindFiles(FindOptions);
				TCVector<CStr> BackupSources;
				for (auto &File : FoundFiles)
				{
					CStr BackupSource = File.m_Path.f_Extract(FindPath.f_GetLen() + 1);
					if (CBackupManager::fs_IsValidBackupSource(BackupSource, nullptr, nullptr))
						BackupSources.f_Insert(BackupSource);
				}
				return BackupSources;
			}
		;
	}
	
	TCVector<CStr> CBackupManagerServer::fp_FilterBackupSourcesByPermissions(CStr const &_CallingHostID, TCVector<CStr> const &_Sources)
	{
		TCVector<CStr> BackupSources;
		
		bool bListAllAccess = mp_Permissions.f_HostHasAnyPermission(_CallingHostID, "Backup/ListAll", "Backup/ReadAll");
		
		for (auto &Backup : _Sources)
		{
			if (!bListAllAccess && !mp_Permissions.f_HostHasPermission(_CallingHostID, fg_Format("Backup/Read/{}", Backup)))
				continue;
			BackupSources.f_Insert(Backup);
		}
		
		return BackupSources;
	}

	TCContinuation<TCVector<CStr>> CBackupManagerServer::CBackupManagerImplementation::f_ListBackupSources()
	{
		auto pThis = m_pThis;
		if (pThis->f_IsDestroyed())
			return DMibErrorInstance("Shutting down");
			
		auto Auditor = pThis->mp_AppState.f_Auditor();
		NConcurrency::TCContinuation<TCVector<CStr>> Continuation;
		auto QueryFileActor = pThis->fp_GetQueryFileActor();

		Auditor.f_Info("Listing backup sources");
		
		CStr CallingHostID = fg_GetCallingHostID();
		
		fg_Dispatch
			(
				QueryFileActor
				, g_fFindBackupSources
			)
			> [pThis, Continuation, Auditor, CallingHostID](TCAsyncResult<TCVector<CStr>> &&_Result)
			{
				if (!_Result)
				{
					CStr DetailedErrror = fg_Format("Error listing backup sources: {}", _Result.f_GetExceptionStr());
					Continuation.f_SetException(Auditor.f_Exception({"File error when running query. Consult logs on backup server to diagnose.", DetailedErrror}));
					return;
				}
				TCVector<CStr> BackupSources;
				BackupSources = pThis->fp_FilterBackupSourcesByPermissions(CallingHostID, *_Result);

				Auditor.f_Info(fg_Format("Listed backup sources: {vs,vb}", BackupSources));
				
				Continuation.f_SetResult(fg_Move(BackupSources));
			}
		;
		return Continuation;
	}

	auto CBackupManagerServer::CBackupManagerImplementation::f_ListBackups(CStr const &_ForBackupSource) -> TCContinuation<TCMap<CStr, TCVector<CBackupID>>>
	{
		auto pThis = m_pThis;
		if (pThis->f_IsDestroyed())
			return DMibErrorInstance("Shutting down");
			
		auto Auditor = pThis->mp_AppState.f_Auditor();
		auto CallingHostID = fg_GetCallingHostID();
			
		NConcurrency::TCContinuation<TCMap<CStr, TCVector<CBackupID>>> Continuation;
		auto QueryFileActor = pThis->fp_GetQueryFileActor();

		Auditor.f_Info("Listing backups");
		
		auto fListBackups = [pThis, Continuation, Auditor](TCVector<CStr> const &_BackupSources)
			{
				auto QueryFileActor = pThis->fp_GetQueryFileActor();
				
				fg_Dispatch
					(
						QueryFileActor
						, [_BackupSources]() -> TCMap<CStr, TCVector<CBackupID>>
						{
							CStr ProgramDirectory = CFile::fs_GetProgramDirectory();
							TCMap<CStr, TCVector<CBackupID>> BackupsPerSource;
							for (auto &BackupSource : _BackupSources)
							{
								auto &Backups = BackupsPerSource[BackupSource];
								CStr FindPath = fg_Format("{}/Backups/{}", ProgramDirectory, BackupSource);
								CFile::CFindFilesOptions FindOptions(FindPath + "/*_*_*", false);
								FindOptions.m_AttribMask = EFileAttrib_Directory;
								auto FoundFiles = CFile::fs_FindFiles(FindOptions);
								for (auto &File : FoundFiles)
								{
									CStr Backup = File.m_Path.f_Extract(FindPath.f_GetLen() + 1);
									CStr BackupID;
									CTime Time;
									if (CBackupManager::fs_IsValidBackup(Backup, &BackupID, &Time))
									{
										auto &OutBackup = Backups.f_Insert();
										OutBackup.m_Time = Time;
										OutBackup.m_ID = BackupID;
									}
								}
							}
							return BackupsPerSource;
						}
					)
					> [Continuation, Auditor](TCAsyncResult<TCMap<CStr, TCVector<CBackupID>>> &&_Result)
					{
						if (!_Result)
						{
							CStr DetailedError = fg_Format("Error listing backups: {}", _Result.f_GetExceptionStr());
							Continuation.f_SetException(Auditor.f_Exception({"File error when running query. See Backup Server logs.", DetailedError}));
							return;
						}
						Auditor.f_Info(fg_Format("Listed backup sources: {}", *_Result));
						
						Continuation.f_SetResult(fg_Move(*_Result));
					}
				;
			}
		;
		
		if (!_ForBackupSource.f_IsEmpty())
		{
			if (!CBackupManager::fs_IsValidBackupSource(_ForBackupSource, nullptr, nullptr))
				return Auditor.f_Exception("Invalid backup source format");
				
			TCVector<CStr> BackupSources;
			BackupSources.f_Insert(_ForBackupSource);
			if (pThis->fp_FilterBackupSourcesByPermissions(CallingHostID, BackupSources).f_IsEmpty())
				return Auditor.f_AccessDenied("(List backups)");
			fListBackups(BackupSources);
		}
		else
		{
			g_Dispatch(QueryFileActor) > g_fFindBackupSources > [pThis, Continuation, Auditor, fListBackups, CallingHostID](TCAsyncResult<TCVector<CStr>> &&_Result)
				{
					if (!_Result)
					{
						CStr DetailedError = fg_Format("Error listing backup sources when listing backups: {}", _Result.f_GetExceptionStr());
						Continuation.f_SetException(Auditor.f_Exception({"File error when running query. See Backup Server logs.", DetailedError}));
						return;
					}
					fListBackups(pThis->fp_FilterBackupSourcesByPermissions(CallingHostID, *_Result));
				}
			;
		}
		return Continuation;
	}
}
