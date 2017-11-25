// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Process/Platform>

#include "Malterlib_Cloud_BackupManagerClient.h"
#include "Malterlib_Cloud_BackupManagerClient_Internal.h"
#include "Malterlib_Cloud_BackupManagerClient_BackupInstance.h"

namespace NMib::NCloud
{

	void CBackupManagerClient::CInternal::f_BackupInstance_ReportFinishedStarting(TCActor<NPrivate::CBackupManagerClient_Instance> const &_BackupInstance)
	{
		_BackupInstance(&NPrivate::CBackupManagerClient_Instance::f_BackupFinishedStarting) > [this](TCAsyncResult<void> &&_Result)
			{
				if (!_Result)
				{
					DMibLogCategoryStr(m_Config.m_LogCategory);
					DMibLog(Error, "Failed to report backup finished starting: {}", _Result.f_GetExceptionStr());
				}
			}
		;
	}

	void CBackupManagerClient::CInternal::f_BackupFinishedStarting()
	{
		m_bBackupFinishedStarting = true;
		for (auto &RunningInstance : m_RunningBackupInstances)
			f_BackupInstance_ReportFinishedStarting(RunningInstance.m_Instance);
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

		auto pActive = Internal.f_MarkInstancesActive();

		TCContinuation<void> Continuation;
		Internal.f_SubscribeChanges() > [pActive, Continuation, pThis = m_pThis, NewConfig = fg_Move(NewConfig)](TCAsyncResult<void> &&_Result) mutable
			{
				if (pThis->f_IsDestroyed())
				{
					Continuation.f_SetException(DMibErrorInstance("Destroyed"));
					return;
				}
				
				auto &Internal = *pThis->mp_pInternal;

				if (!_Result)
					Internal.f_ReportBackupError("Failed to subscribe to file notifications when appending manifest: {}"_f << _Result.f_GetExceptionStr(), false);

				g_Dispatch(Internal.m_FileActor) > [NewConfig = fg_Move(NewConfig), pDestroyed = Internal.m_pDestroyed]()
					-> TCTuple<CDirectoryManifest, TCMap<CStr, CUniqueFileIdentifier>, TCMap<CStr, TCSharedPointer<CAppendFileState>>>
					{
						TCMap<CStr, CFile::CFileChecksumState_SHA256> SourceAppendStates;
						auto Manifest = CDirectoryManifest::fs_GetManifest(NewConfig, [&]{ fs_CheckDestroy(pDestroyed); }, &SourceAppendStates, gc_ChecksumFileFlags);
						Manifest.m_Files.f_Remove("");
						TCMap<CStr, CUniqueFileIdentifier> FileIDs;
						TCMap<CStr, TCSharedPointer<CAppendFileState>> AppendStates;

						for (auto &File : Manifest.m_Files)
						{
							if (File.m_Attributes & (EFileAttrib_Link | EFileAttrib_Directory))
								continue;

							auto &FileName = Manifest.m_Files.fs_GetKey(File);

							FileIDs[FileName] = CFile::fs_GetUniqueIdentifier(CFile::fs_AppendPath(NewConfig.m_Root, File.m_OriginalPath));

							if (auto *pSourceAppendState = SourceAppendStates.f_FindEqual(FileName))
							{
								auto &AppendState = *(AppendStates[FileName] = fg_Construct());
								AppendState.m_ChecksumState.m_DigestState = pSourceAppendState->m_Hash;
								AppendState.m_File = fg_Move(*pSourceAppendState->m_pFile);
								AppendState.m_ManifestFile = File;
								AppendState.m_bIsValid = true;
								AppendState.m_ChecksumState.m_Position = AppendState.m_File.f_GetPosition();
							}
						}

						return {fg_Move(Manifest), fg_Move(FileIDs), fg_Move(AppendStates)};
					}
					> [pThis, Continuation, pActive]
					(TCAsyncResult<TCTuple<CDirectoryManifest, TCMap<CStr, CUniqueFileIdentifier>, TCMap<CStr, TCSharedPointer<CAppendFileState>>>> &&_Manifest)
					{
						if (pThis->f_IsDestroyed())
						{
							Continuation.f_SetException(DMibErrorInstance("Destroyed"));
							return;
						}

						auto &Internal = *pThis->mp_pInternal;

						if (!_Manifest)
						{
							Internal.f_ReportBackupError("Failed to get manifest when appending: {}"_f << _Manifest.f_GetExceptionStr(), true);
							Continuation.f_SetException(fg_Move(_Manifest));
							return;
						}

						auto &NewManifest = fg_Get<0>(*_Manifest);
						auto &NewManifestFileIDs = fg_Get<1>(*_Manifest);

						auto &AppendStates = fg_Get<2>(*_Manifest);
						for (auto &pAppendState : AppendStates)
						{
							auto &FileName = AppendStates.fs_GetKey(pAppendState);
							if (Internal.m_AppendStates.f_FindEqual(FileName))
								continue;

							Internal.m_ChecksumState[FileName] = pAppendState->m_ChecksumState;
							Internal.m_AppendStates[FileName] = fg_Move(pAppendState);
						}

						g_Dispatch(Internal.m_FileActor) > [AppendStates = fg_Move(AppendStates)]() mutable
							{
								AppendStates.f_Clear();
							}
							> fg_DiscardResult()
						;

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
						
						TCActorResultVector<void> ManifestChangedResults;

						auto fSendManifestChange = [&]
							(
							 	CStr const &_Path
							 	, CBackupManagerBackup::CManifestChange const &_ManifestChange
							 	, bool _bDirty
							 	, CBackupManagerClient_ChecksumState const &_ChecksumState
							)
							{
#if defined DMibContractConfigure_CheckEnabled
								CStr ManifestError;
								DMibCheck(CBackupManagerBackup::fs_ManifestChangeValid(_Path, _ManifestChange, ManifestError));
#endif

								for (auto &RunningInstance : Internal.m_RunningBackupInstances)
								{
									RunningInstance.m_Instance(&NPrivate::CBackupManagerClient_Instance::f_ManifestChanged, _Path, _ManifestChange, _bDirty, _ChecksumState)
										> ManifestChangedResults.f_AddResult()
									;
								}
							}
						;
						
						for (auto &NewFile : NewManifest.m_Files)
						{
							auto &NewFileName = NewManifest.m_Files.fs_GetKey(NewFile);
							if (Internal.m_Manifest.m_Files.f_FindEqual(NewFileName))
								continue;
							Internal.m_Manifest.m_Files[NewFileName] = NewFile;

							auto pChecksumState = Internal.m_ChecksumState.f_FindEqual(NewFileName);

							fSendManifestChange
								(
								 	NewFileName
								 	, CBackupManagerBackup::CManifestChange_Add{fg_Move(NewFile)}
								 	, false
								 	, pChecksumState ? *pChecksumState : CBackupManagerClient_ChecksumState{}
								)
							;
						}
						
						Internal.m_ManifestFileIDs += NewManifestFileIDs;
						
						ManifestChangedResults.f_GetResults() > Continuation.f_ReceiveAny();
					}
				;
			}
		;
		
		return Continuation;
	}
}
