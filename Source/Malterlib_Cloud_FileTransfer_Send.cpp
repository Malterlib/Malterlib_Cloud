// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

#include <Mib/Compression/ZstandardAsync>
#include <Mib/Concurrency/ActorSequencerActor>
#include <Mib/Concurrency/AsyncDestroy>
#include <Mib/Concurrency/LogError>
#include <Mib/File/Generators>

#include "Malterlib_Cloud_FileTransfer.h"
#include "Malterlib_Cloud_FileTransfer_Internal.h"

namespace NMib::NCloud
{
	using namespace NCompression;
	using namespace NConcurrency;
	using namespace NContainer;
	using namespace NFile;
	using namespace NStorage;
	using namespace NStr;
	using namespace NTime;

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
				bool f_IsFinished() const
				{
					return m_Position == m_Size;
				}

				CStr m_FileName;
				CStr m_RelativeFileName;
				uint64 m_Position;
				uint64 m_Size;
			};

			TCLinkedList<CEntry> m_Queue;
		};

		struct CUploadQueueEntry
		{
			CStr m_FileName;
			CStr m_RelativeFileName;
			EFileAttrib m_FileAttributes = EFileAttrib_None;
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

		CSendFilesOptions m_Options;

		TCOptional<CDeprecated> m_Deprecated;

		NContainer::TCVector<CBasePath> m_BasePaths;
		uint64 m_MaxQueueSize = 0;
		NConcurrency::TCPromise<CFileTransferResult> m_Promise;
		CTransferStats m_TransferStats;
		mint m_nTransfers = 0;

		bool m_bCalled = false;
	};

	CFileTransferSend::~CFileTransferSend() = default;

	CFileTransferSend::CFileTransferSend(NContainer::TCVector<CBasePath> &&_BasePaths, uint64 _MaxQueueSize)
		: mp_pInternal(fg_Construct(this))
	{
		auto &Internal = *mp_pInternal;
		Internal.m_BasePaths = fg_Move(_BasePaths);
		Internal.m_MaxQueueSize = _MaxQueueSize;
	}

	CFileTransferSend::CFileTransferSend(NStr::CStr const &_BasePath, uint64 _MaxQueueSize)
		: CFileTransferSend({CBasePath{.m_Path = _BasePath}}, _MaxQueueSize)
	{
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

		if (Internal.m_BasePaths.f_GetLen() != 1)
			co_return DMibErrorInstance("Deprecated files send only supports one base path");

		if (Internal.m_BasePaths[0].m_Name)
			co_return DMibErrorInstance("Deprecated files cannot override send file name");

		Deprecated.m_pFileState->m_RootPath = Internal.m_BasePaths[0].m_Path;

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
				g_Dispatch(BlockingActorCheckout) / [BasePaths = m_BasePaths, bIncludeRootDirectoryName = m_Options.m_bIncludeRootDirectoryName]() mutable
				-> TCFuture<TCVector<CUploadQueueEntry>>
				{
					auto CaptureExceptinos = co_await (g_CaptureExceptions % "Failed to determine what to send");

					TCVector<NPrivate::CUploadQueueEntry> FilesToSend;

					auto fAddEntry = [&](CStr const &_RelativePath, CStr const &_Path, EFileAttrib _Attributes)
						{
							auto &Entry = FilesToSend.f_Insert();
							Entry.m_FileName = _Path;
							Entry.m_RelativeFileName = _RelativePath;
							Entry.m_FileAttributes = _Attributes;
						}
					;

					for (auto &BasePath : BasePaths)
					{
						if (CFile::fs_FileExists(BasePath.m_Path, EFileAttrib_File))
						{
							CStr RelativePath = BasePath.m_Name;
							if (RelativePath.f_IsEmpty())
								RelativePath = CFile::fs_GetFile(BasePath.m_Path);

							fAddEntry(RelativePath, BasePath.m_Path, CFile::fs_GetAttributes(BasePath.m_Path));

							continue;
						}

						if (!CFile::fs_FileExists(BasePath.m_Path, EFileAttrib_Directory))
							co_return DMibErrorInstance("Send path does not exist");

						CFile::CFindFilesOptions FindOptions(BasePath.m_Path + "/*", true);
						FindOptions.m_AttribMask = EFileAttrib_File | EFileAttrib_Link | EFileAttrib_Directory | EFileAttrib_FindDirectoryLast;

						CStr RelativeRoot = BasePath.m_Name;
						if (bIncludeRootDirectoryName && RelativeRoot.f_IsEmpty())
							RelativeRoot = CFile::fs_GetFile(BasePath.m_Path);

						auto FoundFiles = CFile::fs_FindFiles(FindOptions);
						for (auto &File : FoundFiles)
						{
							CStr RelativePath  = RelativeRoot / File.m_Path.f_Extract(BasePath.m_Path.f_GetLen() + 1);
							fAddEntry(RelativePath, File.m_Path, File.m_Attribs);
						}

						fAddEntry(RelativeRoot, BasePath.m_Path, CFile::fs_GetAttributes(BasePath.m_Path));
					}

					co_return fg_Move(FilesToSend);
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
			if (_FileSize < _StartPosition || m_Options.m_bCompressZstandard)
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

		auto ReadFileGenerator = NFile::fg_ReadFileGenerator
			(
				CReadFileGeneratorParams
				{
					.m_Path = _FileName
					, .m_fOnError = g_ActorFunctorWeak / [this](NException::CExceptionPointer _pError) -> TCFuture<void>
					{
						if (!m_Promise.f_IsSet())
							m_Promise.f_SetException(fg_Move(_pError));

						co_return {};
					}
					, .m_fOnProgress = g_ActorFunctorWeak / [this](uint64 _nBytesProgress, uint64 _nTransferredBytes, uint64 _nTotalBytes) -> TCFuture<void>
					{
						m_TransferStats.m_nTransferredBytes += _nBytesProgress;

						co_return {};
					}
					, .m_StartPosition = _StartPosition
					, .m_FileSize = _FileSize
				}
			)
		;

		co_return CFileTransferSendDownloadFileContents
			{
				.m_DataGenerator = m_Options.m_bCompressZstandard
				? fg_CompressZstandardAsync(fg_Move(ReadFileGenerator), CZStandardCompressionOptions{.m_KnownSize = _FileSize, .m_CompressionLevel = m_Options.m_ZstandardLevel})
				: fg_Move(ReadFileGenerator)
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
		uint64 FileSize = TCLimitsInt<uint64>::mc_Max;
		NTime::CTime WriteTime;
		if (_UploadEntry.m_FileAttributes & EFileAttrib_Link)
		{
			auto BlockingActorCheckout = fg_BlockingActor();
			WriteTime = co_await
				(
					g_Dispatch(BlockingActorCheckout) / [FileName = _UploadEntry.m_FileName]() mutable
					{
						return CFile::fs_GetWriteTimeOnLink(FileName);
					}
				)
			;
		}
		else
		{
			auto BlockingActorCheckout = fg_BlockingActor();
			auto [FileSizeResult, WriteTimeResult] = co_await
				(
					g_Dispatch(BlockingActorCheckout) / [FileName = _UploadEntry.m_FileName]() mutable
					{
						return fg_Tuple
							(
								(uint64)CFile::fs_GetFileSize(FileName)
								, CFile::fs_GetWriteTime(FileName)
							)
						;
					}
				)
			;
			FileSize = FileSizeResult;
			WriteTime = WriteTimeResult;
		}

		++m_nTransfers;

		CStr SymlinkContents;

		if (_UploadEntry.m_FileAttributes & EFileAttrib_Link)
			SymlinkContents = CFile::fs_ResolveSymbolicLink(_UploadEntry.m_FileName);

		co_return CFileTransferSendDownloadFile
			{
				.m_FilePath = _UploadEntry.m_RelativeFileName
				, .m_FileAttributes = _UploadEntry.m_FileAttributes
				, .m_WriteTime = WriteTime
				, .m_FileSize = m_Options.m_bCompressZstandard ? TCLimitsInt<uint64>::mc_Max : FileSize
				, .m_SymlinkContents = fg_Move(SymlinkContents)
				, .m_fGetDataGenerator = g_ActorFunctor
				(
					g_ActorSubscription / [this]
					{
						--m_nTransfers;
						f_CheckTransferDone();
					}
				)
				/ [this, FileName = _UploadEntry.m_FileName, FileSize, AllowDestroy = NConcurrency::g_AllowWrongThreadDestroy, FileAttributes = _UploadEntry.m_FileAttributes]
				(uint64 _StartPosition, NCryptography::CHashDigest_SHA256 _StartDigest) mutable
				-> NConcurrency::TCFuture<CFileTransferSendDownloadFileContents>
				{
					if ((FileAttributes & EFileAttrib_Link) || !(FileAttributes & EFileAttrib_File))
						co_return DMibErrorInstance("File has no contents");

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

	auto CFileTransferSend::f_SendFiles(CSendFilesOptions _Options) -> NConcurrency::TCFuture<CSendFilesResult>
	{
		auto &Internal = *mp_pInternal;

		if (Internal.m_bCalled)
			co_return DMibErrorInstance("Send files has already been called");
		Internal.m_bCalled = true;

		Internal.m_Options = fg_Move(_Options);

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
