// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

#include "Malterlib_Cloud_BackupManagerClient.h"
#include "Malterlib_Cloud_BackupManagerClient_Internal.h"
#include "Malterlib_Cloud_BackupManagerClient_BackupInstance.h"

namespace NMib::NCloud
{
	TCContinuation<void> CBackupManagerClient::CInternal::f_RetrySubscribeChanges()
	{
		if (m_pThis->mp_bDestroyed)
			return DMibErrorInstance("Destroyed");

		TCContinuation<void> Continuation;
		
		if (m_bRunningRetrySubscribe)
		{
			TCContinuation<void> FinishedContinuation = m_SubscribeChangesContinuations.f_Insert();
			FinishedContinuation > Continuation / [this, Continuation]
				{
					f_RetrySubscribeChanges() > Continuation;
				}
			;
			return Continuation;
		}
		
		DMibCheck(!m_bRunningRetrySubscribe);
		m_bRunningRetrySubscribe = true;
		m_bRerunRetrySubscribe = false;
		
		auto Cleanup = g_OnScopeExitActor > [this]
			{
				m_bRunningRetrySubscribe = false;
				
				auto SubscribeChangesContinuations = fg_Move(m_SubscribeChangesContinuations);
				
				for (auto &Continuation : SubscribeChangesContinuations)
					Continuation.f_SetResult();
			}
		;
		
		struct CPendingInfo
		{
			TCSet<CStr> m_PendingPaths;
			TCSet<CStr> m_MissingPaths;
		};
		
		TCSet<CStr> PendingPaths;
		
		for (auto &WatchedPath : m_WatchedPaths)
		{
			if (WatchedPath.m_bPending)
				PendingPaths[WatchedPath.f_GetPath()];
		}
		
		g_Dispatch(m_FileActor) > [PendingPaths]
			{
				CPendingInfo PendingInfo;
				
				for (auto &Path : PendingPaths)
				{
					if (CFile::fs_FileExists(Path, EFileAttrib_Directory))
						continue;
					
					CStr MissingPath = CFile::fs_GetPath(Path);
					while (!MissingPath.f_IsEmpty() && !CFile::fs_FileExists(MissingPath, EFileAttrib_Directory))
						MissingPath = CFile::fs_GetPath(MissingPath);
					
					if (!MissingPath.f_IsEmpty() && !PendingInfo.m_MissingPaths.f_FindEqual(MissingPath))
					{
						bool bFound = false;
						for (auto &Path : PendingInfo.m_MissingPaths)
						{
							if (MissingPath.f_StartsWith(Path))
							{
								bFound = true;
								break;
							}
							else if (Path.f_StartsWith(MissingPath))
							{
								PendingInfo.m_MissingPaths.f_Remove(Path);
								break;
							}
						}
						if (!bFound)
							PendingInfo.m_MissingPaths[MissingPath];
					}
					PendingInfo.m_PendingPaths[Path];
				}
				return PendingInfo;
			}
			> Continuation / [this, Continuation, PendingPaths, Cleanup](CPendingInfo &&_PendingInfo)
			{
				if (m_pThis->mp_bDestroyed)
					return;
				
				TCActorResultMap<CStr, CActorSubscription> SubscribeResults;
				TCActorResultMap<CStr, CActorSubscription> SubscribeResultsMissing;
				
				for (auto &Path : PendingPaths)
				{
					if (_PendingInfo.m_PendingPaths.f_FindEqual(Path))
						continue;
					
					auto *pWatchedPath = m_WatchedPaths.f_FindEqual(Path);
					if (!pWatchedPath)
						continue;

					auto &WatchedPath = *pWatchedPath;
					
					EFileChange Changes = EFileChange_All & (~EFileChange_Recursive);
					if (WatchedPath.m_bRecursive)
						Changes |= EFileChange_Recursive;
					
					m_FileChangeNotificationsActor
						(
							&CFileChangeNotificationActor::f_RegisterForChanges
							, Path
							, Changes
							, [this, Path](CFileChangeNotification::CNotification const &_Notification)
							{
								CFileChangeNotification::CNotification Notification = _Notification;
								Notification.m_Path = CFile::fs_AppendPath(Path, _Notification.m_Path);
								
								if (!Notification.m_PathFrom.f_IsEmpty())
									Notification.m_PathFrom = CFile::fs_AppendPath(Path, _Notification.m_PathFrom);
								
								f_OnFileChanged(Notification);
							}
							, fg_ThisActor(m_pThis)
						)
						> SubscribeResults.f_AddResult(Path)
					;
				}
				
				for (auto &MissingPath : _PendingInfo.m_MissingPaths)
				{
					auto Mapped = m_WatchedPathsMissing(MissingPath);
					if (!Mapped.f_WasCreated())
						continue;
					m_FileChangeNotificationsActor
						(
							&CFileChangeNotificationActor::f_RegisterForChanges
							, MissingPath
							, EFileChange_FileName | EFileChange_DirectoryName
							, [this](CFileChangeNotification::CNotification const &_Notification)
							{
								if (!m_bRerunRetrySubscribe)
								{
									m_bRerunRetrySubscribe = true;
									f_RetrySubscribeChanges();
								}
							}
							, fg_ThisActor(m_pThis)
						)
						> SubscribeResultsMissing.f_AddResult(MissingPath)
					;
				}
				
				TCSet<CStr> MissingPathsToRemove;
				for (auto &WatchedPath : m_WatchedPathsMissing)
				{
					auto &Path = m_WatchedPathsMissing.fs_GetKey(WatchedPath);
					if (!_PendingInfo.m_MissingPaths.f_FindEqual(Path))
						MissingPathsToRemove[Path];
				}
				
				for (auto &ToRemove : MissingPathsToRemove)
					m_WatchedPathsMissing.f_Remove(ToRemove);

				SubscribeResults.f_GetResults()
					+ SubscribeResultsMissing.f_GetResults()
					> Continuation / [Continuation, this, Cleanup]
					(TCMap<CStr, TCAsyncResult<CActorSubscription>> &&_Results, TCMap<CStr, TCAsyncResult<CActorSubscription>> &&_ResultsMissing) mutable
					{
						for (auto &Result : _Results)
						{
							auto &Path = _Results.fs_GetKey(Result);
							if (!Result)
							{
								DMibLogCategoryStr(m_Config.m_LogCategory);
								DMibLog(Error, "One file change notification '{}' failed to register: {}", Path, Result.f_GetExceptionStr());
								continue;
							}
							auto &WatchedPath = m_WatchedPaths[Path];
							WatchedPath.m_Subscription = fg_Move(*Result);
							WatchedPath.m_bPending = false; 
						}
						for (auto &Result : _ResultsMissing)
						{
							auto &Path = _ResultsMissing.fs_GetKey(Result);
							if (!Result)
							{
								DMibLogCategoryStr(m_Config.m_LogCategory);
								DMibLog(Error, "One file change notification for missing '{}' failed to register: {}", Path, Result.f_GetExceptionStr());
								continue;
							}
							m_WatchedPathsMissing[Path].m_Subscription = fg_Move(*Result);
						}

						Continuation.f_SetResult();
					}
				;
			}
		;
		
		return Continuation;
	}

