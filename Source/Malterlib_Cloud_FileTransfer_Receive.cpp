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
	using namespace NStorage;

	struct CFileTransferReceive::CInternal : public CActorInternal
	{
		struct CFileCache
		{
			TCActor<CActor> m_FileActor;
			CStr m_FileName;
			CFile m_File;
			~CFileCache()
			{
				if (m_File.f_IsValid())
				{
					fg_Dispatch
						(
							m_FileActor
							, [File = fg_Move(m_File), FileActor = m_FileActor]() mutable
							{
								File.f_Close();
							}
						)
						> fg_DiscardResult()
					;
				}
			}
		};

		CStr m_BasePath;
		TCPromise<CFileTransferResult> m_DonePromise;
		TCSharedPointer<CFileCache> m_pFileCache = fg_Construct();
		EFileAttrib m_AttributeMask = EFileAttrib_UserRead | EFileAttrib_UserWrite | EFileAttrib_UserExecute | EFileAttrib_UnixAttributesValid;
		EFileAttrib m_AttributeAdd = EFileAttrib_None;
		CStr m_RootDirectory;
		bool m_bCalled = false;
		bool m_bDoneCalled = false;
	};

	CFileTransferReceive::~CFileTransferReceive() = default;
	
	CFileTransferReceive::CFileTransferReceive(NStr::CStr const &_BasePath, EFileAttrib _AttributeMask, NFile::EFileAttrib _AttributeAdd, TCActor<CActor> const &_FileActor)
		: mp_pInternal(fg_Construct()) 
	{
		auto &Internal = *mp_pInternal;
		Internal.m_AttributeMask = _AttributeMask;
		Internal.m_AttributeAdd = _AttributeAdd;
		Internal.m_BasePath = _BasePath;
		auto &Cache = *Internal.m_pFileCache;
		Cache.m_FileActor = _FileActor;
		if (!Cache.m_FileActor)
			Cache.m_FileActor = fg_ConstructActor<CSeparateThreadActor>(fg_Construct("File transfer receive file access"));
	}
	
	NConcurrency::TCFuture<CFileTransferContext> CFileTransferReceive::f_ReceiveFiles(uint64 _QueueSize, EReceiveFlag _Flags)
	{
		auto &Internal = *mp_pInternal;
		Internal.m_RootDirectory = Internal.m_BasePath;
		if (Internal.m_bCalled)
			return DMibErrorInstance("The file transfer context has already been gotten");
		Internal.m_bCalled = true;
		
		NConcurrency::TCPromise<CFileTransferContext> Promise;
		
		auto &Cache = *Internal.m_pFileCache;
		
		fg_Dispatch
			(
				Cache.m_FileActor
				, [RootDirectory = Internal.m_RootDirectory, _Flags]() -> CFileTransferContext::CInternal::CManifest
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
			)
			> Promise % "Failed to generate current manifest" 
			/ [this, Promise, _QueueSize, ThisActor = fg_ThisActor(this)]
			(CFileTransferContext::CInternal::CManifest &&_Manifest)
			{
				CFileTransferContext StartDownloadResult;
				CFileTransferContext::CInternal &StartDownload = *(StartDownloadResult.mp_pInternal);
				StartDownload.m_Manifest = fg_Move(_Manifest);
				StartDownload.m_QueueSize = _QueueSize;
				StartDownload.m_DispatchActor = ThisActor; 
				
				StartDownload.m_fStateChange = [this, AllowDestroy = g_AllowWrongThreadDestroy](CFileTransferContext::CInternal::CStateChange &&_StateChange) mutable 
					-> NConcurrency::TCFuture<CFileTransferContext::CInternal::CStateChange::CResult>
					{
						CFileTransferContext::CInternal::CStateChange::CResult Result = _StateChange.f_GetResult();
						auto &Internal = *mp_pInternal;
						
						if (Internal.m_DonePromise.f_IsSet())
							return fg_Explicit(Result);
						
						if (_StateChange.m_State == CFileTransferContext::CInternal::EState_Error)
							Internal.m_DonePromise.f_SetException(DMibErrorInstance(_StateChange.m_Error));
						else if (_StateChange.m_State == CFileTransferContext::CInternal::EState_Finished)
							Internal.m_DonePromise.f_SetResult(_StateChange.m_Finished);
						return fg_Explicit(Result);
					}
				;
				
				StartDownload.m_fSendPart = [this, AllowDestroy = g_AllowWrongThreadDestroy](CFileTransferContext::CInternal::CSendPart &&_Part) mutable
					-> NConcurrency::TCFuture<CFileTransferContext::CInternal::CSendPart::CResult>
					{
						auto &Internal = *mp_pInternal;
						auto &Cache = *Internal.m_pFileCache;
						NConcurrency::TCPromise<CFileTransferContext::CInternal::CSendPart::CResult> Promise;
						fg_Dispatch
							(
								Cache.m_FileActor
								,
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
							> Promise
						;
						return Promise.f_MoveFuture();
					}
				;
				
				Promise.f_SetResult(fg_Move(StartDownloadResult));
			}
		;
		return Promise.f_MoveFuture();
	}
	
	NConcurrency::TCFuture<CFileTransferResult> CFileTransferReceive::f_GetResult()
	{
		auto &Internal = *mp_pInternal;
		if (Internal.m_bDoneCalled)
			return DMibErrorInstance("The file result has already been gotten");
		Internal.m_bDoneCalled = true;
		return Internal.m_DonePromise.f_Future();
	}
}
