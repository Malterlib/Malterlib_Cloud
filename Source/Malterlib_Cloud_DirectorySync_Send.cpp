// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_DirectorySync.h"
#include <Mib/Cryptography/RandomID>
#include <Mib/File/RSync>
#include <Mib/Concurrency/ActorSubscription>

namespace NMib::NCloud
{
	using namespace NStr;
	using namespace NConcurrency;
	using namespace NContainer;
	using namespace NAtomic;
	using namespace NCryptography;
	using namespace NPtr;
	using namespace NFile;
	using namespace NStream;
	using namespace NFunction;
	using namespace NTime;
	
	struct CDirectorySyncSend::CInternal
	{
		struct CByteStats
		{
			uint64 m_nIncoming = 0;
			uint64 m_nOutgoing = 0;
		};
		
		struct CRunningSyncState : public TCSharedPointerIntrusiveBase<ESharedPointerOption_SupportWeakPointer>
		{
			TCContinuation<CByteStats> f_Destroy();
		
			TCActor<CSeparateThreadActor> m_FileActor;
			TCBinaryStreamFile<> m_File;
			CBinaryStreamMemory<> m_FileMemory;
			TCUniquePointer<CRSyncServer> m_pRSyncServer;
			CByteStats m_ByteStats;
			CActorSubscription m_Subscription;
		};
		
		CInternal(CDirectorySyncSend *_pThis, CConfig &&_Config);
		static void fs_CheckDestroy(TCSharedPointer<NAtomic::TCAtomic<bool>> const &_pDestroyed);
		CExceptionPointer f_CheckFileName(CStr const &_FileName);
		auto f_StartRSync(TCActorSubscriptionWithID<> &&_Subscription, TCFunctionMutable<void (CRunningSyncState &_State)> &&_fOpenRSync) -> TCContinuation<FRunRSync>;

		CDirectorySyncSend *m_pThis = nullptr;
		TCSharedPointer<CConfig> m_pConfig;
		TCSharedPointer<CDirectoryManifest> m_pManifest = fg_Construct();
		TCActor<CSeparateThreadActor> m_FileActor;
		TCSharedPointer<TCAtomic<bool>> m_pDestroyed = fg_Construct(false);
		TCMap<CStr, TCSharedPointerSupportWeak<CRunningSyncState>> m_RSyncStates;
		CClock m_Clock{true};
		CDirectorySyncStats m_Stats;
		bool m_bFinished = false;
		bool m_bUseOriginalLocation = false;
	};

	CDirectorySyncSend::CInternal::CInternal(CDirectorySyncSend *_pThis, CConfig &&_Config)
		: m_pThis(_pThis)
		, m_pConfig(fg_Construct(fg_Move(_Config)))
	{
		m_FileActor = fg_Construct(fg_Construct(), "Directory sync file access");
		if (m_pConfig->m_bUseOriginalLocation)
			m_bUseOriginalLocation = *m_pConfig->m_bUseOriginalLocation;
		else if (m_pConfig->m_Manifest.f_IsOfType<CDirectoryManifestConfig>())
			m_bUseOriginalLocation = true;
	}
	
	CDirectorySyncSend::CDirectorySyncSend(CConfig &&_Config)
		: mp_pInternal(fg_Construct(this, fg_Move(_Config)))
	{
	}
	
	CDirectorySyncSend::~CDirectorySyncSend() = default;
	
	auto CDirectorySyncSend::f_GetResult() -> TCContinuation<CSyncResult>
	{
		auto &Internal = *mp_pInternal;
		
		CSyncResult Result;
		Result.m_bFinished = Internal.m_bFinished;
		Result.m_Stats = Internal.m_Stats;
		Result.m_Stats.m_nSeconds = Internal.m_Clock.f_GetTime();
		
		return fg_Explicit(Result);
	}
	
	void CDirectorySyncSend::CInternal::fs_CheckDestroy(TCSharedPointer<NAtomic::TCAtomic<bool>> const &_pDestroyed)
	{
		if (*_pDestroyed)
			DMibError("Directory sync send destroyed");
	}
	
