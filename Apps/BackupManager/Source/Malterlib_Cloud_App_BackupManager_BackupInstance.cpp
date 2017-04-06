
#include "Malterlib_Cloud_App_BackupManager_BackupInstance.h"

#include <Mib/Cryptography/RandomID>
#include <Mib/File/RSync>
#include <Mib/Concurrency/ActorSubscription>

namespace NMib::NCloud::NBackupManager
{
	struct CBackupInstance::CInternal
	{
		struct CRSyncContext
		{
			CStr const &f_GetSyncID() const
			{
				return TCMap<CStr, CRSyncContext>::fs_GetKey(*this);
			}

			CStr m_RelativeFileName;
			uint64 m_FileLength = 0;
			
			TCBinaryStreamFile<> m_File;
			TCBinaryStreamFile<> m_SourceFile;
			TCBinaryStreamFile<> m_TempFile;
			
			TCUniquePointer<CRSyncClient> m_pClient;
			
			TCActorFunctorWithID<TCContinuation<CSecureByteVector> (CSecureByteVector &&_Packet)> m_fRunProtocol;
			
			uint64 m_BytesTransferredIn = 0;
			uint64 m_BytesTransferredOut = 0;
		};
		
		CInternal(CStr const &_Name, CTime const &_StartTime, CStr const &_ID)
			: m_Name(_Name)
			, m_StartTime(_StartTime)
			, m_ID(_ID)
		{
			m_BackupDirectory = fg_Format("{}/Backups/{}/{tst.,tsb_}_{}", CFile::fs_GetProgramDirectory(), _Name, _StartTime, _ID);
			m_LatestBackupDirectory = CFile::fs_AppendPath(CFile::fs_GetPath(m_BackupDirectory), "Latest");
		}

		void f_RunRSyncProtocol(CRSyncContext &_Context, CSecureByteVector &&_ServerPacket);
		CExceptionPointer f_CheckFileName(CStr const &_FileName, CManifestFile **o_pManifestFile);
		
		CStr m_Name;
		CTime m_StartTime;
		CStr m_ID;
		
		CStr m_BackupDirectory;
		CStr m_LatestBackupDirectory;
		
		CManifest m_Manifest;
		
		TCMap<CStr, CFile> m_FileCache;
		TCMap<CStr, CRSyncContext> m_RSyncContexts;
	};
	
	CBackupInstance::CBackupInstance(CStr const &_Name, CTime const &_StartTime, CStr const &_ID)
		: mp_pInternal(fg_Construct(_Name, _StartTime, _ID))
	{
	}
	
	CBackupInstance::~CBackupInstance()
	{
	}
	
	CExceptionPointer CBackupInstance::CInternal::f_CheckFileName(CStr const &_FileName, CManifestFile **o_pManifestFile)
	{
		if (CFile::fs_IsPathAbsolute(_FileName))
			return fg_ExceptionPointer(DMibErrorInstance("Absolute paths not allowed"));

		if (CFile::fs_GetExpandedPath(_FileName, false) != _FileName)
			return fg_ExceptionPointer(DMibErrorInstance("Relative path components such as '..' are not allowed"));

		{
			CStr Error;
			if (!CFile::fs_IsValidFilePath(_FileName, Error))
				return fg_ExceptionPointer(DMibErrorInstance(fg_Format("The path cannot {}", Error)));
		}

		if (!o_pManifestFile)
			return nullptr;
		
		auto *pManifestFile = m_Manifest.m_Files.f_FindEqual(_FileName);
		
		if (!pManifestFile)
			return fg_ExceptionPointer(DMibErrorInstance("File does not exists in manifest"));
		
		*o_pManifestFile = pManifestFile; 
		
		return nullptr;
	}

