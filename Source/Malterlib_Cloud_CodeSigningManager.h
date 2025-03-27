// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <Mib/Core/Core>

#include <Mib/Cloud/FileTransfer>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Encoding/EJson>

namespace NMib::NCloud
{
	struct CCodeSigningManager : public NConcurrency::CActor
	{
		static constexpr ch8 const *mc_pDefaultNamespace = "com.malterlib/Cloud/CodeSigningManager";

		enum : uint32
		{
			EProtocolVersion_Min = 0x101
			, EProtocolVersion_SupportExecutableSigning = 0x102
			, EProtocolVersion_Current = 0x102
		};

		struct CDownloadFileContents
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NConcurrency::TCAsyncGenerator<NContainer::CIOByteVector> m_DataGenerator;
			NConcurrency::TCActorSubscriptionWithID<> m_Subscription;
			uint64 m_StartPosition = 0;
		};

		struct CDownloadFile
		{
			using CDownloadFileContents = CCodeSigningManager::CDownloadFileContents;

			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NStr::CStr m_FilePath;
			NFile::EFileAttrib m_FileAttributes = NFile::EFileAttrib_None;
			NTime::CTime m_WriteTime;
			uint64 m_FileSize = 0;
			NStr::CStr m_SymlinkContents;
			NConcurrency::TCActorFunctorWithID
			<
				NConcurrency::TCFuture<CDownloadFileContents> (uint64 _StartPosition, NCryptography::CHashDigest_SHA256 _StartDigest)
			> m_fGetDataGenerator;
		};

		struct CSignFiles
		{
			struct CResult
			{
				template <typename tf_CStream>
				void f_Stream(tf_CStream &_Stream);

				NConcurrency::TCActorFunctorWithID<NConcurrency::TCFuture<NEncoding::CEJsonSorted> ()> m_fGetSignature;
			};

			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NStorage::TCOptional<NStr::CStr> m_Authority;
			NStorage::TCOptional<NStr::CStr> m_SigningCert;
			uint64 m_QueueSize = NFile::gc_IdealNetworkQueueSize;
			NConcurrency::TCAsyncGeneratorWithID<CDownloadFile> m_FilesGenerator;
		};

		CCodeSigningManager();
		~CCodeSigningManager();

		virtual NConcurrency::TCFuture<CSignFiles::CResult> f_SignFiles(CSignFiles _Params) = 0;
	};
}

#include "Malterlib_Cloud_CodeSigningManager.hpp"
