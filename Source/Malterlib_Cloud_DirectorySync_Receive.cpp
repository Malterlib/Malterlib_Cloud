// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_DirectorySync.h"
#include "Malterlib_Cloud_DirectorySync_ReceiveInternal.h"

namespace NMib::NCloud
{
	CDirectorySyncReceive::CDirectorySyncReceive(CConfig &&_Config, TCDistributedActorInterface<CDirectorySyncClient> &&_Client)
		: mp_pInternal(fg_Construct(this, fg_Move(_Config), fg_Move(_Client)))
	{
	}

	CDirectorySyncReceive::CInternal::CInternal(CDirectorySyncReceive *_pThis, CConfig &&_Config, TCDistributedActorInterface<CDirectorySyncClient> &&_Client)
		: m_pThis(_pThis)
		, m_pConfig(fg_Construct(fg_Move(_Config)))
		, m_Client(fg_Move(_Client))
		, m_FileActor(fg_Construct(), "Directory sync receive file access")
	{
		if (m_pConfig->m_TempDirectory.f_IsEmpty())
			DMibError("You need to specify a valid temp directory");
	}
	
	CDirectorySyncReceive::~CDirectorySyncReceive() = default;
	
	auto CDirectorySyncReceive::CInternal::CRunningSyncState::f_Destroy() -> TCContinuation<void>
	{
		TCContinuation<void> Continuation;

		m_fRunProtocol.f_Destroy() > Continuation / [pThis = TCSharedPointerSupportWeak<CRunningSyncState>(this), Continuation]
			{
				if (!pThis->m_pClient)
					return Continuation.f_SetResult();
				
				g_Dispatch(pThis->m_FileActor) > [pThis]
					{
						pThis->m_pClient.f_Clear();
						pThis->m_File.f_Close();
						pThis->m_SourceFile.f_Close();
						pThis->m_TempFile.f_Close();
						for (auto &File : pThis->m_TempFiles)
						{
							try
							{
								CFile::fs_DeleteFile(File);
							}
							catch (CExceptionFile const &)
							{
							}
						}
						pThis->m_TempFiles.f_Clear();
					}
					> Continuation
				;
			}
		;

		return Continuation;
	}

	void CDirectorySyncReceive::CInternal::fs_CheckDestroy(TCSharedPointer<NAtomic::TCAtomic<bool>> const &_pDestroyed)
	{
		if (_pDestroyed->f_Load(NAtomic::EMemoryOrder_Relaxed))
			DMibError("Directory sync receive destroyed");
	}
	
	TCContinuation<void> CDirectorySyncReceive::fp_Destroy()
	{
		auto &Internal = *mp_pInternal;
		*Internal.m_pDestroyed = true;
		
		TCActorResultVector<void> RSyncDestroys;
		
		for (auto &pRSync : Internal.m_RSyncStates)
			pRSync->f_Destroy() > RSyncDestroys.f_AddResult();
		
		TCContinuation<void> Continuation;
		RSyncDestroys.f_GetResults() > Continuation / [=]
			{
				auto &Internal = *mp_pInternal;
				
				if (Internal.m_Client)
					Internal.m_Client.f_Destroy() > Continuation;
				else
					Continuation.f_SetResult();
			}
		;
		return Continuation;
	}
	
	auto CDirectorySyncReceive::f_PerformSync() -> TCContinuation<CSyncResult>
	{
		auto &Internal = *mp_pInternal;
		
		if (Internal.m_bStartedSync)
			return DMibErrorInstance("Sync already perfromed");
		
		Internal.m_bStartedSync = true;
		
		TCContinuation<CSyncResult> Continuation;
		
		Internal.f_SyncManifest() > Continuation / [=]
			{
				auto &Internal = *mp_pInternal;
				Internal.f_HandleExcessFiles() > Continuation / [=]
					{
						auto &Internal = *mp_pInternal;
						auto &Config = *Internal.m_pConfig;
						
						for (auto &File : Internal.m_pManifest->m_Files)
						{
							auto &FileName = Internal.m_pManifest->m_Files.fs_GetKey(File);
							if (!Config.m_IncludeWildcards.f_IsEmpty())
							{
								if (!fg_StrMatchesAnyWildcardInMap(FileName, Config.m_IncludeWildcards))
									continue;
							}
							if (fg_StrMatchesAnyWildcardInMap(FileName, Config.m_ExcludeWildcards))
								continue;
							Internal.m_PendingFileSyncs[FileName];
							CStr Directory = CFile::fs_GetPath(FileName);
							while (!Directory.f_IsEmpty())
							{
								Internal.m_PendingFileSyncs[Directory];
								Directory = CFile::fs_GetPath(Directory);
							}
						}

						TCContinuation<void> SyncsContinuation;
						Internal.f_RunFileSyncs(SyncsContinuation);
						
						SyncsContinuation.f_Dispatch() > Continuation / [=]
							{
								auto &Internal = *mp_pInternal;
								
								DMibCallActor
									(
										Internal.m_Client
										, CDirectorySyncClient::f_Finished
									)
									> Continuation / [=]
									{
										auto &Internal = *mp_pInternal;
										
										CSyncResult SyncResult;
										SyncResult.m_Manifest = fg_Move(*Internal.m_pManifest);
										SyncResult.m_Stats = Internal.m_Stats;
										SyncResult.m_Stats.m_nSeconds = Internal.m_Clock.f_GetTime();
										
										Continuation.f_SetResult(fg_Move(SyncResult));
									}
								;
							}
						;
					}
				;
			}
		;
		
		return Continuation;
	}
}
