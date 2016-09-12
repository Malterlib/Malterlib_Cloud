// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

#include "Malterlib_Cloud_FileTransfer.h"
#include "Malterlib_Cloud_FileTransfer_Internal.h"

namespace NMib::NCloud
{
	using namespace NConcurrency;
	using namespace NStr;
	using namespace NContainer;
	using namespace NTime;
	using namespace NFile;
	using namespace NPtr;
	
	struct CFileTransferReceive::CInternal
	{
		~CInternal()
		{
			if (m_FileCache.f_IsValid())
			{
				fg_Dispatch
					(
						m_FileActor
						, [File = fg_Move(m_FileCache), FileActor = m_FileActor]() mutable
						{
							File.f_Close();
						}
					)
					> fg_DiscardResult()
				;
			}
		}
		
		CStr m_BasePath;
		TCActor<CActor> m_FileActor;
		TCContinuation<CFileTransferResult> m_DoneContinuation;
		CStr m_FileCacheFileName;
		CFile m_FileCache;
		CStr m_RootDirectory;
		bool m_bCalled = false;
		bool m_bDoneCalled = false;
	};

	CFileTransferReceive::~CFileTransferReceive() = default;
	CFileTransferReceive::CFileTransferReceive(CFileTransferReceive &&_Other) = default;
	CFileTransferReceive &CFileTransferReceive::operator =(CFileTransferReceive &&_Other) = default;
	
	CFileTransferReceive::CFileTransferReceive(NStr::CStr const &_BasePath, TCActor<CActor> const &_FileActor)
		: mp_pInternal(fg_Construct()) 
	{
		auto &Internal = *mp_pInternal;
		Internal.m_BasePath = _BasePath;
		Internal.m_FileActor = _FileActor;
		if (!Internal.m_FileActor)
			Internal.m_FileActor = fg_ConstructActor<CSeparateThreadActor>(fg_Construct("File transfer receive file access"));
	}
	
