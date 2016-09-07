// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>

namespace NMib
{
	namespace NCloud
	{
		struct CBackupManager : public NConcurrency::CActor
		{
			enum 
			{
				EProtocolVersion = 0x101
			};
		
			static bool fs_IsValidRelativePath(NStr::CStr const &_String, NStr::CStr &o_Error);
			static bool fs_IsValidHostname(NStr::CStr const &_String);
			static bool fs_IsValidBackupSource(NStr::CStr const &_String, NStr::CStr *o_pFriendlyName, NStr::CStr *o_pHostID);
			static bool fs_IsValidBackup(NStr::CStr const &_String, NStr::CStr *o_pBackupID, NTime::CTime *o_pTime);
			
			struct CBackupKey
			{
				NStr::CStr m_FriendlyName;
				NTime::CTime m_Time;
				NStr::CStr m_ID;

				void f_Feed(NStream::CBinaryStream &_Stream) const;
				void f_Consume(NStream::CBinaryStream &_Stream);
			};
			
			struct CStartBackup
			{
				struct CResult
				{
					void f_Feed(NStream::CBinaryStream &_Stream) const;
					void f_Consume(NStream::CBinaryStream &_Stream);

					uint32 m_Version = 0;
					NStr::CStr m_FriendlyName;
					uint64 m_BackupSize;
					uint64 m_OplogSize;
				};

				CResult f_GetResult() const;
				void f_Feed(NStream::CBinaryStream &_Stream) const;
				void f_Consume(NStream::CBinaryStream &_Stream);
				
				uint32 m_Version = EProtocolVersion;
				CBackupKey m_BackupKey;
			};
			
			struct CUploadData
			{
				struct CResult
				{
					void f_Feed(NStream::CBinaryStream &_Stream) const;
					void f_Consume(NStream::CBinaryStream &_Stream);

					uint32 m_Version = 0;
				};
				
				enum EFlag
				{
					EFlag_None = 0
					, EFlag_Finished = DMibBit(0)
				};

				CResult f_GetResult() const;
				void f_Feed(NStream::CBinaryStream &_Stream) const;
				void f_Consume(NStream::CBinaryStream &_Stream);

				uint32 m_Version = EProtocolVersion;
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
					void f_Feed(NStream::CBinaryStream &_Stream) const;
					void f_Consume(NStream::CBinaryStream &_Stream);

					uint32 m_Version = 0;
				};

				CResult f_GetResult() const;
				void f_Feed(NStream::CBinaryStream &_Stream) const;
				void f_Consume(NStream::CBinaryStream &_Stream);
				
				uint32 m_Version = EProtocolVersion;
				CBackupKey m_BackupKey;
			};
			
			struct CListBackupSources
			{
				struct CResult
				{
					void f_Feed(NStream::CBinaryStream &_Stream) const;
					void f_Consume(NStream::CBinaryStream &_Stream);
					
					uint32 m_Version = 0;
					NContainer::TCVector<NStr::CStr> m_BackupSources;
				};

				CResult f_GetResult() const;
				void f_Feed(NStream::CBinaryStream &_Stream) const;
				void f_Consume(NStream::CBinaryStream &_Stream);

				uint32 m_Version = EProtocolVersion;
			};

			struct CListBackups
			{
				struct CBackup
				{
					void f_Feed(NStream::CBinaryStream &_Stream) const;
					void f_Consume(NStream::CBinaryStream &_Stream);
					void f_Format(NStr::CStrAggregate &o_Str) const;
					
					NTime::CTime m_Time;
					NStr::CStr m_BackupID;
					
				};
				
				struct CResult
				{
					void f_Feed(NStream::CBinaryStream &_Stream) const;
					void f_Consume(NStream::CBinaryStream &_Stream);
					void f_Format(NStr::CStrAggregate &o_Str) const;
					
					uint32 m_Version = 0;
					NContainer::TCMap<NStr::CStr, NContainer::TCVector<CBackup>> m_Backups;
				};

				CResult f_GetResult() const;
				void f_Feed(NStream::CBinaryStream &_Stream) const;
				void f_Consume(NStream::CBinaryStream &_Stream);

				uint32 m_Version = EProtocolVersion;
				NStr::CStr m_ForBackupSource;
			};
			