	CExceptionPointer CDirectorySyncSend::CInternal::f_CheckFileName(CStr const &_FileName)
	{
		if (CFile::fs_IsPathAbsolute(_FileName))
			return fg_ExceptionPointer(DMibErrorInstance("Absolute paths not allowed"));

		if (CFile::fs_HasRelativeComponents(_FileName))
			return fg_ExceptionPointer(DMibErrorInstance("Relative path components such as '..' are not allowed"));

		{
			CStr Error;
			if (!CFile::fs_IsValidFilePath(_FileName, Error))
				return fg_ExceptionPointer(DMibErrorInstance(fg_Format("The path cannot {}", Error)));
		}

		return nullptr;
	}
	
	TCContinuation<void> CDirectorySyncSend::fp_Destroy()
	{
		auto &Internal = *mp_pInternal;
		
		*Internal.m_pDestroyed = true;
		
		TCActorResultVector<CInternal::CByteStats> StateDestroys;
		
		for (auto &pState : Internal.m_RSyncStates)
			pState->f_Destroy() > StateDestroys.f_AddResult();
		
		TCContinuation<void> Continuation;
		
		StateDestroys.f_GetResults() > Continuation / [this, Continuation]
			{
				auto &Internal = *mp_pInternal;
				Internal.m_FileActor->f_Destroy() > Continuation;
			}
		;
		
		return Continuation;
	}

	auto CDirectorySyncSend::CInternal::CRunningSyncState::f_Destroy() -> TCContinuation<CByteStats>
	{
		TCContinuation<void> SubscriptionDestroyContinuation;
		
		if (m_Subscription)
			SubscriptionDestroyContinuation = m_Subscription->f_Destroy();
		else
			SubscriptionDestroyContinuation.f_SetResult();
	
		TCContinuation<CByteStats> Continuation;

		SubscriptionDestroyContinuation.f_Dispatch() > Continuation / [pThis = TCSharedPointerSupportWeak<CRunningSyncState>(this), Continuation]
			{
				if (!pThis->m_pRSyncServer)
					return Continuation.f_SetResult(CByteStats{});
				
				g_Dispatch(pThis->m_FileActor) > [pThis]
					{
						pThis->m_pRSyncServer.f_Clear();
						pThis->m_FileMemory.f_Clear();
						pThis->m_File.f_Close();
						return pThis->m_ByteStats;
					}
					> Continuation
				;
			}
		;

		return Continuation;
	}
	
	auto CDirectorySyncSend::CInternal::f_StartRSync(TCActorSubscriptionWithID<> &&_Subscription, TCFunctionMutable<void (CRunningSyncState &_State)> &&_fOpenRSync)
		-> TCContinuation<FRunRSync>
	{
		CStr RSyncID = fg_RandomID();
		auto &pRSyncState = m_RSyncStates[RSyncID] = fg_Construct();
		auto &RSyncState = *pRSyncState;
		
		auto pCleanup = g_OnScopeExitActor > [this, RSyncID]
			{
				m_RSyncStates.f_Remove(RSyncID);
			}
		;
		
		RSyncState.m_FileActor = m_FileActor;
		RSyncState.m_Subscription = fg_Move(_Subscription);
		
		TCContinuation<FRunRSync> Continuation;
		
		g_Dispatch(RSyncState.m_FileActor) > [pRSyncState, fOpenRSync = fg_Move(_fOpenRSync)]() mutable
			{
				fOpenRSync(*pRSyncState);
			}
			> Continuation / [=]()
			{
				Continuation.f_SetResult
					(
						g_ActorFunctor(pRSyncState->m_FileActor)
						(
							g_ActorSubscription > [=]() -> TCContinuation<void>
							{
								m_RSyncStates.f_Remove(RSyncID);
								
								TCContinuation<void> Continuation;
								pRSyncState->f_Destroy() > Continuation / [this, Continuation](CByteStats &&_Stats)
									{
										m_Stats.m_OutgoingBytes += _Stats.m_nOutgoing;
										m_Stats.m_IncomingBytes += _Stats.m_nIncoming;
										++m_Stats.m_nSyncedFiles;
										
										Continuation.f_SetResult();
									}
								;
								return Continuation;
							}
						)
						> [=](CSecureByteVector &&_Packet) -> TCContinuation<CSecureByteVector>
						{
							return TCContinuation<CSecureByteVector>::fs_RunProtected() > [&]
								{
									if (!pRSyncState->m_pRSyncServer)
										DMibError("RSync server destroyed");
									
									CSecureByteVector ToSendToClient;
									pRSyncState->m_pRSyncServer->f_ProcessPacket(_Packet, ToSendToClient);

									pRSyncState->m_ByteStats.m_nIncoming += _Packet.f_GetLen();
									pRSyncState->m_ByteStats.m_nOutgoing += ToSendToClient.f_GetLen();
									
									return ToSendToClient;
								}
							;
						}
					)
				;
				pCleanup->f_Clear();
			}
		;
		
		return Continuation;
	}
						
