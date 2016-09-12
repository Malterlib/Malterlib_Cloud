// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedActor>

namespace NMib::NCloud
{
	struct CFileTransferSend;
	struct CFileTransferReceive;
	
	struct CFileTransferResult
	{
		uint64 m_nBytes;
		fp64 m_nSeconds;
		
		fp64 f_BytesPerSecond() const;

		void f_Feed(NConcurrency::CDistributedActorWriteStream &_Stream) const;
		void f_Consume(NConcurrency::CDistributedActorReadStream &_Stream);
	};
	
	struct CFileTransferContext
	{
		CFileTransferContext();
		~CFileTransferContext();
		CFileTransferContext(CFileTransferContext const &_Other) = delete;
		CFileTransferContext &operator =(CFileTransferContext const &_Other) = delete;
		CFileTransferContext(CFileTransferContext &&_Other);
		CFileTransferContext &operator =(CFileTransferContext &&_Other);
		static bool fs_IsValidRelativePath(NStr::CStr const &_String, NStr::CStr &o_Error);
		void f_Feed(NConcurrency::CDistributedActorWriteStream &_Stream) const;
		void f_Consume(NConcurrency::CDistributedActorReadStream &_Stream);
		
	private:
		friend struct CFileTransferSend;
		friend struct CFileTransferReceive;
		struct CInternal;
		NPtr::TCUniquePointer<CInternal> mp_pInternal;
	};
	
	struct CFileTransferSend : public NConcurrency::CActor
	{
		~CFileTransferSend();
		CFileTransferSend(NStr::CStr const &_BasePath, NConcurrency::TCActor<CActor> const &_FileActor = {});
		CFileTransferSend(CFileTransferSend &&_Other);
		CFileTransferSend &operator =(CFileTransferSend &&_Other);
		CFileTransferSend(CFileTransferSend const &_Other) = delete;
		CFileTransferSend &operator =(CFileTransferSend const &_Other) = delete;
		
		NConcurrency::TCContinuation<NConcurrency::CActorSubscription> f_SendFiles(CFileTransferContext &&_TransferContext);
		NConcurrency::TCContinuation<CFileTransferResult> f_GetResult(); 
		
	private:
		struct CInternal;
		NPtr::TCUniquePointer<CInternal> mp_pInternal;
	};
	
	struct CFileTransferReceive : public NConcurrency::CActor
	{
		~CFileTransferReceive();
		CFileTransferReceive(NStr::CStr const &_BasePath, NConcurrency::TCActor<CActor> const &_FileActor = {});
		CFileTransferReceive(CFileTransferReceive &&_Other);
		CFileTransferReceive &operator =(CFileTransferReceive &&_Other);
		CFileTransferReceive(CFileTransferReceive const &_Other) = delete;
		CFileTransferReceive &operator =(CFileTransferReceive const &_Other) = delete;
		
		enum EReceiveFlag
		{
			EReceiveFlag_None = 0
			, EReceiveFlag_IgnoreExisting = DMibBit(0)
			, EReceiveFlag_FailOnExisting = DMibBit(1)
			, EReceiveFlag_DeleteExisting = DMibBit(2)
		};

		NConcurrency::TCContinuation<CFileTransferContext> f_ReceiveFiles(uint64 _QueueSize, EReceiveFlag _Flags);
		NConcurrency::TCContinuation<CFileTransferResult> f_GetResult(); 
		
	private:
		struct CInternal;
		NPtr::TCUniquePointer<CInternal> mp_pInternal;
	};
}