	auto CBackupInstance::f_StartBackup(CManifest const &_Manifest) -> TCContinuation<CStartBackupResult>
	{
		return TCContinuation<CStartBackupResult>::fs_RunProtected<CExceptionFile>()
			> [&]()
			{
				auto &Internal = *mp_pInternal;
				Internal.m_Manifest = _Manifest;
				
				CStartBackupResult BackupResult;
				
				bool bIsLatest = CFile::fs_FileExists(Internal.m_LatestBackupDirectory) 
					&& CFile::fs_ResolveSymbolicLink(Internal.m_LatestBackupDirectory) == CFile::fs_GetFile(Internal.m_BackupDirectory)
				;
				
				for (auto &File : _Manifest.m_Files)
				{
					CStr FileName = CFile::fs_AppendPath(Internal.m_BackupDirectory, File.f_GetFileName());
					CStr OldFileName = CFile::fs_AppendPath(Internal.m_LatestBackupDirectory, File.f_GetFileName());
					
					if (!CFile::fs_FileExists(FileName))
					{
						if (!bIsLatest && CFile::fs_FileExists(OldFileName) && CFile::fs_GetFileChecksum_SHA256(OldFileName) == File.m_Digest)
						{
							CFile::fs_CreateDirectory(CFile::fs_GetPath(FileName));
							CFile::fs_CopyFile(OldFileName, FileName);
						}
						else
						{
							BackupResult.m_FilesNotUpToDate[File.f_GetFileName()] = 0;
							continue;
						}
					}
					
					auto Hash = CFile::fs_GetFileChecksum_SHA256(FileName);
					if (Hash == File.m_Digest)
						continue;
					
					BackupResult.m_FilesNotUpToDate[File.f_GetFileName()] = CFile::fs_GetFileSize(FileName);
				}
				
				return BackupResult;
			}
		;
	}

	TCContinuation<void> CBackupInstance::f_ManifestChange(CStr const &_FileName, CManifestChange const &_Change)
	{
		auto &Internal = *mp_pInternal;
		
		switch (_Change.f_GetTypeID())
		{
		case EManifestChange_Add:
			{
				if (auto pException = Internal.f_CheckFileName(_FileName, nullptr))
					return fg_Move(pException);
				
				auto &Change = _Change.f_Get<EManifestChange_Add>();
				
				Internal.m_Manifest.m_Files[_FileName] = Change.m_ManifestFile;
				
				DMibConOut("Add manifest: {}\n", _FileName);
				
				return fg_Explicit();
			}
		case EManifestChange_Change:
			{
				if (auto pException = Internal.f_CheckFileName(_FileName, nullptr))
					return fg_Move(pException);
				
				auto &Change = _Change.f_Get<EManifestChange_Change>();

				Internal.m_Manifest.m_Files[_FileName] = Change.m_ManifestFile;
				DMibConOut("Change manifest: {}\n", _FileName);
				
				return fg_Explicit();
			}
		case EManifestChange_Remove:
			{
				CManifestFile *pManifestFile;
				if (auto pException = Internal.f_CheckFileName(_FileName, &pManifestFile))
					return fg_Move(pException);
				
				Internal.m_Manifest.m_Files.f_Remove(pManifestFile);

				CStr AbsolutePath = CFile::fs_AppendPath(Internal.m_BackupDirectory, _FileName);

				DMibConOut("Remove manifest: {}\n", _FileName);
				
				return TCContinuation<void>::fs_RunProtected<CExceptionFile>()
					> [&]()
					{
						if (CFile::fs_FileExists(AbsolutePath))
							CFile::fs_DeleteFile(AbsolutePath);
					}
				;
			}
		case EManifestChange_Rename:
			{
				auto &Change = _Change.f_Get<EManifestChange_Rename>();
				
				CManifestFile *pManifestFile;
				if (auto pException = Internal.f_CheckFileName(Change.m_FromFileName, &pManifestFile))
					return fg_Move(pException);
				if (auto pException = Internal.f_CheckFileName(_FileName, nullptr))
					return fg_Move(pException);
				
				Internal.m_Manifest.m_Files.f_Remove(pManifestFile);
				Internal.m_Manifest.m_Files[_FileName] = Change.m_ManifestFile;
				
				CStr AbsoluteFrom = CFile::fs_AppendPath(Internal.m_BackupDirectory, Change.m_FromFileName);
				CStr AbsoluteTo = CFile::fs_AppendPath(Internal.m_BackupDirectory, _FileName);
				
				DMibConOut2("Rename manifest: {} -> \n", Change.m_FromFileName, _FileName);
				
				return TCContinuation<void>::fs_RunProtected<CExceptionFile>()
					> [&]()
					{
						if (CFile::fs_FileExists(AbsoluteFrom))
							CFile::fs_RenameFile(AbsoluteFrom, AbsoluteTo);
					}
				;
			}
		}
	}

