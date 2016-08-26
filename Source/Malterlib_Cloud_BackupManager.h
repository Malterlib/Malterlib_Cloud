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

					NStr::CStr m_FriendlyName;
					uint64 m_BackupSize;
					uint64 m_OplogSize;
				};

				void f_Feed(NStream::CBinaryStream &_Stream) const;
				void f_Consume(NStream::CBinaryStream &_Stream);
				
				CBackupKey m_BackupKey;
			};
			
			struct CUploadData
			{
				struct CResult
				{
					void f_Feed(NStream::CBinaryStream &_Stream) const;
					void f_Consume(NStream::CBinaryStream &_Stream);
				};
				
				enum EFlag
				{
					EFlag_None = 0
					, EFlag_Finished = DMibBit(0)
				};

				void f_Feed(NStream::CBinaryStream &_Stream) const;
				void f_Consume(NStream::CBinaryStream &_Stream);

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
				};

				void f_Feed(NStream::CBinaryStream &_Stream) const;
				void f_Consume(NStream::CBinaryStream &_Stream);
				
				CBackupKey m_BackupKey;
			};
			
			virtual NConcurrency::TCContinuation<CStartBackup::CResult> f_StartBackup(CStartBackup &&_Params) = 0;
			virtual NConcurrency::TCContinuation<CStopBackup::CResult> f_StopBackup(CStopBackup &&_Params) = 0; // When we have notification support over remote actors, use that instead
			virtual NConcurrency::TCContinuation<CUploadData::CResult> f_UploadData(CUploadData &&_Params) = 0;
		};
	}
}
		
