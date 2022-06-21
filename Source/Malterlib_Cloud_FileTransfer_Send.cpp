// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

#include "Malterlib_Cloud_FileTransfer.h"
#include "Malterlib_Cloud_FileTransfer_Internal.h"

namespace NMib::NCloud
{
	using namespace NConcurrency;
	using namespace NStr;
	using namespace NFile;
	using namespace NTime;
	using namespace NContainer;
	using namespace NStorage;

	namespace NPrivate
	{
		struct CTransferStats
		{
			uint64 m_nTransferredBytes = 0;
			CClock m_Clock{true};
		};
		struct CWorkQueue
		{
			struct CEntry
			{
				CStr m_FileName;
				CStr m_RelativeFileName;
				uint64 m_Position;
				uint64 m_Size;
				bool f_IsFinished() const
				{
					return m_Position == m_Size;
				}
			};
			
			TCLinkedList<CEntry> m_Queue;
		};
	}
	using namespace NPrivate;
	
	struct CFileTransferSend::CInternal : public CActorInternal
	{
		CInternal(CFileTransferSend *_pThis)
			: m_pThis(_pThis)
		{
		}
		void fp_DetermineWhatToSend();
		void fp_PerformFileSend();
		void fp_ReportError(CStr const &_Error);
		void fp_ReportFinished();
		
		CFileTransferSend *m_pThis;
		TCActor<CActor> m_FileActor;
		
		struct CFileState
		{
			CStr m_FileName;
			CFile m_File;
			CStr m_RootPath;
		};

		CTransferStats m_TransferStats;
		TCActorFunctorWeak<TCFuture<CFileTransferContext::CInternal::CSendPart::CResult> (CFileTransferContext::CInternal::CSendPart &&_Part)> m_fUploadCallback;
		TCActorFunctorWeak<TCFuture<CFileTransferContext::CInternal::CStateChange::CResult> (CFileTransferContext::CInternal::CStateChange &&_State)> m_fStateCallback;
		uint32 m_Version = 0;
		CFileTransferContext::CInternal m_Params;
		CWorkQueue m_Queue;
		uint64 m_OutstandingBytes = 0;
		TCSharedPointer<CFileState> m_pFileState = fg_Construct();
		
		NConcurrency::TCPromise<CFileTransferResult> m_Promise;
		bool m_bDelayedFinish = false;
		bool m_bCalled = false;
		bool m_bDoneCalled = false;
	};

	CFileTransferSend::~CFileTransferSend() = default;

	CFileTransferSend::CFileTransferSend(NStr::CStr const &_BasePath, NConcurrency::TCActor<CActor> const &_FileActor)
		: mp_pInternal(fg_Construct(this)) 
	{
		auto &Internal = *mp_pInternal;
		Internal.m_FileActor = _FileActor;
		Internal.m_pFileState->m_RootPath = _BasePath; 
		if (!Internal.m_FileActor)
			Internal.m_FileActor = fg_ConstructActor<CSeparateThreadActor>(fg_Construct("File transfer send file access"));
	}
	
	void CFileTransferSend::CInternal::fp_ReportError(CStr const &_Error)
	{
		CFileTransferContext::CInternal::CStateChange StateChange{m_Version};
		StateChange.m_State = CFileTransferContext::CInternal::EState_Error;
		StateChange.m_Error = _Error;
		m_fStateCallback(fg_Move(StateChange)) > fg_DiscardResult();
		if (!m_Promise.f_IsSet() && !m_bDelayedFinish)
			m_Promise.f_SetException(DMibErrorInstance(_Error));
	}
	
	void CFileTransferSend::CInternal::fp_ReportFinished()
	{
		CFileTransferContext::CInternal::CStateChange StateChange{m_Version};
		StateChange.m_State = CFileTransferContext::CInternal::EState_Finished;
		StateChange.m_Finished.m_nBytes = m_TransferStats.m_nTransferredBytes;
		StateChange.m_Finished.m_nSeconds = m_TransferStats.m_Clock.f_GetTime();
		auto Future = m_fStateCallback(fg_TempCopy(StateChange)).f_Future();
		m_bDelayedFinish = true;
		fg_Move(Future) > [this, Finished = StateChange.m_Finished](TCAsyncResult<CFileTransferContext::CInternal::CStateChange::CResult> &&_Result)
			{
				if (!m_Promise.f_IsSet())
					m_Promise.f_SetResult(Finished);
			}
		;
	}
	
