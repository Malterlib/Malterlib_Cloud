// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Cloud/BackupManager>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/Actor/Timer>
#include <Mib/Encoding/JSONShortcuts>

#include "Malterlib_Cloud_App_CloudClient.h"

namespace NMib::NCloud::NCloudClient
{
	void CCloudClientAppActor::fp_BackupManager_RegisterCommands(CDistributedAppCommandLineSpecification::CSection _BackupManagerSection)
	{
		auto OptionalBackupHost = "BackupHost?"_=
			{
				"Names"_= {"--host"}
				, "Default"_= ""
				, "Description"_= "Limit backup query to only specified host ID"
			}
		;
		_BackupManagerSection.f_RegisterCommand
			(
				{
					"Names"_= {"--backup-manager-list-sources"}
					, "Description"_= "List backup sources available on remote backup managers"
					, "Options"_=
					{
						OptionalBackupHost
					}
				}
				, [this](CEJSON const &_Params)
				{
					return fp_CommandLine_BackupManager_ListBackupSources(_Params);
				}
			)
		;
		_BackupManagerSection.f_RegisterCommand
			(
				{
					"Names"_= {"--backup-manager-list-backups"}
					, "Description"_= "List backups available on remote backup managers"
					, "Options"_=
					{
						OptionalBackupHost
					}
					, "Parameters"_= 
					{
						"BackupSource?"_=
						{
							"Default"_= ""
							, "Description"_= "The backup source to list backups for.\n"
								"If left empty backups will be listed for all sources you have access to.\n"
						}
					}
				}
				, [this](CEJSON const &_Params)
				{
					return fp_CommandLine_BackupManager_ListBackups(_Params);
				}
			)
		;
		_BackupManagerSection.f_RegisterCommand
			(
				{
					"Names"_= {"--backup-manager-download-backup"}
					, "Description"_= "Download a backup from remote backup manager\n"
						"If a backup already exists the download will be resumed or ammended with the latest changes. Only appended files such as oplogs are supported\n"
					, "Options"_=
					{
						"BackupHost"_=
						{
							"Names"_= {"--host"}
							, "Type"_= ""
							, "Description"_= "The host ID of the host to download the backup from."
						}					
						, "BackupSource"_=
						{
							"Names"_= {"--source"}
							, "Type"_= ""
							, "Description"_= "The backup source to download from."
						}					
					}
					, "Parameters"_= 
					{
						"Backup?"_=
						{
							"Default"_= ""
							, "Description"_= "The backup to download.\n"
								"This is in the format 'Time - BackupID' as displayed in the output from --backup-manager-list-backups.\n"
						}
					}
				}
				, [this](CEJSON const &_Params)
				{
					return fp_CommandLine_BackupManager_DownloadBackup(_Params);
				}
			)
		;
	}
	