			struct CStartDownloadBackup
			{
				void f_Feed(NStream::CBinaryStream &_Stream) const;
				void f_Consume(NStream::CBinaryStream &_Stream);
				
				struct CFileInfo
				{
					void f_Feed(NStream::CBinaryStream &_Stream) const;
					void f_Consume(NStream::CBinaryStream &_Stream);

					NStr::CStr const &f_GetPath() const;
					uint64 m_FileSize = 0;
				};
				
				struct CManifest
				{
					void f_Feed(NStream::CBinaryStream &_Stream) const;
					void f_Consume(NStream::CBinaryStream &_Stream);

					NContainer::TCMap<NStr::CStr, CFileInfo> m_Files;
				};

				NStr::CStr f_GetDesc() const
				{
					return fg_Format
						(
							"{}/{tst.} - {}"
							, m_BackupSource
							, m_Time
							, m_BackupID
						)
					;
				}
				
				CManifest m_Manifest;
				NStr::CStr m_BackupSource;
				NTime::CTime m_Time;
				NStr::CStr m_BackupID;
				uint32 m_Version = EProtocolVersion;
			};

			struct CDownloadSendPart
			{
				CDownloadSendPart() = default;
				CDownloadSendPart(uint32 _Version);
				
				struct CResult
				{
					void f_Feed(NStream::CBinaryStream &_Stream) const;
					void f_Consume(NStream::CBinaryStream &_Stream);
					
					uint32 m_Version = 0;
				};

				CResult f_GetResult() const;
				void f_Feed(NStream::CBinaryStream &_Stream) const;
				void f_Consume(NStream::CBinaryStream &_Stream);

				uint32 m_Version = 0;
				
				NStr::CStr m_FilePath;
				uint64 m_FilePosition;
				NContainer::TCVector<uint8, NMem::CAllocator_HeapSecure> m_Data;
				uint8 m_bFinished = false;
			};

			enum EDownloadState
			{
				EDownloadState_None
				, EDownloadState_Error
				, EDownloadState_Finished
			};
			
			struct CDownloadStateChange
			{
				CDownloadStateChange() = default;
				CDownloadStateChange(uint32 _Version);

				struct CResult
				{
					void f_Feed(NStream::CBinaryStream &_Stream) const;
					void f_Consume(NStream::CBinaryStream &_Stream);
					
					uint32 m_Version = 0;
				};
				
				CResult f_GetResult() const;
				void f_Feed(NStream::CBinaryStream &_Stream) const;
				void f_Consume(NStream::CBinaryStream &_Stream);

				uint32 m_Version = 0;
				EDownloadState m_State = EDownloadState_None;
				NStr::CStr m_Message;
			};
			
			uint32 f_GetProtocolVersion(uint32 _Version);
			virtual NConcurrency::TCContinuation<CStartBackup::CResult> f_StartBackup(CStartBackup &&_Params) = 0;
			virtual NConcurrency::TCContinuation<CStopBackup::CResult> f_StopBackup(CStopBackup &&_Params) = 0; // When we have notification support over remote actors, use that instead
			virtual NConcurrency::TCContinuation<CUploadData::CResult> f_UploadData(CUploadData &&_Params) = 0;
			virtual NConcurrency::TCContinuation<CListBackupSources::CResult> f_ListBackupSources(CListBackupSources &&_Params) = 0;
			virtual NConcurrency::TCContinuation<CListBackups::CResult> f_ListBackups(CListBackups &&_Params) = 0;
			
			// Not optimal, but currently it's not supported to put subscriptions, actors and functors inside structs
			virtual NConcurrency::TCContinuation<NConcurrency::CActorSubscription> f_StartDownloadBackup
				(
					CStartDownloadBackup &&_Params
					, NConcurrency::TCActor<> &&_DispatchActor 
					, NFunction::TCFunction<NConcurrency::TCContinuation<CDownloadSendPart::CResult> (NFunction::CThisTag &, CDownloadSendPart &&_Part)> &&_fSendBackup
					, NFunction::TCFunction<NConcurrency::TCContinuation<CDownloadStateChange::CResult> (NFunction::CThisTag &, CDownloadStateChange &&_State)> &&_fStateChange
				) = 0
			;
		};
	}
}
		