	TCContinuation<void> CBackupManagerClient::CInternal::f_SubscribeChanges()
	{
		if (m_pThis->mp_bDestroyed)
			return DMibErrorInstance("Destroyed");
		
		auto &ManifestConfig = m_Config.m_ManifestConfig;
		
		TCMap<CStr, zbool> Paths; 
		for (auto &Destination : ManifestConfig.m_IncludeWildcards)
		{
			auto &Wildcard = ManifestConfig.m_IncludeWildcards.fs_GetKey(Destination);
			
			bool bRecursive = false;
			auto WildcardParsed = CDirectoryManifestConfig::fs_ParseWildcard(Wildcard, bRecursive);
			
			auto &bRecursiveForPath = Paths[CFile::fs_GetPath(CFile::fs_AppendPath(ManifestConfig.m_Root, WildcardParsed))];
			if (bRecursive)
				bRecursiveForPath = bRecursive; 
		}

		for (auto &bRecursive : Paths)
		{
			auto &Path = Paths.fs_GetKey(bRecursive);
			
			auto Mapping = m_WatchedPaths(Path);
			auto &WatchedPath = *Mapping;
			if (Mapping.f_WasCreated())
			{
				WatchedPath.m_bPending = true;
				WatchedPath.m_bRecursive = bRecursive;
			}
			else
			{
				if (bRecursive && !WatchedPath.m_bRecursive)
				{
					WatchedPath.m_bRecursive = true;
					WatchedPath.m_bPending = true;
				}
			}
		}
		
		return f_RetrySubscribeChanges();
	}
	
