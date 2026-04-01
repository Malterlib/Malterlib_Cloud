// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>
#include <Mib/Concurrency/AsyncDestroy>
#include <Mib/Concurrency/LogError>
#include <Mib/Cryptography/RandomID>
#include <Mib/File/File>

#include "Malterlib_Cloud_DebugManagerHelper.h"

namespace NMib::NCloud
{
	using namespace NConcurrency;
	using namespace NContainer;
	using namespace NCryptography;
	using namespace NEncoding;
	using namespace NFile;
	using namespace NStorage;
	using namespace NStr;
	using namespace NTime;

	namespace
	{
		struct CState
		{
			virtual ~CState() = default;
			virtual TCFuture<void> f_Abort() = 0;

			CIntrusiveRefCountWithWeak m_RefCount;
		};

		struct CDownloadState : public CState
		{
			TCActor<CFileTransferReceive> m_DownloadReceive;
			CActorSubscription m_DownloadSubscription;

			TCFuture<void> f_Abort()
			{
				auto This = co_await fg_MoveThis(*this);

				if (This.m_DownloadSubscription)
					co_await fg_DestroySubscription(This.m_DownloadSubscription).f_Wrap() > fg_LogError("DebugManagerHelper", "Failed to abort download subscription");

				if (This.m_DownloadReceive)
					co_await fg_Move(This.m_DownloadReceive).f_Destroy().f_Wrap() > fg_LogError("DebugManagerHelper", "Failed to abort download receive");

				co_return {};
			}
		};

		struct CUploadState : public CState
		{
			TCActor<CFileTransferSend> m_UploadSend;
			TCActorFunctor<NConcurrency::TCFuture<void> ()> m_fFinish;

			TCFuture<void> f_Abort()
			{
				auto This = co_await fg_MoveThis(*this);

				co_await fg_Move(This.m_fFinish).f_Destroy();
				if (This.m_UploadSend)
					co_await fg_Move(This.m_UploadSend).f_Destroy().f_Wrap() > fg_LogError("DebugManagerHelper", "Failed to abort upload send");

				co_return {};
			}
		};
	}

	struct CDebugManagerHelperInternal
	{
		CDebugManagerHelperInternal(uint64 _QueueSize, fp64 _Timeout, CStr const &_RootDirectory)
			: m_QueueSize(_QueueSize)
			, m_Timeout(_Timeout)
			, m_RootDirectory(_RootDirectory)
		{
		}

		template <typename t_CResult>
		TCUnsafeFuture<CDebugManagerHelper::CUploadResult> f_Upload
			(
				NConcurrency::TCActorFunctor<TCFuture<t_CResult> (NConcurrency::TCAsyncGeneratorWithID<CDebugManager::CDownloadFile> _FilesGenerator)> _fDoUpload
				, CStr _Source
				, int32 _CompressionLevel
			)
		;

		template <typename t_CResult>
		TCUnsafeFuture<CFileTransferResult> f_Download
			(
				NConcurrency::TCActorFunctor<TCFuture<t_CResult> (NConcurrency::CActorSubscription _Subscription)> _fDoDownload
				, CStr _DestinationDirectory
	 			, CFileTransferReceive::EReceiveFlag _ReceiveFlags
				, uint64 _QueueSize
			)
		;

		CIntrusiveRefCount m_RefCount;

		CStr m_RootDirectory;
		TCMap<CStr, TCWeakPointer<CState>> m_States;
		fp64 m_Timeout = 0.0;
		uint64 m_QueueSize = 0;
	};

	CDebugManagerHelper::CDebugManagerHelper
		(
			CStr const &_RootDirectory
			, uint64 _QueueSize
			, fp64 _Timeout
		)
		: mp_pInternal(fg_Construct(_QueueSize, _Timeout, _RootDirectory))
	{
	}

	CDebugManagerHelper::~CDebugManagerHelper() = default;
	CDebugManagerHelper::CDebugManagerHelper(CDebugManagerHelper const &) = default;
	CDebugManagerHelper::CDebugManagerHelper(CDebugManagerHelper &&) = default;
	CDebugManagerHelper &CDebugManagerHelper::operator = (CDebugManagerHelper const &) = default;
	CDebugManagerHelper &CDebugManagerHelper::operator = (CDebugManagerHelper &&) = default;