	void CBackupInstance::CInternal::f_RunRSyncProtocol(CRSyncContext &_Context, CSecureByteVector &&_ServerPacket)
	{
		_Context.m_BytesTransferredIn += _ServerPacket.f_GetLen();
		
		bool bWantOneMoreProcess = true;
		bool bDone = false;
		while (bWantOneMoreProcess)
		{
			CSecureByteVector ToSendToServer;
			if (_Context.m_pClient->f_ProcessPacket(_ServerPacket, ToSendToServer, bWantOneMoreProcess))
				bDone = true;
			_ServerPacket.f_Clear();
			if (!ToSendToServer.f_IsEmpty())
			{
				_Context.m_BytesTransferredOut += ToSendToServer.f_GetLen();

				_Context.m_fRunProtocol(fg_Move(ToSendToServer)) > [this, SyncID = _Context.f_GetSyncID()](TCAsyncResult<CSecureByteVector> &&_ServerPacket)
					{
						auto pContext = m_RSyncContexts.f_FindEqual(SyncID);
						if (!pContext)
							return;
						
						if (!_ServerPacket)
						{
							DMibLogWithCategory(Mib/Cloud/BackupManager, Error, "Failed run RSync protocol: {}", _ServerPacket.f_GetExceptionStr());
							m_RSyncContexts.f_Remove(SyncID);
							return;
						}
						
						f_RunRSyncProtocol(*pContext, fg_Move(*_ServerPacket));
					}
				;
			}
		}
		
		if (bDone)
		{
			uint64 BytesTransferred = _Context.m_BytesTransferredIn + _Context.m_BytesTransferredOut;
			if (BytesTransferred == 0)
				BytesTransferred = 1;
			
			DMibLogWithCategory
				(
					Mib/Cloud/BackupManager
					, Debug
					, "RSync protocol finished for file '{}':   {} incoming bytes   {} outgoing bytes   {fe1} speedup"
					, _Context.m_RelativeFileName
					, _Context.m_BytesTransferredIn
					, _Context.m_BytesTransferredOut
					, fp64(_Context.m_FileLength) / fp64(BytesTransferred)  
				)
			;
			
			m_RSyncContexts.f_Remove(&_Context);
		}
	}
	
