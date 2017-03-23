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
		if (m_bRunningRetrySubscribe)
		{
			m_bRerunRetrySubscribe = true;
			return fg_Explicit();
		}
		
		m_bRunningRetrySubscribe = true;
		
		auto Cleanup = g_OnScopeExitActor > [this]
			{
				m_bRunningRetrySubscribe = false;
				if (m_bRerunRetrySubscribe)
				{
					m_bRerunRetrySubscribe = false;
					f_RetrySubscribeChanges();
				}
			}
		;

		TCContinuation<void> Continuation;
		
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
								f_RetrySubscribeChanges();
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
		
		TCMap<CStr, zbool> Paths; 
		for (auto &Wildcard : m_Config.m_IncludeWildcards)
		{
			bool bRecursive = false;
			auto WildcardParsed = fs_ParseWildcard(Wildcard, bRecursive);
			
			auto &bRecursiveForPath = Paths[CFile::fs_GetPath(CFile::fs_AppendPath(m_Config.m_Root, WildcardParsed))];
			if (bRecursive)
				bRecursiveForPath = bRecursive; 
		}

		for (auto &bRecursive : Paths)
		{
			auto &Path = Paths.fs_GetKey(bRecursive);
			
			auto &WatchedPath = m_WatchedPaths[Path];
			WatchedPath.m_bPending = true;
			WatchedPath.m_bRecursive = bRecursive;
		}
		
		return f_RetrySubscribeChanges();
	}
	
	void CBackupManagerClient::CInternal::f_OnFileChanged(CFileChangeNotification::CNotification const &_Notification)
	{
		bool bIsIncluded = false;
		
		CStr RelativePath = CFile::fs_MakePathRelative(_Notification.m_Path, m_Config.m_Root);
		CStr NotificationPath = CFile::fs_GetPath(RelativePath);
		CStr NotificationFile = CFile::fs_GetFile(RelativePath);
		
		for (auto &Wildcard : m_Config.m_IncludeWildcards)
		{
			bool bRecursive = false;
			auto WildcardParsed = fs_ParseWildcard(Wildcard, bRecursive);
			
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
			
			bIsIncluded = true;
			break;
		}
		
		if (!bIsIncluded)
			return;
		
		if (fs_MatchesAnyWildcard(RelativePath, m_Config.m_ExcludeWildcards))
			return;
		
		f_UpdateManifest(RelativePath) > [this, _Notification, RelativePath](TCAsyncResult<CUpdateManifestResult> &&_Updated)
			{
				if (!_Updated)
				{
					DMibLogCategoryStr(m_Config.m_LogCategory);
					DMibLog(Error, "Failed to update manifest: {}", _Updated.f_GetExceptionStr());
					return;
				}
				
				for (auto &RunningInstance : m_RunningBackupInstances)
					RunningInstance(&NPrivate::CBackupManagerClient_Instance::f_ManifestChanged, RelativePath, *_Updated) > fg_DiscardResult();
			}
		;
	}
}
