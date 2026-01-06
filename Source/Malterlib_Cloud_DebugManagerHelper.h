// Copyright © 2025 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>

#include "Malterlib_Cloud_DebugManager.h"

namespace NMib::NCloud
{
	struct CDebugManagerHelperInternal;
}

DMibDefineSharedPointerType(NMib::NCloud::CDebugManagerHelperInternal, true, false);

namespace NMib::NCloud
{
	struct CDebugManagerHelper
	{
		CDebugManagerHelper
			(
				NStr::CStr const &_RootDirectory
				, uint64 _QueueSize = NFile::gc_IdealNetworkQueueSize
				, fp64 _Timeout = 30.0
			)
		;
		~CDebugManagerHelper();

		CDebugManagerHelper(CDebugManagerHelper const &);
		CDebugManagerHelper(CDebugManagerHelper &&);
		CDebugManagerHelper &operator = (CDebugManagerHelper const &);
		CDebugManagerHelper &operator = (CDebugManagerHelper &&);

		struct CUploadResult
		{
			CFileTransferResult m_TransferResult;
		};

		NConcurrency::TCFuture<void> f_AbortAll() const;

		NConcurrency::TCUnsafeFuture<CUploadResult> f_Asset_Upload
			(
				NConcurrency::TCDistributedActor<CDebugManager> _DebugManager
				, NStr::CStr _Source
				, CDebugManager::EAssetType _AssetType
				, CDebugManager::CMetadata _Metadata
				, CDebugManager::EUploadFlag _Flags = CDebugManager::EUploadFlag::mc_None
				, uint64 _QueueSize = 0
				, int32 _CompressionLevel = 3
			) const
		;

		NConcurrency::TCUnsafeFuture<CFileTransferResult> f_Asset_Download
			(
				NConcurrency::TCDistributedActor<CDebugManager> _DebugManager
				, CDebugManager::CAssetFilter _Filter
				, NStr::CStr _DestinationDirectory
				, CFileTransferReceive::EReceiveFlag _ReceiveFlags = CFileTransferReceive::EReceiveFlag_IgnoreExisting
				, uint64 _QueueSize = 0
			) const
		;

		NConcurrency::TCUnsafeFuture<CUploadResult> f_CrashDump_Upload
			(
				NConcurrency::TCDistributedActor<CDebugManager> _DebugManager
				, NStr::CStr _Source
				, NStr::CStr _ID
				, CDebugManager::CMetadata _Metadata
				, NStorage::TCOptional<NStr::CStr> _ExceptionInfo
				, CDebugManager::EUploadFlag _Flags = CDebugManager::EUploadFlag::mc_None
				, uint64 _QueueSize = 0
				, int32 _CompressionLevel = 3
			) const
		;

		NConcurrency::TCUnsafeFuture<CFileTransferResult> f_CrashDump_Download
			(
				NConcurrency::TCDistributedActor<CDebugManager> _DebugManager
				, CDebugManager::CCrashDumpFilter _Filter
				, NStr::CStr _DestinationDirectory
				, CFileTransferReceive::EReceiveFlag _ReceiveFlags = CFileTransferReceive::EReceiveFlag_IgnoreExisting
				, uint64 _QueueSize = 0
			) const
		;

	private:
		NStorage::TCSharedPointer<CDebugManagerHelperInternal> mp_pInternal;
	};
}
