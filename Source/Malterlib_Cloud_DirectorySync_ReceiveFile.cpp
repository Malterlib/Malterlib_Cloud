// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_DirectorySync.h"
#include "Malterlib_Cloud_DirectorySync_ReceiveInternal.h"

namespace NMib::NCloud
{
	TCContinuation<void> CDirectorySyncReceive::CInternal::f_HandleExcessFiles()
	{
		TCContinuation<void> Continuation;
		if (m_pConfig->m_ExcessFilesAction == EExcessFilesAction_Ignore)
			return fg_Explicit();
		
		g_Dispatch(m_FileActor) > [pConfig = m_pConfig, pManifest = m_pManifest, pDestroyed = m_pDestroyed]
			{
				auto &Config = *pConfig;
				auto &Manifest = *pManifest;

				CFile::CFindFilesOptions Options{Config.m_BasePath + "/*", true};
				Options.m_bFollowLinks = false;
				Options.m_fCheckAbort = [&]{ CInternal::fs_CheckDestroy(pDestroyed); };
				Options.m_AttribMask = EFileAttrib_Directory | EFileAttrib_File | EFileAttrib_Link;
				
				for (auto &File : CFile::fs_FindFiles(Options))
				{
					CStr RelativePath = File.m_Path.f_Extract(Config.m_BasePath.f_GetLen() + 1);
					if (Manifest.m_Files.f_FindEqual(RelativePath))
						continue;
					
					switch (Config.m_ExcessFilesAction)
					{
					case EExcessFilesAction_Fail:
						{
							DMibError(fg_Format("Found execess file in directory sync destination: {}", RelativePath));
							break;
						}
					case EExcessFilesAction_Delete:
						{
							if (File.m_Attribs & (EFileAttrib_Link | EFileAttrib_File))
								CFile::fs_DeleteFile(File.m_Path);
							else
								CFile::fs_DeleteDirectoryRecursive(File.m_Path);
							break;
						}
					}
				}
			}
			> Continuation
		;
		
		return Continuation;
	}
	