	bool CBackupManagerClient::CInternal::f_IsPathInManifest(CStr const &_Path, CStr &o_FileName)
	{
		bool bIsIncluded = false;
		
		CStr NotificationPath = CFile::fs_GetPath(_Path);
		CStr NotificationFile = CFile::fs_GetFile(_Path);
		
		auto &ManifestConfig = m_Config.m_ManifestConfig;
		
		for (auto &Destination : ManifestConfig.m_IncludeWildcards)
		{
			auto &Wildcard = ManifestConfig.m_IncludeWildcards.fs_GetKey(Destination);
			
			bool bRecursive = false;
			auto WildcardParsed = CDirectoryManifestConfig::fs_ParseWildcard(Wildcard, bRecursive);
			
			CStr WildcardPath = CFile::fs_GetPath(WildcardParsed);
			CStr WildcardFileName = CFile::fs_GetFile(WildcardParsed);
			
			if (!bRecursive)
			{
				if (NotificationPath != WildcardPath)
					continue;
			}
			else
			{
				if (NotificationPath != WildcardPath && !NotificationPath.f_StartsWith(WildcardPath + "/"))
					continue;
			}

			if (fg_StrMatchWildcard(NotificationFile.f_GetStr(), WildcardFileName.f_GetStr()) != EMatchWildcardResult_WholeStringMatchedAndPatternExhausted)
				continue;
			
			CStr DestinationFileName;
			if (Destination)
				DestinationFileName = CFile::fs_AppendPath(*Destination, _Path.f_Extract(WildcardPath.f_GetLen() + 1));
			else
				DestinationFileName = _Path;
			
			if (!bIsIncluded)
				o_FileName = DestinationFileName;
			else if (o_FileName != DestinationFileName)
				DMibError("Matched file to two different destinations in manifest");
			
			bIsIncluded = true;
			break;
		}
		
		if (!bIsIncluded)
			return false;
		
		if (fg_StrMatchesAnyWildcardInMap(_Path, ManifestConfig.m_ExcludeWildcards))
			return false;
		
		return true;
	}
	
