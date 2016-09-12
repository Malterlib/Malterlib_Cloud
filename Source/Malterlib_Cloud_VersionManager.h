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

			NStr::CStr m_Branch;
			uint32 m_Major = 0;
			uint32 m_Minor = 0;
			uint32 m_Revision = 0;
		};

		struct CVersionInformation
		{
			void f_Feed(CDistributedActorWriteStream &_Stream) const;
			void f_Consume(CDistributedActorReadStream &_Stream);

			NTime::CTime m_Time;
			NStr::CStr m_Configuration;
			NEncoding::CEJSON m_ExtraInfo;
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
			NFunction::TCFunction<NConcurrency::TCContinuation<CStartUploadTransfer::CResult> (NFunction::CThisTag &, CStartUploadTransfer &&_Params)> m_fStartTransfer;
		};
		
		struct CStartDownloadVersion
		{
			struct CResult
			{
				void f_Feed(CDistributedActorWriteStream &_Stream) const;
				void f_Consume(CDistributedActorReadStream &_Stream);
				
				NConcurrency::CActorSubscription m_Subscription;
			};

			void f_Feed(CDistributedActorWriteStream &_Stream) const;
			void f_Consume(CDistributedActorReadStream &_Stream);
			
			NStr::CStr m_Application;
			CVersionIdentifier m_VersionID;
			CFileTransferContext m_TransferContext;
		};
		
		uint32 f_GetProtocolVersion(uint32 _Version);
		virtual NConcurrency::TCContinuation<CListApplications::CResult> f_ListApplications(CListApplications &&_Params) = 0;
		virtual NConcurrency::TCContinuation<CListVersions::CResult> f_ListVersions(CListVersions &&_Params) = 0;
		virtual NConcurrency::TCContinuation<CStartUploadVersion::CResult> f_UploadVersion(CStartUploadVersion &&_Params) = 0;
		virtual NConcurrency::TCContinuation<CStartDownloadVersion::CResult> f_DownloadVersion(CStartDownloadVersion &&_Params) = 0;
	};
}
