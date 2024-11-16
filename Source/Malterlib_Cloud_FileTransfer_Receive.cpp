// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
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

	NConcurrency::TCFuture<CFileTransferContext> CFileTransferReceive::f_ReceiveFiles(uint64 _QueueSize, EReceiveFlag _Flags)
	{
		auto &Internal = *mp_pInternal;
		Internal.m_RootDirectory = Internal.m_BasePath;
		if (Internal.m_bCalled)
			co_return DMibErrorInstance("The file transfer context has already been gotten");
		Internal.m_bCalled = true;
		
		CFileTransferContext::CInternal::CManifest Manifest;
		{
			auto BlockingActorCheckout = fg_BlockingActor();
			Manifest = co_await
				(
					g_Dispatch(BlockingActorCheckout) / [RootDirectory = Internal.m_RootDirectory, _Flags]() -> CFileTransferContext::CInternal::CManifest
					{
						CFileTransferContext::CInternal::CManifest Manifest;

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
		CFileTransferContext StartDownloadResult;
		CFileTransferContext::CInternal &StartDownload = *(StartDownloadResult.mp_pInternal);
		StartDownload.m_Manifest = fg_Move(Manifest);
		StartDownload.m_QueueSize = _QueueSize;
		StartDownload.m_DispatchActor = fg_ThisActor(this);

		StartDownload.m_fStateChange = [this, AllowDestroy = g_AllowWrongThreadDestroy](CFileTransferContext::CInternal::CStateChange _StateChange) mutable
			-> NConcurrency::TCFuture<CFileTransferContext::CInternal::CStateChange::CResult>
			{
				CFileTransferContext::CInternal::CStateChange::CResult Result = _StateChange.f_GetResult();
				auto &Internal = *mp_pInternal;

				if (Internal.m_DonePromise.f_IsSet())
					co_return fg_Move(Result);

				if (_StateChange.m_State == CFileTransferContext::CInternal::EState_Error)
					Internal.m_DonePromise.f_SetException(DMibErrorInstance(_StateChange.m_Error));
				else if (_StateChange.m_State == CFileTransferContext::CInternal::EState_Finished)
					Internal.m_DonePromise.f_SetResult(_StateChange.m_Finished);

				co_return fg_Move(Result);
			}
		;

		StartDownload.m_fSendPart = [this, AllowDestroy = g_AllowWrongThreadDestroy](CFileTransferContext::CInternal::CSendPart _Part) mutable
			-> NConcurrency::TCFuture<CFileTransferContext::CInternal::CSendPart::CResult>
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
						() mutable -> CFileTransferContext::CInternal::CSendPart::CResult
						{
							CStr Error;
							if (!CFileTransferContext::fs_IsSafeRelativePath(DownloadPart.m_FilePath, Error))
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
	
	NConcurrency::TCFuture<CFileTransferResult> CFileTransferReceive::f_GetResult()
	{
		auto &Internal = *mp_pInternal;

		if (Internal.m_bDoneCalled)
			co_return DMibErrorInstance("The file result has already been gotten");

		Internal.m_bDoneCalled = true;

		co_return co_await Internal.m_DonePromise.f_Future();
	}

	NConcurrency::TCFuture<NConcurrency::CActorSubscription> CFileTransferReceive::f_GetAbortSubscription()
	{
		co_return g_ActorSubscription / [this]
			{
				auto &Internal = *mp_pInternal;
				if (Internal.m_DonePromise.f_IsSet())
					return;

				Internal.m_DonePromise.f_SetException(DMibErrorInstance("Remote disconnected or aborted"));
			}
		;
	}
}
