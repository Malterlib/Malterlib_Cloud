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
			
			
			virtual NConcurrency::TCContinuation<CStartBackup::CResult> f_StartBackup(CStartBackup &&_Params) = 0;
			virtual NConcurrency::TCContinuation<CStopBackup::CResult> f_StopBackup(CStopBackup &&_Params) = 0; // When we have notification support over remote actors, use that instead
			virtual NConcurrency::TCContinuation<CUploadData::CResult> f_UploadData(CUploadData &&_Params) = 0;
			virtual NConcurrency::TCContinuation<CListBackupSources::CResult> f_ListBackupSources(CListBackupSources &&_Params) = 0;
			virtual NConcurrency::TCContinuation<CListBackups::CResult> f_ListBackups(CListBackups &&_Params) = 0;
		};
	}
}
		