	TCContinuation<void> CDirectorySyncReceive::CInternal::f_SyncFile(CStr const &_FileName)
	{
		return f_RSync
			(
				[_FileName, pConfig = m_pConfig, pManifest = m_pManifest](CRunningSyncState &_RSyncState) -> bool
				{
					auto &Config = *pConfig;
					
					CStr Destination = CFile::fs_AppendPath(Config.m_BasePath, _FileName);
					
					auto *pManifestFile = pManifest->m_Files.f_FindEqual(_FileName);
					
					if (!pManifestFile)
						return true;
					
					auto &ManifestFile = *pManifestFile;
					
					if (ManifestFile.m_Attributes & EFileAttrib_Link)
					{
						if (CFile::fs_FileExists(Destination, EFileAttrib_Link))
						{
							if (CFile::fs_ResolveSymbolicLink(Destination) == ManifestFile.m_SymlinkData)
								return true;
							CFile::fs_DeleteFile(Destination);
						}
						else if (CFile::fs_FileExists(Destination, EFileAttrib_Directory))
							CFile::fs_DeleteDirectoryRecursive(Destination);
						else if (CFile::fs_FileExists(Destination))
							CFile::fs_DeleteFile(Destination);
						
						ESymbolicLinkFlag Flags = ESymbolicLinkFlag_AllowEmulation;
						
						if (!CFile::fs_IsPathAbsolute(ManifestFile.m_SymlinkData))
							Flags |= ESymbolicLinkFlag_Relative;
						
						CFile::fs_CreateSymbolicLink(ManifestFile.m_SymlinkData, Destination, ManifestFile.m_Attributes, Flags);
						
						return true;
					}
					else if (ManifestFile.m_Attributes & EFileAttrib_Directory)
					{
						if (CFile::fs_FileExists(Destination, EFileAttrib_Link | EFileAttrib_File))
							CFile::fs_DeleteFile(Destination);

						CFile::fs_CreateDirectory(Destination);

						return true;
					}
					
					if (CFile::fs_FileExists(Destination, EFileAttrib_Link))
						CFile::fs_DeleteFile(Destination);
					else if (CFile::fs_FileExists(Destination, EFileAttrib_Directory))
						CFile::fs_DeleteDirectoryRecursive(Destination);
					
					CStr Source;
					if (CFile::fs_FileExists(Destination))
					{
						Source = Destination;
						if (CFile::fs_GetFileChecksum_SHA256(Source) == ManifestFile.m_Digest)
							return true;
					}
					else
						Source = CFile::fs_AppendPath(Config.m_PreviousBasePath, _FileName);
					
					CFile::fs_CreateDirectory(CFile::fs_GetPath(Destination));
					
					auto Attributes = EFileAttrib_UnixAttributesValid | EFileAttrib_UserRead | EFileAttrib_UserWrite;

					_RSyncState.m_File.f_Open(Destination, EFileOpen_Read | EFileOpen_Write | EFileOpen_DontTruncate | EFileOpen_ShareAll, Attributes);

					if (Source != Destination)
					{
						if (CFile::fs_FileExists(Source))
							_RSyncState.m_SourceFile.f_Open(Source, EFileOpen_Read | EFileOpen_ShareAll);
						_RSyncState.m_pClient = fg_Construct(_RSyncState.m_SourceFile, _RSyncState.m_File, 256, 4*1024*1024, 8*1024*1024, nullptr, ERSyncClientFlag_TruncateOutput);
					}
					else
					{
						CStr TempFileName = CFile::fs_AppendPath(Config.m_TempDirectory, fg_Format("{}.rsynctemp", fg_RandomID()));
						_RSyncState.m_TempFiles.f_Insert(TempFileName);
						CFile::fs_CreateDirectory(Config.m_TempDirectory);

						_RSyncState.m_TempFile.f_Open(TempFileName, EFileOpen_Read | EFileOpen_Write | EFileOpen_DontTruncate | EFileOpen_ShareAll, Attributes);
						_RSyncState.m_pClient = fg_Construct(_RSyncState.m_File, _RSyncState.m_File, 256, 4*1024*1024, 8*1024*1024, &_RSyncState.m_TempFile, ERSyncClientFlag_TruncateOutput);
					}
					
					return false;
				}
				, [=, pConfig = m_pConfig, pManifest = m_pManifest]() -> TCContinuation<void>
				{
					auto &Config = *pConfig;
					if (!(Config.m_SyncFlags & (ESyncFlag_WriteTime | ESyncFlag_Owner | ESyncFlag_Group | ESyncFlag_Attributes)))
						return fg_Explicit();
					
					return g_Dispatch(m_FileActor) > [=]
						{
							auto &Config = *pConfig;
							CStr Destination = CFile::fs_AppendPath(Config.m_BasePath, _FileName);
							
							auto *pManifestFile = pManifest->m_Files.f_FindEqual(_FileName);
							
							if (!pManifestFile)
								return;

							auto &ManifestFile = *pManifestFile;
							
							if (Config.m_SyncFlags & ESyncFlag_WriteTime)
							{
								if (ManifestFile.m_Attributes & EFileAttrib_Link)
									CFile::fs_SetWriteTimeOnLink(Destination, ManifestFile.m_WriteTime);
								else
									CFile::fs_SetWriteTime(Destination, ManifestFile.m_WriteTime);
							}

							if (Config.m_SyncFlags & ESyncFlag_Attributes)
							{
								if (ManifestFile.m_Attributes & EFileAttrib_Link)
									CFile::fs_SetAttributesOnLink(Destination, ManifestFile.m_Attributes | NMib::NFile::EFileAttrib_UnixAttributesValid);
								else
									CFile::fs_SetAttributes(Destination, ManifestFile.m_Attributes | NMib::NFile::EFileAttrib_UnixAttributesValid);
							}

							if (Config.m_SyncFlags & ESyncFlag_Group)
							{
								if (ManifestFile.m_Attributes & EFileAttrib_Link)
									CFile::fs_SetGroupOnLink(Destination, ManifestFile.m_Group);
								else
									CFile::fs_SetGroup(Destination, ManifestFile.m_Group);
							}

							if (Config.m_SyncFlags & ESyncFlag_Owner)
							{
								if (ManifestFile.m_Attributes & EFileAttrib_Link)
									CFile::fs_SetOwnerOnLink(Destination, ManifestFile.m_Owner);
								else
									CFile::fs_SetOwner(Destination, ManifestFile.m_Owner);
							}
						}
					;
				}
				, [this, _FileName](CActorSubscription &&_Subscription) -> TCContinuation<CDirectorySyncClient::FRunRSync>
				{
					return DMibCallActor
						(
							m_Client
							, CDirectorySyncClient::f_StartRSync
							, _FileName
							, fg_Move(_Subscription)
						)
					;
				}
			)
		;
	}

	void CDirectorySyncReceive::CInternal::f_RunFileSyncs(TCContinuation<void> const &_Continuation)
	{
		auto &Config = *m_pConfig;
	
		while (m_nRunningSyncs < Config.m_RSyncConcurrency && !m_PendingFileSyncs.f_IsEmpty() && !m_pThis->mp_bDestroyed)
		{
			auto FileName = *m_PendingFileSyncs.f_FindSmallest();
			m_PendingFileSyncs.f_Remove(FileName);
			
			++m_nRunningSyncs;
			
			f_SyncFile(FileName) > [this, _Continuation](TCAsyncResult<void> &&_Result)
				{
					if (!_Result)
						fg_AddStrSep(m_SyncErrors, _Result.f_GetExceptionStr(), "\n");
					--m_nRunningSyncs;
					f_RunFileSyncs(_Continuation);
				}
			;
		}
		
		if (m_PendingFileSyncs.f_IsEmpty() && m_nRunningSyncs == 0 && !_Continuation.f_IsSet())
		{
			if (m_SyncErrors.f_IsEmpty())
				_Continuation.f_SetResult();
			else
				_Continuation.f_SetException(DMibErrorInstance(m_SyncErrors));
		}
	}
}
