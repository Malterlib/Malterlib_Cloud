// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Concurrency/Actor/Timer>
#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud::NAppManager
{
	CAppManagerActor::CAppManagerActor()
		: CDistributedAppActor(CDistributedAppActor_Settings("AppManager").f_AuditCategory("Malterlib/Cloud/AppManager"))
	{
	}
	
	void CAppManagerActor::fp_OnApplicationAdded(TCSharedPointer<CApplication> const &_pApplication)
	{
		CRemoteApplicationKey RemoteKey{_pApplication->m_Settings};
	
		if (mp_KnownRemoteApplications[RemoteKey](mp_State.m_HostID).f_WasCreated())
			fp_NewRemoteKnownApplication(RemoteKey, mp_State.m_HostID);
		
		fp_SendAppToRemoteAppManagers(_pApplication);
		
		if (_pApplication->f_IsChildApp())
			return; // Parent cannot have parents
		auto &NewApplication = *_pApplication;
		for (auto &pApplication : mp_Applications)
		{
			auto &Application = *pApplication;
			if (Application.m_Settings.m_ParentApplication != NewApplication.m_Name)
				continue;
			Application.m_pParentApplication = &NewApplication;
			NewApplication.m_Children.f_Insert(Application);
		}
	}
	
	void CAppManagerActor::fp_InitApplications()
	{
		for (auto &pApplication : mp_Applications)
		{
			auto &Application = *pApplication;
			if (!Application.m_LastInstalledVersion.m_Platform.f_IsEmpty())
				mp_KnownPlatforms[Application.m_LastInstalledVersion.m_Platform];
			if (Application.f_IsChildApp())
			{
				auto *pParentApplication = mp_Applications.f_FindEqual(Application.m_Settings.m_ParentApplication);
				if (pParentApplication && !(*pParentApplication)->f_IsChildApp())
				{
					Application.m_pParentApplication = &**pParentApplication;
					Application.m_pParentApplication->m_Children.f_Insert(Application);
				}
			}
		}
	}
	
	void CAppManagerActor::fp_InitialStartupFailed(CExceptionPointer const &_pException)
	{
		if (mp_InitialStartupResult.f_IsSet())
			return;
		
		mp_InitialStartupResult.f_SetException(_pException);
	}

	TCFuture<void> CAppManagerActor::fp_ReadState()
	{
		bool bChangedDatabase = false;
		try
		{
			CStr PendingSelfUpdateName; 
			CVersionManager::CVersionIDAndPlatform PendingSelfUpdateVersionID;
			CTime PendingSelfUpdateTime;
			uint32 PendingSelfUpdateSequence = 0;

			if (auto *pUserNameTransform = mp_State.m_ConfigDatabase.m_Data.f_GetMember("UserGroupNameTransform"))
				mp_pUniqueUserGroup->m_UserGroupNameTransform = pUserNameTransform->f_String();

			if (auto *pPendingSelfUpdateProcess = mp_State.m_StateDatabase.m_Data.f_GetMember("PendingSelfUpdateProcess"))
			{
				auto &Pending = *pPendingSelfUpdateProcess;
				
				PendingSelfUpdateName = Pending["Name"].f_String(); 
				PendingSelfUpdateVersionID = CVersionManager::CVersionIDAndPlatform::fs_FromJSON(Pending["VersionID"]);
				PendingSelfUpdateTime = Pending["VersionTime"].f_Date();
				PendingSelfUpdateSequence = Pending["VersionRetrySequence"].f_Integer();

				mp_State.m_StateDatabase.m_Data.f_RemoveMember("PendingSelfUpdateProcess");
				bChangedDatabase = true;
			}
			
			if (auto pApplication = mp_State.m_StateDatabase.m_Data.f_GetMember("Applications"))
			{
				for (auto &ApplicationEntry : pApplication->f_Object())
				{
					CStr const &Name = ApplicationEntry.f_Name();
					auto &ApplicationJSON = ApplicationEntry.f_Value(); 
					
					auto &Application = *(mp_Applications[Name] = fg_Construct(Name, this));
					
					auto &Settings = Application.m_Settings; 
					
					Settings.m_Executable = ApplicationJSON["Executable"].f_String(); 
					Settings.m_RunAsUser = ApplicationJSON["RunAsUser"].f_String(); 
#ifdef DPlatformFamily_Windows
					if (auto pValue = ApplicationJSON.f_GetMember("RunAsUserPassword", EJSONType_String))
						Settings.m_RunAsUserPassword = pValue->f_String();
#endif
					Settings.m_RunAsGroup = ApplicationJSON["RunAsGroup"].f_String();
					
					if (auto pValue = ApplicationJSON.f_GetMember("RunAsUserHasShell", EJSONType_Boolean))
						Settings.m_bRunAsUserHasShell = pValue->f_Boolean();

					if (auto *pValue = ApplicationJSON.f_GetMember("Backup"))
					{
						auto &BackupJSON = *pValue;
						
						if (BackupJSON["IncludeWildcards"].f_IsArray())
						{
							for (auto &Wildcard : BackupJSON["IncludeWildcards"].f_Array())
								Settings.m_Backup_IncludeWildcards[Wildcard.f_String()];
						}
						else
						{
							for (auto &Wildcard : BackupJSON["IncludeWildcards"].f_Object())
							{
								auto &Destination = Settings.m_Backup_IncludeWildcards[Wildcard.f_Name()];
								if (Wildcard.f_Value().f_IsNull())
									Destination = CDirectoryManifestConfig::CDestination{};
								else
									Destination = Wildcard.f_Value().f_String();
							}
						}
						for (auto &Wildcard : BackupJSON["ExcludeWildcards"].f_Array())
							Settings.m_Backup_ExcludeWildcards[Wildcard.f_String()];
						for (auto &Wildcard : BackupJSON["AddSyncFlagsWildcards"].f_Object())
							Settings.m_Backup_AddSyncFlagsWildcards[Wildcard.f_Name()] = CDirectoryManifestFile::fs_ParseSyncFlags(Wildcard.f_Value());
						for (auto &Wildcard : BackupJSON["RemoveSyncFlagsWildcards"].f_Object())
							Settings.m_Backup_RemoveSyncFlagsWildcards[Wildcard.f_Name()] = CDirectoryManifestFile::fs_ParseSyncFlags(Wildcard.f_Value());
						
						Settings.m_Backup_NewBackupInterval = CTimeSpanConvert::fs_CreateSpanFromHours(BackupJSON["NewBackupIntervalHours"].f_Float());
						Settings.m_bBackupEnabled = BackupJSON["Enabled"].f_Boolean();
					}
					
					if (auto pValue = ApplicationJSON.f_GetMember("DistributedApp", EJSONType_Boolean))
						Settings.m_bDistributedApp = pValue->f_Boolean();
					for (auto &Parameter : ApplicationJSON["Parameters"].f_Array())
						Settings.m_ExecutableParameters.f_Insert(Parameter.f_String());
					for (auto &File : ApplicationJSON["Files"].f_Array())
						Application.m_Files.f_Insert(File.f_String());
					Settings.m_EncryptionStorage = ApplicationJSON["EncryptionStorage"].f_String();
					if (auto pValue = ApplicationJSON.f_GetMember("EncryptionFileSystem", EJSONType_String))
						Settings.m_EncryptionFileSystem = pValue->f_String();
					else if (!Settings.m_EncryptionStorage.f_IsEmpty())
						Settings.m_EncryptionFileSystem = "zfs";
					if (auto pValue = ApplicationJSON.f_GetMember("ParentApplication", EJSONType_String))
						Settings.m_ParentApplication = pValue->f_String();
					if (auto pValue = ApplicationJSON.f_GetMember("VersionManagerApplication", EJSONType_String))
						Settings.m_VersionManagerApplication = pValue->f_String();
					if (auto pValue = ApplicationJSON.f_GetMember("LastInstalledVersion", EJSONType_Object))
						Application.m_LastInstalledVersion = CVersionManager::CVersionIDAndPlatform::fs_FromJSON(*pValue);
					if (auto pValue = ApplicationJSON.f_GetMember("LastInstalledVersionInfo", EJSONType_Object))
						Application.m_LastInstalledVersionInfo = CVersionManager::CVersionInformation::fs_FromJSON(*pValue);
					
					if (auto pValue = ApplicationJSON.f_GetMember("LastInstalledVersionFinished", EJSONType_Object))
						Application.m_LastInstalledVersionFinished = CVersionManager::CVersionIDAndPlatform::fs_FromJSON(*pValue);
					else
						Application.m_LastInstalledVersionFinished = Application.m_LastInstalledVersion;
					
					if (auto pValue = ApplicationJSON.f_GetMember("LastInstalledVersionInfoFinished", EJSONType_Object))
						Application.m_LastInstalledVersionInfoFinished = CVersionManager::CVersionInformation::fs_FromJSON(*pValue);
					else
						Application.m_LastInstalledVersionInfoFinished = Application.m_LastInstalledVersionInfo; 
						
					if (auto pValue = ApplicationJSON.f_GetMember("LastTriedInstalledVersion", EJSONType_Object))
						Application.m_LastTriedInstalledVersion = CVersionManager::CVersionIDAndPlatform::fs_FromJSON(*pValue);
					if (auto pValue = ApplicationJSON.f_GetMember("LastTriedInstalledVersionInfo", EJSONType_Object))
						Application.m_LastTriedInstalledVersionInfo = CVersionManager::CVersionInformation::fs_FromJSON(*pValue);
					
					if 
						(
							Application.m_LastTriedInstalledVersion != Application.m_LastInstalledVersionFinished 
							|| Application.m_LastTriedInstalledVersionInfo.m_Time != Application.m_LastInstalledVersionInfoFinished.m_Time
							|| Application.m_LastTriedInstalledVersionInfo.m_RetrySequence != Application.m_LastInstalledVersionInfoFinished.m_RetrySequence
						)
					{
						if 
							(
								Name != PendingSelfUpdateName 
								|| Application.m_LastTriedInstalledVersion != PendingSelfUpdateVersionID 
								|| Application.m_LastTriedInstalledVersionInfo.m_Time != PendingSelfUpdateTime 
								|| Application.m_LastTriedInstalledVersionInfo.m_RetrySequence != PendingSelfUpdateSequence
							)
						{
							Application.m_LastFailedInstalledVersion = Application.m_LastTriedInstalledVersion; 
							Application.m_LastFailedInstalledVersionTime = Application.m_LastTriedInstalledVersionInfo.m_Time;
							Application.m_LastFailedInstalledVersionRetrySequence = Application.m_LastTriedInstalledVersionInfo.m_RetrySequence;
						}
					}
						
					if (auto pValue = ApplicationJSON.f_GetMember("AutoUpdate", EJSONType_Boolean))
						Settings.m_bAutoUpdate = pValue->f_Boolean();
					if (auto pValue = ApplicationJSON.f_GetMember("AutoUpdateTags", EJSONType_Array))
					{
						for (auto &Tag : pValue->f_Array())
							Settings.m_AutoUpdateTags[Tag.f_String()];
					}
					if (auto pValue = ApplicationJSON.f_GetMember("AutoUpdateBranches", EJSONType_Array))
					{
						for (auto &Tag : pValue->f_Array())
							Settings.m_AutoUpdateBranches[Tag.f_String()];
					}
					if (auto pValue = ApplicationJSON.f_GetMember("UpdateScripts", EJSONType_Object))
					{
						Settings.m_UpdateScripts.m_PreUpdate = (*pValue)["PreUpdate"].f_String();
						Settings.m_UpdateScripts.m_PostUpdate = (*pValue)["PostUpdate"].f_String();
						Settings.m_UpdateScripts.m_PostLaunch = (*pValue)["PostLaunch"].f_String();
						Settings.m_UpdateScripts.m_OnError = (*pValue)["OnError"].f_String();
					}
					if (auto pValue = ApplicationJSON.f_GetMember("SelfUpdateSource", EJSONType_Boolean))
						Settings.m_bSelfUpdateSource = pValue->f_Boolean();
					if (auto pValue = ApplicationJSON.f_GetMember("UpdateGroup", EJSONType_String))
						Settings.m_UpdateGroup = pValue->f_String();
					if (auto pRegisterInfo = ApplicationJSON.f_GetMember("RegisterInfo", EJSONType_Object))
					{
						if (auto pValue = pRegisterInfo->f_GetMember("UpdateType", EJSONType_Integer))
							Application.m_RegisterInfo.m_UpdateType = (EDistributedAppUpdateType)pValue->f_Integer();
						if (auto pValue = pRegisterInfo->f_GetMember("ResourcesFiles", EJSONType_Integer))
							Application.m_RegisterInfo.m_Resources_Files = pValue->f_Integer();
						if (auto pValue = pRegisterInfo->f_GetMember("ResourcesFilesPerProcess", EJSONType_Integer))
							Application.m_RegisterInfo.m_Resources_FilesPerProcess = pValue->f_Integer();
						if (auto pValue = pRegisterInfo->f_GetMember("ResourcesThreads", EJSONType_Integer))
							Application.m_RegisterInfo.m_Resources_Threads = pValue->f_Integer();
						if (auto pValue = pRegisterInfo->f_GetMember("ResourcesProcesses", EJSONType_Integer))
							Application.m_RegisterInfo.m_Resources_Processes = pValue->f_Integer();
					}
					if (auto pValue = ApplicationJSON.f_GetMember("AssociatedHostID", EJSONType_String))
						Application.m_AssociatedHostID = pValue->f_String();
					if (auto pValue = ApplicationJSON.f_GetMember("Dependencies", EJSONType_Array))
					{
						for (auto &Tag : pValue->f_Array())
							Settings.m_Dependencies[Tag.f_String()];
					}
					if (auto pValue = ApplicationJSON.f_GetMember("StopOnDependencyFailure", EJSONType_Boolean))
						Settings.m_bStopOnDependencyFailure = pValue->f_Boolean();
					if (auto pValue = ApplicationJSON.f_GetMember("PreventLaunchUser", EJSONType_Boolean))
						Application.m_bPreventLaunch_User = pValue->f_Boolean();
					if (auto pValue = ApplicationJSON.f_GetMember("PreventLaunchUpdate", EJSONType_Boolean))
						Application.m_bPreventLaunch_Update = pValue->f_Boolean();
					
					mp_KnownRemoteApplications[CRemoteApplicationKey{Settings}][mp_State.m_HostID];
				}					
			}

			if (auto *pKnownRemoteHosts = mp_State.m_StateDatabase.m_Data.f_GetMember("KnownRemoteApplications"))
			{
				for (auto &Group : pKnownRemoteHosts->f_Object())
				{
					CRemoteApplicationKey RemoteKey;
					RemoteKey.m_Group = Group.f_Name();

					for (auto &Application : Group.f_Value().f_Object())
					{
						RemoteKey.m_VersionManagerApplication = Application.f_Name();

						for (auto &KnownHost : Application.f_Value().f_Object())
							mp_KnownRemoteApplications[RemoteKey][KnownHost.f_Name()];
					}
				}
			}
			
			if (!PendingSelfUpdateName.f_IsEmpty())
				fp_StartPendingSelfUpdateReporting(PendingSelfUpdateName, PendingSelfUpdateVersionID, PendingSelfUpdateTime, PendingSelfUpdateSequence);
		}
		catch (NException::CException const &_Exception)
		{
			return _Exception;
		}
		
		if (bChangedDatabase)
			return fp_SaveStateDatabase();
		else
			return fg_Explicit();
	}
	
	void CAppManagerActor::fp_KeyManagerAvailable()
	{
		fp_UpdateApplicationDependencies();
	}

	TCFuture<void> CAppManagerActor::fp_StartApp(NEncoding::CEJSON const &_Params)
	{
		mp_bLogLaunchesToStdErr = _Params["LogLaunchesToStdErr"].f_Boolean();
		
		mp_FileActor = fg_ConstructActor<CSeparateThreadActor>(fg_Construct("App manager file operations"));
		mp_KnownPlatforms[DMalterlibCloudPlatform];
		
		TCPromise<void> Promise;
		fp_ReadState() > Promise / [this, Promise]
			{
				if (mp_State.m_bStoppingApp)
					return Promise.f_SetException(DMibErrorInstance("Startup aborted"));
				
				fp_PublishAppInterface()
					+ fp_SetupLimits()
					> Promise / [this, Promise]
					{
						if (mp_State.m_bStoppingApp)
							return Promise.f_SetException(DMibErrorInstance("Startup aborted"));
						
						fp_InitApplications();
						fp_UpdateApplicationDependencies();
						fp_PublishCoordinationInterface() > [](TCAsyncResult<void> &&_Result)
							{
								if (!_Result)
									DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Failed to publish coordination interface: {}", _Result.f_GetExceptionStr());
							}
						;
						fp_SubscribeCoordinationInterface() > [](TCAsyncResult<void> &&_Result)
							{
								if (!_Result)
									DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Failed to subscribe to coordination interface: {}", _Result.f_GetExceptionStr());
							}
						;
						fp_SetupAppManagerInterfacePermissions() > [this](TCAsyncResult<void> &&_Result)
							{
								if (!_Result)
								{
									DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Failed to setup permissions: {}", _Result.f_GetExceptionStr());
									return;
								}
								if (mp_State.m_bStoppingApp)
									return;
								
								fp_PublishAppManagerInterface() > [](TCAsyncResult<void> &&_Result)
									{
										if (!_Result)
											DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Failed to publish app manager interface: {}", _Result.f_GetExceptionStr());
									}
								;
							}
						;
						
						mp_State.m_TrustManager
							(
								&CDistributedActorTrustManager::f_SubscribeTrustedActors<CKeyManager>
								, "com.malterlib/Cloud/KeyManager"
								, fg_ThisActor(this)
							)
							+ mp_State.m_TrustManager
							(
								&CDistributedActorTrustManager::f_SubscribeTrustedActors<CVersionManager>
								, "com.malterlib/Cloud/VersionManager"
								, fg_ThisActor(this)
							)
							> Promise / [this, Promise](TCTrustedActorSubscription<CKeyManager> &&_KeySubscrption, TCTrustedActorSubscription<CVersionManager> &&_VersionSubscrption)
							{
								if (mp_State.m_bStoppingApp)
									return Promise.f_SetException(DMibErrorInstance("Startup aborted"));
								
								mp_KeyManagerSubscription = fg_Move(_KeySubscrption);

								mp_KeyManagerSubscription.f_OnActor
									(
										[this](TCDistributedActor<CKeyManager> const &_KeyManager, CTrustedActorInfo const &_ActorInfo)
										{
											fp_KeyManagerAvailable();
										}
									)
								;

								mp_VersionManagerSubscription = fg_Move(_VersionSubscrption);
								
								mp_VersionManagerSubscription.f_OnActor
									(
										[this](TCDistributedActor<CVersionManager> const &_VersionManager, CTrustedActorInfo const &_ActorInfo)
										{
											fp_VersionManagerAdded(_VersionManager, _ActorInfo);
										}
									)
								;
								
								mp_VersionManagerSubscription.f_OnRemoveActor
									(
										[this](TCWeakDistributedActor<CActor> const &_VersionManagero)
										{
											fp_VersionManagerRemoved(_VersionManagero);
										}
									)
								;

								Promise.f_SetResult();
							}
						;
					}
				;
			}
		;
		
		TCPromise<void> ReturnPromise;
		Promise.f_Dispatch() > [this, ReturnPromise](TCAsyncResult<void> &&_Result)
			{
				if (!_Result)
					fp_InitialStartupFailed(_Result.f_GetException());
				
				ReturnPromise.f_SetResult(fg_Move(_Result));
			}
		;
		return ReturnPromise.f_MoveFuture();
	}
	
	TCFuture<void> CAppManagerActor::fp_StopApp()
	{	
		TCPromise<void> Promise;
		
		auto pCanDestroy = fg_Move(mp_pCanDestroy);
		
		pCanDestroy->f_Future() > Promise / [=]
			{
				fp_CancelAllApplicationUpdatesOnStopAppManager() > Promise / [=]
					{
						mp_AppManagerInterface.f_Destroy()
							+ mp_AppManagerCoordinationInterface.f_Destroy() > Promise / [=]
							{
								TCActorResultVector<uint32> ApplicationStops;
								for (auto &pApplication : mp_Applications)
									pApplication->f_Stop(EStopFlag_CloseEncryption) > ApplicationStops.f_AddResult();
								
								ApplicationStops.f_GetResults() > Promise / [this, Promise](TCVector<TCAsyncResult<uint32>> &&_Results)
									{
										for (auto &pApplication : mp_Applications)
										{
											pApplication->f_AbortPendingLaunches();
											pApplication->f_Clear();
										}

										mp_AppInterfaceServer.f_Destroy() > [this, Promise](TCAsyncResult<void> &&)
											{
												TCActorResultVector<void> Destroys;

												for (auto &Launch : mp_LaunchActors)
													Launch->f_Destroy() > Destroys.f_AddResult();
												mp_LaunchActors.f_Clear();

												Destroys.f_GetResults() > Promise.f_ReceiveAny();
											}
										;
									}
								;
							}
						;
					}
				;
			}
		;
		
		return Promise.f_MoveFuture();
	}
}

namespace NMib::NCloud
{
	TCActor<CDistributedAppActor> fg_ConstructApp_AppManager()
	{
		return fg_Construct<NAppManager::CAppManagerActor>();
	}
}
