// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Concurrency/AsyncDestroy>
#include <Mib/Concurrency/ActorSequencerActor>
#include <Mib/Concurrency/LogError>

#include "Malterlib_Cloud_FileTransfer.h"
#include "Malterlib_Cloud_FileTransfer_Internal.h"

namespace NMib::NCloud
{
	using namespace NConcurrency;
	using namespace NStr;
	using namespace NContainer;
	using namespace NTime;
	using namespace NFile;
	using namespace NStorage;

	struct CFileTransferReceive::CInternal : public CActorInternal
	{
		struct CFileCache
		{
			CStr m_FileName;
			CFile m_File;
			~CFileCache()
			{
				if (m_File.f_IsValid())
				{
					auto BlockingActorCheckout = fg_BlockingActor();
					auto BlockingActor = BlockingActorCheckout.f_Actor();

					fg_Dispatch
						(
							BlockingActor
							, [File = fg_Move(m_File)]() mutable
							{
								File.f_Close();
							}
						)
						.f_OnResultSet(BlockingActorCheckout.f_MoveResultHandler("FileTransferReceive", "Error closing file"))
					;
				}
			}
		};

		CStr m_BasePath;
		TCPromise<CFileTransferResult> m_DonePromise;
		TCSharedPointer<CFileCache> m_pFileCache = fg_Construct();
		CSequencer m_WriteSequencer{"File transfer receive"};
		EFileAttrib m_AttributeMask = EFileAttrib_UserRead | EFileAttrib_UserWrite | EFileAttrib_UserExecute | EFileAttrib_UnixAttributesValid;
		EFileAttrib m_AttributeAdd = EFileAttrib_None;
		CStr m_RootDirectory;
		bool m_bCalled = false;
		bool m_bDoneCalled = false;
		bool m_bAbortByDestroy = false;
		bool m_bTransferDone = false;
	};

	CFileTransferReceive::~CFileTransferReceive() = default;
	
	CFileTransferReceive::CFileTransferReceive(NStr::CStr const &_BasePath, EFileAttrib _AttributeMask, NFile::EFileAttrib _AttributeAdd)
		: mp_pInternal(fg_Construct()) 
	{
		auto &Internal = *mp_pInternal;
		Internal.m_AttributeMask = _AttributeMask;
		Internal.m_AttributeAdd = _AttributeAdd;
		Internal.m_BasePath = _BasePath;
	}

	TCFuture<void> CFileTransferReceive::fp_Destroy()
	{
		auto &Internal = *mp_pInternal;

		co_await fg_Move(Internal.m_WriteSequencer).f_Destroy().f_Wrap() > fg_LogError("FileTransferReceive", "Failed to destroy sequencer");

		co_return {};
	}

