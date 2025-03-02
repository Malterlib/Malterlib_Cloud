// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Concurrency/LogError>
#include <Mib/Concurrency/ActorSequencerActor>
#include <Mib/Concurrency/AsyncDestroy>

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

		struct CWorkQueueDeprecated
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

		struct CUploadQueueEntry
		{
			CStr m_FileName;
			CStr m_RelativeFileName;
		};
	}
	using namespace NPrivate;
	
	struct CFileTransferSend::CInternal : public CActorInternal
	{
		struct CDeprecated
		{
			struct CFileState
			{
				CStr m_FileName;
				CFile m_File;
				CStr m_RootPath;
			};

			TCUnsafeFuture<void> f_DetermineWhatToSend();
			TCUnsafeFuture<void> f_PerformFileSend();
			void f_ReportError(CStr const &_Error);
			void f_ReportFinished();

			CInternal *m_pInternal = nullptr;

			TCActorFunctorWeak<TCFuture<CFileTransferContextDeprecated::CInternal::CSendPart::CResult> (CFileTransferContextDeprecated::CInternal::CSendPart _Part)> m_fUploadCallback;
			TCActorFunctorWeak<TCFuture<CFileTransferContextDeprecated::CInternal::CStateChange::CResult> (CFileTransferContextDeprecated::CInternal::CStateChange _State)> m_fStateCallback;
			uint32 m_Version = 0;
			CFileTransferContextDeprecated::CInternal m_Params;
			CWorkQueueDeprecated m_Queue;
			uint64 m_OutstandingBytes = 0;
			TCSharedPointer<CFileState> m_pFileState = fg_Construct();

			CSequencer m_ReadSequencer{"File transfer send"};
			bool m_bDelayedFinish = false;
		};

		CInternal(CFileTransferSend *_pThis)
			: m_pThis(_pThis)
		{
		}

		TCFuture<TCVector<CUploadQueueEntry>> f_DetermineWhatToSend();
		TCFuture<CFileTransferSendDownloadFile> f_SendFile(CUploadQueueEntry _UploadEntry);
		TCFuture<CFileTransferSendDownloadFileContents> f_SendFileContents(CStr _FileName, uint64 _StartPosition, NCryptography::CHashDigest_SHA256 _StartDigest, uint64 _FileSize);

		void f_CheckTransferDone();
		void f_ReportError(CStr const &_Error);

		CFileTransferSend *m_pThis;

		TCOptional<CDeprecated> m_Deprecated;

		CStr m_RootPath;
		uint64 m_MaxQueueSize = 0;
		NConcurrency::TCPromise<CFileTransferResult> m_Promise;
		CTransferStats m_TransferStats;
		mint m_nTransfers = 0;

		bool m_bCalled = false;
	};

	CFileTransferSend::~CFileTransferSend() = default;

	CFileTransferSend::CFileTransferSend(NStr::CStr const &_BasePath, uint64 _MaxQueueSize)
		: mp_pInternal(fg_Construct(this)) 
	{
		auto &Internal = *mp_pInternal;
		Internal.m_RootPath = _BasePath;
		Internal.m_MaxQueueSize = _MaxQueueSize;
	}

	TCFuture<void> CFileTransferSend::fp_Destroy()
	{
		auto &Internal = *mp_pInternal;

		if (Internal.m_Deprecated)
			co_await fg_Move(Internal.m_Deprecated->m_ReadSequencer).f_Destroy().f_Wrap() > fg_LogError("FileTransferSend", "Failed to destroy sequencer");

		co_return {};
	}


	void CFileTransferSend::CInternal::CDeprecated::f_ReportError(CStr const &_Error)
	{
		CFileTransferContextDeprecated::CInternal::CStateChange StateChange{m_Version};
		StateChange.m_State = CFileTransferContextDeprecated::CInternal::EState_Error;
		StateChange.m_Error = _Error;
		m_fStateCallback(fg_Move(StateChange)).f_DiscardResult();
		if (!m_pInternal->m_Promise.f_IsSet() && !m_bDelayedFinish)
			m_pInternal->m_Promise.f_SetException(DMibErrorInstance(_Error));
	}
	
	void CFileTransferSend::CInternal::CDeprecated::f_ReportFinished()
	{
		CFileTransferContextDeprecated::CInternal::CStateChange StateChange{m_Version};
		StateChange.m_State = CFileTransferContextDeprecated::CInternal::EState_Finished;
		StateChange.m_Finished.m_nBytes = m_pInternal->m_TransferStats.m_nTransferredBytes;
		StateChange.m_Finished.m_nSeconds = m_pInternal->m_TransferStats.m_Clock.f_GetTime();
		auto Future = m_fStateCallback(fg_TempCopy(StateChange));
		m_bDelayedFinish = true;
		fg_Move(Future) > [this, Finished = StateChange.m_Finished](TCAsyncResult<CFileTransferContextDeprecated::CInternal::CStateChange::CResult> &&_Result)
			{
				if (!m_pInternal->m_Promise.f_IsSet())
					m_pInternal->m_Promise.f_SetResult(Finished);
			}
		;
	}
	
	void CFileTransferSend::CInternal::f_ReportError(CStr const &_Error)
	{
		if (!m_Promise.f_IsSet())
			m_Promise.f_SetException(DMibErrorInstance(_Error));
	}

	void CFileTransferSend::CInternal::f_CheckTransferDone()
	{
		if (m_nTransfers)
			return;

		if (!m_Promise.f_IsSet())
		{
			CFileTransferResult Result;
			Result.m_nBytes = m_TransferStats.m_nTransferredBytes;
			Result.m_nSeconds = m_TransferStats.m_Clock.f_GetTime();

			m_Promise.f_SetResult(fg_Move(Result));
		}
	}

	TCUnsafeFuture<void> CFileTransferSend::CInternal::CDeprecated::f_DetermineWhatToSend()
	{
		{
			auto BlockingActorCheckout = fg_BlockingActor();
			m_Queue = co_await
				(
					g_Dispatch(BlockingActorCheckout) / [Manifest = m_Params.m_Manifest, pFileState = m_pFileState]() mutable -> CWorkQueueDeprecated
					{
						CWorkQueueDeprecated Queue;

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
									// Re-upload last 1024 KiB, aligned
									if (pFile->m_FileSize > NFile::gc_IdealIoSize)
										Entry.m_Position = fg_AlignDown(pFile->m_FileSize - NFile::gc_IdealIoSize, NFile::gc_IdealIoSize);
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
			;
		}

		if (m_Queue.m_Queue.f_IsEmpty())
		{
			f_ReportFinished();
			co_return {};
		}

		f_PerformFileSend() > fg_LogError("FileTransferSend", "Error sending files");

		co_return {};
	}
	
	TCUnsafeFuture<void> CFileTransferSend::CInternal::CDeprecated::f_PerformFileSend()
	{
		if (m_Queue.m_Queue.f_IsEmpty())
		{
			if (m_OutstandingBytes != 0)
				co_return {};

			f_ReportFinished();
			co_return {};
		}
		auto &Params = m_Params;
		
		if (m_OutstandingBytes >= Params.m_QueueSize)
			co_return {};

		auto SequencerSubscription = co_await m_ReadSequencer.f_Sequence();

		auto BlockingActorCheckout = fg_BlockingActor();

		while (m_OutstandingBytes < Params.m_QueueSize && !m_Queue.m_Queue.f_IsEmpty())
		{
			auto &Entry = m_Queue.m_Queue.f_GetFirst();
			CStr FileName = Entry.m_FileName;
			CStr RelativeFileName = Entry.m_RelativeFileName;
			uint64 Position = Entry.m_Position;
			uint64 nBytes = fg_Min(Entry.m_Size - Entry.m_Position, NFile::gc_IdealIoSize, CActorDistributionManager::mc_HalfMaxMessageSize);
			Entry.m_Position += nBytes;
			bool bFinished = Entry.f_IsFinished(); 
			if (bFinished)
				m_Queue.m_Queue.f_Remove(Entry);
			
			m_OutstandingBytes += nBytes;
			
			auto Result = co_await
				(
					g_Dispatch(BlockingActorCheckout) / [FileName, RelativeFileName, Position, nBytes, pFileState = m_pFileState, bFinished]()
					-> CFileTransferContextDeprecated::CInternal::CSendPart
					{
						if (pFileState->m_FileName != FileName)
						{
							pFileState->m_FileName = FileName;
							pFileState->m_File.f_Open(FileName, EFileOpen_Read | EFileOpen_ShareAll);
						}

						CFileTransferContextDeprecated::CInternal::CSendPart ToSend;
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
				.f_Wrap()
			;

			if (!Result)
			{
				f_ReportError(fg_Format("Error reading file data for file transfer: {}", Result.f_GetExceptionStr()));
				co_return {};
			}

			Result->m_Version = m_Version;

			m_fUploadCallback(fg_Move(*Result)) > [this, nBytes](TCAsyncResult<CFileTransferContextDeprecated::CInternal::CSendPart::CResult> &&_Result)
				{
					if (!_Result)
					{
						f_ReportError(fg_Format("Error transferring file data to remote: {}", _Result.f_GetExceptionStr()));
						return;
					}
					m_OutstandingBytes -= nBytes;
					m_pInternal->m_TransferStats.m_nTransferredBytes += nBytes;
					f_PerformFileSend() > fg_LogError("FileTransferSend", "Error sending files");
				}
			;
		}

		co_return {};
	}

	auto CFileTransferSend::f_SendFilesDeprecated(CFileTransferContextDeprecated _TransferContext) -> NConcurrency::TCFuture<CSendFilesResultDeprecated>
	{
		auto &Internal = *mp_pInternal;
		if (Internal.m_bCalled)
			co_return DMibErrorInstance("Send files has already been called");
		Internal.m_bCalled = true;

		Internal.m_Deprecated = CInternal::CDeprecated();
		auto &Deprecated = *Internal.m_Deprecated;
		Deprecated.m_pInternal = &Internal;

		Deprecated.m_pFileState->m_RootPath = Internal.m_RootPath;

		auto &Params = *_TransferContext.mp_pInternal;

		if (Params.m_QueueSize > Internal.m_MaxQueueSize)
			co_return DMibErrorInstance("Queue size larger than maximum allowed");

		for (auto &FileInfo : Params.m_Manifest.m_Files)
		{
			CStr Error;
			if (!NFile::CFile::fs_IsSafeRelativePath(FileInfo.f_GetPath(), Error))
				co_return DMibErrorInstance(fg_Format("Invalid relative path '{}' in file transfer manifest. Path cannot {}", FileInfo.f_GetPath(), Error));
		}

		Deprecated.m_Params = fg_Move(*_TransferContext.mp_pInternal);
		Deprecated.m_Version = Deprecated.m_Params.m_Version;
		Deprecated.m_fUploadCallback = g_ActorFunctorWeak(Deprecated.m_Params.m_DispatchActor) / fg_Move(Deprecated.m_Params.m_fSendPart);
		Deprecated.m_fStateCallback = g_ActorFunctorWeak(Deprecated.m_Params.m_DispatchActor) / fg_Move(Deprecated.m_Params.m_fStateChange);
		Deprecated.f_DetermineWhatToSend() > [this](auto &&_Result)
			{
				auto &Internal = *mp_pInternal;

				if (!_Result)
					Internal.m_Deprecated->f_ReportError(fg_Format("Error determining what to send: {}", _Result.f_GetExceptionStr()));
			}
		;

		co_return CSendFilesResultDeprecated
			{
				.m_Subscription = g_ActorSubscription / [this]() -> TCFuture<void>
				{
					auto &Internal = *mp_pInternal;
					auto &Deprecated = *Internal.m_Deprecated;

					TCFutureVector<void> Destroys;

					if (!Internal.m_Promise.f_IsSet() && !Deprecated.m_bDelayedFinish)
						Internal.m_Promise.f_SetException(DMibErrorInstance("File transfer aborted"));

					fg_Move(Deprecated.m_fUploadCallback).f_Destroy() > Destroys;
					fg_Move(Deprecated.m_fStateCallback).f_Destroy() > Destroys;

					co_await fg_AllDone(Destroys);

					co_return {};
				}
				, .m_Result = Internal.m_Promise.f_Future()
			}
		;
	}

	TCFuture<TCVector<CUploadQueueEntry>> CFileTransferSend::CInternal::f_DetermineWhatToSend()
	{
		auto BlockingActorCheckout = fg_BlockingActor();
		co_return co_await
			(
				g_Dispatch(BlockingActorCheckout) / [RootPath = m_RootPath]() mutable -> TCVector<CUploadQueueEntry>
				{
					TCVector<NPrivate::CUploadQueueEntry> FilesToSend;

					auto fAddEntry = [&](CStr const &_RelativePath, CStr const &_Path)
						{
							auto &Entry = FilesToSend.f_Insert();
							Entry.m_FileName = _Path;
							Entry.m_RelativeFileName = _RelativePath;
						}
					;

					if (CFile::fs_FileExists(RootPath, EFileAttrib_File))
					{
						CStr RelativePath = CFile::fs_GetFile(RootPath);
						fAddEntry(RelativePath, RootPath);
						return FilesToSend;
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

					return FilesToSend;
				}
			)
		;
	}

	TCFuture<CFileTransferSendDownloadFileContents> CFileTransferSend::CInternal::f_SendFileContents
		(
			CStr _FileName
			, uint64 _StartPosition
			, NCryptography::CHashDigest_SHA256 _StartDigest
			, uint64 _FileSize
		)
	{
		if (_StartPosition != 0)
		{
			if (_FileSize < _StartPosition)
				_StartPosition = 0;
			else
			{
				auto BlockingActorCheckout = fg_BlockingActor();
				_StartPosition = co_await
					(
						g_Dispatch(BlockingActorCheckout) / [_FileName, _StartDigest, _StartPosition, _FileSize]() mutable -> uint64
						{
							CFile File;
							File.f_Open(_FileName, EFileOpen_Read | EFileOpen_ShareAll | EFileOpen_NoLocalCache);

							if (_StartDigest == CFile::fs_GetFileChecksum_SHA256(File, _StartPosition))
							{
								if (_StartPosition == _FileSize)
									return _StartPosition;
								else
									return (_StartPosition / gc_IdealIoSize) * gc_IdealIoSize;
							}
							else
								return 0;
						}
					)
				;
			}
		}

		++m_nTransfers;

		co_return CFileTransferSendDownloadFileContents
			{
				.m_DataGenerator = fg_CallSafe
				(
					[this, FileName = _FileName, Position = _StartPosition, FileSize = _FileSize]() mutable -> TCAsyncGenerator<NContainer::CSecureByteVector>
					{
						auto CaptureScope = co_await g_CaptureExceptions;

						auto BlockingActorCheckout = fg_BlockingActor();
						auto FileResult = co_await
							(
								g_Dispatch(BlockingActorCheckout) / [FileName]() mutable
								{
									TCSharedPointer<CFile> pFile = fg_Construct();
									pFile->f_Open(FileName, EFileOpen_Read | EFileOpen_ShareAll | EFileOpen_NoLocalCache);
									return pFile;
								}
							)
							.f_Wrap()
						;

						if (!FileResult)
						{
							if (!m_Promise.f_IsSet())
								m_Promise.f_SetException(FileResult.f_GetException());

							co_return FileResult.f_GetException();
						}

						auto pFile = fg_Move(*FileResult);

						auto DestoryFile = co_await fg_AsyncDestroyGeneric
							(
								[&]() -> TCFuture<void>
								{
									auto pLocalFile = fg_Move(pFile);

									auto BlockingActorCheckout = fg_BlockingActor();
									co_await
										(
											g_Dispatch(BlockingActorCheckout) / [pLocalFile = fg_Move(pLocalFile)]() mutable
											{
												pLocalFile->f_Close();
											}
										)
									;

									co_return {};
								}
							)
						;

						mint ReadAhead = 16;
						TCLinkedList<TCFuture<NContainer::CSecureByteVector>> ReadAheadFutures;

						CRoundRobinBlockingActors BlockingActors(4);

						while (Position < FileSize)
						{
							mint ThisTime = fg_Min(FileSize - Position, gc_IdealIoSize);

							ReadAheadFutures.f_Insert
								(
									g_Dispatch(*BlockingActors) / [pFile, ThisTime, Position]() mutable
									{
										NContainer::CSecureByteVector Buffer;
										Buffer.f_SetLen(ThisTime);

										pFile->f_ReadNoLocalCache(Position, Buffer.f_GetArray(), ThisTime);

										return Buffer;
									}
								)
							;

							if (ReadAhead == 0)
							{
								auto Buffer = co_await ReadAheadFutures.f_PopFirst().f_Wrap();
								if (!Buffer)
								{
									if (!m_Promise.f_IsSet())
										m_Promise.f_SetException(Buffer.f_GetException());

									co_return Buffer.f_GetException();
								}

								co_yield fg_Move(*Buffer);
							}
							else
								--ReadAhead;

							Position += ThisTime;
							m_TransferStats.m_nTransferredBytes += ThisTime;
						}

						while (!ReadAheadFutures.f_IsEmpty())
						{
							auto Buffer = co_await ReadAheadFutures.f_PopFirst().f_Wrap();
							if (!Buffer)
							{
								if (!m_Promise.f_IsSet())
									m_Promise.f_SetException(Buffer.f_GetException());

								co_return Buffer.f_GetException();
							}

							co_yield fg_Move(*Buffer);
						}

						co_return {};
					}
				)
				, .m_Subscription = g_ActorSubscription / [this]
				{
					--m_nTransfers;
					f_CheckTransferDone();
				}
				, .m_StartPosition = _StartPosition
			}
		;

		co_return {};
	}

	TCFuture<CFileTransferSendDownloadFile> CFileTransferSend::CInternal::f_SendFile(CUploadQueueEntry _UploadEntry)
	{
		auto BlockingActorCheckout = fg_BlockingActor();
		auto FileSize = co_await
			(
				g_Dispatch(BlockingActorCheckout) / [FileName = _UploadEntry.m_FileName]() mutable -> uint64
				{
					return (uint64)CFile::fs_GetFileSize(FileName);
				}
			)
		;

		++m_nTransfers;

		co_return CFileTransferSendDownloadFile
			{
				.m_FilePath = _UploadEntry.m_RelativeFileName
				, .m_FileAttributes = CFile::fs_GetAttributes(_UploadEntry.m_FileName)
				, .m_WriteTime = CFile::fs_GetWriteTime(_UploadEntry.m_FileName)
				, .m_FileSize = FileSize
				, .m_fGetDataGenerator = g_ActorFunctor
				(
					g_ActorSubscription / [this]
					{
						--m_nTransfers;
						f_CheckTransferDone();
					}
				)
				/ [this, FileName = _UploadEntry.m_FileName, FileSize, AllowDestroy = NConcurrency::g_AllowWrongThreadDestroy]
				(uint64 _StartPosition, NCryptography::CHashDigest_SHA256 _StartDigest) mutable
				-> NConcurrency::TCFuture<CFileTransferSendDownloadFileContents>
				{
					auto SendContentsResult = co_await f_SendFileContents(FileName, _StartPosition, _StartDigest, FileSize).f_Wrap();

					if (!SendContentsResult)
					{
						if (!m_Promise.f_IsSet())
							m_Promise.f_SetException(SendContentsResult.f_GetException());

						co_return SendContentsResult.f_GetException();
					}

					co_return fg_Move(*SendContentsResult);
				}
			}
		;
	}

	auto CFileTransferSend::f_SendFiles() -> NConcurrency::TCFuture<CSendFilesResult>
	{
		auto &Internal = *mp_pInternal;
		if (Internal.m_bCalled)
			co_return DMibErrorInstance("Send files has already been called");
		Internal.m_bCalled = true;

		co_return CSendFilesResult
			{
				.m_FilesGenerator = fg_CallSafe
				(
					[this, FilesToSend = co_await Internal.f_DetermineWhatToSend()]() -> TCAsyncGenerator<CFileTransferSendDownloadFile>
					{
						for (auto &File : FilesToSend)
						{
							auto &Internal = *mp_pInternal;

							auto SendFile = co_await Internal.f_SendFile(File).f_Wrap();

							if (!SendFile)
							{
								if (!Internal.m_Promise.f_IsSet())
									Internal.m_Promise.f_SetException(SendFile.f_GetException());

								co_return {};
							}

							co_yield fg_Move(*SendFile);
						}

						co_return {};
					}
				)
				, .m_Subscription = g_ActorSubscription / [this]() -> TCFuture<void>
				{
					auto &Internal = *mp_pInternal;

					Internal.f_ReportError("File transfer aborted");

					co_return {};
				}
				, .m_Result = Internal.m_Promise.f_Future()
			}
		;
	}
}