	TCContinuation<TCActorSubscriptionWithID<>> CBackupInstance::f_StartRSync
		(
			CStr const &_FileName
			, TCActorFunctorWithID<TCContinuation<CSecureByteVector> (CSecureByteVector &&_Packet)> &&_fRunProtocol
		)
	{
		auto &Internal = *mp_pInternal;
		
		CManifestFile *pManifestFile;
		
		if (auto pException = Internal.f_CheckFileName(_FileName, &pManifestFile))
			return fg_Move(pException);
		
		CStr RSyncID = fg_RandomID();
		auto &RSyncContext = Internal.m_RSyncContexts[RSyncID];
		
		CStr FileName = CFile::fs_AppendPath(Internal.m_BackupDirectory, _FileName);
		CStr OldFileName = CFile::fs_AppendPath(Internal.m_LatestBackupDirectory, _FileName);
		CStr TempFileName = fg_Format("{}.{}.tmp", FileName);
		
		auto pActorSubscription = g_ActorSubscription > [this, RSyncID, TempFileName]() -> TCContinuation<void>
			{
				TCContinuation<void> Continuation;
				
				auto &Internal = *mp_pInternal;
				
				auto pRsyncContext = Internal.m_RSyncContexts.f_FindEqual(RSyncID);
				if (pRsyncContext)
					Continuation = pRsyncContext->m_fRunProtocol.f_Destroy();
				else
					Continuation.f_SetResult();
				
				Internal.m_RSyncContexts.f_Remove(RSyncID);
				
				try
				{
					if (CFile::fs_FileExists(TempFileName))
						CFile::fs_DeleteFile(TempFileName);
				}
				catch (CExceptionFile const &_Exception)
				{
					DMibLogWithCategory(Mib/Cloud/BackupManager, Error, "Failed to cleanup tempfile for rsync: {}", _Exception);
				}
				
				return Continuation;
			}
		;

		CFile::fs_CreateDirectory(CFile::fs_GetPath(FileName));
		
		RSyncContext.m_RelativeFileName = _FileName;
		RSyncContext.m_FileLength = pManifestFile->m_Length;
		if 
			(
				CFile::fs_FileExists(FileName) 
				|| !CFile::fs_FileExists(OldFileName) 
				|| 
				(
					CFile::fs_FileExists(Internal.m_LatestBackupDirectory) 
					&& CFile::fs_ResolveSymbolicLink(Internal.m_LatestBackupDirectory) == CFile::fs_GetFile(Internal.m_BackupDirectory)
				)
			)
		{
			RSyncContext.m_File.f_Open(FileName, EFileOpen_Read | EFileOpen_Write | EFileOpen_DontTruncate | EFileOpen_ShareAll);
			RSyncContext.m_TempFile.f_Open(TempFileName, EFileOpen_Read | EFileOpen_Write | EFileOpen_DontTruncate | EFileOpen_ShareAll);
			RSyncContext.m_pClient = fg_Construct(RSyncContext.m_File, RSyncContext.m_File, 256, 4*1024*1024, 8*1024*1024, &RSyncContext.m_TempFile, ERSyncClientFlag_TruncateOutput);
		}
		else
		{
			RSyncContext.m_File.f_Open(FileName, EFileOpen_Read | EFileOpen_Write | EFileOpen_DontTruncate | EFileOpen_ShareAll);
			RSyncContext.m_SourceFile.f_Open(OldFileName, EFileOpen_Read | EFileOpen_ShareAll);
			RSyncContext.m_pClient = fg_Construct(RSyncContext.m_SourceFile, RSyncContext.m_File, 256, 4*1024*1024, 8*1024*1024, nullptr, ERSyncClientFlag_TruncateOutput);
		}
		RSyncContext.m_fRunProtocol = fg_Move(_fRunProtocol);
		
		Internal.f_RunRSyncProtocol(RSyncContext, {});
		
		return fg_Explicit(fg_Move(pActorSubscription));
	}
	
	TCContinuation<void> CBackupInstance::f_UploadData(CStr const &_FileName, uint64 _Position, CSecureByteVector &&_Data)
	{
		auto &Internal = *mp_pInternal;
		
		CManifestFile *pManifestFile;
		
		if (auto pException = Internal.f_CheckFileName(_FileName, &pManifestFile))
			return fg_Move(pException);
		
		return TCContinuation<void>::fs_RunProtected<CExceptionFile>()
			> [&]()
			{
				auto &CacheFile = Internal.m_FileCache[_FileName];
				if (!CacheFile.f_IsValid())
				{
					CStr FileName = CFile::fs_AppendPath(Internal.m_BackupDirectory, _FileName);
					CFile::fs_CreateDirectory(CFile::fs_GetPath(FileName));
					CacheFile.f_Open(FileName, EFileOpen_Write | EFileOpen_DontTruncate | EFileOpen_ShareAll | EFileOpen_NoLocalCache);
				}
				CacheFile.f_SetPosition(_Position);
				CacheFile.f_Write(_Data.f_GetArray(), _Data.f_GetLen());
			}
		;
	}
	
	TCContinuation<void> CBackupInstance::f_InitialBackupFinished()
	{
		auto &Internal = *mp_pInternal;
		
		return TCContinuation<void>::fs_RunProtected<CExceptionFile>()
			> [&]()
			{
				if (CFile::fs_FileExists(Internal.m_LatestBackupDirectory))
					CFile::fs_DeleteFile(Internal.m_LatestBackupDirectory);
				CFile::fs_CreateSymbolicLink(CFile::fs_GetFile(Internal.m_BackupDirectory), Internal.m_LatestBackupDirectory, EFileAttrib_Directory, ESymbolicLinkFlag_Relative);
			}
		;
	}
}