	void CBackupManagerClient::CInternal::f_OnFileChanged(CFileChangeNotification::CNotification const &_Notification)
	{
		CFileChangeNotification::CNotification Notification = _Notification;
		
		auto &ManifestConfig = m_Config.m_ManifestConfig;
		
		CStr RelativePath;
		CStr RelativePathFrom;
		
		CStr OriginalPath = CFile::fs_MakePathRelative(Notification.m_Path, ManifestConfig.m_Root);
		CStr OriginalPathFrom;
		
		bool bDirtyHint = false;
		
		if (Notification.m_Notification == EFileChangeNotification_Renamed)
		{
			OriginalPathFrom = CFile::fs_MakePathRelative(Notification.m_PathFrom, ManifestConfig.m_Root);
			
			bool bFromValid = f_IsPathInManifest(OriginalPathFrom, RelativePathFrom);
			bool bToValid = f_IsPathInManifest(OriginalPath, RelativePath);
			if (!bFromValid && !bToValid)
				return;
			
			if (bFromValid && !bToValid)
			{
				Notification.m_Notification = EFileChangeNotification_Removed;
				Notification.m_Path = Notification.m_PathFrom;
				RelativePath = RelativePathFrom;
				RelativePathFrom.f_Clear();
			}
			else if (!bFromValid && bToValid)
			{
				Notification.m_Notification = EFileChangeNotification_Added;
				RelativePathFrom.f_Clear();
			}
			bDirtyHint = true;
		}
		else if (!f_IsPathInManifest(OriginalPath, RelativePath))
			return;
		else if (Notification.m_Notification == EFileChangeNotification_Added || Notification.m_Notification == EFileChangeNotification_Removed)
			bDirtyHint = true;
		
		f_UpdateManifest(RelativePath, OriginalPath, bDirtyHint)
			+ f_UpdateManifest(RelativePathFrom, OriginalPathFrom, bDirtyHint)
			> [=](TCAsyncResult<CUpdateManifestResult> &&_Change, TCAsyncResult<CUpdateManifestResult> &&_ChangeFrom)
			{
				if (!_Change)
				{
					DMibLogCategoryStr(m_Config.m_LogCategory);
					DMibLog(Error, "Failed to update manifest: {}", _Change.f_GetExceptionStr());
					return;
				}
				
				if (!_ChangeFrom)
				{
					DMibLogCategoryStr(m_Config.m_LogCategory);
					DMibLog(Error, "Failed to update manifest: {}", _ChangeFrom.f_GetExceptionStr());
					return;
				}
				
				auto fSendManifestChange = [&](CStr const &_Path, CBackupManagerBackup::CManifestChange const &_ManifestChange, bool _bDirty)
					{
						for (auto &RunningInstance : m_RunningBackupInstances)
							RunningInstance(&NPrivate::CBackupManagerClient_Instance::f_ManifestChanged, _Path, _ManifestChange, _bDirty) > fg_DiscardResult();
					}
				;
				
				auto &Change = *_Change;
				auto &ChangeFrom = *_ChangeFrom;
				
				TCMap<CStr, CUpdatedDirectory> UpdatedDirectories = Change.m_UpdatedDirectories;
				for (auto &UpdatedDirectory : ChangeFrom.m_UpdatedDirectories)
				{
					CStr const &Path = ChangeFrom.m_UpdatedDirectories.fs_GetKey(UpdatedDirectory);
					
					auto Mapped = UpdatedDirectories(Path);
					
					if (Mapped.f_WasCreated())
						*Mapped = UpdatedDirectory;
					else
					{
						if (UpdatedDirectory.m_bAdded)
							(*Mapped).m_bAdded = true;
					}
				}
				
				for (auto &UpdatedDirectory : UpdatedDirectories)
				{
					auto &Path = UpdatedDirectories.fs_GetKey(UpdatedDirectory);
					if (UpdatedDirectory.m_bAdded)
						fSendManifestChange(Path, CBackupManagerBackup::CManifestChange_Add{fg_Move(UpdatedDirectory.m_ManifestFile)}, false);
					else
						fSendManifestChange(Path, CBackupManagerBackup::CManifestChange_Change{fg_Move(UpdatedDirectory.m_ManifestFile)}, false);
				}
				
				if (Notification.m_Notification == EFileChangeNotification_Renamed)
				{
					if (ChangeFrom.m_bRemoved)
					{
						if (Change.m_bAdded)
							fSendManifestChange(RelativePath, CBackupManagerBackup::CManifestChange_Rename{fg_Move(Change.m_ManifestFile), RelativePathFrom}, Change.m_bIDChanged);
						else if (Change.m_bRemoved)
						{
							fSendManifestChange(RelativePathFrom, CBackupManagerBackup::CManifestChange_Remove{}, ChangeFrom.m_bIDChanged);
							if (Change.m_bRemoved)
								fSendManifestChange(RelativePath, CBackupManagerBackup::CManifestChange_Remove{}, Change.m_bIDChanged);
							else if (Change.m_bExists)
								fSendManifestChange(RelativePath, CBackupManagerBackup::CManifestChange_Change{fg_Move(Change.m_ManifestFile)}, Change.m_bIDChanged);
						}
						return;
					}
					else
					{
						if (ChangeFrom.m_bAdded)
							fSendManifestChange(RelativePathFrom, CBackupManagerBackup::CManifestChange_Add{fg_Move(ChangeFrom.m_ManifestFile)}, ChangeFrom.m_bIDChanged);
						else if (ChangeFrom.m_bExists)
							fSendManifestChange(RelativePathFrom, CBackupManagerBackup::CManifestChange_Change{fg_Move(ChangeFrom.m_ManifestFile)}, ChangeFrom.m_bIDChanged);
					}
				}
				
				if (Change.m_bAdded)
					fSendManifestChange(RelativePath, CBackupManagerBackup::CManifestChange_Add{fg_Move(Change.m_ManifestFile)}, Change.m_bIDChanged);
				else if (Change.m_bRemoved)
					fSendManifestChange(RelativePath, CBackupManagerBackup::CManifestChange_Remove{}, Change.m_bIDChanged);
				else if (Change.m_bExists)
					fSendManifestChange(RelativePath, CBackupManagerBackup::CManifestChange_Change{fg_Move(Change.m_ManifestFile)}, Change.m_bIDChanged);
			}
		;
	}
}
