// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_DirectorySync.h"
#include "Malterlib_Cloud_DirectorySync_ReceiveInternal.h"

namespace NMib::NCloud
{
	TCContinuation<void> CDirectorySyncReceive::CInternal::f_SyncManifest()
	{
		struct CState
		{
			CStr m_ManifestDestination;
		};
		
		TCSharedPointer<CState> pState = fg_Construct();
		
		return f_RSync
			(
				[pState, pConfig = m_pConfig](CRunningSyncState &_RSyncState)
				{
					auto &Config = *pConfig;
					
					CStr PreviousManifestFileName;
					switch (Config.m_PreviousManifest.f_GetTypeID())
					{
					case 0:
						{
							break;
						}
					case 1:
						{
							auto &Manifest = Config.m_PreviousManifest.f_Get<1>();

							PreviousManifestFileName = CFile::fs_AppendPath(Config.m_TempDirectory, fg_Format("{}.manifest", fg_RandomID()));
							_RSyncState.m_TempFiles.f_Insert(PreviousManifestFileName);

							CFile::fs_CreateDirectory(Config.m_TempDirectory);

							TCBinaryStreamFile<> Stream;
							Stream.f_Open(PreviousManifestFileName, EFileOpen_Write);
							Stream << Manifest;

							break;
						}
					case 2:
						{
							PreviousManifestFileName = Config.m_PreviousManifest.f_Get<2>();
							break;
						}
					default:
						DMibNeverGetHere;
						break;
					}

					CStr ManifestDestination = Config.m_OutputManifestPath;
					if (ManifestDestination.f_IsEmpty())
					{
						ManifestDestination = CFile::fs_AppendPath(Config.m_TempDirectory, fg_Format("{}.manifest", fg_RandomID()));
						_RSyncState.m_TempFiles.f_Insert(ManifestDestination);
					}

					CFile::fs_CreateDirectory(CFile::fs_GetPath(ManifestDestination));

					_RSyncState.m_File.f_Open(ManifestDestination, EFileOpen_Read | EFileOpen_Write | EFileOpen_DontTruncate | EFileOpen_ShareAll);

					if (ManifestDestination != PreviousManifestFileName)
					{
						if (CFile::fs_FileExists(PreviousManifestFileName))
							_RSyncState.m_SourceFile.f_Open(PreviousManifestFileName, EFileOpen_Read | EFileOpen_ShareAll);
						_RSyncState.m_pClient = fg_Construct(_RSyncState.m_SourceFile, _RSyncState.m_File, 256, 4*1024*1024, 8*1024*1024, nullptr, ERSyncClientFlag_TruncateOutput);
					}
					else
					{
						CStr TempFileName = CFile::fs_AppendPath(Config.m_TempDirectory, fg_Format("{}.rsynctemp", fg_RandomID()));
						_RSyncState.m_TempFiles.f_Insert(TempFileName);
						CFile::fs_CreateDirectory(Config.m_TempDirectory);

						_RSyncState.m_TempFile.f_Open(TempFileName, EFileOpen_Read | EFileOpen_Write | EFileOpen_DontTruncate | EFileOpen_ShareAll);
						_RSyncState.m_pClient = fg_Construct(_RSyncState.m_File, _RSyncState.m_File, 256, 4*1024*1024, 8*1024*1024, &_RSyncState.m_TempFile, ERSyncClientFlag_TruncateOutput);
					}
					
					pState->m_ManifestDestination = ManifestDestination;
					
					return false;
				}
				, [pState, this]() -> TCContinuation<void>
				{
					TCContinuation<void> Continuation;

					g_Dispatch(m_FileActor) > [=]
						{
							CDirectoryManifest Manifest;
							TCBinaryStreamFile<> Stream;
							Stream.f_Open(pState->m_ManifestDestination, EFileOpen_Read | EFileOpen_ShareAll);
							Stream >> Manifest;
							
							return Manifest;
						}
						> Continuation / [=](CDirectoryManifest &&_Manifest)
						{
							m_pManifest = fg_Construct(fg_Move(_Manifest));
							Continuation.f_SetResult();
						}
					;

					return Continuation;
				}
				, [this ](CActorSubscription &&_Subscription) -> TCContinuation<CDirectorySyncClient::FRunRSync>
				{
					return DMibCallActor
						(
							m_Client
							, CDirectorySyncClient::f_StartManifestRSync
							, fg_Move(_Subscription)
						)
					;
				}
			)
		;
	}
}
