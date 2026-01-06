// Copyright © 2025 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>

#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Encoding/EJson>

#include "Malterlib_Cloud_FileTransfer.h"

namespace NMib::NCloud
{
	struct CDebugManager : public NConcurrency::CActor
	{
		static constexpr ch8 const *mc_pDefaultNamespace = "com.malterlib/Cloud/DebugManager";

		enum : uint32
		{
			EProtocolVersion_Min = 0x101
			, EProtocolVersion_Current = 0x101
		};

		struct CDownloadFileContents
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NConcurrency::TCAsyncGenerator<NContainer::CIOByteVector> m_DataGenerator;
			NConcurrency::TCActorSubscriptionWithID<> m_Subscription;
			uint64 m_StartPosition;
		};

		struct CDownloadFile
		{
			using CDownloadFileContents = CDownloadFileContents;

			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NStr::CStr m_FilePath;
			NFile::EFileAttrib m_FileAttributes = NFile::EFileAttrib_None;
			NTime::CTime m_WriteTime;
			uint64 m_FileSize = 0;
			NStr::CStr m_SymlinkContents;
			NConcurrency::TCActorFunctorWithID<NConcurrency::TCFuture<CDownloadFileContents> (uint64 _StartPosition, NCryptography::CHashDigest_SHA256 _StartDigest)> m_fGetDataGenerator;
		};

		enum class EAssetType : uint32
		{
			mc_Executable = 0
			, mc_DebugInfo = 1
		};

		enum class EUploadFlag : uint32
		{
			mc_None = 0
			, mc_ForceOverwrite = DMibBit(0)
		};