	TCFuture<void> CDebugManagerHelper::f_AbortAll() const
	{
		auto &Internal = *mp_pInternal;

		TCFutureVector<void> Destroys;

		for (auto &pState : Internal.m_States)
		{
			auto pLockedState = pState.f_Lock();
			if (pLockedState)
				pLockedState->f_Abort() > Destroys;
		}

		TCPromiseFuturePair<void> Promise;

		fg_AllDoneWrapped(Destroys) > fg_Move(Promise.m_Promise).f_ReceiveAny();

		return fg_Move(Promise.m_Future);
	}

	template <typename t_CResult>
	TCUnsafeFuture<CDebugManagerHelper::CUploadResult> CDebugManagerHelperInternal::f_Upload
		(
			NConcurrency::TCActorFunctor<TCFuture<t_CResult> (NConcurrency::TCAsyncGeneratorWithID<CDebugManager::CDownloadFile> _FilesGenerator)> _fDoUpload
			, CStr _Source
			, int32 _CompressionLevel
		)
	{
		auto CheckDestroy = co_await fg_CurrentActorCheckDestroyedOnResume();

		TCSharedPointer<CUploadState, CSupportWeakTag> pState = fg_Construct();

		pState->m_UploadSend = fg_ConstructActor<CFileTransferSend>(_Source);

		auto SendFilesResult = co_await pState->m_UploadSend.f_Bind<&CFileTransferSend::f_SendFiles>
			(
				CFileTransferSend::CSendFilesOptions{.m_ZstandardLevel = _CompressionLevel, .m_bIncludeRootDirectoryName = true, .m_bCompressZstandard = true}
			)
		;

		NConcurrency::TCAsyncGeneratorWithID<CDebugManager::CDownloadFile> FilesGenerator
			= CFileTransferSendDownloadFile::fs_TranslateGenerator<CDebugManager::CDownloadFile>(fg_Move(SendFilesResult.m_FilesGenerator))
		;
		FilesGenerator.f_SetSubscription(fg_Move(SendFilesResult.m_Subscription));
		auto TransferResultFuture = fg_Move(SendFilesResult.m_Result);

		auto AbortDestroyOnTimeout = co_await fg_AsyncDestroy
			(
				[pState] -> TCFuture<void>
				{
					auto pLocalState = pState;
					co_await pLocalState->f_Abort();
					co_return {};
				}
			)
		;

		auto CleanupResult = g_OnScopeExit / [&TransferResultFuture]
			{
				fg_Move(TransferResultFuture).f_DiscardResult();
			}
		;

		CStr StateID = fg_FastRandomID(m_States);
		m_States[StateID] = pState;

		auto StateCleanup = g_ActorSubscription / [pInternal = TCSharedPointer<CDebugManagerHelperInternal>(fg_Explicit(this)), StateID]
			{
				auto &Internal = *pInternal;
				Internal.m_States.f_Remove(StateID);
			}
		;

		auto Result = co_await (_fDoUpload(fg_Move(FilesGenerator)).f_Timeout(m_Timeout, "Timed out waiting for debug manager to reply") % "Failed to start upload on remote server");

		AbortDestroyOnTimeout.f_Clear();

		pState->m_fFinish = fg_Move(Result.m_fFinish);

		CleanupResult.f_Clear();

		auto TransferResult = co_await fg_Move(TransferResultFuture).f_Wrap();

		if (!TransferResult)
		{
			if (TransferResult.f_GetExceptionStr() == "File transfer aborted" && !pState->m_fFinish.f_IsEmpty() && pState->m_fFinish.f_GetFunctor())
				co_await pState->m_fFinish();

			co_return TransferResult.f_GetException();
		}

		if (!pState->m_fFinish.f_IsEmpty() && pState->m_fFinish.f_GetFunctor())
			co_await pState->m_fFinish();

		pState->m_fFinish.f_Clear();

		CDebugManagerHelper::CUploadResult ReturnResult;
		ReturnResult.m_TransferResult = fg_Move(*TransferResult);

		co_return fg_Move(ReturnResult);
	}

