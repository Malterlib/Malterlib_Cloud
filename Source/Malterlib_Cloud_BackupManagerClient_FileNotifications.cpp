// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Function/MergeFunctors>

#include "Malterlib_Cloud_BackupManagerClient.h"
#include "Malterlib_Cloud_BackupManagerClient_Internal.h"
#include "Malterlib_Cloud_BackupManagerClient_BackupInstance.h"

namespace NMib::NCloud
{
	CFileChangeNotificationActor::CCoalesceSettings CBackupManagerClient::CInternal::f_CoalesceSettings()
	{
		return CFileChangeNotificationActor::CCoalesceSettings{5, m_Config.m_ChangeAggregationTime};
	}
	
	TCFuture<void> CBackupManagerClient::CInternal::f_RetrySubscribeChanges()
	{
		if (m_pThis->f_IsDestroyed())
			co_return DMibErrorInstance("Destroyed");

		DMibCloudBackupManagerDebugOut("Retry subscribe\n");

		auto pActive = f_MarkInstancesActive();

		if (m_bRunningRetrySubscribe)
		{
			co_await m_SubscribeChangesPromises.f_Insert().f_Future();
			co_return co_await f_RetrySubscribeChanges();
		}
		
		DMibCheck(!m_bRunningRetrySubscribe);
		m_bRunningRetrySubscribe = true;
		m_bRerunRetrySubscribe = false;
		
		auto Cleanup = g_OnScopeExitActor / [this]
			{
				m_bRunningRetrySubscribe = false;
				
				auto SubscribeChangesPromises = fg_Move(m_SubscribeChangesPromises);
				
				for (auto &Promise : SubscribeChangesPromises)
					Promise.f_SetResult();
			}
		;
		
		struct CPendingInfo
		{
			TCSet<CStr> m_PendingPaths;
			TCSet<CStr> m_MissingPaths;
		};
		
		TCSet<CStr> PendingPaths;
		TCSet<CStr> ToRemovePaths;

		for (auto &WatchedPath : m_WatchedPaths)
		{
			if (WatchedPath.m_bPending)
				PendingPaths[WatchedPath.f_GetPath()];
			else if (WatchedPath.m_bToBeRemoved)
				ToRemovePaths[WatchedPath.f_GetPath()];
		}
		
		CPendingInfo PendingInfo;

		{
			auto BlockingActorCheckout = fg_BlockingActor();

			PendingInfo = co_await
				(
					g_Dispatch(BlockingActorCheckout) / [PendingPaths]
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
				)
			;
		}

		if (m_pThis->f_IsDestroyed())
			co_return DMibErrorInstance("Destroyed");

		TCActorResultMap<CStr, CActorSubscription> SubscribeResults;
		TCActorResultMap<CStr, CActorSubscription> SubscribeResultsMissing;

		for (auto &Path : PendingPaths)
		{
			if (PendingInfo.m_PendingPaths.f_FindEqual(Path))
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
					, g_ActorFunctor / [this, Path](TCVector<CFileChangeNotification::CNotification> const &_Notifications) -> TCFuture<void>
					{
						for (auto Notification : _Notifications)
						{
							Notification.m_Path = CFile::fs_AppendPath(Path, Notification.m_Path);

							if (!Notification.m_PathFrom.f_IsEmpty())
								Notification.m_PathFrom = CFile::fs_AppendPath(Path, Notification.m_PathFrom);

#ifdef DMibCloudBackupManagerDebug
							switch (Notification.m_Notification)
							{
							case EFileChangeNotification_Undefined: DMibCloudBackupManagerDebugOut("&&& Undefined {}\n", Notification.m_Path); break;
							case EFileChangeNotification_Unknown: DMibCloudBackupManagerDebugOut("&&& Unknown {}\n", Notification.m_Path); break;
							case EFileChangeNotification_Added: DMibCloudBackupManagerDebugOut("&&& Added {}\n", Notification.m_Path); break;
							case EFileChangeNotification_Removed: DMibCloudBackupManagerDebugOut("&&& Removed {}\n", Notification.m_Path); break;
							case EFileChangeNotification_Modified: DMibCloudBackupManagerDebugOut("&&& Modified {}\n", Notification.m_Path); break;
							case EFileChangeNotification_Renamed: DMibCloudBackupManagerDebugOut("&&& Renamed {} -> {}\n", Notification.m_PathFrom, Notification.m_Path); break;
							}
#endif
							f_OnFileChanged(Notification, false);
						}

						co_return {};
					}
					, m_pThis->mp_pInternal->f_CoalesceSettings()
				)
				> SubscribeResults.f_AddResult(Path)
			;
		}

