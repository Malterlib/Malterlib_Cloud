// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Concurrency/LogError>

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
		fp_SendAppChange_AddedOrChanged(*_pApplication);

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
					if (auto pValue = ApplicationJSON.f_GetMember("LastTriedInstalledVersionError", EJSONType_String))
						Application.m_LastTriedInstalledVersionError = pValue->f_String();

					if (auto pValue = ApplicationJSON.f_GetMember("NewestUnconditionalVersion", EJSONType_Object))
						Application.m_NewestUnconditionalVersion = CVersionManager::CVersionIDAndPlatform::fs_FromJSON(*pValue);
					if (auto pValue = ApplicationJSON.f_GetMember("NewestUnconditionalVersionInfo", EJSONType_Object))
						Application.m_NewestUnconditionalVersionInfo = CVersionManager::CVersionInformation::fs_FromJSON(*pValue);

					if (auto pValue = ApplicationJSON.f_GetMember("WantVersion", EJSONType_Object))
						Application.m_WantVersion = CVersionManager::CVersionIDAndPlatform::fs_FromJSON(*pValue);
					if (auto pValue = ApplicationJSON.f_GetMember("WantVersionInfo", EJSONType_Object))
						Application.m_WantVersionInfo = CVersionManager::CVersionInformation::fs_FromJSON(*pValue);

					if (auto pValue = ApplicationJSON.f_GetMember("LastFailedInstalledVersionFailureStage", EJSONType_Integer))
						Application.m_LastFailedInstalledVersionFailureStage = (EUpdateStage)pValue->f_Integer();
					else
						Application.m_LastFailedInstalledVersionFailureStage = EUpdateStage::EUpdateStage_Failed;

					if
						(
							Application.m_LastTriedInstalledVersion != Application.m_LastInstalledVersionFinished
							|| Application.m_LastTriedInstalledVersionInfo.m_Time != Application.m_LastInstalledVersionInfoFinished.m_Time
							|| Application.m_LastTriedInstalledVersionInfo.m_RetrySequence != Application.m_LastInstalledVersionInfoFinished.m_RetrySequence
						)
					{
						if
							(
								(
									Name != PendingSelfUpdateName
									|| Application.m_LastTriedInstalledVersion != PendingSelfUpdateVersionID
									|| Application.m_LastTriedInstalledVersionInfo.m_Time != PendingSelfUpdateTime
									|| Application.m_LastTriedInstalledVersionInfo.m_RetrySequence != PendingSelfUpdateSequence
								)
							 	&& (Application.m_LastFailedInstalledVersionFailureStage > EUpdateStage::EUpdateStage_DownloadVersion)
							)
						{
							Application.m_LastFailedInstalledVersion = Application.m_LastTriedInstalledVersion;
							Application.m_LastFailedInstalledVersionTime = Application.m_LastTriedInstalledVersionInfo.m_Time;
							Application.m_LastFailedInstalledVersionRetrySequence = Application.m_LastTriedInstalledVersionInfo.m_RetrySequence;
						}
					}

					if (auto pValue = ApplicationJSON.f_GetMember("AutoUpdate", EJSONType_Boolean))
						Settings.m_bAutoUpdate = pValue->f_Boolean();

					{
						auto pValue = ApplicationJSON.f_GetMember("UpdateTags", EJSONType_Array);
						if (!pValue)
							pValue = ApplicationJSON.f_GetMember("AutoUpdateTags", EJSONType_Array);

						if (pValue)
						{
							for (auto &Tag : pValue->f_Array())
								Settings.m_UpdateTags[Tag.f_String()];
						}
					}

					{
						auto pValue = ApplicationJSON.f_GetMember("UpdateBranches", EJSONType_Array);
						if (!pValue)
							pValue = ApplicationJSON.f_GetMember("AutoUpdateBranches", EJSONType_Array);

						if (pValue)
						{
							for (auto &Tag : pValue->f_Array())
								Settings.m_UpdateBranches[Tag.f_String()];
						}
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
					if (auto pValue = ApplicationJSON.f_GetMember("AppManagerVersion", EJSONType_Integer))
						Settings.m_AppManagerVersion = pValue->f_Integer();
					else
						Settings.m_AppManagerVersion = 0;

					if (auto pValue = ApplicationJSON.f_GetMember("LaunchInProcess", EJSONType_Boolean))
						Settings.m_bLaunchInProcess = pValue->f_Boolean();

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
		catch (NException::CException const &)
		{
			co_return fg_CurrentException();
		}

		if (bChangedDatabase)
			co_await fp_SaveStateDatabase();

		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_KeyManagerAvailable()
	{
		fp_UpdateApplicationDependencies();
		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_InitApp()
	{
		auto OnResume = g_OnResume / [&]
			{
				if (mp_State.m_bStoppingApp || f_IsDestroyed())
					DMibError("Startup aborted");
			}
		;

		co_await fp_InitSensor();
		co_await fp_InitLog();
		co_await fp_InitHostMonitor();

		co_await fp_ReadState();

		co_await (fp_PublishAppInterface() + fp_SetupLimits());

		fp_InitApplications();
		fp_UpdateApplicationDependencies();

		CLogError LogError("Malterlib/Cloud/AppManager");

		fp_PublishCoordinationInterface() > LogError("Failed to publish coordination interface");
		fp_SubscribeCoordinationInterface() > LogError("Failed to subscribe to coordination interface");

		co_await (fp_SetupAppManagerInterfacePermissions() % "Failed to setup permissions");
		co_await (fp_PublishAppManagerInterface() % "Failed to publish app manager interface");

		auto [KeySubscription, VersionSubscription, CloudSubscription] = co_await
			(
				mp_State.m_TrustManager->f_SubscribeTrustedActors<CKeyManager>()
				+ mp_State.m_TrustManager->f_SubscribeTrustedActors<CVersionManager>()
				+ mp_State.m_TrustManager->f_SubscribeTrustedActors<CCloudManager>()
			)
		;

		mp_KeyManagerSubscription = fg_Move(KeySubscription);
		co_await mp_KeyManagerSubscription.f_OnActor
			(
				g_ActorFunctor / [this](TCDistributedActor<CKeyManager> const &_KeyManager, CTrustedActorInfo const &_ActorInfo) -> TCFuture<void>
				{
					co_await fp_KeyManagerAvailable();
					co_return {};
				}
				, nullptr
			)
		;

		mp_VersionManagerSubscription = fg_Move(VersionSubscription);
		co_await mp_VersionManagerSubscription.f_OnActor
			(
				g_ActorFunctor / [this](TCDistributedActor<CVersionManager> const &_VersionManager, CTrustedActorInfo const &_ActorInfo) -> TCFuture<void>
				{
					co_await self(&CAppManagerActor::fp_VersionManagerAdded, _VersionManager, _ActorInfo);
					co_return {};
				}
				, g_ActorFunctor / [this](TCWeakDistributedActor<CActor> const &_VersionManager, CTrustedActorInfo &&_ActorInfo) -> TCFuture<void>
				{
					fp_VersionManagerRemoved(_VersionManager);

					co_return {};
				}
			)
		;

		mp_CloudManagerSubscription = fg_Move(CloudSubscription);
		co_await mp_CloudManagerSubscription.f_OnActor
			(
				g_ActorFunctor / [this](TCDistributedActor<CCloudManager> const &_CloudManager, CTrustedActorInfo const &_ActorInfo) -> TCFuture<void>
				{
					co_await self(&CAppManagerActor::fp_CloudManagerAdded, _CloudManager, _ActorInfo);
					co_return {};
				}
				, g_ActorFunctor / [this](TCWeakDistributedActor<CActor> const &_CloudManager, CTrustedActorInfo &&_ActorInfo) -> TCFuture<void>
				{
					fp_CloudManagerRemoved(_CloudManager);

					co_return {};
				}
				, "CloudManager"
				, "Failed to {} cloud manager"
			)
		;

		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_StartApp(NEncoding::CEJSON const &_Params)
	{
		mp_bLogLaunchesToStdErr = _Params["LogLaunchesToStdErr"].f_Boolean();
		if (auto pValue = _Params.f_GetMember("HostMonitorInterval"))
			mp_HostMonitorInterval = pValue->f_Float();
		else
			mp_HostMonitorInterval = mp_State.m_ConfigDatabase.m_Data.f_GetMemberValue("HostMonitorInterval", mp_HostMonitorInterval).f_Float();

		mp_FileActor = fg_ConstructActor<CSeparateThreadActor>(fg_Construct("App manager file operations"));
		mp_KnownPlatforms[DMalterlibCloudPlatform];

		auto InitResult = co_await fp_InitApp().f_Wrap();

		if (!InitResult)
			fp_InitialStartupFailed(InitResult.f_GetException());

		co_return fg_Move(InitResult);
	}

	TCFuture<void> CAppManagerActor::fp_StopApp()
	{
		auto CanDestroyFuture = mp_pCanDestroy->f_Future();
		mp_pCanDestroy.f_Clear();
		co_await fg_Move(CanDestroyFuture);

		co_await fp_CancelAllApplicationUpdatesOnStopAppManager();

		co_await (mp_AppManagerInterface.f_Destroy() + mp_AppManagerCoordinationInterface.f_Destroy());

		{
			TCActorResultVector<uint32> ApplicationStops;
			for (auto &pApplication : mp_Applications)
				pApplication->f_Stop(EStopFlag_CloseEncryption) > ApplicationStops.f_AddResult();

			co_await ApplicationStops.f_GetResults().f_Wrap();
		}

		for (auto &pApplication : mp_Applications)
		{
			pApplication->f_AbortPendingLaunches();
			pApplication->f_Clear();
		}

		co_await mp_AppInterfaceServer.f_Destroy().f_Wrap();

		{
			TCActorResultVector<void> Destroys;

			for (auto &Launch : mp_LaunchActors)
				fg_Move(Launch).f_Destroy() > Destroys.f_AddResult();
			mp_LaunchActors.f_Clear();

			co_await Destroys.f_GetResults().f_Wrap();
		}

		{
			TCActorResultVector<void> Destroys;

			for (auto &CloudManager : mp_CloudManagers)
			{
				if (CloudManager.m_RegisterSubscription)
					CloudManager.m_RegisterSubscription->f_Destroy() > Destroys.f_AddResult();
				if (CloudManager.m_SensorReporterSubscription)
					CloudManager.m_SensorReporterSubscription->f_Destroy() > Destroys.f_AddResult();
				if (CloudManager.m_LogReporterSubscription)
					CloudManager.m_LogReporterSubscription->f_Destroy() > Destroys.f_AddResult();
			}

			mp_SensorReporterInterface.f_Destroy() > Destroys.f_AddResult();
			mp_LogReporterInterface.f_Destroy() > Destroys.f_AddResult();

			mp_CloudManagers.f_Clear();

			co_await Destroys.f_GetResults().f_Wrap();
		}

		if (mp_MainDirectoryMonitorSubscription)
			co_await fg_Exchange(mp_MainDirectoryMonitorSubscription, nullptr)->f_Destroy();

		if (mp_HostMonitor)
			co_await fg_Move(mp_HostMonitor).f_Destroy();

		co_return {};
	}
}

namespace NMib::NCloud
{
	TCActor<CDistributedAppActor> fg_ConstructApp_AppManager()
	{
		return fg_Construct<NAppManager::CAppManagerActor>();
	}
}
