// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Encoding/EJSON>

#include "Malterlib_Cloud_FileTransfer.h"

namespace NMib::NCloud
{
	struct CVersionManager : public NConcurrency::CActor
	{
		using CDistributedActorWriteStream = NConcurrency::CDistributedActorWriteStream;
		using CDistributedActorReadStream = NConcurrency::CDistributedActorReadStream;

		enum 
		{
			EMinProtocolVersion = 0x101
			, EProtocolVersion = 0x101
		};

		static bool fs_IsValidApplicationName(NStr::CStr const &_String);
		static bool fs_IsValidProtocolVersion(uint32 _Version);

		struct CVersionIdentifier
		{
			void f_Feed(CDistributedActorWriteStream &_Stream) const;
			void f_Consume(CDistributedActorReadStream &_Stream);
			void f_Format(NStr::CStrAggregate &o_Str) const;
			
			NStr::CStr f_EncodeFileName() const;
			static NStr::CStr fs_DecodeFileName(NStr::CStr const &_FileName);
			
			bool operator == (CVersionIdentifier const &_Right) const;
			bool operator < (CVersionIdentifier const &_Right) const;
			
			NEncoding::CEJSON f_ToJSON() const;
			static CVersionIdentifier fs_FromJSON(NEncoding::CEJSON const &_JSON);

			NStr::CStr m_Branch;
			uint32 m_Major = 0;
			uint32 m_Minor = 0;
			uint32 m_Revision = 0;
		};

		struct CVersionInformation
		{
			void f_Feed(CDistributedActorWriteStream &_Stream) const;
			void f_Consume(CDistributedActorReadStream &_Stream);

			NEncoding::CEJSON f_ToJSON() const;
			static CVersionInformation fs_FromJSON(NEncoding::CEJSON const &_JSON);
			
			NTime::CTime m_Time;
			NStr::CStr m_Configuration;
			NEncoding::CEJSON m_ExtraInfo;
			uint32 m_nFiles = 0;
			uint64 m_nBytes = 0;
		};

		static bool fs_IsValidVersionIdentifier(NStr::CStr const &_String, NStr::CStr &o_Error, CVersionIdentifier *o_pVersionID);
		static bool fs_IsValidVersionIdentifier(CVersionIdentifier const &_VersionID, NStr::CStr &o_Error);
		
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

				NContainer::TCMap<NStr::CStr, NContainer::TCMap<CVersionIdentifier, CVersionInformation>> m_Versions;
			};

			void f_Feed(CDistributedActorWriteStream &_Stream) const;
			void f_Consume(CDistributedActorReadStream &_Stream);

			NStr::CStr m_ForApplication;
		};

		struct CStartUploadTransfer
		{
			struct CResult
			{
				void f_Feed(CDistributedActorWriteStream &_Stream) const;
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
				void f_Feed(CDistributedActorWriteStream &_Stream) const;
				void f_Consume(CDistributedActorReadStream &_Stream);
				
				NConcurrency::CActorSubscription m_Subscription;
			};
			
			enum EFlag
			{
				EFlag_None = 0
				, EFlag_ForceOverwrite = DMibBit(0)
			};

			void f_Feed(CDistributedActorWriteStream &_Stream) const;
			void f_Consume(CDistributedActorReadStream &_Stream);
			
			NStr::CStr m_Application;
			CVersionIdentifier m_VersionID;
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
				void f_Feed(CDistributedActorWriteStream &_Stream) const;
				void f_Consume(CDistributedActorReadStream &_Stream);
				
				NConcurrency::CActorSubscription m_Subscription;
				CVersionInformation m_VersionInfo;
			};

			void f_Feed(CDistributedActorWriteStream &_Stream) const;
			void f_Consume(CDistributedActorReadStream &_Stream);
			
			NStr::CStr m_Application;
			CVersionIdentifier m_VersionID;
			CFileTransferContext m_TransferContext;
		};
		
		struct CNewVersionNotification
		{
			struct CResult
			{
				void f_Feed(CDistributedActorWriteStream &_Stream) const;
				void f_Consume(CDistributedActorReadStream &_Stream);
			};

			void f_Feed(CDistributedActorWriteStream &_Stream) const;
			void f_Consume(CDistributedActorReadStream &_Stream);
			
			NStr::CStr m_Application;
			CVersionIdentifier m_VersionID;
			CVersionInformation m_VersionInfo;
		};

		struct CSubscribeToUpdates
		{
			struct CResult
			{
				void f_Feed(CDistributedActorWriteStream &_Stream) const;
				void f_Consume(CDistributedActorReadStream &_Stream);
				
				NConcurrency::CActorSubscription m_Subscription;
			};

			void f_Feed(CDistributedActorWriteStream &_Stream) const;
			void f_Consume(CDistributedActorReadStream &_Stream);
			
			NStr::CStr m_Application; /// Leave empty to subscribe to all applications 
			NConcurrency::TCActor<> m_DispatchActor;
			NFunction::TCFunctionMutable<NConcurrency::TCContinuation<void> ()> m_fOnPermissionsChanged; // Will be followed by m_fOnNewVersion being called with the currently accessible versions
			NFunction::TCFunctionMutable<NConcurrency::TCContinuation<CNewVersionNotification::CResult> (CNewVersionNotification &&_VersionInfo)> m_fOnNewVersion;
			uint32 m_nInitial = 10;
		};
		
		virtual NConcurrency::TCContinuation<CListApplications::CResult> f_ListApplications(CListApplications &&_Params) = 0;
		virtual NConcurrency::TCContinuation<CListVersions::CResult> f_ListVersions(CListVersions &&_Params) = 0;
		virtual NConcurrency::TCContinuation<CStartUploadVersion::CResult> f_UploadVersion(CStartUploadVersion &&_Params) = 0;
		virtual NConcurrency::TCContinuation<CStartDownloadVersion::CResult> f_DownloadVersion(CStartDownloadVersion &&_Params) = 0;
		virtual NConcurrency::TCContinuation<CSubscribeToUpdates::CResult> f_SubscribeToUpdates(CSubscribeToUpdates &&_Params) = 0;
	};
}