		for (auto &MissingPath : PendingInfo.m_MissingPaths)
		{
			auto Mapped = m_WatchedPathsMissing(MissingPath);
			if (!Mapped.f_WasCreated())
				continue;
			m_FileChangeNotificationsActor
				(
					&CFileChangeNotificationActor::f_RegisterForChanges
					, MissingPath
					, EFileChange_FileName | EFileChange_DirectoryName
					, g_ActorFunctor / [this](TCVector<CFileChangeNotification::CNotification> const &_Notifications) -> TCFuture<void>
					{
						if (!m_bRerunRetrySubscribe)
						{
							m_bRerunRetrySubscribe = true;
							f_RetrySubscribeChanges() > fg_DiscardResult();
						}

						co_return {};
					}
					, m_pThis->mp_pInternal->f_CoalesceSettings()
				)
				> SubscribeResultsMissing.f_AddResult(MissingPath)
			;
		}

		TCSet<CStr> MissingPathsToRemove;
		for (auto &WatchedPath : m_WatchedPathsMissing)
		{
			auto &Path = m_WatchedPathsMissing.fs_GetKey(WatchedPath);
			if (!PendingInfo.m_MissingPaths.f_FindEqual(Path))
				MissingPathsToRemove[Path];
		}

		for (auto &ToRemove : MissingPathsToRemove)
			m_WatchedPathsMissing.f_Remove(ToRemove);

		auto [Results, ResultsMissing] = co_await (SubscribeResults.f_GetResults() + SubscribeResultsMissing.f_GetResults());

		bool bChanged = false;
		for (auto &Result : Results)
		{
			auto &Path = Results.fs_GetKey(Result);
			if (!Result)
			{
				DMibLogCategoryStr(m_Config.m_LogCategory);
				DMibLog(Error, "One file change notification '{}' failed to register: {}", Path, Result.f_GetExceptionStr());
				continue;
			}
			auto &WatchedPath = m_WatchedPaths[Path];
			if (WatchedPath.m_Subscription)
			{
				auto Subscription = fg_Move(WatchedPath.m_Subscription);
				Subscription->f_Destroy() > [](TCAsyncResult<void> &&_Result)
					{
#ifdef DMibCloudBackupManagerDebug
						if (!_Result)
							DMibCloudBackupManagerDebugOut("FAILED to destroy watch subscription {}\n", _Result.f_GetExceptionStr());
#endif
					}
				;
			}
			WatchedPath.m_Subscription = fg_Move(*Result);
			WatchedPath.m_bPending = false;
			if (m_bInitialSubscribeDone)
				f_NewPathWatched(Path);
			bChanged = true;
		}
		for (auto &Result : ResultsMissing)
		{
			auto &Path = ResultsMissing.fs_GetKey(Result);
			if (!Result)
			{
				DMibLogCategoryStr(m_Config.m_LogCategory);
				DMibLog(Error, "One file change notification for missing '{}' failed to register: {}", Path, Result.f_GetExceptionStr());
				continue;
			}
			m_WatchedPathsMissing[Path].m_Subscription = fg_Move(*Result);
			bChanged = true;
		}
		for (auto &Path : ToRemovePaths)
		{
			auto *pWatchedPath = m_WatchedPaths.f_FindEqual(Path);
			if (!pWatchedPath || !pWatchedPath->m_bToBeRemoved)
				continue;
			m_WatchedPaths.f_Remove(Path);
		}