	void CFileTransferSend::CInternal::fp_DetermineWhatToSend()
	{
		fg_Dispatch
			(
				m_FileActor
				, [Manifest = m_Params.m_Manifest, pFileState = m_pFileState]() mutable -> CWorkQueue
				{
					CWorkQueue Queue;
					
					CStr RootPath = pFileState->m_RootPath;
					
					auto fAddEntry = [&](CStr const &_RelativePath, CStr const &_Path)
						{
							uint64 ExpectedSize = CFile::fs_GetFileSize(_Path); 
							auto *pFile = Manifest.m_Files.f_FindEqual(_RelativePath);
							if (pFile && pFile->m_FileSize == ExpectedSize)
								return;
							
							auto &Entry = Queue.m_Queue.f_Insert();
							Entry.m_Size = ExpectedSize;
							if (!pFile)
								Entry.m_Position = 0;
							else if (pFile->m_FileSize > ExpectedSize)
								Entry.m_Position = ExpectedSize;
							else
							{
								// Re-upload last 128 KiB
								if (pFile->m_FileSize > 128*1024)
									Entry.m_Position = pFile->m_FileSize - 128*1024; 
								else
									Entry.m_Position = 0; 
							}
							Entry.m_FileName = _Path;
							Entry.m_RelativeFileName = _RelativePath;
						}
					;

					if (CFile::fs_FileExists(RootPath, EFileAttrib_File))
					{
						CStr RelativePath = CFile::fs_GetFile(RootPath);
						fAddEntry(RelativePath, RootPath);
						return Queue;
					}
					
					if (!CFile::fs_FileExists(RootPath, EFileAttrib_Directory))
						DMibError("Send path does not exist");

					CFile::CFindFilesOptions FindOptions(RootPath + "/*", true);
					FindOptions.m_AttribMask = EFileAttrib_File;
					
					auto FoundFiles = CFile::fs_FindFiles(FindOptions);
					for (auto &File : FoundFiles)
					{
						CStr RelativePath = File.m_Path.f_Extract(RootPath.f_GetLen() + 1);
						fAddEntry(RelativePath, File.m_Path);
					}
					return Queue;
				}
			)
			> [this](TCAsyncResult<CWorkQueue> &&_Result)
			{
				if (!_Result)
				{
					fp_ReportError(fg_Format("Error determining what to send: {}", _Result.f_GetExceptionStr()));
					return;
				}
				
				m_Queue = fg_Move(*_Result);
				
				if (m_Queue.m_Queue.f_IsEmpty())
				{
					fp_ReportFinished();
					return;
				}
				
				fp_PerformFileSend();
			}
		;
	}
	