	NConcurrency::TCFuture<CFileTransferContextDeprecated> CFileTransferReceive::f_ReceiveFilesDeprecated(uint64 _QueueSize, EReceiveFlag _Flags)
	{
		auto &Internal = *mp_pInternal;
		Internal.m_RootDirectory = Internal.m_BasePath;
		if (Internal.m_bCalled)
			co_return DMibErrorInstance("The file transfer context has already been gotten");
		Internal.m_bCalled = true;

		CFileTransferContextDeprecated::CInternal::CManifest Manifest;
		{
			auto BlockingActorCheckout = fg_BlockingActor();
			Manifest = co_await
				(
					g_Dispatch(BlockingActorCheckout) / [RootDirectory = Internal.m_RootDirectory, _Flags]() -> CFileTransferContextDeprecated::CInternal::CManifest
					{
						CFileTransferContextDeprecated::CInternal::CManifest Manifest;

						if (CFile::fs_FileExists(RootDirectory, EFileAttrib_File))
						{
							if (_Flags & EReceiveFlag_DeleteExisting)
							{
								CFile::fs_DeleteDirectoryRecursive(RootDirectory);
								return Manifest;
							}
							DMibError("Destination already exists but is a file");
						}

						if (!CFile::fs_FileExists(RootDirectory, EFileAttrib_Directory))
							return Manifest;

						if (_Flags & EReceiveFlag_DeleteExisting)
						{
							try
							{
								CFile::fs_DeleteDirectoryRecursive(RootDirectory);
							}
							catch (CExceptionFile const &)
							{
								if (!CFile::fs_FileExists(RootDirectory))
									return Manifest;
								throw;
							}

							return Manifest;
						}

						if (_Flags & EReceiveFlag_FailOnExisting)
							DMibError("Directory already exists");

						if (_Flags & EReceiveFlag_IgnoreExisting)
							return Manifest;

						{
							CFile::CFindFilesOptions FindOptions(RootDirectory + "/*", true);
							FindOptions.m_AttribMask = EFileAttrib_File;
							auto FoundFiles = CFile::fs_FindFiles(FindOptions);
							for (auto &File : FoundFiles)
							{
								CStr RelativePath = File.m_Path.f_Extract(RootDirectory.f_GetLen() + 1);
								auto &OutFile = Manifest.m_Files[RelativePath];
								OutFile.m_FileSize = CFile::fs_GetFileSize(File.m_Path);
							}
						}
						return Manifest;
					}
					% "Failed to generate current manifest"
				)
			;
		}
		CFileTransferContextDeprecated StartDownloadResult;
		CFileTransferContextDeprecated::CInternal &StartDownload = *(StartDownloadResult.mp_pInternal);
		StartDownload.m_Manifest = fg_Move(Manifest);
		StartDownload.m_QueueSize = _QueueSize;
		StartDownload.m_DispatchActor = fg_ThisActor(this);

		StartDownload.m_fStateChange = [this, AllowDestroy = g_AllowWrongThreadDestroy](CFileTransferContextDeprecated::CInternal::CStateChange _StateChange) mutable
			-> NConcurrency::TCFuture<CFileTransferContextDeprecated::CInternal::CStateChange::CResult>
			{
				CFileTransferContextDeprecated::CInternal::CStateChange::CResult Result = _StateChange.f_GetResult();
				auto &Internal = *mp_pInternal;

				if (Internal.m_DonePromise.f_IsSet())
					co_return fg_Move(Result);

				if (_StateChange.m_State == CFileTransferContextDeprecated::CInternal::EState_Error)
					Internal.m_DonePromise.f_SetException(DMibErrorInstance(_StateChange.m_Error));
				else if (_StateChange.m_State == CFileTransferContextDeprecated::CInternal::EState_Finished)
					Internal.m_DonePromise.f_SetResult(_StateChange.m_Finished);

				co_return fg_Move(Result);
			}
		;

		StartDownload.m_fSendPart = [this, AllowDestroy = g_AllowWrongThreadDestroy](CFileTransferContextDeprecated::CInternal::CSendPart _Part) mutable
			-> NConcurrency::TCFuture<CFileTransferContextDeprecated::CInternal::CSendPart::CResult>
			{
				auto &Internal = *mp_pInternal;
				auto SequenceSubscription = co_await Internal.m_WriteSequencer.f_Sequence();
				auto BlockingActorCheckout = fg_BlockingActor();
				auto Result = co_await
					(
						g_Dispatch(BlockingActorCheckout) /
						[
							pCache = Internal.m_pFileCache
							, RootDirectory = Internal.m_RootDirectory
							, DownloadPart = fg_Move(_Part)
							, AttributeMask = Internal.m_AttributeMask
							, AttributeAdd = Internal.m_AttributeAdd
						]
						() mutable -> CFileTransferContextDeprecated::CInternal::CSendPart::CResult
						{
							CStr Error;
							if (!NFile::CFile::fs_IsSafeRelativePath(DownloadPart.m_FilePath, Error))
								DMibError(fg_Format("File path cannot {}", Error));

							CStr FilePath = fg_Format("{}/{}", RootDirectory, DownloadPart.m_FilePath);

							auto &Cache = *pCache;

							auto Attributes = DownloadPart.m_FileAttributes;

							if (Attributes == EFileAttrib_None)
								Attributes = EFileAttrib_UserWrite | EFileAttrib_UserRead | EFileAttrib_UnixAttributesValid;

							Attributes = ((Attributes & AttributeMask) | AttributeAdd);

							if (Cache.m_FileName != FilePath)
							{
								CFile::fs_CreateDirectory(CFile::fs_GetPath(FilePath));
								Cache.m_File.f_Open
									(
										FilePath
										, EFileOpen_Write | EFileOpen_DontTruncate | EFileOpen_ShareAll
										, Attributes | EFileAttrib_UserWrite | EFileAttrib_UserRead | EFileAttrib_UnixAttributesValid
									)
								;
							}

							Cache.m_File.f_SetPosition(DownloadPart.m_FilePosition);
							Cache.m_File.f_Write(DownloadPart.m_Data.f_GetArray(), DownloadPart.m_Data.f_GetLen());

							if (DownloadPart.m_bFinished)
							{
								Cache.m_File.f_SetLength(DownloadPart.m_FilePosition + DownloadPart.m_Data.f_GetLen());
								Cache.m_File.f_SetAttributes(Attributes);
								Cache.m_File.f_SetWriteTime(DownloadPart.m_WriteTime);
								Cache.m_FileName.f_Clear();
								Cache.m_File.f_Close();
							}

							return DownloadPart.f_GetResult();
						}
					)
				;
				co_return fg_Move(Result);
			}
		;

		co_return fg_Move(StartDownloadResult);
	}

