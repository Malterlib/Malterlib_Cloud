// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Encoding/EJSON>

#include "Malterlib_Cloud_FileTransfer.h"
#include "Malterlib_Cloud_VersionInfo.h"

namespace NMib::NCloud
{
	struct CVersionManagerHelperInternal;
}

DMibDefineSharedPointerType(NMib::NCloud::CVersionManagerHelperInternal, false, false);

namespace NMib::NCloud
{
	struct CVersionManager : public NConcurrency::CActor
	{
		static constexpr ch8 const *mc_pDefaultNamespace = "com.malterlib/Cloud/VersionManager";

		enum : uint32
		{
			EProtocolVersion_Min = 0x103
			, EProtocolVersion_SupportSubscribeToTags = 0x104
			, EProtocolVersion_SupportIncreaseRetrySequence = 0x105
			, EProtocolVersion_RefactorToActorFunctorsUploadDownload = 0x106
			, EProtocolVersion_RefactorToActorFunctorsSubscribeChanges = 0x107
			, EProtocolVersion_RenamePlatforms = 0x108
			, EProtocolVersion_Current = 0x108
		};

		struct CVersionID : public CCloudVersion
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			template <typename tf_CStr>
			void f_Format(tf_CStr &o_Str) const;

			bool f_IsValid() const;
			
			NStr::CStr f_EncodeFileName() const;
			static NStr::CStr fs_DecodeFileName(NStr::CStr const &_FileName);
			
			auto operator <=> (CVersionID const &_Right) const = default;
			
			NEncoding::CEJSON f_ToJson() const;
			static CVersionID fs_FromJson(NEncoding::CEJSON const &_JSON);
		};