	template <typename t_CResult>
	TCUnsafeFuture<CFileTransferResult> CDebugManagerHelperInternal::f_Download
		(
			NConcurrency::TCActorFunctor<TCFuture<t_CResult> (NConcurrency::CActorSubscription _Subscription)> _fDoDownload
			, CStr _DestinationDirectory
			, CFileTransferReceive::EReceiveFlag _ReceiveFlags
			, uint64 _QueueSize
		)
	{
		auto CheckDestroy = co_await fg_CurrentActorCheckDestroyedOnResume();

		TCSharedPointer<CDownloadState, CSupportWeakTag> pState = fg_Construct();

		pState->m_DownloadReceive = fg_ConstructActor<CFileTransferReceive>
			(
				_DestinationDirectory
				, EFileAttrib_UserRead | EFileAttrib_UserWrite | EFileAttrib_UserExecute | EFileAttrib_UnixAttributesValid
				, EFileAttrib_UserRead | EFileAttrib_UserWrite | EFileAttrib_UnixAttributesValid
			)
		;

		CStr StateID = fg_FastRandomID(m_States);
		m_States[StateID] = pState;

		auto StateCleanup = g_ActorSubscription / [StateID, pInternal = TCSharedPointer<CDebugManagerHelperInternal>(fg_Explicit(this))]
			{
				auto &Internal = *pInternal;
				Internal.m_States.f_Remove(StateID);
			}
		;

		auto Subscription = co_await pState->m_DownloadReceive.f_Bind<&CFileTransferReceive::f_GetAbortSubscription>();

		auto DestroyAbort = co_await fg_AsyncDestroy
			(
				[pState]() -> TCFuture<void>
				{
					auto pLocalState = pState;
					co_await pLocalState->f_Abort();
					co_return {};
				}
			)
		;

		auto Result = co_await
			(
				_fDoDownload(fg_Move(Subscription))
				.f_Timeout(m_Timeout, "Timed out waiting for debug manager to reply")
				% "Failed to start download on remote server"
			)
		;

		if (!Result.m_FilesGenerator.f_IsValid())
			co_return DMibErrorInstance("Download missing files generator");

		pState->m_DownloadSubscription = fg_Move(Result.m_Subscription);

		auto ReceiveResult = co_await
			(
				pState->m_DownloadReceive
				(
					&CFileTransferReceive::f_ReceiveFiles
					, CFileTransferSendDownloadFile::fs_TranslateGenerator<CFileTransferSendDownloadFile>(fg_Move(Result.m_FilesGenerator))
					, _QueueSize ? _QueueSize : m_QueueSize
					, _ReceiveFlags | CFileTransferReceive::EReceiveFlag_DecompressZstandard
				)
				% "Failed to receive files"
			)
			.f_Wrap()
		;

		if (pState->m_DownloadSubscription)
			(void)co_await fg_Exchange(pState->m_DownloadSubscription, nullptr)->f_Destroy().f_Wrap();

		co_return fg_Move(ReceiveResult);
	}

	TCUnsafeFuture<CDebugManagerHelper::CUploadResult> CDebugManagerHelper::f_Asset_Upload
		(
			NConcurrency::TCDistributedActor<CDebugManager> _DebugManager
			, NStr::CStr _Source
			, CDebugManager::EAssetType _AssetType
			, CDebugManager::CMetadata _Metadata
			, CDebugManager::EUploadFlag _Flags
			, uint64 _QueueSize
			, int32 _CompressionLevel
		) const
	{
		auto &Internal = *mp_pInternal;

		co_return co_await Internal.f_Upload<CDebugManager::CAssetUpload::CResult>
			(
				g_ActorFunctor / [_DebugManager, _AssetType, QueueSize = _QueueSize ? _QueueSize : Internal.m_QueueSize, _Flags, Metadata = fg_Move(_Metadata)]
				(NConcurrency::TCAsyncGeneratorWithID<CDebugManager::CDownloadFile> _FilesGenerator) mutable -> TCFuture<CDebugManager::CAssetUpload::CResult>
				{
					CDebugManager::CAssetUpload UploadParams;
					UploadParams.m_AssetType = _AssetType;
					UploadParams.m_QueueSize = QueueSize;
					UploadParams.m_Flags = _Flags;
					UploadParams.m_FilesGenerator = fg_Move(_FilesGenerator);
					UploadParams.m_Metadata = fg_Move(Metadata);

					co_return co_await _DebugManager.f_CallActor(&CDebugManager::f_Asset_Upload)(fg_Move(UploadParams));
				}
				, _Source
				, _CompressionLevel
			)
		;
	}