	void CFileTransferSend::CInternal::fp_PerformFileSend()
	{
		if (m_Queue.m_Queue.f_IsEmpty())
		{
			if (m_OutstandingBytes != 0)
				return;
			fp_ReportFinished();
			return;
		}
		auto &Params = m_Params;
		
		if (m_OutstandingBytes > Params.m_QueueSize)
			return;
		
		while (m_OutstandingBytes < Params.m_QueueSize && !m_Queue.m_Queue.f_IsEmpty())
		{
			auto &Entry = m_Queue.m_Queue.f_GetFirst();
			CStr FileName = Entry.m_FileName;
			CStr RelativeFileName = Entry.m_RelativeFileName;
			uint64 Position = Entry.m_Position;
			uint64 nBytes = fg_Min(Entry.m_Size - Entry.m_Position, 64u*1024u, CActorDistributionManager::mc_HalfMaxMessageSize);
			Entry.m_Position += nBytes;
			bool bFinished = Entry.f_IsFinished(); 
			if (bFinished)
				m_Queue.m_Queue.f_Remove(Entry);
			
			m_OutstandingBytes += nBytes;
			
			fg_Dispatch
				(
					m_FileActor
					, [FileName, RelativeFileName, Position, nBytes, pFileState = m_pFileState, bFinished]() -> CFileTransferContext::CInternal::CSendPart
					{
						if (pFileState->m_FileName != FileName)
						{
							pFileState->m_FileName = FileName;
							pFileState->m_File.f_Open(FileName, EFileOpen_Read | EFileOpen_ShareAll);
						}
						
						CFileTransferContext::CInternal::CSendPart ToSend;
						ToSend.m_Data.f_SetLen(nBytes);
						ToSend.m_FilePosition = Position;
						ToSend.m_bFinished = bFinished;
						ToSend.m_FilePath = RelativeFileName;

						pFileState->m_File.f_SetPosition(Position);
						pFileState->m_File.f_Read(ToSend.m_Data.f_GetArray(), ToSend.m_Data.f_GetLen());
						if (bFinished)
						{
							ToSend.m_FileAttributes = pFileState->m_File.f_GetAttributes();
							ToSend.m_WriteTime = pFileState->m_File.f_GetWriteTime();
							pFileState->m_FileName.f_Clear();
							pFileState->m_File.f_Close();
						}
						
						return ToSend;
					}
				)
				> [this, nBytes](TCAsyncResult<CFileTransferContext::CInternal::CSendPart> &&_Result)
				{
					if (!_Result)
					{
						fp_ReportError(fg_Format("Error reading file data for file transfer: {}", _Result.f_GetExceptionStr()));
						return;
					}
					
					_Result->m_Version = m_Version;
					
					m_fUploadCallback(fg_Move(*_Result)) > [this, nBytes](TCAsyncResult<CFileTransferContext::CInternal::CSendPart::CResult> &&_Result)
						{
							if (!_Result)
							{
								fp_ReportError(fg_Format("Error transferring file data to remote: {}", _Result.f_GetExceptionStr()));
								return;
							}
							m_OutstandingBytes -= nBytes;
							m_TransferStats.m_nTransferredBytes += nBytes;
							fp_PerformFileSend();
						}
					;
				}
			;
		}
	}

	NConcurrency::TCFuture<NConcurrency::CActorSubscription> CFileTransferSend::f_SendFiles(CFileTransferContext &&_TransferContext)
	{
		auto &Internal = *mp_pInternal;
		if (Internal.m_bCalled)
			co_return DMibErrorInstance("Send files has already been called");
		Internal.m_bCalled = true;

		auto &Params = *_TransferContext.mp_pInternal;

		for (auto &FileInfo : Params.m_Manifest.m_Files)
		{
			CStr Error;
			if (!CFileTransferContext::fs_IsSafeRelativePath(FileInfo.f_GetPath(), Error))
				co_return DMibErrorInstance(fg_Format("Invalid relative path '{}' in file transfer manifest. Path cannot {}", FileInfo.f_GetPath(), Error));
		}

		Internal.m_Params = fg_Move(*_TransferContext.mp_pInternal);
		Internal.m_Version = Internal.m_Params.m_Version;
		Internal.m_fUploadCallback = g_ActorFunctorWeak(Internal.m_Params.m_DispatchActor) / fg_Move(Internal.m_Params.m_fSendPart);
		Internal.m_fStateCallback = g_ActorFunctorWeak(Internal.m_Params.m_DispatchActor) / fg_Move(Internal.m_Params.m_fStateChange);
		Internal.fp_DetermineWhatToSend();

		co_return g_ActorSubscription / [this]() -> TCFuture<void>
			{
				auto &Internal = *mp_pInternal;

				TCActorResultVector<void> Destroys;

				if (!Internal.m_Promise.f_IsSet() && !Internal.m_bDelayedFinish)
					Internal.m_Promise.f_SetException(DMibErrorInstance("File transfer aborted"));

				fg_Move(Internal.m_fUploadCallback).f_Destroy() > Destroys.f_AddResult();
				fg_Move(Internal.m_fStateCallback).f_Destroy() > Destroys.f_AddResult();

				co_await Destroys.f_GetResults();

				co_return {};
			}
		;
	}
	
	NConcurrency::TCFuture<CFileTransferResult> CFileTransferSend::f_GetResult()
	{
		NConcurrency::TCPromise<CFileTransferResult> Promise;

		auto &Internal = *mp_pInternal;
		if (Internal.m_bDoneCalled)
			return Promise <<= DMibErrorInstance("The file result has already been gotten");

		Internal.m_bDoneCalled = true;

		return Promise <<= Internal.m_Promise.f_Future();
	}
}