		struct CVersionIDAndPlatform
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);
			
			template <typename tf_CStr>
			void f_Format(tf_CStr &o_Str) const;

			bool f_IsValid() const;

			NStr::CStr f_EncodeFileName() const;

			static NStr::CStr fs_ConvertFromOldPlatform(NStr::CStr const &_Platform);

			auto operator <=> (CVersionIDAndPlatform const &_Right) const = default;
			
			NEncoding::CEJSON f_ToJson() const;
			static CVersionIDAndPlatform fs_FromJson(NEncoding::CEJSON const &_JSON);
			
			CVersionID m_VersionID;
			NStr::CStr m_Platform;
		};
		
		struct CVersionInformation
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NEncoding::CEJSON f_ToJson() const;
			static CVersionInformation fs_FromJson(NEncoding::CEJSON const &_JSON);

			auto operator <=> (CVersionInformation const &_Right) const = default;

			NTime::CTime m_Time;
			NStr::CStr m_Configuration;
			NContainer::TCSet<NStr::CStr> m_Tags;
			NEncoding::CEJSON m_ExtraInfo;
			uint32 m_RetrySequence = 0;
			uint32 m_nFiles = 0; /// The version manager set this value. Ignored when uploading
			uint64 m_nBytes = 0; /// The version manager set this value. Ignored when uploading
		};

		struct CListApplications
		{
			struct CResult
			{
				template <typename tf_CStream>
				void f_Stream(tf_CStream &_Stream);
				
				NContainer::TCSet<NStr::CStr> m_Applications;
			};

			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);
		};

		struct CListVersions
		{
			struct CResult
			{
				template <typename tf_CStream>
				void f_Stream(tf_CStream &_Stream);

				NContainer::TCMap<NStr::CStr, NContainer::TCMap<CVersionIDAndPlatform, CVersionInformation>> m_Versions;
			};

			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NStr::CStr m_ForApplication;
		};

		struct CStartUploadTransfer
		{
			struct CResult
			{
				template <typename tf_CStream>
				void f_Stream(tf_CStream &_Stream);

				NConcurrency::CActorSubscription m_Subscription;
			};

			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			CFileTransferContext m_TransferContext;
		};
		
		struct CStartUploadVersion
		{
			struct CResult
			{
				template <typename tf_CStream>
				void f_Stream(tf_CStream &_Stream);
				
				NConcurrency::TCActorFunctorWithID<NConcurrency::TCFuture<void> ()> m_fFinish;
				NContainer::TCSet<NStr::CStr> m_DeniedTags;
			};
			
			enum EFlag
			{
				EFlag_None = 0
				, EFlag_ForceOverwrite = DMibBit(0)
			};

			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);
		
			NStr::CStr m_Application;
			CVersionIDAndPlatform m_VersionIDAndPlatform;
			CVersionInformation m_VersionInfo;
			uint64 m_QueueSize = 8*1024*1024;
			EFlag m_Flags = EFlag_None;
			NConcurrency::TCActorFunctorWithID<NConcurrency::TCFuture<CStartUploadTransfer::CResult> (CStartUploadTransfer &&_Params)> m_fStartTransfer;
		};
		
		struct CStartDownloadVersion
		{
			struct CResult
			{
				template <typename tf_CStream>
				void f_Stream(tf_CStream &_Stream);
				
				NConcurrency::CActorSubscription m_Subscription;
				CVersionInformation m_VersionInfo;
			};

			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);
			
			NStr::CStr m_Application;
			CVersionIDAndPlatform m_VersionIDAndPlatform;
			CFileTransferContext m_TransferContext;
			NConcurrency::CActorSubscription m_Subscription;
		};
		
		struct CNewVersionNotification
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);
			
			NStr::CStr m_Application;
			CVersionIDAndPlatform m_VersionIDAndPlatform;
			CVersionInformation m_VersionInfo;
		};

		struct CNewVersionNotifications
		{
			struct CResult
			{
				template <typename tf_CStream>
				void f_Stream(tf_CStream &_Stream);
			};

			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);
			
			bool m_bFullResend = false;
			NContainer::TCVector<CNewVersionNotification> m_NewVersions;
		};
		
		struct CSubscribeToUpdates
		{
			struct CResult
			{
				template <typename tf_CStream>
				void f_Stream(tf_CStream &_Stream);
				
				NConcurrency::TCActorSubscriptionWithID<> m_Subscription;
			};

			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);
			
			NStr::CStr m_Application; /// Leave empty to subscribe to all applications
			NContainer::TCSet<NStr::CStr> m_Platforms; /// Leave empty to subscribe to all platforms
			NContainer::TCSet<NStr::CStr> m_Tags; /// Leave empty to subscribe to all tags
			NConcurrency::TCActorFunctorWithID<NConcurrency::TCFuture<CNewVersionNotifications::CResult> (CNewVersionNotifications &&_VersionInfo)> m_fOnNewVersions;
			uint32 m_nInitial = 10;
		};

		struct CChangeTags
		{
			struct CResult
			{
				template <typename tf_CStream>
				void f_Stream(tf_CStream &_Stream);
				
				NContainer::TCSet<NStr::CStr> m_DeniedTags;
			};
			
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);
			
			NStr::CStr m_Application;
			CVersionID m_VersionID;
			NStr::CStr m_Platform; /// Leave empty to tag all platforms for version
			NContainer::TCSet<NStr::CStr> m_AddTags;
			NContainer::TCSet<NStr::CStr> m_RemoveTags;
			bool m_bIncreaseRetrySequence = false;
		};

		static bool fs_IsValidApplicationName(NStr::CStr const &_String);
		static bool fs_IsValidProtocolVersion(uint32 _Version);
		static bool fs_IsValidTag(NStr::CStr const &_String);
		static bool fs_IsValidBranch(NStr::CStr const &_String, bool _bAllowWildCards = false);
		static bool fs_IsValidVersionIdentifier(NStr::CStr const &_String, NStr::CStr &o_Error, CVersionID *o_pVersionID);
		static bool fs_IsValidVersionIdentifier(CVersionID const &_VersionID, NStr::CStr &o_Error);
		static bool fs_IsValidPlatform(NStr::CStr const &_String);
		
		CVersionManager();
		~CVersionManager();
		
		virtual NConcurrency::TCFuture<CListApplications::CResult> f_ListApplications(CListApplications &&_Params) = 0;
		virtual NConcurrency::TCFuture<CListVersions::CResult> f_ListVersions(CListVersions &&_Params) = 0;
		virtual NConcurrency::TCFuture<CStartUploadVersion::CResult> f_UploadVersion(CStartUploadVersion &&_Params) = 0;
		virtual NConcurrency::TCFuture<CStartDownloadVersion::CResult> f_DownloadVersion(CStartDownloadVersion &&_Params) = 0;
		virtual NConcurrency::TCFuture<CSubscribeToUpdates::CResult> f_SubscribeToUpdates(CSubscribeToUpdates &&_Params) = 0;
		virtual NConcurrency::TCFuture<CChangeTags::CResult> f_ChangeTags(CChangeTags &&_Params) = 0;
	};
	
	struct CVersionManagerHelper
	{
		CVersionManagerHelper
			(
				NStr::CStr const &_RootDirectory
				, NConcurrency::TCActor<NConcurrency::CSeparateThreadActor> const &_FileActor = {}
				, uint64 _QueueSize = 8*1024*1024
				, fp64 _Timeout = 30.0
			)
		;
		~CVersionManagerHelper();

		CVersionManagerHelper(CVersionManagerHelper const &);
		CVersionManagerHelper(CVersionManagerHelper &&);
		CVersionManagerHelper &operator = (CVersionManagerHelper const &);
		CVersionManagerHelper &operator = (CVersionManagerHelper &&);
		
		struct CUploadResult
		{
			NContainer::TCSet<NStr::CStr> m_DeniedTags;
			CFileTransferResult m_TransferResult;
		};
		
		struct CPackageInfo
		{
			CVersionManager::CVersionIDAndPlatform m_VersionID;
			CVersionManager::CVersionInformation m_VersionInfo;
		};
		
		NConcurrency::TCFuture<CUploadResult> f_Upload
			(
				NConcurrency::TCDistributedActor<CVersionManager> const &_VersionManager
				, NStr::CStr const &_Application
				, CVersionManager::CVersionIDAndPlatform const &_VersionID
				, CVersionManager::CVersionInformation const &_VersionInfo
				, NStr::CStr const &_SourceTGZFile
				, CVersionManager::CStartUploadVersion::EFlag _Flags = CVersionManager::CStartUploadVersion::EFlag_None
				, uint64 _QueueSize = 0
			) const
		;
		
		NConcurrency::TCFuture<CFileTransferResult> f_Download
			(
				NConcurrency::TCDistributedActor<CVersionManager> const &_VersionManager
				, NStr::CStr const &_Application
				, CVersionManager::CVersionIDAndPlatform const &_VersionID
				, NStr::CStr const &_DestinationDirectory
				, CFileTransferReceive::EReceiveFlag _ReceiveFlags = CFileTransferReceive::EReceiveFlag_IgnoreExisting
				, uint64 _QueueSize = 0
			) const
		;
		
		NConcurrency::TCFuture<CPackageInfo> f_CreatePackage(NStr::CStr const &_SourceDirectory, NStr::CStr const &_DestinationFileName, uint32 _CompressionLevel) const;
		NConcurrency::TCFuture<CPackageInfo> f_GetPackageInfo(NStr::CStr const &_PackageFile) const;
		NConcurrency::TCFuture<void> f_AbortAll() const;
		
		NConcurrency::TCActor<NConcurrency::CSeparateThreadActor> f_GetFileActor() const;
		
	private:
		NStorage::TCSharedPointer<CVersionManagerHelperInternal> mp_pInternal;
	};
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NCloud;
#endif

#include "Malterlib_Cloud_VersionManager.hpp"
