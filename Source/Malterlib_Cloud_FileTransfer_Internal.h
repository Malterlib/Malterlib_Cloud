// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedActor>

namespace NMib::NCloud
{
	struct CFileTransferContext::CInternal
	{
		enum 
		{
			EProtocolVersion = 0x101
		};

		struct CSendPart
		{
			CSendPart() = default;
			CSendPart(uint32 _Version);
			
			struct CResult
			{
				void f_Feed(NConcurrency::CDistributedActorWriteStream &_Stream) const;
				void f_Consume(NConcurrency::CDistributedActorReadStream &_Stream);
				
				uint32 m_Version = 0;
			};

			CResult f_GetResult() const;
			void f_Feed(NConcurrency::CDistributedActorWriteStream &_Stream) const;
			void f_Consume(NConcurrency::CDistributedActorReadStream &_Stream);

			uint32 m_Version = 0;
			
			NStr::CStr m_FilePath;
			uint64 m_FilePosition;
			NContainer::TCVector<uint8, NMem::CAllocator_HeapSecure> m_Data;
			uint8 m_bFinished = false;
		};

		enum EState
		{
			EState_None
			, EState_Error
			, EState_Finished
		};
		
		struct CStateChange
		{
			CStateChange() = default;
			CStateChange(uint32 _Version);

			struct CResult
			{
				void f_Feed(NConcurrency::CDistributedActorWriteStream &_Stream) const;
				void f_Consume(NConcurrency::CDistributedActorReadStream &_Stream);
				
				uint32 m_Version = 0;
			};
			
			CResult f_GetResult() const;
			void f_Feed(NConcurrency::CDistributedActorWriteStream &_Stream) const;
			void f_Consume(NConcurrency::CDistributedActorReadStream &_Stream);

			uint32 m_Version = 0;
			EState m_State = EState_None;
			NStr::CStr m_Error;
			CFileTransferResult m_Finished;
		};

		struct CFileInfo
		{
			void f_Feed(NConcurrency::CDistributedActorWriteStream &_Stream) const;
			void f_Consume(NConcurrency::CDistributedActorReadStream &_Stream);

			NStr::CStr const &f_GetPath() const;
			uint64 m_FileSize = 0;
		};
		
		struct CManifest
		{
			void f_Feed(NConcurrency::CDistributedActorWriteStream &_Stream) const;
			void f_Consume(NConcurrency::CDistributedActorReadStream &_Stream);

			NContainer::TCMap<NStr::CStr, CFileInfo> m_Files;
		};

		void f_Feed(NConcurrency::CDistributedActorWriteStream &_Stream) const;
		void f_Consume(NConcurrency::CDistributedActorReadStream &_Stream);
		
		uint32 m_Version = EProtocolVersion;
		
		CManifest m_Manifest;
		uint64 m_QueueSize = 8*1024*1024;
		NConcurrency::TCActor<> m_DispatchActor; 
		NFunction::TCFunction<NConcurrency::TCContinuation<CInternal::CSendPart::CResult> (NFunction::CThisTag &, CInternal::CSendPart &&_Part)> m_fSendPart;
		NFunction::TCFunction<NConcurrency::TCContinuation<CInternal::CStateChange::CResult> (NFunction::CThisTag &, CInternal::CStateChange &&_State)> m_fStateChange;
	};
}
