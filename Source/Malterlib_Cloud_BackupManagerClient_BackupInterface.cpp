// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Process/Platform>

#include "Malterlib_Cloud_BackupManagerClient.h"
#include "Malterlib_Cloud_BackupManagerClient_Internal.h"
#include "Malterlib_Cloud_BackupManagerClient_BackupInstance.h"

namespace NMib::NCloud
{
	void CBackupManagerClient::CInternal::f_BackupFinishedStarting()
	{
		m_bBackupFinishedStarting = true;
		for (auto &RunningInstance : m_RunningBackupInstances)
		{
			RunningInstance(&NPrivate::CBackupManagerClient_Instance::f_BackupFinishedStarting) > [this](TCAsyncResult<void> &&_Result)
				{
					if (!_Result)
					{
						DMibLogCategoryStr(m_Config.m_LogCategory);
						DMibLog(Error, "Failed to report backup finished starting: {}", _Result.f_GetExceptionStr());
					}
				}
			;
		}
	}

	TCContinuation<void> CBackupManagerClient::CInternal::CDistributedAppInterfaceBackupImplementation::f_AppendManifest(CDirectoryManifestConfig const &_Config)
	{
		if (!_Config.m_Root.f_IsEmpty())
			return DMibErrorInstance("You cannot change manifest root when appending manifest");

		if (!_Config.m_ExcludeWildcards.f_IsEmpty())
			return DMibErrorInstance("You cannot add exclude wildcards when appending manifest");
		
		if (!_Config.m_AddSyncFlagsWildcards.f_IsEmpty() || !_Config.m_RemoveSyncFlagsWildcards.f_IsEmpty())
			return DMibErrorInstance("You cannot change sync flags when appending manifest");
		
		auto &Internal = *m_pThis->mp_pInternal;

		CDirectoryManifestConfig NewConfig = Internal.m_Config.m_ManifestConfig;
		NewConfig.m_IncludeWildcards.f_Clear();
		
		for (auto &Destination : _Config.m_IncludeWildcards)
		{
			auto &Wildcard = _Config.m_IncludeWildcards.fs_GetKey(Destination);
			
			auto *pOldDestination = Internal.m_Config.m_ManifestConfig.m_IncludeWildcards.f_FindEqual(Wildcard);
			if (pOldDestination)
			{
				if (*pOldDestination != Destination)
					return DMibErrorInstance("You cannot change the destination for a wildcard");
			}
			else
				NewConfig.m_IncludeWildcards(Wildcard, Destination);
		}

		Internal.m_Config.m_ManifestConfig.m_IncludeWildcards += _Config.m_IncludeWildcards;
		
		if (NewConfig.m_IncludeWildcards.f_IsEmpty())
			return fg_Explicit();
		
		TCContinuation<void> Continuation;
		Internal.f_SubscribeChanges() > [Continuation, pThis = m_pThis, NewConfig = fg_Move(NewConfig)](TCAsyncResult<void> &&_Result) mutable
			{
				if (pThis->mp_bDestroyed)
				{
					Continuation.f_SetException(DMibErrorInstance("Destroyed"));
					return;
				}
				
				auto &Internal = *pThis->mp_pInternal;

				if (!_Result)
				{
					DMibLogCategoryStr(Internal.m_Config.m_LogCategory);
					DMibLog(Error, "Failed to subscribe to file notifications when appending manifest: {}", _Result.f_GetExceptionStr());
				}

				g_Dispatch(Internal.m_FileActor) > [NewConfig = fg_Move(NewConfig), pDestroyed = Internal.m_pDestroyed]() -> TCTuple<CDirectoryManifest, TCMap<CStr, CUniqueFileIdentifier>>
					{
						auto Manifest = CDirectoryManifest::fs_GetManifest(NewConfig, [&]{ fs_CheckDestroy(pDestroyed); });
						TCMap<CStr, CUniqueFileIdentifier> FileIDs;

						for (auto &File : Manifest.m_Files)
						{
							if (File.m_Attributes & (EFileAttrib_Link | EFileAttrib_Directory))
								continue;

							auto &FileName = Manifest.m_Files.fs_GetKey(File);

							FileIDs[FileName] = CFile::fs_GetUniqueIdentifier(CFile::fs_AppendPath(NewConfig.m_Root, File.m_OriginalPath));
						}

						return {fg_Move(Manifest), fg_Move(FileIDs)};
					}
					> [pThis, Continuation](TCAsyncResult<TCTuple<CDirectoryManifest, TCMap<CStr, CUniqueFileIdentifier>>> &&_Manifest)
					{
						if (pThis->mp_bDestroyed)
						{
							Continuation.f_SetException(DMibErrorInstance("Destroyed"));
							return;
						}

						auto &Internal = *pThis->mp_pInternal;

						if (!_Manifest)
						{
							DMibLogCategoryStr(Internal.m_Config.m_LogCategory);
							DMibLog(Error, "Failed to get manifest when appending: {}", _Manifest.f_GetExceptionStr());
							Continuation.f_SetException(fg_Move(_Manifest));
							return;
						}

						auto &NewManifest = fg_Get<0>(*_Manifest);
						auto &NewManifestFileIDs = fg_Get<1>(*_Manifest);
						
						for (auto &NewFile : NewManifest.m_Files)
						{
							auto &NewFileName = NewManifest.m_Files.fs_GetKey(NewFile);
							auto *pOldFile = Internal.m_Manifest.m_Files.f_FindEqual(NewFileName);
							if (pOldFile && pOldFile->m_OriginalPath != NewFile.m_OriginalPath)
							{
								if (pOldFile->f_IsDirectory() && NewFile.f_IsDirectory())
								{
									// Allow remapping of added directories
									if (pOldFile->m_OriginalPath != NewFileName && NewFile.m_OriginalPath.f_GetLen() < pOldFile->m_OriginalPath.f_GetLen())
										Internal.m_Manifest.m_Files.f_Remove(NewFileName);
								}
								else
								{
									Continuation.f_SetException
										(
											DMibErrorInstance
											(
												fg_Format
												(
													"Manifest file '{}' changed original path which is not allowed. '{}' (Dir {}) != '{}' (Dir {})"
													, NewFileName
													, pOldFile->m_OriginalPath
													, pOldFile->f_IsDirectory()
													, NewFile.m_OriginalPath
													, NewFile.f_IsDirectory()
												)
											)
										)
									;
									return;
								}
							}
						}

						auto fSendManifestChange = [&](CStr const &_Path, CBackupManagerBackup::CManifestChange const &_ManifestChange, bool _bDirty)
							{
								for (auto &RunningInstance : Internal.m_RunningBackupInstances)
									RunningInstance(&NPrivate::CBackupManagerClient_Instance::f_ManifestChanged, _Path, _ManifestChange, _bDirty) > fg_DiscardResult();
							}
						;
						
						for (auto &NewFile : NewManifest.m_Files)
						{
							auto &NewFileName = NewManifest.m_Files.fs_GetKey(NewFile);
							if (Internal.m_Manifest.m_Files.f_FindEqual(NewFileName))
								continue;
							Internal.m_Manifest.m_Files[NewFileName] = NewFile;

							fSendManifestChange(NewFileName, CBackupManagerBackup::CManifestChange_Add{fg_Move(NewFile)}, false);
						}
						
						Internal.m_ManifestFileIDs += NewManifestFileIDs;
						
						Continuation.f_SetResult();
					}
				;
			}
		;
		
		return Continuation;
	}
}