	TCContinuation<void> CCloudClientAppActor::fp_BackupManager_SubscribeToBackupServers()
	{
		DMibLogWithCategory(Malterlib/Cloud/CloudClient, Info, "Subscribing to backup managers");
		
		TCContinuation<void> Continuation;
		
		mp_State.m_TrustManager
			(
				&CDistributedActorTrustManager::f_SubscribeTrustedActors<NCloud::CBackupManager>
				, "MalterlibCloudBackupManager"
				, fg_ThisActor(this)
			)
			> [this, Continuation](TCAsyncResult<TCTrustedActorSubscription<CBackupManager>> &&_Subscription)
			{
				if (!_Subscription)
				{
					DMibLogWithCategory(Malterlib/Cloud/CloudClient, Error, "Failed to subscripbe to backup managers: {}", _Subscription.f_GetExceptionStr());
					Continuation.f_SetException(_Subscription);
					return;
				}
				mp_BackupManagers = fg_Move(*_Subscription);
				if (mp_BackupManagers.m_Actors.f_IsEmpty())
				{
					Continuation.f_SetException(DMibErrorInstance("Not connected to any backup managers, or they are not trusted for 'MalterlibCloudBackupManager' namespace"));
					return;
				}
				TCActorResultMap<TCWeakDistributedActor<CBackupManager>, uint32> Versions;
				for (auto &BackupManagerInfo : mp_BackupManagers.m_Actors)
				{
					auto &BackupManager = mp_BackupManagers.m_Actors.fs_GetKey(BackupManagerInfo);
					BackupManager(&CBackupManager::f_GetProtocolVersion, CBackupManager::EProtocolVersion) > Versions.f_AddResult(BackupManager);
				}
				
				Versions.f_GetResults() > [this, Continuation](TCAsyncResult<TCMap<TCWeakDistributedActor<CBackupManager>, TCAsyncResult<uint32>>> &&_Versions)
					{
						if (!_Versions)
						{
							DMibLogWithCategory(Malterlib/Cloud/CloudClient, Error, "Failed to query backup manangers version: {}", _Versions.f_GetExceptionStr());
							Continuation.f_SetException(_Versions);
							return;
						}
						for (auto &Version : *_Versions)
						{
							if (!Version)
							{
								DMibLogWithCategory(Malterlib/Cloud/CloudClient, Error, "Failed to query backup mananger version: {}", Version.f_GetExceptionStr());
								continue;
							}
							mp_BackupManagerProtocolVersion[_Versions->fs_GetKey(Version)] = *Version;
						}
						
						if (mp_BackupManagerProtocolVersion.f_IsEmpty())
						{
							CStr Error = "None of the connected backup managers is reporting a protocol version";
							DMibLogWithCategory(Malterlib/Cloud/CloudClient, Error, Error);
							Continuation.f_SetException(DMibErrorInstance(Error));
							return;
						}
						
						Continuation.f_SetResult();
					}
				;
			}
		;
		return Continuation;
	}

	TCContinuation<CDistributedAppCommandLineResults> CCloudClientAppActor::fp_CommandLine_BackupManager_ListBackupSources(CEJSON const &_Params)
	{
		TCContinuation<CDistributedAppCommandLineResults> Continuation;
		CStr BackupHost = _Params["BackupHost"].f_String();
		
		fg_ThisActor(this)(&CCloudClientAppActor::fp_BackupManager_SubscribeToBackupServers).f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for backup servers") 
			> Continuation / [this, Continuation, BackupHost]
			{
				TCActorResultMap<CHostInfo, CBackupManager::CListBackupSources::CResult> BackupSources;
				
				for (auto &BackupManagerInfo : mp_BackupManagers.m_Actors)
				{
					if (!BackupHost.f_IsEmpty() && BackupManagerInfo.m_HostInfo.m_HostID != BackupHost)
						continue;
					auto &BackupManager = mp_BackupManagers.m_Actors.fs_GetKey(BackupManagerInfo);
					auto *pVersion = mp_BackupManagerProtocolVersion.f_FindEqual(BackupManager);
					if (!pVersion)
						continue;
					CBackupManager::CListBackupSources Command;
					Command.m_Version = *pVersion;
					DMibCallActor
						(
							BackupManager
							, CBackupManager::f_ListBackupSources
							, fg_Move(Command)
						)
						.f_Timeout(mp_Timeout, "Timed out waiting for backup manager to reply")
						> BackupSources.f_AddResult(BackupManagerInfo.m_HostInfo)
					;
				}
				
				BackupSources.f_GetResults() > Continuation / [this, Continuation](TCMap<CHostInfo, TCAsyncResult<CBackupManager::CListBackupSources::CResult>> &&_Results)
					{
						CDistributedAppCommandLineResults CommandLineResults;
						for (auto &Result : _Results)
						{
							auto &HostInfo = _Results.fs_GetKey(Result);
							CommandLineResults.f_AddStdOut(fg_Format("{}\n", HostInfo.f_GetDesc()));
							if (!Result)
							{
								CommandLineResults.f_AddStdErr(fg_Format("    Failed getting backup sources for this host: {}\n", Result.f_GetExceptionStr()));
								continue;
							}
							for (auto &Source : Result->m_BackupSources)
								CommandLineResults.f_AddStdOut(fg_Format("    {}\n", Source));
						}
						Continuation.f_SetResult(fg_Move(CommandLineResults));
					}
				;
			}
		;
		
		return Continuation;
	}
	
