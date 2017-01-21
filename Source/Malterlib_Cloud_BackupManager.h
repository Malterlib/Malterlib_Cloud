// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedApp>
#include "Malterlib_Cloud_FileTransfer.h"

namespace NMib::NCloud
{
	struct CBackupManager : public NConcurrency::CActor
	{
		using CDistributedActorWriteStream = NConcurrency::CDistributedActorWriteStream;
		using CDistributedActorReadStream = NConcurrency::CDistributedActorReadStream;
		enum 
		{
			EMinProtocolVersion = 0x101
			, EProtocolVersion = 0x101
		};
		
		CBackupManager();
		
		static bool fs_IsValidProtocolVersion(uint32 _Version);
	
		static bool fs_IsValidHostname(NStr::CStr const &_String);
		static bool fs_IsValidBackupSource(NStr::CStr const &_String, NStr::CStr *o_pFriendlyName, NStr::CStr *o_pHostID);
		static bool fs_IsValidBackup(NStr::CStr const &_String, NStr::CStr *o_pBackupID, NTime::CTime *o_pTime);
		
		struct CBackupKey
		{
			NStr::CStr m_FriendlyName;
			NTime::CTime m_Time;
			NStr::CStr m_ID;

			void f_Feed(CDistributedActorWriteStream &_Stream) const;
			void f_Consume(CDistributedActorReadStream &_Stream);
		};
		
		struct CStartBackup
		{
			struct CResult
			{
				void f_Feed(CDistributedActorWriteStream &_Stream) const;
				void f_Consume(CDistributedActorReadStream &_Stream);

				NStr::CStr m_FriendlyName;
				uint64 m_BackupSize;
				uint64 m_OplogSize;
			};

			void f_Feed(CDistributedActorWriteStream &_Stream) const;
			void f_Consume(CDistributedActorReadStream &_Stream);
			
			CBackupKey m_BackupKey;
		};
		
		struct CUploadData
		{
			struct CResult
			{
				void f_Feed(CDistributedActorWriteStream &_Stream) const;
				void f_Consume(CDistributedActorReadStream &_Stream);

				uint32 m_Version = 0;
			};
			
			enum EFlag
			{
				EFlag_None = 0
				, EFlag_Finished = DMibBit(0)
			};

			void f_Feed(CDistributedActorWriteStream &_Stream) const;
			void f_Consume(CDistributedActorReadStream &_Stream);

			CBackupKey m_BackupKey;
			NStr::CStr m_File;
			uint64 m_Position;
			uint64 m_Size;
			EFlag m_Flags = EFlag_None;
			NContainer::TCVector<uint8, NMem::CAllocator_HeapSecure> m_Data;
		};
		
		struct CStopBackup
		{
			struct CResult
			{
				void f_Feed(CDistributedActorWriteStream &_Stream) const;
				void f_Consume(CDistributedActorReadStream &_Stream);
			};

			void f_Feed(CDistributedActorWriteStream &_Stream) const;
			void f_Consume(CDistributedActorReadStream &_Stream);
			
			CBackupKey m_BackupKey;
		};
		
		struct CListBackupSources
		{
			struct CResult
			{
				void f_Feed(CDistributedActorWriteStream &_Stream) const;
				void f_Consume(CDistributedActorReadStream &_Stream);
				
				NContainer::TCVector<NStr::CStr> m_BackupSources;
			};

			void f_Feed(CDistributedActorWriteStream &_Stream) const;
			void f_Consume(CDistributedActorReadStream &_Stream);
		};

		struct CListBackups
		{
			struct CBackup
			{
				void f_Feed(CDistributedActorWriteStream &_Stream) const;
				void f_Consume(CDistributedActorReadStream &_Stream);
				void f_Format(NStr::CStrAggregate &o_Str) const;
				
				NTime::CTime m_Time;
				NStr::CStr m_BackupID;
				
			};
			
			struct CResult
			{
				void f_Feed(CDistributedActorWriteStream &_Stream) const;
				void f_Consume(CDistributedActorReadStream &_Stream);
				void f_Format(NStr::CStrAggregate &o_Str) const;
				
				NContainer::TCMap<NStr::CStr, NContainer::TCVector<CBackup>> m_Backups;
			};

			void f_Feed(CDistributedActorWriteStream &_Stream) const;
			void f_Consume(CDistributedActorReadStream &_Stream);

			NStr::CStr m_ForBackupSource;
		};
		
		struct CStartDownloadBackup
		{
			struct CResult
			{
				void f_Feed(CDistributedActorWriteStream &_Stream) &&;
				void f_Consume(CDistributedActorReadStream &_Stream);
				
				NConcurrency::CActorSubscription m_Subscription;
			};

			void f_Feed(CDistributedActorWriteStream &_Stream) const;
			void f_Consume(CDistributedActorReadStream &_Stream);
			NStr::CStr f_GetDesc() const;
			
			NStr::CStr m_BackupSource;
			NTime::CTime m_Time;
			NStr::CStr m_BackupID;
			CFileTransferContext m_TransferContext;
		};
		
		virtual NConcurrency::TCContinuation<CStartBackup::CResult> f_StartBackup(CStartBackup &&_Params) = 0;
		virtual NConcurrency::TCContinuation<CStopBackup::CResult> f_StopBackup(CStopBackup &&_Params) = 0; // When we have notification support over remote actors, use that instead
		virtual NConcurrency::TCContinuation<CUploadData::CResult> f_UploadData(CUploadData &&_Params) = 0;
		virtual NConcurrency::TCContinuation<CListBackupSources::CResult> f_ListBackupSources(CListBackupSources &&_Params) = 0;
		virtual NConcurrency::TCContinuation<CListBackups::CResult> f_ListBackups(CListBackups &&_Params) = 0;
		virtual NConcurrency::TCContinuation<CStartDownloadBackup::CResult> f_StartDownloadBackup(CStartDownloadBackup &&_Params) = 0;
	};
}