	TCUnsafeFuture<CFileTransferResult> CDebugManagerHelper::f_Asset_Download
		(
			NConcurrency::TCDistributedActor<CDebugManager> _DebugManager
			, CDebugManager::CAssetFilter _Filter
			, NStr::CStr _DestinationDirectory
			, CFileTransferReceive::EReceiveFlag _ReceiveFlags
			, uint64 _QueueSize
		) const
	{
		auto &Internal = *mp_pInternal;

		co_return co_await Internal.f_Download<CDebugManager::CAssetDownload::CResult>
			(
				g_ActorFunctor / [_DebugManager, Filter = fg_Move(_Filter)](NConcurrency::CActorSubscription _Subscription) mutable -> TCFuture<CDebugManager::CAssetDownload::CResult>
				{
					CDebugManager::CAssetDownload DownloadParams;
					DownloadParams.m_Filter = fg_Move(Filter);
					DownloadParams.m_Subscription = fg_Move(_Subscription);

					co_return co_await _DebugManager.f_CallActor(&CDebugManager::f_Asset_Download)(fg_Move(DownloadParams));
				}
				, _DestinationDirectory
				, _ReceiveFlags
				, _QueueSize
			)
		;
	}

	TCUnsafeFuture<CDebugManagerHelper::CUploadResult> CDebugManagerHelper::f_CrashDump_Upload
		(
			NConcurrency::TCDistributedActor<CDebugManager> _DebugManager
			, NStr::CStr _Source
			, NStr::CStr _ID
			, CDebugManager::CMetadata _Metadata
			, NStorage::TCOptional<CStr> _ExceptionInfo
			, CDebugManager::EUploadFlag _Flags
			, uint64 _QueueSize
			, int32 _CompressionLevel
		) const
	{
		auto &Internal = *mp_pInternal;

		co_return co_await Internal.f_Upload<CDebugManager::CCrashDumpUpload::CResult>
			(
				g_ActorFunctor
				/ [_DebugManager, _ID, QueueSize = _QueueSize ? _QueueSize : Internal.m_QueueSize, _Flags, Metadata = fg_Move(_Metadata), ExceptionInfo = fg_Move(_ExceptionInfo)]
				(NConcurrency::TCAsyncGeneratorWithID<CDebugManager::CDownloadFile> _FilesGenerator) mutable -> TCFuture<CDebugManager::CCrashDumpUpload::CResult>
				{
					CDebugManager::CCrashDumpUpload UploadParams;
					UploadParams.m_ID = _ID;
					UploadParams.m_QueueSize = QueueSize;
					UploadParams.m_Flags = _Flags;
					UploadParams.m_FilesGenerator = fg_Move(_FilesGenerator);
					UploadParams.m_Metadata = fg_Move(Metadata);
					UploadParams.m_ExceptionInfo = fg_Move(ExceptionInfo);

					co_return co_await _DebugManager.f_CallActor(&CDebugManager::f_CrashDump_Upload)(fg_Move(UploadParams));
				}
				, _Source
				, _CompressionLevel
			)
		;
	}

	TCUnsafeFuture<CFileTransferResult> CDebugManagerHelper::f_CrashDump_Download
		(
			NConcurrency::TCDistributedActor<CDebugManager> _DebugManager
			, CDebugManager::CCrashDumpFilter _Filter
			, NStr::CStr _DestinationDirectory
			, CFileTransferReceive::EReceiveFlag _ReceiveFlags
			, uint64 _QueueSize
		) const
	{
		auto &Internal = *mp_pInternal;

		co_return co_await Internal.f_Download<CDebugManager::CCrashDumpDownload::CResult>
			(
				g_ActorFunctor / [_DebugManager, Filter = fg_Move(_Filter)](NConcurrency::CActorSubscription _Subscription) mutable -> TCFuture<CDebugManager::CCrashDumpDownload::CResult>
				{
					CDebugManager::CCrashDumpDownload DownloadParams;
					DownloadParams.m_Filter = fg_Move(Filter);
					DownloadParams.m_Subscription = fg_Move(_Subscription);

					co_return co_await _DebugManager.f_CallActor(&CDebugManager::f_CrashDump_Download)(fg_Move(DownloadParams));
				}
				, _DestinationDirectory
				, _ReceiveFlags
				, _QueueSize
			)
		;
	}
}