	TCContinuation<CDistributedAppCommandLineResults> CCloudClientAppActor::fp_CommandLine_BackupManager_ListBackups(CEJSON const &_Params)
	{
		TCContinuation<CDistributedAppCommandLineResults> Continuation;
		
		CStr BackupHost = _Params["BackupHost"].f_String();
		CStr BackupSource = _Params["BackupSource"].f_String();
		
		fg_ThisActor(this)(&CCloudClientAppActor::fp_BackupManager_SubscribeToBackupServers).f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for backup servers") 
			> Continuation / [this, Continuation, BackupSource, BackupHost]
			{
				TCActorResultMap<CHostInfo, CBackupManager::CListBackups::CResult> Backups;
				
				for (auto &BackupManagerInfo : mp_BackupManagers.m_Actors)
				{
					if (!BackupHost.f_IsEmpty() && BackupManagerInfo.m_HostInfo.m_HostID != BackupHost)
						continue;
					auto &BackupManager = mp_BackupManagers.m_Actors.fs_GetKey(BackupManagerInfo);
					auto *pVersion = mp_BackupManagerProtocolVersion.f_FindEqual(BackupManager);
					if (!pVersion)
						continue;
					CBackupManager::CListBackups Options;
					Options.m_Version = *pVersion;
					Options.m_ForBackupSource = BackupSource;
					DMibCallActor
						(
							BackupManager
							, CBackupManager::f_ListBackups
							, fg_Move(Options)
						)
						.f_Timeout(mp_Timeout, "Timed out waiting for backup manager to reply")
						> Backups.f_AddResult(BackupManagerInfo.m_HostInfo)
					;
				}
				
				Backups.f_GetResults() > Continuation / [this, Continuation](TCMap<CHostInfo, TCAsyncResult<CBackupManager::CListBackups::CResult>> &&_Results)
					{
						CDistributedAppCommandLineResults CommandLineResults;
						for (auto &Result : _Results)
						{
							auto &HostInfo = _Results.fs_GetKey(Result);
							CommandLineResults.f_AddStdOut(fg_Format("{}\n", HostInfo.f_GetDesc()));
							if (!Result)
							{
								CommandLineResults.f_AddStdErr(fg_Format("    Failed getting backups for this host: {}\n", Result.f_GetExceptionStr()));
								continue;
							}
							for (auto &Backups : Result->m_Backups)
							{
								auto &BackupSouce = Result->m_Backups.fs_GetKey(Backups);
								CommandLineResults.f_AddStdOut(fg_Format("    {}\n", BackupSouce));
								for (auto &Backup : Backups)
									CommandLineResults.f_AddStdOut(fg_Format("        {}\n", Backup));
							}
						}
						Continuation.f_SetResult(fg_Move(CommandLineResults));
					}
				;
			}
		;
		return Continuation;
	}
	
