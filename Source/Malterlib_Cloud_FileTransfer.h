// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedActor>

namespace NMib::NCloud
{
	struct CFileTransferSend;
	struct CFileTransferReceive;
	
	struct CFileTransferResult
	{
		uint64 m_nBytes = 0;
		fp64 m_nSeconds = 0.0;

		fp64 f_BytesPerSecond() const;

		void f_Feed(NConcurrency::CDistributedActorWriteStream &_Stream) const;
		void f_Consume(NConcurrency::CDistributedActorReadStream &_Stream);
	};
	
	struct CFileTransferContextDeprecated
	{
		CFileTransferContextDeprecated();
		~CFileTransferContextDeprecated();
		CFileTransferContextDeprecated(CFileTransferContextDeprecated const &_Other) = delete;
		CFileTransferContextDeprecated &operator =(CFileTransferContextDeprecated const &_Other) = delete;
		CFileTransferContextDeprecated(CFileTransferContextDeprecated &&_Other);
		CFileTransferContextDeprecated &operator =(CFileTransferContextDeprecated &&_Other);
		static bool fs_IsSafeRelativePath(NStr::CStr const &_String, NStr::CStr &o_Error);
		void f_Feed(NConcurrency::CDistributedActorWriteStream &_Stream) const;
		void f_Consume(NConcurrency::CDistributedActorReadStream &_Stream);
		
	private:
		friend struct CFileTransferSend;
		friend struct CFileTransferReceive;
		struct CInternal;
		NStorage::TCUniquePointer<CInternal> mp_pInternal;
	};

	struct CFileTransferSendDownloadFileContents
	{
		NConcurrency::TCAsyncGenerator<NContainer::CIOByteVector> m_DataGenerator;
		NConcurrency::CActorSubscription m_Subscription;
		uint64 m_StartPosition;
	};
	
	struct CFileTransferSendDownloadFile
	{
		using CDownloadFileContents = CFileTransferSendDownloadFileContents;

		NStr::CStr m_FilePath;
		NFile::EFileAttrib m_FileAttributes = NFile::EFileAttrib_None;
		NTime::CTime m_WriteTime;
		uint64 m_FileSize = 0;
		NStr::CStr m_SymlinkContents;
		NConcurrency::TCActorFunctor<NConcurrency::TCFuture<CDownloadFileContents> (uint64 _StartPosition, NCryptography::CHashDigest_SHA256 _StartDigest)> m_fGetDataGenerator;

		template <typename tf_CTypeTo, typename tf_CTypeFrom>
		static NConcurrency::TCAsyncGenerator<tf_CTypeTo> fs_TranslateGenerator(NConcurrency::TCAsyncGenerator<tf_CTypeFrom> _FilesGenerator);
	};

	struct CFileTransferSend : public NConcurrency::CActor
	{
		struct CSendFilesResult
		{
			NConcurrency::TCAsyncGenerator<CFileTransferSendDownloadFile> m_FilesGenerator;
			NConcurrency::CActorSubscription m_Subscription;
			NConcurrency::TCFuture<CFileTransferResult> m_Result;
		};

		struct CSendFilesResultDeprecated
		{
			NConcurrency::CActorSubscription m_Subscription;
			NConcurrency::TCFuture<CFileTransferResult> m_Result;
		};

		struct CSendFilesOptions
		{
			int32 m_ZstandardLevel = 3;
			bool m_bIncludeRootDirectoryName:1 = false;
			bool m_bCompressZstandard:1 = false;
		};

		struct CBasePath
		{
			NStr::CStr m_Path;
			NStr::CStr m_Name;
		};

		~CFileTransferSend();
		CFileTransferSend(NStr::CStr const &_BasePath, uint64 _MaxQueueSize = NFile::gc_IdealNetworkQueueSize);
		CFileTransferSend(NContainer::TCVector<CBasePath> &&_BasePaths, uint64 _MaxQueueSize = NFile::gc_IdealNetworkQueueSize);

		NConcurrency::TCFuture<CSendFilesResultDeprecated> f_SendFilesDeprecated(CFileTransferContextDeprecated _TransferContext);

		NConcurrency::TCFuture<CSendFilesResult> f_SendFiles(CSendFilesOptions _Options);

	private:
		NConcurrency::TCFuture<void> fp_Destroy();

		struct CInternal;
		NStorage::TCUniquePointer<CInternal> mp_pInternal;
	};
	
	struct CFileTransferReceive : public NConcurrency::CActor
	{
		~CFileTransferReceive();
		CFileTransferReceive
			(
				NStr::CStr const &_BasePath
				, NFile::EFileAttrib _AttributeMask = NFile::EFileAttrib_UserRead | NFile::EFileAttrib_UserWrite | NFile::EFileAttrib_UserExecute | NFile::EFileAttrib_UnixAttributesValid
				, NFile::EFileAttrib _AttributeAdd = NFile::EFileAttrib_None
			)
		;
		
		enum EReceiveFlag
		{
			EReceiveFlag_None = 0
			, EReceiveFlag_IgnoreExisting = DMibBit(0)
			, EReceiveFlag_FailOnExisting = DMibBit(1)
			, EReceiveFlag_DeleteExisting = DMibBit(2)
			, EReceiveFlag_DecompressZstandard = DMibBit(3)
		};

		NConcurrency::TCFuture<CFileTransferContextDeprecated> f_ReceiveFilesDeprecated(uint64 _QueueSize, EReceiveFlag _Flags);
		NConcurrency::TCFuture<CFileTransferResult> f_GetResultDeprecated();

		NConcurrency::TCFuture<CFileTransferResult> f_ReceiveFiles(NConcurrency::TCAsyncGenerator<CFileTransferSendDownloadFile> _FilesGenerator, uint64 _QueueSize, EReceiveFlag _Flags);

		NConcurrency::TCFuture<NConcurrency::CActorSubscription> f_GetAbortSubscription();

	private:
		NConcurrency::TCFuture<void> fp_Destroy() override;

		struct CInternal;
		NStorage::TCUniquePointer<CInternal> mp_pInternal;
	};
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NCloud;
#endif

#include "Malterlib_Cloud_FileTransfer.hpp"
