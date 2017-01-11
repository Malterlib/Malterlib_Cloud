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
	struct CVersionManager : public NConcurrency::CActor
	{
		using CDistributedActorWriteStream = NConcurrency::CDistributedActorWriteStream;
		using CDistributedActorReadStream = NConcurrency::CDistributedActorReadStream;

		enum 
		{
			EMinProtocolVersion = 0x103
			, EProtocolVersion = 0x104
		};

		struct CVersionID : public CCloudVersion
		{
			void f_Feed(CDistributedActorWriteStream &_Stream) const;
			void f_Consume(CDistributedActorReadStream &_Stream);
			void f_Format(NStr::CStrAggregate &o_Str) const;
			
			bool f_IsValid() const;
			
			NStr::CStr f_EncodeFileName() const;
			static NStr::CStr fs_DecodeFileName(NStr::CStr const &_FileName);
			
			bool operator == (CVersionID const &_Right) const;
			bool operator < (CVersionID const &_Right) const;
			
			NEncoding::CEJSON f_ToJSON() const;
			static CVersionID fs_FromJSON(NEncoding::CEJSON const &_JSON);
		};

		struct CVersionIDAndPlatform
		{
			void f_Feed(CDistributedActorWriteStream &_Stream) const;
			void f_Consume(CDistributedActorReadStream &_Stream);
			void f_Format(NStr::CStrAggregate &o_Str) const;

			bool f_IsValid() const;

			NStr::CStr f_EncodeFileName() const;

			bool operator == (CVersionIDAndPlatform const &_Right) const;
			bool operator < (CVersionIDAndPlatform const &_Right) const;
			
			NEncoding::CEJSON f_ToJSON() const;
			static CVersionIDAndPlatform fs_FromJSON(NEncoding::CEJSON const &_JSON);
			
			CVersionID m_VersionID;
			NStr::CStr m_Platform;
		};
		
		struct CVersionInformation
		{
			void f_Feed(CDistributedActorWriteStream &_Stream) const;
			void f_Consume(CDistributedActorReadStream &_Stream);

			NEncoding::CEJSON f_ToJSON() const;
			static CVersionInformation fs_FromJSON(NEncoding::CEJSON const &_JSON);
			
			NTime::CTime m_Time;
			NStr::CStr m_Configuration;
			NContainer::TCSet<NStr::CStr> m_Tags;
			NEncoding::CEJSON m_ExtraInfo;
			uint32 m_nFiles = 0;
			uint64 m_nBytes = 0;
		};

		struct CListApplications
		{
			struct CResult
			{
				void f_Feed(CDistributedActorWriteStream &_Stream) const;
				void f_Consume(CDistributedActorReadStream &_Stream);
				
				NContainer::TCSet<NStr::CStr> m_Applications;
			};

			void f_Feed(CDistributedActorWriteStream &_Stream) const;
			void f_Consume(CDistributedActorReadStream &_Stream);
		};

		struct CListVersions
		{
			struct CResult
			{
				void f_Feed(CDistributedActorWriteStream &_Stream) const;
				void f_Consume(CDistributedActorReadStream &_Stream);

				NContainer::TCMap<NStr::CStr, NContainer::TCMap<CVersionIDAndPlatform, CVersionInformation>> m_Versions;
			};

			void f_Feed(CDistributedActorWriteStream &_Stream) const;
			void f_Consume(CDistributedActorReadStream &_Stream);

			NStr::CStr m_ForApplication;
		};

		struct CStartUploadTransfer
		{
			struct CResult
			{
				void f_Feed(CDistributedActorWriteStream &_Stream) &&;
				void f_Consume(CDistributedActorReadStream &_Stream);

				NConcurrency::CActorSubscription m_Subscription;
			};

			void f_Feed(CDistributedActorWriteStream &_Stream) const;
			void f_Consume(CDistributedActorReadStream &_Stream);

			CFileTransferContext m_TransferContext;
		};
		
		struct CStartUploadVersion
		{
			struct CResult
			{
				void f_Feed(CDistributedActorWriteStream &_Stream) &&;
				void f_Consume(CDistributedActorReadStream &_Stream);
				
				NConcurrency::CActorSubscription m_Subscription;
				NContainer::TCSet<NStr::CStr> m_DeniedTags;
			};
			
			enum EFlag
			{
				EFlag_None = 0
				, EFlag_ForceOverwrite = DMibBit(0)
			};

			void f_Feed(CDistributedActorWriteStream &_Stream) const;
			void f_Consume(CDistributedActorReadStream &_Stream);
			
			NStr::CStr m_Application;
			CVersionIDAndPlatform m_VersionIDAndPlatform;
			CVersionInformation m_VersionInfo;
			uint64 m_QueueSize = 8*1024*1024;
			EFlag m_Flags = EFlag_None;
			NConcurrency::TCActor<> m_DispatchActor;
			NFunction::TCFunctionMutable<NConcurrency::TCContinuation<CStartUploadTransfer::CResult> (CStartUploadTransfer &&_Params)> m_fStartTransfer;
		};
		
		struct CStartDownloadVersion
		{
			struct CResult
			{
				void f_Feed(CDistributedActorWriteStream &_Stream) &&;
				void f_Consume(CDistributedActorReadStream &_Stream);
				
				NConcurrency::CActorSubscription m_Subscription;
				CVersionInformation m_VersionInfo;
			};

			void f_Feed(CDistributedActorWriteStream &_Stream) const;
			void f_Consume(CDistributedActorReadStream &_Stream);
			
			NStr::CStr m_Application;
			CVersionIDAndPlatform m_VersionIDAndPlatform;
			CFileTransferContext m_TransferContext;
		};
		
		struct CNewVersionNotification
		{
			void f_Feed(CDistributedActorWriteStream &_Stream) const;
			void f_Consume(CDistributedActorReadStream &_Stream);
			
			NStr::CStr m_Application;
			CVersionIDAndPlatform m_VersionIDAndPlatform;
			CVersionInformation m_VersionInfo;
		};

		struct CNewVersionNotifications
		{
			struct CResult
			{
				void f_Feed(CDistributedActorWriteStream &_Stream) const;
				void f_Consume(CDistributedActorReadStream &_Stream);
			};

			void f_Feed(CDistributedActorWriteStream &_Stream) const;
			void f_Consume(CDistributedActorReadStream &_Stream);
			
			bool m_bFullResend = false;
			NContainer::TCVector<CNewVersionNotification> m_NewVersions;
		};
		
		struct CSubscribeToUpdates
		{
			struct CResult
			{
				void f_Feed(CDistributedActorWriteStream &_Stream) &&;
				void f_Consume(CDistributedActorReadStream &_Stream);
				
				NConcurrency::CActorSubscription m_Subscription;
			};

			void f_Feed(CDistributedActorWriteStream &_Stream) const;
			void f_Consume(CDistributedActorReadStream &_Stream);
			
			NStr::CStr m_Application; /// Leave empty to subscribe to all applications
			NContainer::TCSet<NStr::CStr> m_Platforms; /// Leave empty to subscribe to all platforms
			NContainer::TCSet<NStr::CStr> m_Tags; /// Leave empty to subscribe to all tags
			NConcurrency::TCActor<> m_DispatchActor;
			NFunction::TCFunctionMutable<NConcurrency::TCContinuation<CNewVersionNotifications::CResult> (CNewVersionNotifications &&_VersionInfo)> m_fOnNewVersions;
			uint32 m_nInitial = 10;
		};

		struct CChangeTags
		{
			struct CResult
			{
				void f_Feed(CDistributedActorWriteStream &_Stream) const;
				void f_Consume(CDistributedActorReadStream &_Stream);
				
				NContainer::TCSet<NStr::CStr> m_DeniedTags;
			};
			
			void f_Feed(CDistributedActorWriteStream &_Stream) const;
			void f_Consume(CDistributedActorReadStream &_Stream);
			
			NStr::CStr m_Application;
			CVersionID m_VersionID;
			NStr::CStr m_Platform; /// Leave empty to tag all platforms for version
			NContainer::TCSet<NStr::CStr> m_AddTags;
			NContainer::TCSet<NStr::CStr> m_RemoveTags;
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
		
		virtual NConcurrency::TCContinuation<CListApplications::CResult> f_ListApplications(CListApplications &&_Params) = 0;
		virtual NConcurrency::TCContinuation<CListVersions::CResult> f_ListVersions(CListVersions &&_Params) = 0;
		virtual NConcurrency::TCContinuation<CStartUploadVersion::CResult> f_UploadVersion(CStartUploadVersion &&_Params) = 0;
		virtual NConcurrency::TCContinuation<CStartDownloadVersion::CResult> f_DownloadVersion(CStartDownloadVersion &&_Params) = 0;
		virtual NConcurrency::TCContinuation<CSubscribeToUpdates::CResult> f_SubscribeToUpdates(CSubscribeToUpdates &&_Params) = 0;
		virtual NConcurrency::TCContinuation<CChangeTags::CResult> f_ChangeTags(CChangeTags &&_Params) = 0;
	};
}