	namespace
	{
		struct CResumeFileInfoParams
		{
			uint64 m_StartPosition = 0;
			NCryptography::CHashDigest_SHA256 m_StartDigest;
			CFile m_File;
		};

		struct CResumeFileInfo : public CResumeFileInfoParams
		{
			using CResumeFileInfoParams::CResumeFileInfoParams;

			CResumeFileInfo() = default;
			CResumeFileInfo &operator = (CResumeFileInfo &&) = default;
			CResumeFileInfo(CResumeFileInfo &&) = default;
			CResumeFileInfo(CResumeFileInfoParams &&_Other)
				: CResumeFileInfoParams(fg_Move(_Other))
			{
			}

			~CResumeFileInfo()
			{
				if (!m_File.f_IsValid())
					return;

				auto BlockingActorCheckout = fg_BlockingActor();
				auto BlockingActor = BlockingActorCheckout.f_Actor();

				fg_Dispatch
					(
						BlockingActor
						, [File = fg_Move(m_File)]() mutable
						{
							File.f_Close();
						}
					)
					.f_OnResultSet(BlockingActorCheckout.f_MoveResultHandler("FileTransferReceive", "Error closing file"))
				;
			}
		};
	}

	NConcurrency::TCFuture<CFileTransferResult> CFileTransferReceive::f_ReceiveFiles
		(
			NConcurrency::TCAsyncGenerator<CFileTransferSendDownloadFile> _FilesGenerator
			, uint64 _QueueSize
			, EReceiveFlag _Flags
		)
	{
		auto &Internal = *mp_pInternal;
		Internal.m_RootDirectory = Internal.m_BasePath;

		if (Internal.m_bCalled)
			co_return DMibErrorInstance("Receive files was already called previously");

		Internal.m_bCalled = true;
		Internal.m_bAbortByDestroy = true;

		{
			auto BlockingActorCheckout = fg_BlockingActor();
			co_await
				(
					g_Dispatch(BlockingActorCheckout) / [RootDirectory = Internal.m_RootDirectory, _Flags]() -> TCFuture<void>
					{
						auto CaptureScope = co_await g_CaptureExceptions;

						if (CFile::fs_FileExists(RootDirectory, EFileAttrib_File))
						{
							if (_Flags & EReceiveFlag_DeleteExisting)
							{
								CFile::fs_DeleteDirectoryRecursive(RootDirectory);
								co_return {};
							}
							co_return DMibErrorInstance("Destination already exists but is a file");
						}

						if (!CFile::fs_FileExists(RootDirectory, EFileAttrib_Directory))
							co_return {};

						if (_Flags & EReceiveFlag_DeleteExisting)
						{
							try
							{
								CFile::fs_DeleteDirectoryRecursive(RootDirectory);
							}
							catch (CExceptionFile const &)
							{
								if (!CFile::fs_FileExists(RootDirectory))
									co_return {};

								co_return NException::fg_CurrentException();
							}

							co_return {};
						}

						if (_Flags & EReceiveFlag_FailOnExisting)
							co_return DMibErrorInstance("Directory already exists");

						co_return {};
					}
					% ("Failed to generate current manifest: {}"_f << Internal.m_RootDirectory)
				)
			;
		}

		auto DownloadPipelineLength = _QueueSize / gc_IdealIoSize;

		CFileTransferResult Result;
		CClock Clock{true};

		auto iFile = co_await (fg_Move(_FilesGenerator).f_GetPipelinedIterator() % "GetPipelined File");
		auto DestroyIterator = co_await NConcurrency::fg_AsyncDestroy(iFile);

		CActorSubscription LastSubscriptions;

		for (; iFile; co_await (++iFile % "Next File"))
		{
			auto &&RemoteFile = *iFile;

			CStr Error;
			if (!NFile::CFile::fs_IsSafeRelativePath(RemoteFile.m_FilePath, Error))
				co_return DMibErrorInstance("File path cannot {}"_f << Error);

			CStr FilePath = Internal.m_RootDirectory / RemoteFile.m_FilePath;

			auto Attributes = RemoteFile.m_FileAttributes;

			if (Attributes == EFileAttrib_None)
				Attributes = EFileAttrib_UserWrite | EFileAttrib_UserRead | EFileAttrib_UnixAttributesValid;

			Attributes = ((Attributes & Internal.m_AttributeMask) | Internal.m_AttributeAdd);

			TCSharedPointer<CResumeFileInfo> pResumeFileInfo = fg_Construct();
			{
				auto BlockingActorCheckout = fg_BlockingActor();
				*pResumeFileInfo = co_await
					(
						g_Dispatch(BlockingActorCheckout) / [FilePath, _Flags, ExpectedLen = RemoteFile.m_FileSize, Attributes]() -> TCFuture<CResumeFileInfo>
						{
							auto CaptureScope = co_await g_CaptureExceptions;

							CFile::fs_CreateDirectory(CFile::fs_GetPath(FilePath));

							auto FileAttributes = Attributes | EFileAttrib_UserWrite | EFileAttrib_UserRead | EFileAttrib_UnixAttributesValid;

							if (_Flags & EReceiveFlag_IgnoreExisting)
							{
								CFile File;
								File.f_Open
									(
										FilePath
										, EFileOpen_Write | EFileOpen_ShareAll | EFileOpen_NoLocalCache
										, FileAttributes
									)
								;
								co_return CResumeFileInfoParams{.m_File = fg_Move(File)};
							}

							CFile File;
							File.f_Open
								(
									FilePath
									, EFileOpen_Read | EFileOpen_Write | EFileOpen_DontTruncate | EFileOpen_ShareAll | EFileOpen_NoLocalCache
									, FileAttributes
								)
							;

							uint64 CurrentLength = File.f_GetLength();
							uint64 ResumeLength;
							if (CurrentLength == ExpectedLen)
								ResumeLength = ExpectedLen;
							else
								ResumeLength = (fg_Min(CurrentLength, ExpectedLen) / gc_IdealIoSize) * gc_IdealIoSize;

							auto HashDigest = CFile::fs_GetFileChecksum_SHA256(File, ResumeLength);

							co_return CResumeFileInfoParams
								{
									.m_StartPosition = ResumeLength
									, .m_StartDigest = HashDigest
									, .m_File = fg_Move(File)
								}
							;
						}
						% "Failed to generate resume file info"
					)
				;
			}

			auto DownloadContents = co_await (RemoteFile.m_fGetDataGenerator(pResumeFileInfo->m_StartPosition, pResumeFileInfo->m_StartDigest) % "Get Data Generator");

			uint64 FilePosition = fg_Min(DownloadContents.m_StartPosition, pResumeFileInfo->m_StartPosition);

			TCFutureVector<void> Writes;

			CRoundRobinBlockingActors BlockingActors(4);

			for (auto iData = co_await (fg_Move(DownloadContents.m_DataGenerator).f_GetPipelinedIterator(DownloadPipelineLength) % "GetPipelined Data"); iData; co_await (++iData % "Next Data"))
			{
				auto &&Data = *iData;
				auto DataLen = Data.f_GetLen();
				(
					g_Dispatch(*BlockingActors) /
					[
						pResumeFileInfo
						, FilePosition
						, Data = fg_Move(Data)
					]
					() mutable
					{
						auto &File = pResumeFileInfo->m_File;
						File.f_WriteNoLocalCache(FilePosition, Data.f_GetArray(), Data.f_GetLen());
					}
				)
				> Writes;

				FilePosition += DataLen;
				Result.m_nBytes += DataLen;
			}

			co_await (fg_AllDone(Writes) % "Writes");

			if (FilePosition != RemoteFile.m_FileSize)
				co_return DMibErrorInstance("Number of transferred bytes does not match expected file size");

			{
				auto BlockingActorCheckout = fg_BlockingActor();
				co_await
					(
						g_Dispatch(BlockingActorCheckout) /
						[
							pResumeFileInfo
							, Attributes
							, WriteTime = RemoteFile.m_WriteTime
							, FileSize = RemoteFile.m_FileSize
						]
						() mutable
						{
							auto &File = pResumeFileInfo->m_File;
							File.f_SetLength(FileSize);
							File.f_SetAttributes(Attributes);
							File.f_SetWriteTime(WriteTime);
							File.f_Close();
						}
					)
				;
			}

			LastSubscriptions = fg_Move(DownloadContents.m_Subscription);
		}

		Internal.m_bTransferDone = true;

		if (LastSubscriptions)
			co_await fg_Exchange(LastSubscriptions, nullptr)->f_Destroy();

		Result.m_nSeconds = Clock.f_GetTime();

		co_return Result;
	}
	
	NConcurrency::TCFuture<CFileTransferResult> CFileTransferReceive::f_GetResultDeprecated()
	{
		auto &Internal = *mp_pInternal;

		if (Internal.m_bDoneCalled)
			co_return DMibErrorInstance("The file result has already been gotten");

		Internal.m_bDoneCalled = true;

		co_return co_await Internal.m_DonePromise.f_Future();
	}

	NConcurrency::TCFuture<NConcurrency::CActorSubscription> CFileTransferReceive::f_GetAbortSubscription()
	{
		co_return g_ActorSubscription / [this]() -> TCFuture<void>
			{
				auto &Internal = *mp_pInternal;
				if (Internal.m_bAbortByDestroy)
				{
					if (!Internal.m_bTransferDone)
						co_await fg_ThisActor(this).f_Destroy().f_Wrap() > fg_LogError("FileTransferReceive", "Failed to destroy file transfer when aborting");

					co_return {};
				}

				if (Internal.m_DonePromise.f_IsSet())
					co_return {};

				Internal.m_DonePromise.f_SetException(DMibErrorInstance("Remote disconnected or aborted"));

				co_return {};
			}
		;
	}
}