	auto CDirectorySyncSend::f_StartManifestRSync(TCActorSubscriptionWithID<> &&_Subscription) -> TCContinuation<FRunRSync>
	{
		auto &Internal = *mp_pInternal;
		return Internal.f_StartRSync
			(
				fg_Move(_Subscription)
				, [pManifest = Internal.m_pManifest, pConfig = Internal.m_pConfig, pDestroyed = Internal.m_pDestroyed](CInternal::CRunningSyncState &_RSyncState)
				{
					auto &Config = *pConfig;
					
					CBinaryStream *pBinaryStream = nullptr;
					switch (Config.m_Manifest.f_GetTypeID())
					{
					case 0:
						{
							auto &Manifest = Config.m_Manifest.f_Get<0>();
							_RSyncState.m_FileMemory << Manifest;
							pBinaryStream = &_RSyncState.m_FileMemory;
							*pManifest = Manifest;
							break;
						}
					case 1:
						{
							auto &ManifestConfig = Config.m_Manifest.f_Get<1>();
							auto Manifest = CDirectoryManifest::fs_GetManifest(ManifestConfig, [&]{ CInternal::fs_CheckDestroy(pDestroyed); });
							_RSyncState.m_FileMemory << Manifest;
							pBinaryStream = &_RSyncState.m_FileMemory;
							*pManifest = fg_Move(Manifest);
							break;
						}
					case 2:
						{
							auto &ManifestFileName = Config.m_Manifest.f_Get<2>();
							_RSyncState.m_File.f_Open(ManifestFileName, EFileOpen_Read | EFileOpen_ShareRead);
							_RSyncState.m_File >> *pManifest;
							_RSyncState.m_File.f_SetPosition(0);
							pBinaryStream = &_RSyncState.m_File;
							break;
						}
					default:
						DMibError("Invalid manifest type");
					}
							
					_RSyncState.m_pRSyncServer = fg_Construct(*pBinaryStream, 8*1024*1024);
				}
			)
		;
	}
	
	auto CDirectorySyncSend::f_StartRSync(NStr::CStr const &_FileName, TCActorSubscriptionWithID<> &&_Subscription) -> TCContinuation<FRunRSync>
	{
		auto &Internal = *mp_pInternal;
		
		if (auto pException = Internal.f_CheckFileName(_FileName))
			return pException;
		
		auto *pFile = Internal.m_pManifest->m_Files.f_FindEqual(_FileName);
		
		if (!pFile)
			return DMibErrorInstance("File does not exist in manifest");

		if (!pFile->f_IsFile())
			return DMibErrorInstance("Is not a file in manifest");
			
		CStr FilePath;
		
		if (Internal.m_bUseOriginalLocation)
			FilePath = pFile->m_OriginalPath;
		else
			FilePath = _FileName;

		return Internal.f_StartRSync
			(
				fg_Move(_Subscription)
				, [=, pConfig = Internal.m_pConfig](CInternal::CRunningSyncState &_RSyncState)
				{
					_RSyncState.m_File.f_Open(CFile::fs_AppendPath(pConfig->m_BasePath, FilePath), EFileOpen_Read | EFileOpen_ShareRead);
					_RSyncState.m_pRSyncServer = fg_Construct(_RSyncState.m_File, 8*1024*1024);
				}
			)
		;
	}

	TCContinuation<void> CDirectorySyncSend::f_Finished()
	{
		auto &Internal = *mp_pInternal;
		Internal.m_bFinished = true;
		
		return fg_Explicit();
	}
}