		struct CFileInfo
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NStr::CStr m_FileName;
			NTime::CTime m_Timestamp;
			NCryptography::CHashDigest_SHA256 m_Digest;
			uint64 m_Size = 0;
			uint64 m_CompressedSize = 0;
		};

		struct CMetadata
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NStorage::TCOptional<NStr::CStr> m_Product; // Defaults to workspace name
			NStorage::TCOptional<NStr::CStr> m_Application;
			NStorage::TCOptional<NStr::CStr> m_Configuration;
			NStorage::TCOptional<NStr::CStr> m_GitBranch;
			NStorage::TCOptional<NStr::CStr> m_GitCommit;
			NStorage::TCOptional<NStr::CStr> m_Platform;
			NStorage::TCOptional<NStr::CStr> m_Version;
			NStorage::TCOptional<NContainer::TCSet<NStr::CStr>> m_Tags;
		};

		struct CAssetFilter
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NStorage::TCOptional<EAssetType> m_AssetType;
			NStorage::TCOptional<NStr::CStr> m_BuildID;
			NStorage::TCOptional<NStr::CStr> m_FileName;
			NStorage::TCOptional<NTime::CTime> m_TimestampStart;
			NStorage::TCOptional<NTime::CTime> m_TimestampEnd;
			CMetadata m_Metadata;
		};

		struct CAssetList
		{
			struct CAsset
			{
				template <typename tf_CStream>
				void f_Stream(tf_CStream &_Stream);

				EAssetType m_AssetType;
				NStr::CStr m_BuildID;
				CFileInfo m_FileInfo;
				CMetadata m_Metadata;
				NStr::CStr m_MainAssetFile;
			};

			struct CResult
			{
				template <typename tf_CStream>
				void f_Stream(tf_CStream &_Stream);

				NConcurrency::TCAsyncGenerator<NContainer::TCVector<CAsset>> m_AssetsGenerator;
			};

			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			CAssetFilter m_Filter;
		};

		struct CAssetUpload
		{
			struct CResult
			{
				template <typename tf_CStream>
				void f_Stream(tf_CStream &_Stream);

				NConcurrency::TCActorFunctorWithID<NConcurrency::TCFuture<void> ()> m_fFinish;
			};

			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			EAssetType m_AssetType = EAssetType::mc_Executable;

			uint64 m_QueueSize = NFile::gc_IdealNetworkQueueSize;
			EUploadFlag m_Flags = EUploadFlag::mc_None;
			NConcurrency::TCAsyncGeneratorWithID<CDownloadFile> m_FilesGenerator;
			CMetadata m_Metadata;
		};

		struct CAssetDownload
		{
			struct CResult
			{
				template <typename tf_CStream>
				void f_Stream(tf_CStream &_Stream);

				NConcurrency::CActorSubscription m_Subscription;
				NConcurrency::TCAsyncGenerator<CDownloadFile> m_FilesGenerator;
			};

			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			CAssetFilter m_Filter;
			NConcurrency::CActorSubscription m_Subscription;
		};

		struct CAssetDelete
		{
			struct CResult
			{
				template <typename tf_CStream>
				void f_Stream(tf_CStream &_Stream);

				uint64 m_nAssetsDeleted = 0;
				uint64 m_nFilesDeleted = 0;
				uint64 m_nBytesDeleted = 0;
			};

			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			CAssetFilter m_Filter;
			uint64 m_nMaxToDelete = 1;
			bool m_bPretend = true;
		};

		struct CCrashDumpFilter
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NStorage::TCOptional<NStr::CStr> m_ID;
			NStorage::TCOptional<NStr::CStr> m_FileName;
			NStorage::TCOptional<NTime::CTime> m_TimestampStart;
			NStorage::TCOptional<NTime::CTime> m_TimestampEnd;
			CMetadata m_Metadata;
			NStorage::TCOptional<NStr::CStr> m_ExceptionInfo;
		};

		struct CCrashDumpList
		{
			struct CCrashDump
			{
				template <typename tf_CStream>
				void f_Stream(tf_CStream &_Stream);

				NStr::CStr m_ID;
				CFileInfo m_FileInfo;
				CMetadata m_Metadata;
				NStorage::TCOptional<NStr::CStr> m_ExceptionInfo;
			};

			struct CResult
			{
				template <typename tf_CStream>
				void f_Stream(tf_CStream &_Stream);

				NConcurrency::TCAsyncGenerator<NContainer::TCVector<CCrashDump>> m_CrashDumpsGenerator;
			};

			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			CCrashDumpFilter m_Filter;
		};

		struct CCrashDumpUpload
		{
			struct CResult
			{
				template <typename tf_CStream>
				void f_Stream(tf_CStream &_Stream);

				NConcurrency::TCActorFunctorWithID<NConcurrency::TCFuture<void> ()> m_fFinish;
			};

			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NStr::CStr m_ID;

			uint64 m_QueueSize = NFile::gc_IdealNetworkQueueSize;
			EUploadFlag m_Flags = EUploadFlag::mc_None;
			NConcurrency::TCAsyncGeneratorWithID<CDownloadFile> m_FilesGenerator;
			CMetadata m_Metadata;
			NStorage::TCOptional<NStr::CStr> m_ExceptionInfo;
		};

		struct CCrashDumpDownload
		{
			struct CResult
			{
				template <typename tf_CStream>
				void f_Stream(tf_CStream &_Stream);

				NConcurrency::CActorSubscription m_Subscription;
				NConcurrency::TCAsyncGenerator<CDownloadFile> m_FilesGenerator;
			};

			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			CCrashDumpFilter m_Filter;

			NConcurrency::CActorSubscription m_Subscription;
		};

		struct CCrashDumpDelete
		{
			struct CResult
			{
				template <typename tf_CStream>
				void f_Stream(tf_CStream &_Stream);

				uint64 m_nCrashDumpsDeleted = 0;
				uint64 m_nFilesDeleted = 0;
				uint64 m_nBytesDeleted = 0;
			};

			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			CCrashDumpFilter m_Filter;
			uint64 m_nMaxToDelete = 1;
			bool m_bPretend = true;
		};

		struct CCrashDumpInfos
		{
			struct CUniqueCrashDump
			{
				NContainer::TCVector<NStr::CStr> m_Sources;
				CMetadata m_Metadata;
				NStorage::TCOptional<NStr::CStr> m_ExceptionInfo;
			};

			NContainer::TCMap<NStr::CStr, CUniqueCrashDump> m_Uploads;
			NContainer::TCMap<NStr::CStr, NStr::CStr> m_Errors;
		};

		CDebugManager();
		~CDebugManager();

		virtual NConcurrency::TCFuture<CAssetList::CResult> f_Asset_List(CAssetList _Params) = 0;
		virtual NConcurrency::TCFuture<CAssetUpload::CResult> f_Asset_Upload(CAssetUpload _Params) = 0;
		virtual NConcurrency::TCFuture<CAssetDownload::CResult> f_Asset_Download(CAssetDownload _Params) = 0;
		virtual NConcurrency::TCFuture<CAssetDelete::CResult> f_Asset_Delete(CAssetDelete _Params) = 0;

		virtual NConcurrency::TCFuture<CCrashDumpList::CResult> f_CrashDump_List(CCrashDumpList _Params) = 0;
		virtual NConcurrency::TCFuture<CCrashDumpUpload::CResult> f_CrashDump_Upload(CCrashDumpUpload _Params) = 0;
		virtual NConcurrency::TCFuture<CCrashDumpDownload::CResult> f_CrashDump_Download(CCrashDumpDownload _Params) = 0;
		virtual NConcurrency::TCFuture<CCrashDumpDelete::CResult> f_CrashDump_Delete(CCrashDumpDelete _Params) = 0;

		static NStr::CStr fs_AssetTypeToStr(EAssetType _AssetType);
		static EAssetType fs_AssetTypeFromStr(NStr::CStr const &_String);
		static NConcurrency::TCFuture<CCrashDumpInfos> fs_GatherCrashDumpInfos
			(
				NContainer::TCVector<NStr::CStr> _Sources
				, CMetadata _DefaultMetadata
				, NStorage::TCOptional<NStr::CStr> _DefaultExceptionInfo
			)
		;
	};
}

#include "Malterlib_Cloud_DebugManager.hpp"