	TCContinuation<CDistributedAppCommandLineResults> CCloudClientAppActor::fp_CommandLine_BackupManager_DownloadBackup(CEJSON const &_Params)
	{
		TCContinuation<CDistributedAppCommandLineResults> Continuation;
		
		CStr BackupHost = _Params["BackupHost"].f_String();
		CStr BackupSource = _Params["BackupSource"].f_String();
		CStr Backup = _Params["Backup"].f_String();
		
		if (BackupHost.f_IsEmpty())
			return DMibErrorInstance("Backup host must be specified");
		if (BackupSource.f_IsEmpty())
			return DMibErrorInstance("Backup source must be specified");
		if (Backup.f_IsEmpty())
			return DMibErrorInstance("Backup must be specified");
		
		CStr BackupID;
		CTime BackupTime;
		if (!CBackupManager::fs_IsValidBackup(Backup, &BackupID, &BackupTime))
			return DMibErrorInstance("Backup name format is invalid");
		
		if (!CBackupManager::fs_IsValidBackupSource(BackupSource, nullptr, nullptr))
			return DMibErrorInstance("Backup source name format is invalid");
		
		fg_ThisActor(this)(&CCloudClientAppActor::fp_BackupManager_SubscribeToBackupServers).f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for backup servers") 
			> Continuation / [this, Continuation, BackupSource, BackupHost, BackupID, BackupTime]
			{
				TCActorResultMap<CHostInfo, CBackupManager::CListBackups::CResult> Backups;
				
				uint32 Version = 0;
				TCDistributedActor<CBackupManager> OneBackupManager;
				CTrustedActorInfo ActorInfo;
				for (auto &BackupManagerInfo : mp_BackupManagers.m_Actors)
				{
					if (BackupManagerInfo.m_HostInfo.m_HostID != BackupHost)
						continue;
					auto &BackupManager = mp_BackupManagers.m_Actors.fs_GetKey(BackupManagerInfo);
					auto *pVersion = mp_BackupManagerProtocolVersion.f_FindEqual(BackupManager);
					if (!pVersion)
						continue;
					ActorInfo = BackupManagerInfo;
					OneBackupManager = BackupManager;
					Version = *pVersion;
					break;
				}
			
				if (!OneBackupManager)
				{
					Continuation.f_SetException(DMibErrorInstance("No suitable backup manager found on this host, or connection failed. Use --log-to-stderr to see more info."));
					return;
				}
				
				mp_BackupFileActor = fg_ConstructActor<CSeparateThreadActor>(fg_Construct("Backup download file access"));

				struct CDownloadState
				{
					~CDownloadState()
					{
						if (m_FileCache.f_IsValid())
						{
							fg_Dispatch
								(
									[File = fg_Move(m_FileCache)]() mutable
									{
										File.f_Close();
									}
								)
								> fg_DiscardResult()
							;
						}
					}
					TCActor<CSeparateThreadActor> m_FileActor;
					CStr m_BackupSource;
					CStr m_BackupID;
					CTime m_BackupTime;
					CStr m_FileCacheFileName;
					CFile m_FileCache;
					CStr m_RootDirectory;
				};
				TCSharedPointer<CDownloadState> pState = fg_Construct();
				auto &State = *pState;
				State.m_BackupSource = BackupSource;
				State.m_BackupID = BackupID;
				State.m_BackupTime = BackupTime;
				State.m_FileActor = mp_BackupFileActor;
				
				fg_Dispatch
					(
						mp_BackupFileActor
						, [pState]() -> CBackupManager::CStartDownloadBackup::CManifest
						{
							auto &State = *pState;
							CBackupManager::CStartDownloadBackup::CManifest Manifest;
							State.m_RootDirectory = fg_Format("{}/{}/{tst.} - {}", CFile::fs_GetProgramDirectory(), State.m_BackupSource, State.m_BackupTime, State.m_BackupID);
							
							if (!CFile::fs_FileExists(State.m_RootDirectory, EFileAttrib_Directory))
								return Manifest;
							
							{
								CFile::CFindFilesOptions FindOptions(State.m_RootDirectory + "/*", true);
								FindOptions.m_AttribMask = EFileAttrib_File;
								auto FoundFiles = CFile::fs_FindFiles(FindOptions);
								for (auto &File : FoundFiles)
								{
									CStr RelativePath = File.m_Path.f_Extract(State.m_RootDirectory.f_GetLen() + 1);
									auto &OutFile = Manifest.m_Files[RelativePath];
									OutFile.m_FileSize = CFile::fs_GetFileSize(File.m_Path);
								}
							}
							
							return Manifest;
						}
					)
					> Continuation % "Failed to extract current manifest" 
					/ [this, Version, pState, OneBackupManager, Continuation]
					(CBackupManager::CStartDownloadBackup::CManifest &&_Manifest)
					{
						auto &State = *pState;
						CBackupManager::CStartDownloadBackup StartDownload;
						StartDownload.m_Version = Version;
						StartDownload.m_BackupSource = State.m_BackupSource;
						StartDownload.m_BackupID = State.m_BackupID;
						StartDownload.m_Time = State.m_BackupTime;
						StartDownload.m_Manifest = fg_Move(_Manifest);
						
						TCSharedPointer<TCContinuation<CDistributedAppCommandLineResults>> pAllDone = fg_Construct();
						
						auto fStateChange = [pAllDone](CBackupManager::CDownloadStateChange &&_StateChange) mutable 
							-> NConcurrency::TCContinuation<CBackupManager::CDownloadStateChange::CResult>  
							{
								CBackupManager::CDownloadStateChange::CResult Result = _StateChange.f_GetResult();
								if (pAllDone->f_IsSet())
									return fg_Explicit(Result);
								
								if (_StateChange.m_State == CBackupManager::EDownloadState_Error)
									pAllDone->f_SetException(DMibErrorInstance(_StateChange.m_Message));
								else if (_StateChange.m_State == CBackupManager::EDownloadState_Finished)
								{
									CDistributedAppCommandLineResults Results;
									Results.f_AddStdOut(_StateChange.m_Message + "\n");
									pAllDone->f_SetResult(fg_Move(Results));
								}
								return fg_Explicit(Result);
							}
						;
						
						auto fReceiveData = [this, pState](CBackupManager::CDownloadSendPart &&_DownloadPart) mutable
							-> NConcurrency::TCContinuation<CBackupManager::CDownloadSendPart::CResult>
							{
								NConcurrency::TCContinuation<CBackupManager::CDownloadSendPart::CResult> Continuation;
								fg_Dispatch
									(
										mp_BackupFileActor
										, [pState, DownloadPart = fg_Move(_DownloadPart)]() mutable -> CBackupManager::CDownloadSendPart::CResult
										{
											auto &State = *pState;
											CStr Error;
											if (!CBackupManager::fs_IsValidRelativePath(DownloadPart.m_FilePath, Error))
												DMibError(fg_Format("File path cann {}"));
											
											CStr FilePath = fg_Format("{}/{}", State.m_RootDirectory, DownloadPart.m_FilePath);
											
											if (State.m_FileCacheFileName != FilePath)
											{
												CFile::fs_CreateDirectory(CFile::fs_GetPath(FilePath));
												State.m_FileCache.f_Open(FilePath, EFileOpen_Write | EFileOpen_DontTruncate);
											}
											
											State.m_FileCache.f_SetPosition(DownloadPart.m_FilePosition);
											State.m_FileCache.f_Write(DownloadPart.m_Data.f_GetArray(), DownloadPart.m_Data.f_GetLen());
											
											if (DownloadPart.m_bFinished)
											{
												State.m_FileCache.f_SetLength(DownloadPart.m_FilePosition + DownloadPart.m_Data.f_GetLen());
												State.m_FileCacheFileName.f_Clear();
												State.m_FileCache.f_Close();
											}
											
											CBackupManager::CDownloadSendPart::CResult Result = DownloadPart.f_GetResult();
											return Result;
										}
									)
									> Continuation
								;
								return Continuation;
							}
						;
						
						DMibCallActor
							(
								OneBackupManager
								, CBackupManager::f_StartDownloadBackup
								, fg_Move(StartDownload)
								, fg_ThisActor(this)
								, fg_Move(fReceiveData) 							
								, fg_Move(fStateChange) 
							)
							.f_Timeout(mp_Timeout, "Timed out waiting for backup manager to reply")
							> Continuation % "Failed to start backup on remote server" / [this, pAllDone, Continuation](NConcurrency::CActorSubscription &&_Subscription)
							{
								mp_DownloadBackupSubscription = fg_Move(_Subscription);
								fg_Dispatch
									(
										[pAllDone]
										{
											return *pAllDone;
										}
									)
									> [this, Continuation](TCAsyncResult<CDistributedAppCommandLineResults> &&_Results)
									{
										mp_DownloadBackupSubscription.f_Clear();
										Continuation.f_SetResult(fg_Move(_Results));
									}
								;
							}
						;
						
					}
				;
			}
		;
		return Continuation;
	}
}
