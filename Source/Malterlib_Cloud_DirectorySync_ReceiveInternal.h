// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_DirectorySync.h"
#include <Mib/Concurrency/ActorSubscription>
#include <Mib/Cryptography/RandomID>
#include <Mib/File/RSync>

namespace NMib::NCloud
{
	using namespace NAtomic;
	using namespace NStr;
	using namespace NConcurrency;
	using namespace NContainer;
	using namespace NCryptography;
	using namespace NFunction;
	using namespace NPtr;
	using namespace NFile;
	using namespace NTime;
	
	struct CDirectorySyncReceive::CInternal
	{
		struct CByteStats
		{
			uint64 m_nIncoming = 0;
			uint64 m_nOutgoing = 0;
		};
		
		struct CRunningSyncState : public TCSharedPointerIntrusiveBase<ESharedPointerOption_SupportWeakPointer>
		{
			TCContinuation<void> f_Destroy();
			
			TCActor<CSeparateThreadActor> m_FileActor;
			
			TCBinaryStreamFile<> m_File;
			TCBinaryStreamFile<> m_SourceFile;
			TCBinaryStreamFile<> m_TempFile;
			
			TCUniquePointer<CRSyncClient> m_pClient;
			
			CDirectorySyncClient::FRunRSync m_fRunProtocol;
			
			TCVector<CStr> m_TempFiles;
			
			CByteStats m_ByteStats;
			bool m_bFinished = false;
		};
		
		CInternal(CDirectorySyncReceive *_pThis, CConfig &&_Config, TCDistributedActorInterface<CDirectorySyncClient> &&_Client);

		TCContinuation<void> f_RunRSyncProtocol(TCSharedPointerSupportWeak<CRunningSyncState> const &_pState);
		
		TCContinuation<void> f_SyncManifest();
		TCContinuation<void> f_HandleExcessFiles();

		TCContinuation<void> f_SyncFile(CStr const &_FileName);

		TCContinuation<void> f_RSync
			(
				TCFunctionMutable<bool (CRunningSyncState &_State)> &&_fInitRSync
				, TCFunctionMutable<TCContinuation<void> ()> &&_fOnDone
				, TCFunctionMutable<TCContinuation<CDirectorySyncClient::FRunRSync> (CActorSubscription &&_Subscription)> &&_fStartRSync
			)
		;
		
		void f_RunFileSyncs(TCContinuation<void> const &_Continuation);
		static void fs_CheckDestroy(TCSharedPointer<NAtomic::TCAtomic<bool>> const &_pDestroyed);
		
		CDirectorySyncReceive *m_pThis;
		TCSharedPointer<CConfig> m_pConfig;
		TCDistributedActorInterface<CDirectorySyncClient> m_Client;
		TCMap<CStr, TCSharedPointerSupportWeak<CRunningSyncState>> m_RSyncStates;
		TCSet<CStr> m_PendingFileSyncs;
		CClock m_Clock{true};
		TCSharedPointer<CDirectoryManifest> m_pManifest;
		CDirectorySyncStats m_Stats;
		TCActor<CSeparateThreadActor> m_FileActor;
		CStr m_SyncErrors;
		TCSharedPointer<TCAtomic<bool>> m_pDestroyed = fg_Construct(false);
		bool m_bStartedSync = false;
		mint m_nRunningSyncs = 0;
		
	private:
		static void fsp_RunRSyncProtocol
			(
				TCSharedPointerSupportWeak<CRunningSyncState> const &_pState
				, CSecureByteVector &&_ServerPacket
				, TCContinuation<CByteStats> const &_Continuation
			)
		;
	};
}