	NConcurrency::TCContinuation<CFileTransferContext> CFileTransferReceive::f_ReceiveFiles(uint64 _QueueSize, EReceiveFlag _Flags)
	{
		auto &Internal = *mp_pInternal;
		Internal.m_RootDirectory = Internal.m_BasePath;
		if (Internal.m_bCalled)
			return DMibErrorInstance("The file transfer context has already been gotten");
		Internal.m_bCalled = true;
		
		NConcurrency::TCContinuation<CFileTransferContext> Continuation;
		
		fg_Dispatch
			(
				Internal.m_FileActor
				, [this, _Flags]() -> CFileTransferContext::CInternal::CManifest
				{
					auto &Internal = *mp_pInternal;
					CFileTransferContext::CInternal::CManifest Manifest;

					if (CFile::fs_FileExists(Internal.m_RootDirectory, EFileAttrib_File))
					{
						if (_Flags & EReceiveFlag_DeleteExisting)
						{
							CFile::fs_DeleteDirectoryRecursive(Internal.m_RootDirectory);
							return Manifest;
						}
						DMibError("Destination already exists but is a file");
					}
					
					if (!CFile::fs_FileExists(Internal.m_RootDirectory, EFileAttrib_Directory))
					{
						return Manifest;
					}

					if (_Flags & EReceiveFlag_DeleteExisting)
					{
						CFile::fs_DeleteDirectoryRecursive(Internal.m_RootDirectory);
						return Manifest;
					}
					
					if (_Flags & EReceiveFlag_FailOnExisting)
						DMibError("Directory already exists");

					if (_Flags & EReceiveFlag_IgnoreExisting)
						return Manifest;
					
					{
						CFile::CFindFilesOptions FindOptions(Internal.m_RootDirectory + "/*", true);
						FindOptions.m_AttribMask = EFileAttrib_File;
						auto FoundFiles = CFile::fs_FindFiles(FindOptions);
						for (auto &File : FoundFiles)
						{
							CStr RelativePath = File.m_Path.f_Extract(Internal.m_RootDirectory.f_GetLen() + 1);
							auto &OutFile = Manifest.m_Files[RelativePath];
							OutFile.m_FileSize = CFile::fs_GetFileSize(File.m_Path);
						}
					}
					return Manifest;
				}
			)
			> Continuation % "Failed to extract current manifest" 
			/ [this, Continuation, _QueueSize, ThisActor = fg_ThisActor(this)]
			(CFileTransferContext::CInternal::CManifest &&_Manifest)
			{
				CFileTransferContext StartDownloadResult;
				CFileTransferContext::CInternal &StartDownload = *(StartDownloadResult.mp_pInternal);
				StartDownload.m_Manifest = fg_Move(_Manifest);
				StartDownload.m_QueueSize = _QueueSize;
				StartDownload.m_DispatchActor = ThisActor; 
				
				StartDownload.m_fStateChange = [this](CFileTransferContext::CInternal::CStateChange &&_StateChange) mutable 
					-> NConcurrency::TCContinuation<CFileTransferContext::CInternal::CStateChange::CResult>  
					{
						CFileTransferContext::CInternal::CStateChange::CResult Result = _StateChange.f_GetResult();
						auto &Internal = *mp_pInternal;
						
						if (Internal.m_DoneContinuation.f_IsSet())
							return fg_Explicit(Result);
						
						if (_StateChange.m_State == CFileTransferContext::CInternal::EState_Error)
							Internal.m_DoneContinuation.f_SetException(DMibErrorInstance(_StateChange.m_Error));
						else if (_StateChange.m_State == CFileTransferContext::CInternal::EState_Finished)
							Internal.m_DoneContinuation.f_SetResult(_StateChange.m_Finished);
						return fg_Explicit(Result);
					}
				;
				
				StartDownload.m_fSendPart = [this](CFileTransferContext::CInternal::CSendPart &&_Part) mutable
					-> NConcurrency::TCContinuation<CFileTransferContext::CInternal::CSendPart::CResult>
					{
						auto &Internal = *mp_pInternal;
						NConcurrency::TCContinuation<CFileTransferContext::CInternal::CSendPart::CResult> Continuation;
						fg_Dispatch
							(
								Internal.m_FileActor
								, [this, DownloadPart = fg_Move(_Part)]() mutable -> CFileTransferContext::CInternal::CSendPart::CResult
								{
									auto &Internal = *mp_pInternal;
									CStr Error;
									if (!CFileTransferContext::fs_IsValidRelativePath(DownloadPart.m_FilePath, Error))
										DMibError(fg_Format("File path cannot {}", Error));
									
									CStr FilePath = fg_Format("{}/{}", Internal.m_RootDirectory, DownloadPart.m_FilePath);
									
									if (Internal.m_FileCacheFileName != FilePath)
									{
										CFile::fs_CreateDirectory(CFile::fs_GetPath(FilePath));
										Internal.m_FileCache.f_Open(FilePath, EFileOpen_Write | EFileOpen_DontTruncate);
									}
									
									Internal.m_FileCache.f_SetPosition(DownloadPart.m_FilePosition);
									Internal.m_FileCache.f_Write(DownloadPart.m_Data.f_GetArray(), DownloadPart.m_Data.f_GetLen());
									
									if (DownloadPart.m_bFinished)
									{
										Internal.m_FileCache.f_SetLength(DownloadPart.m_FilePosition + DownloadPart.m_Data.f_GetLen());
										Internal.m_FileCacheFileName.f_Clear();
										Internal.m_FileCache.f_Close();
									}
									
									return DownloadPart.f_GetResult();
								}
							)
							> Continuation
						;
						return Continuation;
					}
				;
				
				Continuation.f_SetResult(fg_Move(StartDownloadResult));
			}
		;
		return Continuation;
	}
	
	NConcurrency::TCContinuation<CFileTransferResult> CFileTransferReceive::f_GetResult()
	{
		auto &Internal = *mp_pInternal;
		if (Internal.m_bDoneCalled)
			return DMibErrorInstance("The file result has already been gotten");
		Internal.m_bDoneCalled = true;
		return Internal.m_DoneContinuation;
	}
}