		if (bChanged)
			f_RetrySubscribeChanges() > fg_DiscardResult();

		co_return {};
	}

	void CBackupManagerClient::CInternal::f_NewPathWatched(CStr const &_Path)
	{
		DMibCloudBackupManagerDebugOut("New path watched: {}\n", _Path);
		auto BlockingActorCheckout = fg_BlockingActor();
		auto BlockingActor = BlockingActorCheckout.f_Actor();

		g_Dispatch(BlockingActor) / [_Path]
			{
				TCVector<CFileChangeNotification::CNotification> Notifications;
				for (auto &File : CFile::fs_FindFilesEx(_Path / "*", EFileAttrib_File | EFileAttrib_Directory, true, false))
				{
					auto &Notification = Notifications.f_Insert();
					Notification.m_Path = File.m_Path;
					Notification.m_Notification = EFileChangeNotification_Added;
				}

				return Notifications;
			}
			> [this, _Path, BlockingActorCheckout = fg_Move(BlockingActorCheckout)](TCAsyncResult<TCVector<CFileChangeNotification::CNotification>> &&_Notifications)
			{
				if (!_Notifications)
				{
					DMibLogCategoryStr(m_Config.m_LogCategory);
					DMibLog(Error, "Error processing newly watched path '{}': {}", _Path, _Notifications.f_GetExceptionStr());
					return;
				}

				for (auto &Notification : *_Notifications)
					f_OnFileChanged(Notification, false);
			}
		;
	}

	TCFuture<void> CBackupManagerClient::CInternal::f_SubscribeChanges()
	{
		TCPromise<void> Promise;

		if (m_pThis->f_IsDestroyed())
			return Promise <<= DMibErrorInstance("Destroyed");
		
		auto &ManifestConfig = m_Config.m_ManifestConfig;

		struct CWatchedPathSettings
		{
			CStr const &f_GetPath() const
			{
				return TCMap<CStr, CWatchedPathSettings>::fs_GetKey(*this);
			}

			bool m_bRecursive = false;
		};
		
		TCMap<CStr, CWatchedPathSettings> Paths;

		auto fRemoveChildren = [&](CStr const &_Path)
			{
				TCSet<CStr> ToRemove;
				for (auto &OldPath : Paths)
				{
					auto &Directory = OldPath.f_GetPath();
					if (Directory != _Path && Directory.f_StartsWith(Directory))
						ToRemove[Directory];
				}
				for (auto &Remove : ToRemove)
					Paths.f_Remove(Remove);
				return !ToRemove.f_IsEmpty();
			}
		;

		// Find most efficient common set of watch paths that include the watched directories, supporting rename operations
		for (auto &Destination : ManifestConfig.m_IncludeWildcards)
		{
			auto &Wildcard = ManifestConfig.m_IncludeWildcards.fs_GetKey(Destination);
			
			bool bRecursive = false;
			auto WildcardParsed = CDirectoryManifestConfig::fs_ParseWildcard(Wildcard, bRecursive);

			CStr Directory = CFile::fs_GetPath(CFile::fs_AppendPath(ManifestConfig.m_Root, WildcardParsed));

			if (auto *pPath = Paths.f_FindEqual(Directory))
			{
				auto &Path = *pPath;
				if (bRecursive && !Path.m_bRecursive)
				{
					fRemoveChildren(Directory);
					Path.m_bRecursive = true;
				}
				continue;
			}

			bool bFoundParent = false;
			for (CStr ParentDirectorySearch = CFile::fs_GetPath(Directory); true; ParentDirectorySearch = CFile::fs_GetPath(ParentDirectorySearch))
			{
				auto *pPath = Paths.f_FindEqual(ParentDirectorySearch);
				if (!pPath || !pPath->m_bRecursive)
				{
					if (ParentDirectorySearch.f_IsEmpty())
						break;
					continue;
				}

				bFoundParent = true;
				break;
			}
			if (bFoundParent)
				continue;

			CWatchedPathSettings NewPath;
			NewPath.m_bRecursive = bRecursive;

			if (bRecursive)
				fRemoveChildren(Directory);
			Paths[Directory] = fg_Move(NewPath);
		}

		for (auto &WatchedPath : m_WatchedPaths)
			WatchedPath.m_bToBeRemoved = true;

		for (auto &WildcardSettings : Paths)
		{
			auto &Path = WildcardSettings.f_GetPath();

			auto Mapping = m_WatchedPaths(Path);
			auto &WatchedPath = *Mapping;
			if (Mapping.f_WasCreated())
			{
				WatchedPath.m_bPending = true;
				WatchedPath.m_bRecursive = WildcardSettings.m_bRecursive;
			}
			else
			{
				if (WildcardSettings.m_bRecursive && !WatchedPath.m_bRecursive)
				{
					WatchedPath.m_bRecursive = true;
					WatchedPath.m_bPending = true;
				}
				WatchedPath.m_bToBeRemoved = false;
			}
		}

		for (auto &WatchedPath : m_WatchedPaths)
		{
			if (WatchedPath.m_bToBeRemoved)
				WatchedPath.m_bPending = false;
		}

		return Promise <<= f_RetrySubscribeChanges();
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
				if (NotificationPath != WildcardPath && !WildcardPath.f_IsEmpty() && !NotificationPath.f_StartsWith(WildcardPath + "/"))
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
		}
		
		if (!bIsIncluded)
			return false;
		
		if (fg_StrMatchesAnyWildcardInMap(_Path, ManifestConfig.m_ExcludeWildcards))
			return false;
		
		return true;
	}

	void CBackupManagerClient::fp_HashMismatch(NStr::CStr const &_File)
	{
		// We inject a change notification with dirty forced to force new hash to be calculated

		auto &Internal = *mp_pInternal;
		auto &ManifestConfig = Internal.m_Config.m_ManifestConfig;

		CFileChangeNotification::CNotification Notification;
		Notification.m_Path = ManifestConfig.m_Root / _File;
		Notification.m_Notification = EFileChangeNotification_Modified;
		Internal.f_OnFileChanged(Notification, true);
	}

	COnScopeExitShared CBackupManagerClient::CInternal::f_MarkInstancesActive()
	{
		for (auto &RunningInstance : m_RunningBackupInstances)
		{
			if (RunningInstance.m_bSentActive)
				continue;
			RunningInstance.m_bSentActive = true;
			RunningInstance.m_Instance(&NPrivate::CBackupManagerClient_Instance::f_MarkActive, true) > fg_DiscardResult();
		}

		++m_nActive;
		return g_OnScopeExitActor / [this]
			{
				--m_nActive;
				if (m_nActive)
					return;
				for (auto &RunningInstance : m_RunningBackupInstances)
				{
					if (!RunningInstance.m_bSentActive)
						continue;
					RunningInstance.m_bSentActive = false;
					RunningInstance.m_Instance(&NPrivate::CBackupManagerClient_Instance::f_MarkActive, false) > fg_DiscardResult();
				}
			}
		;
	}

	void CBackupManagerClient::CInternal::f_OnFileChanged(CFileChangeNotification::CNotification const &_Notification, bool _bDirty)
	{
		CFileChangeNotification::CNotification Notification = _Notification;

#ifdef DMibCloudBackupManagerDebug
		switch (Notification.m_Notification)
		{
		case EFileChangeNotification_Undefined: DMibCloudBackupManagerDebugOut("%%% Undefined {} {}\n", Notification.m_Path, _bDirty); break;
		case EFileChangeNotification_Unknown: DMibCloudBackupManagerDebugOut("%%% Unknown {} {}\n", Notification.m_Path, _bDirty); break;
		case EFileChangeNotification_Added: DMibCloudBackupManagerDebugOut("%%% Added {} {}\n", Notification.m_Path, _bDirty); break;
		case EFileChangeNotification_Removed: DMibCloudBackupManagerDebugOut("%%% Removed {} {}\n", Notification.m_Path, _bDirty); break;
		case EFileChangeNotification_Modified: DMibCloudBackupManagerDebugOut("%%% Modified {} {}\n", Notification.m_Path, _bDirty); break;
		case EFileChangeNotification_Renamed: DMibCloudBackupManagerDebugOut("%%% Renamed {} -> {} {}\n", Notification.m_PathFrom, Notification.m_Path, _bDirty); break;
		}
#endif
		
		auto &ManifestConfig = m_Config.m_ManifestConfig;
		
		CStr RelativePath;
		CStr RelativePathFrom;
		
		CStr OriginalPath = CFile::fs_MakePathRelative(Notification.m_Path, ManifestConfig.m_Root);
		CStr OriginalPathFrom;
		
		bool bDirtyHint = _bDirty;

		if (Notification.m_Notification == EFileChangeNotification_Removed)
		{
			TCSet<CStr> Watched;
			for (auto &Path : m_WatchedPaths)
				Watched[Path.f_GetPath()];

			auto pWatchedDeleted = m_WatchedPaths.f_FindEqual(Notification.m_Path);
			if (pWatchedDeleted)
			{
				DMibCloudBackupManagerDebugOut("%%% RESUBSCRIBE {}\n", Notification.m_Path);
				pWatchedDeleted->m_bPending = true;
				f_RetrySubscribeChanges() > fg_DiscardResult();
			}
		}

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

		if (RelativePath.f_IsEmpty())
			return; // We don't support root path

		mint Sequence = m_FileNotificationSequence++;

		auto pActive = f_MarkInstancesActive();

		fg_CallSafe(this, &CInternal::f_UpdateManifest, RelativePath, OriginalPath, bDirtyHint)
			+ fg_CallSafe(this, &CInternal::f_UpdateManifest, RelativePathFrom, OriginalPathFrom, bDirtyHint)
			> [=, this](TCAsyncResult<CUpdateManifestResult> &&_Change, TCAsyncResult<CUpdateManifestResult> &&_ChangeFrom)
			{
				(void)pActive;
				DMibCheck(Sequence > m_LastSeenNotificationSequence);
				m_LastSeenNotificationSequence = Sequence;

				if (!_Change)
				{
					DMibLogCategoryStr(m_Config.m_LogCategory);
					DMibLog(Error, "Failed to update manifest ({}): {}", RelativePath, _Change.f_GetExceptionStr());
					return;
				}
				
				if (!_ChangeFrom)
				{
					DMibLogCategoryStr(m_Config.m_LogCategory);
					DMibLog(Error, "Failed to update manifest ({}): {}", RelativePath, _ChangeFrom.f_GetExceptionStr());
					return;
				}
				
				auto fSendManifestChange = [&]
					(
						CStr const &_Path
						, CBackupManagerBackup::CManifestChange const &_ManifestChange
						, bool _bDirty
						, CBackupManagerClient_ChecksumState const &_ChecksumState
					)
					{
#if defined DMibContractConfigure_CheckEnabled
						CStr ManifestError;
						DMibCheck(CBackupManagerBackup::fs_ManifestChangeValid(_Path, _ManifestChange, ManifestError));
#endif
						for (auto &RunningInstance : m_RunningBackupInstances)
							RunningInstance.m_Instance(&NPrivate::CBackupManagerClient_Instance::f_ManifestChanged, _Path, _ManifestChange, _bDirty, _ChecksumState) > fg_DiscardResult();
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
						fSendManifestChange(Path, CBackupManagerBackup::CManifestChange_Add{fg_Move(UpdatedDirectory.m_ManifestFile)}, false, {});
					else
						fSendManifestChange(Path, CBackupManagerBackup::CManifestChange_Change{fg_Move(UpdatedDirectory.m_ManifestFile)}, false, {});
				}

#ifdef DMibCloudBackupManagerDebug
				CStr NotificationStr;
				switch (Notification.m_Notification)
				{
				case EFileChangeNotification_Undefined: NotificationStr = "Undefined"; break;
				case EFileChangeNotification_Unknown: NotificationStr = "Unknown"; break;
				case EFileChangeNotification_Added: NotificationStr = "Added"; break;
				case EFileChangeNotification_Removed: NotificationStr = "Removed"; break;
				case EFileChangeNotification_Modified: NotificationStr = "Modified"; break;
				case EFileChangeNotification_Renamed: NotificationStr = "Renamed"; break;
				}
#endif
				if (Notification.m_Notification == EFileChangeNotification_Renamed)
				{
					if (ChangeFrom.m_bRemoved)
					{
						if (Change.m_bAdded)
						{
							DMibCloudBackupManagerDebugOut("### {},{} {}.m_bRemoved {}.m_bAdded\n", Sequence, NotificationStr, RelativePathFrom, RelativePath);
							fSendManifestChange
								(
									RelativePath, CBackupManagerBackup::CManifestChange_Rename{fg_Move(Change.m_ManifestFile), RelativePathFrom}
									, Change.m_bIDChanged
									, Change.m_ChecksumState
								)
							;
							return;
						}
						DMibCloudBackupManagerDebugOut("### {},{} {}.m_bRemoved\n", Sequence, NotificationStr, RelativePathFrom);
						fSendManifestChange(RelativePathFrom, CBackupManagerBackup::CManifestChange_Remove{}, ChangeFrom.m_bIDChanged, ChangeFrom.m_ChecksumState);
					}
					else if (ChangeFrom.m_bAdded)
					{
						DMibCloudBackupManagerDebugOut("### {},{} {}.m_bAdded\n", Sequence, NotificationStr, RelativePathFrom);
						fSendManifestChange
							(
								RelativePathFrom
								, CBackupManagerBackup::CManifestChange_Add{fg_Move(ChangeFrom.m_ManifestFile)}
								, ChangeFrom.m_bIDChanged, ChangeFrom.m_ChecksumState
							)
						;
					}
					else if (ChangeFrom.m_bExists)
					{
						DMibCloudBackupManagerDebugOut("### {},{} {}.m_bExists\n", Sequence, NotificationStr, RelativePathFrom);
						fSendManifestChange
							(
								RelativePathFrom
								, CBackupManagerBackup::CManifestChange_Change{fg_Move(ChangeFrom.m_ManifestFile)}
								, ChangeFrom.m_bIDChanged
								, ChangeFrom.m_ChecksumState
							)
						;
					}
					else
						DMibCloudBackupManagerDebugOut("### {},{} NOCHANGE {}\n", Sequence, NotificationStr, RelativePath);
				}
				
				if (Change.m_bAdded)
				{
					DMibCloudBackupManagerDebugOut("##> {},{} {}.m_bAdded\n", Sequence, NotificationStr, RelativePath);
					fSendManifestChange(RelativePath, CBackupManagerBackup::CManifestChange_Add{fg_Move(Change.m_ManifestFile)}, Change.m_bIDChanged, Change.m_ChecksumState);
				}
				else if (Change.m_bRemoved)
				{
					DMibCloudBackupManagerDebugOut("##> {},{} {}.m_bRemoved\n", Sequence, NotificationStr, RelativePath);
					fSendManifestChange(RelativePath, CBackupManagerBackup::CManifestChange_Remove{}, Change.m_bIDChanged, Change.m_ChecksumState);
				}
				else if (Change.m_bExists)
				{
					DMibCloudBackupManagerDebugOut("##> {},{} {}.m_bExists\n", Sequence, NotificationStr, RelativePath);
					fSendManifestChange(RelativePath, CBackupManagerBackup::CManifestChange_Change{fg_Move(Change.m_ManifestFile)}, Change.m_bIDChanged, Change.m_ChecksumState);
				}
				else
					DMibCloudBackupManagerDebugOut("##> {},{} NOCHANGE {}\n", Sequence, NotificationStr, RelativePath);
			}
		;
	}
}
