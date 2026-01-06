// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JsonShortcuts>
#include <Mib/Concurrency/LogError>

#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud::NAppManager
{
	CAppManagerActor::CAppManagerActor()
		: CDistributedAppActor(CDistributedAppActor_Settings("AppManager").f_AuditCategory("Malterlib/Cloud/AppManager"))
	{
		mp_InitialStartupResultFuture = mp_InitialStartupResult.f_Future();
	}

	CAppManagerActor::~CAppManagerActor()
	{
		if (mp_InitialStartupResultFuture.f_IsValid())
			fg_Move(mp_InitialStartupResultFuture).f_DiscardResult();
	}

	TCFuture<void> CAppManagerActor::fp_Destroy()
	{
		CLogError LogError("Malterlib/Cloud/AppManager");

		if (mp_RebootScheduleTimerSubscrption)
			co_await fg_Exchange(mp_RebootScheduleTimerSubscrption, nullptr)->f_Destroy().f_Wrap() > LogError.f_Warning("Failed to destroy reboot timer subscription");

		co_await fg_Move(mp_ChangeNotificationsPermissionsChangedSequencer).f_Destroy().f_Wrap() > LogError.f_Warning("Failed to destroy sequencer");

		if (mp_SensorRebootPreventionTimerSubscription)
			co_await fg_Exchange(mp_SensorRebootPreventionTimerSubscription, nullptr)->f_Destroy().f_Wrap() > LogError.f_Warning("Failed to destroy reboot prevention timer");

		TCFutureVector<void> SensorRebootPreventionDestroys;

		for (auto &Watch : mp_SensorRebootPreventionWatch)
		{
			if (Watch.m_SensorSubscription)
				fg_Exchange(Watch.m_SensorSubscription, nullptr)->f_Destroy() > SensorRebootPreventionDestroys;
		}

		co_await fg_AllDone(SensorRebootPreventionDestroys).f_Wrap() > LogError.f_Warning("Failed to destroy reboot prevention subscriptions");

		co_await CDistributedAppActor::fp_Destroy();

		co_return {};
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
		auto CaptureScope = co_await g_CaptureExceptions;

		CPendingSelfupdate PendingSelfUpdate;

		mp_PauseReportingForSecondsAtShutdown = mp_State.m_ConfigDatabase.m_Data.f_GetMemberValue("PauseReportingForSecondsAtShutdown", mp_PauseReportingForSecondsAtShutdown).f_Float();

		if (auto *pUserNameTransform = mp_State.m_ConfigDatabase.m_Data.f_GetMember("UserGroupNameTransform"))
			mp_pUniqueUserGroup->m_UserGroupNameTransform = pUserNameTransform->f_String();

		if (auto *pPendingSelfUpdateProcess = mp_State.m_StateDatabase.m_Data.f_GetMember("PendingSelfUpdateProcess"))
		{
			auto &Pending = *pPendingSelfUpdateProcess;

			PendingSelfUpdate.m_Name = Pending["Name"].f_String();
			PendingSelfUpdate.m_VersionID = CVersionManager::CVersionIDAndPlatform::fs_FromJson(Pending["VersionID"]);
			PendingSelfUpdate.m_VersionTime = Pending["VersionTime"].f_Date();
			PendingSelfUpdate.m_VersionRetrySequence = Pending["VersionRetrySequence"].f_Integer();
			if (auto *pValue = Pending.f_GetMember("StartUpdateTime"))
				PendingSelfUpdate.m_StartUpdateTime = pValue->f_Date();
			if (auto *pValue = Pending.f_GetMember("UniqueUpdateID"))
				PendingSelfUpdate.m_UniqueUpdateID = pValue->f_String();

			mp_State.m_StateDatabase.m_Data.f_RemoveMember("PendingSelfUpdateProcess");
			bChangedDatabase = true;
		}

		if (auto pApplication = mp_State.m_StateDatabase.m_Data.f_GetMember("Applications"))
		{
			for (auto &ApplicationEntry : pApplication->f_Object())
			{
				CStr const &Name = ApplicationEntry.f_Name();
				auto &ApplicationJson = ApplicationEntry.f_Value();

				auto &Application = *(mp_Applications[Name] = fg_Construct(Name, this));

				auto &Settings = Application.m_Settings;

				Settings.m_Executable = ApplicationJson["Executable"].f_String();
				Settings.m_RunAsUser = ApplicationJson["RunAsUser"].f_String();
#ifdef DPlatformFamily_Windows
				if (auto pValue = ApplicationJson.f_GetMember("RunAsUserPassword", EJsonType_String))
					Settings.m_RunAsUserPassword = pValue->f_String();
#endif
				Settings.m_RunAsGroup = ApplicationJson["RunAsGroup"].f_String();

				if (auto pValue = ApplicationJson.f_GetMember("RunAsUserHasShell", EJsonType_Boolean))
					Settings.m_bRunAsUserHasShell = pValue->f_Boolean();

				if (auto *pValue = ApplicationJson.f_GetMember("Backup"))
				{
					auto &BackupJson = *pValue;

					if (BackupJson["IncludeWildcards"].f_IsArray())
					{
						for (auto &Wildcard : BackupJson["IncludeWildcards"].f_Array())
							Settings.m_Backup_IncludeWildcards[Wildcard.f_String()];
					}
					else
					{
						for (auto &Wildcard : BackupJson["IncludeWildcards"].f_Object())
						{
							auto &Destination = Settings.m_Backup_IncludeWildcards[Wildcard.f_Name()];
							if (Wildcard.f_Value().f_IsNull())
								Destination = CDirectoryManifestConfig::CDestination{};
							else
								Destination = Wildcard.f_Value().f_String();
						}
					}
					for (auto &Wildcard : BackupJson["ExcludeWildcards"].f_Array())
						Settings.m_Backup_ExcludeWildcards[Wildcard.f_String()];
					for (auto &Wildcard : BackupJson["AddSyncFlagsWildcards"].f_Object())
						Settings.m_Backup_AddSyncFlagsWildcards[Wildcard.f_Name()] = CDirectoryManifestFile::fs_ParseSyncFlags(Wildcard.f_Value());
					for (auto &Wildcard : BackupJson["RemoveSyncFlagsWildcards"].f_Object())
						Settings.m_Backup_RemoveSyncFlagsWildcards[Wildcard.f_Name()] = CDirectoryManifestFile::fs_ParseSyncFlags(Wildcard.f_Value());

					Settings.m_Backup_NewBackupInterval = CTimeSpanConvert::fs_CreateSpanFromHours(BackupJson["NewBackupIntervalHours"].f_Float());
					Settings.m_bBackupEnabled = BackupJson["Enabled"].f_Boolean();
				}

				if (auto pValue = ApplicationJson.f_GetMember("DistributedApp", EJsonType_Boolean))
					Settings.m_bDistributedApp = pValue->f_Boolean();
				for (auto &Parameter : ApplicationJson["Parameters"].f_Array())
					Settings.m_ExecutableParameters.f_Insert(Parameter.f_String());
				for (auto &File : ApplicationJson["Files"].f_Array())
					Application.m_Files.f_Insert(File.f_String());
				Settings.m_EncryptionStorage = ApplicationJson["EncryptionStorage"].f_String();
				if (auto pValue = ApplicationJson.f_GetMember("EncryptionFileSystem", EJsonType_String))
					Settings.m_EncryptionFileSystem = pValue->f_String();
				else if (!Settings.m_EncryptionStorage.f_IsEmpty())
					Settings.m_EncryptionFileSystem = "zfs";
				if (auto pValue = ApplicationJson.f_GetMember("ParentApplication", EJsonType_String))
					Settings.m_ParentApplication = pValue->f_String();
				if (auto pValue = ApplicationJson.f_GetMember("VersionManagerApplication", EJsonType_String))
					Settings.m_VersionManagerApplication = pValue->f_String();
				if (auto pValue = ApplicationJson.f_GetMember("LastInstalledVersion", EJsonType_Object))
					Application.m_LastInstalledVersion = CVersionManager::CVersionIDAndPlatform::fs_FromJson(*pValue);
				if (auto pValue = ApplicationJson.f_GetMember("LastInstalledVersionInfo", EJsonType_Object))
					Application.m_LastInstalledVersionInfo = CVersionManager::CVersionInformation::fs_FromJson(*pValue);

				if (auto pValue = ApplicationJson.f_GetMember("LastInstalledVersionFinished", EJsonType_Object))
					Application.m_LastInstalledVersionFinished = CVersionManager::CVersionIDAndPlatform::fs_FromJson(*pValue);
				else
					Application.m_LastInstalledVersionFinished = Application.m_LastInstalledVersion;

				if (auto pValue = ApplicationJson.f_GetMember("LastInstalledVersionInfoFinished", EJsonType_Object))
					Application.m_LastInstalledVersionInfoFinished = CVersionManager::CVersionInformation::fs_FromJson(*pValue);
				else
					Application.m_LastInstalledVersionInfoFinished = Application.m_LastInstalledVersionInfo;

				if (auto pValue = ApplicationJson.f_GetMember("LastTriedInstalledVersion", EJsonType_Object))
					Application.m_LastTriedInstalledVersion = CVersionManager::CVersionIDAndPlatform::fs_FromJson(*pValue);
				if (auto pValue = ApplicationJson.f_GetMember("LastTriedInstalledVersionInfo", EJsonType_Object))
					Application.m_LastTriedInstalledVersionInfo = CVersionManager::CVersionInformation::fs_FromJson(*pValue);
				if (auto pValue = ApplicationJson.f_GetMember("LastTriedInstalledVersionError", EJsonType_String))
					Application.m_LastTriedInstalledVersionError = pValue->f_String();

				if (auto pValue = ApplicationJson.f_GetMember("NewestUnconditionalVersion", EJsonType_Object))
					Application.m_NewestUnconditionalVersion = CVersionManager::CVersionIDAndPlatform::fs_FromJson(*pValue);
				if (auto pValue = ApplicationJson.f_GetMember("NewestUnconditionalVersionInfo", EJsonType_Object))
					Application.m_NewestUnconditionalVersionInfo = CVersionManager::CVersionInformation::fs_FromJson(*pValue);

				if (auto pValue = ApplicationJson.f_GetMember("WantVersion", EJsonType_Object))
					Application.m_WantVersion = CVersionManager::CVersionIDAndPlatform::fs_FromJson(*pValue);
				if (auto pValue = ApplicationJson.f_GetMember("WantVersionInfo", EJsonType_Object))
					Application.m_WantVersionInfo = CVersionManager::CVersionInformation::fs_FromJson(*pValue);

				if (auto pValue = ApplicationJson.f_GetMember("LastFailedInstalledVersionFailureStage", EJsonType_Integer))
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
								Name != PendingSelfUpdate.m_Name
								|| Application.m_LastTriedInstalledVersion != PendingSelfUpdate.m_VersionID
								|| Application.m_LastTriedInstalledVersionInfo.m_Time != PendingSelfUpdate.m_VersionTime
								|| Application.m_LastTriedInstalledVersionInfo.m_RetrySequence != PendingSelfUpdate.m_VersionRetrySequence
							)
							&& (Application.m_LastFailedInstalledVersionFailureStage > EUpdateStage::EUpdateStage_DownloadVersion)
						)
					{
						Application.m_LastFailedInstalledVersion = Application.m_LastTriedInstalledVersion;
						Application.m_LastFailedInstalledVersionTime = Application.m_LastTriedInstalledVersionInfo.m_Time;
						Application.m_LastFailedInstalledVersionRetrySequence = Application.m_LastTriedInstalledVersionInfo.m_RetrySequence;
					}
				}

				if (auto pValue = ApplicationJson.f_GetMember("AutoUpdate", EJsonType_Boolean))
					Settings.m_bAutoUpdate = pValue->f_Boolean();

				{
					auto pValue = ApplicationJson.f_GetMember("UpdateTags", EJsonType_Array);
					if (!pValue)
						pValue = ApplicationJson.f_GetMember("AutoUpdateTags", EJsonType_Array);

					if (pValue)
					{
						for (auto &Tag : pValue->f_Array())
							Settings.m_UpdateTags[Tag.f_String()];
					}
				}

				{
					auto pValue = ApplicationJson.f_GetMember("UpdateBranches", EJsonType_Array);
					if (!pValue)
						pValue = ApplicationJson.f_GetMember("AutoUpdateBranches", EJsonType_Array);

					if (pValue)
					{
						for (auto &Tag : pValue->f_Array())
							Settings.m_UpdateBranches[Tag.f_String()];
					}
				}
				if (auto pValue = ApplicationJson.f_GetMember("UpdateScripts", EJsonType_Object))
				{
					Settings.m_UpdateScripts.m_PreUpdate = (*pValue)["PreUpdate"].f_String();
					Settings.m_UpdateScripts.m_PostUpdate = (*pValue)["PostUpdate"].f_String();
					Settings.m_UpdateScripts.m_PostLaunch = (*pValue)["PostLaunch"].f_String();
					Settings.m_UpdateScripts.m_OnError = (*pValue)["OnError"].f_String();
				}
				if (auto pValue = ApplicationJson.f_GetMember("SelfUpdateSource", EJsonType_Boolean))
					Settings.m_bSelfUpdateSource = pValue->f_Boolean();
				if (auto pValue = ApplicationJson.f_GetMember("UpdateGroup", EJsonType_String))
					Settings.m_UpdateGroup = pValue->f_String();
				if (auto pRegisterInfo = ApplicationJson.f_GetMember("RegisterInfo", EJsonType_Object))
				{
					if (auto pValue = pRegisterInfo->f_GetMember("UpdateType", EJsonType_Integer))
						Application.m_RegisterInfo.m_UpdateType = (EDistributedAppUpdateType)pValue->f_Integer();
					if (auto pValue = pRegisterInfo->f_GetMember("ResourcesFiles", EJsonType_Integer))
						Application.m_RegisterInfo.m_Resources_Files = pValue->f_Integer();
					if (auto pValue = pRegisterInfo->f_GetMember("ResourcesFilesPerProcess", EJsonType_Integer))
						Application.m_RegisterInfo.m_Resources_FilesPerProcess = pValue->f_Integer();
					if (auto pValue = pRegisterInfo->f_GetMember("ResourcesThreads", EJsonType_Integer))
						Application.m_RegisterInfo.m_Resources_Threads = pValue->f_Integer();
					if (auto pValue = pRegisterInfo->f_GetMember("ResourcesProcesses", EJsonType_Integer))
						Application.m_RegisterInfo.m_Resources_Processes = pValue->f_Integer();
					if (auto pValue = pRegisterInfo->f_GetMember("ResourcesMaxMapCount", EJsonType_Integer))
						Application.m_RegisterInfo.m_Resources_MaxMapCount = pValue->f_Integer();
				}
				if (auto pValue = ApplicationJson.f_GetMember("AssociatedHostID", EJsonType_String))
					Application.m_AssociatedHostID = pValue->f_String();
				if (auto pValue = ApplicationJson.f_GetMember("Dependencies", EJsonType_Array))
				{
					for (auto &Tag : pValue->f_Array())
						Settings.m_Dependencies[Tag.f_String()];
				}
				if (auto pValue = ApplicationJson.f_GetMember("StopOnDependencyFailure", EJsonType_Boolean))
					Settings.m_bStopOnDependencyFailure = pValue->f_Boolean();
				if (auto pValue = ApplicationJson.f_GetMember("PreventLaunchUser", EJsonType_Boolean))
					Application.m_bPreventLaunch_User = pValue->f_Boolean();
				if (auto pValue = ApplicationJson.f_GetMember("PreventLaunchUpdate", EJsonType_Boolean))
					Application.m_bPreventLaunch_Update = pValue->f_Boolean();
				if (auto pValue = ApplicationJson.f_GetMember("AppManagerVersion", EJsonType_Integer))
					Settings.m_AppManagerVersion = pValue->f_Integer();
				else
					Settings.m_AppManagerVersion = 0;

				if (auto pValue = ApplicationJson.f_GetMember("LaunchInProcess", EJsonType_Boolean))
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

		if (!PendingSelfUpdate.m_Name.f_IsEmpty())
			fp_StartPendingSelfUpdateReporting(PendingSelfUpdate);

		if (bChangedDatabase)
			co_await fp_SaveStateDatabase();

		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_KeyManagersUpdated()
	{
		fp_UpdateApplicationDependencies();
		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_InitApp()
	{
		auto OnResume = co_await fg_OnResume
			(
				[&]() -> CExceptionPointer
				{
					if (mp_State.m_bStoppingApp || f_IsDestroyed())
						return DMibErrorInstance("Startup aborted");
					return {};
				}
			)
		;

		co_await (fp_SetupDatabase() % "Failed to setup database");

		co_await fp_InitSensor();
		co_await fp_InitLog();
		co_await fp_InitHostMonitor();

		co_await fp_ReadState();

		co_await fp_RebootPrevention_WatchInitialSensors();

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
				g_ActorFunctor / [this](TCDistributedActor<CKeyManager> _KeyManager, CTrustedActorInfo _ActorInfo) -> TCFuture<void>
				{
					co_await fp_KeyManagersUpdated();
					co_return {};
				}
				, g_ActorFunctor / [this](TCWeakDistributedActor<CActor> _KeyManager, CTrustedActorInfo _ActorInfo) -> TCFuture<void>
				{
					co_await fp_KeyManagersUpdated();

					co_return {};
				}
			)
		;

		mp_VersionManagerSubscription = fg_Move(VersionSubscription);
		co_await mp_VersionManagerSubscription.f_OnActor
			(
				g_ActorFunctor / [this](TCDistributedActor<CVersionManager> _VersionManager, CTrustedActorInfo _ActorInfo) -> TCFuture<void>
				{
					DMibLogWithCategory(Malterlib/Cloud/AppManager, Debug, "Version manager added {}", _ActorInfo.m_HostInfo);

					co_await fp_VersionManagerAdded(_VersionManager, _ActorInfo);

					co_return {};
				}
				, g_ActorFunctor / [this](TCWeakDistributedActor<CActor> _VersionManager, CTrustedActorInfo _ActorInfo) -> TCFuture<void>
				{
					DMibLogWithCategory(Malterlib/Cloud/AppManager, Debug, "Version manager removed {}", _ActorInfo.m_HostInfo);

					fp_VersionManagerRemoved(_VersionManager);

					co_return {};
				}
			)
		;

		mp_CloudManagerSubscription = fg_Move(CloudSubscription);
		co_await mp_CloudManagerSubscription.f_OnActor
			(
				g_ActorFunctor / [this](TCDistributedActor<CCloudManager> _CloudManager, CTrustedActorInfo _ActorInfo) -> TCFuture<void>
				{
					co_await fp_CloudManagerAdded(_CloudManager, _ActorInfo);
					co_return {};
				}
				, g_ActorFunctor / [this](TCWeakDistributedActor<CActor> _CloudManager, CTrustedActorInfo _ActorInfo) -> TCFuture<void>
				{
					co_await fp_CloudManagerRemoved(_CloudManager).f_Wrap() > fg_LogWarning("Malterlib/Cloud/AppManager", "Failed to remove cloud manager");

					co_return {};
				}
				, "CloudManager"
				, "Failed to handle '{}' for cloud manager"
			)
		;

		co_await (fp_SetupDatabaseCleanup() % "Failed to setup database cleanup");

		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_StartApp(NEncoding::CEJsonSorted const _Params)
	{
		CStr Environment = mp_State.m_ConfigDatabase.m_Data.f_GetMemberValue("Environment", "").f_String();

		if (Environment)
		{
			mp_SensorMetadata["Environment"] = Environment;
			mp_LogMetadata["Environment"] = Environment;
		}

		mp_bLogLaunchesToStdErr = _Params["LogLaunchesToStdErr"].f_Boolean();
		if (auto pValue = _Params.f_GetMember("HostMonitorInterval"))
			mp_HostMonitorInterval = pValue->f_Float();
		else
			mp_HostMonitorInterval = mp_State.m_ConfigDatabase.m_Data.f_GetMemberValue("HostMonitorInterval", mp_HostMonitorInterval).f_Float();

		DMibFastCheck(!mp_HostMonitorInterval.f_IsInvalid());
		if (mp_HostMonitorInterval != 0.0 && mp_HostMonitorInterval < CHostMonitor::mc_MinimumHostMonitorInterval)
		{
			DMibLogWithCategory
				(
					Malterlib/Cloud/AppManager
					, Warning
					, "Limited HostMonitorInterval to {} seconds as this is the lowest supported"
					, CHostMonitor::mc_MinimumHostMonitorInterval
					, mp_HostMonitorInterval
				)
			;
			mp_HostMonitorInterval = CHostMonitor::mc_MinimumHostMonitorInterval;
		}

		if (auto pValue = _Params.f_GetMember("HostMonitorPatchInterval"))
			mp_HostMonitorPatchInterval = pValue->f_Float();
		else
			mp_HostMonitorPatchInterval = mp_State.m_ConfigDatabase.m_Data.f_GetMemberValue("HostMonitorPatchInterval", mp_HostMonitorPatchInterval).f_Float();

		mp_bEnableApplicationStatusSensors = _Params["EnableApplicationStatusSensors"].f_Boolean();
		mp_bEnableEncryptionStatusSensors = _Params["EnableEncryptionStatusSensors"].f_Boolean();

		DMibFastCheck(!mp_HostMonitorPatchInterval.f_IsInvalid());
		if (mp_HostMonitorPatchInterval != 0.0 && mp_HostMonitorPatchInterval < CHostMonitor::mc_MinimumHostMonitorPatchInterval)
		{
			DMibLogWithCategory
				(
					Malterlib/Cloud/AppManager
					, Warning
					, "Limited HostMonitorPatchInterval to {} seconds as this is the lowest supported"
					, CHostMonitor::mc_MinimumHostMonitorPatchInterval
					, mp_HostMonitorPatchInterval
				)
			;
			mp_HostMonitorPatchInterval = CHostMonitor::mc_MinimumHostMonitorPatchInterval;
		}

		if (auto pValue = _Params.f_GetMember("HostMonitorMemoryInterval"))
			mp_HostMonitorMemoryInterval = pValue->f_Float();
		else
			mp_HostMonitorMemoryInterval = mp_State.m_ConfigDatabase.m_Data.f_GetMemberValue("HostMonitorMemoryInterval", mp_HostMonitorMemoryInterval).f_Float();

		if (mp_HostMonitorMemoryInterval != 0.0 && mp_HostMonitorMemoryInterval < CHostMonitor::mc_MinimumHostMonitorMemoryInterval)
		{
			DMibLogWithCategory
				(
					Malterlib/Cloud/AppManager
					, Warning
					, "Limited HostMonitorMemoryInterval to {} seconds as this is the lowest supported"
					, CHostMonitor::mc_MinimumHostMonitorMemoryInterval
					, mp_HostMonitorMemoryInterval
				)
			;
			mp_HostMonitorMemoryInterval = CHostMonitor::mc_MinimumHostMonitorMemoryInterval;
		}

		mp_AutoUpdateDelay = _Params["AutoUpdateDelay"].f_Float();

		mp_KnownPlatforms[DMalterlibCloudPlatform];

		auto InitResult = co_await fp_InitApp().f_Wrap();

		if (!InitResult)
			fp_InitialStartupFailed(InitResult.f_GetException());

		co_return fg_Move(InitResult);
	}

	TCFuture<void> CAppManagerActor::fp_PauseReporting()
	{
		if (mp_PauseReportingForSecondsAtShutdown.f_IsNan())
			co_return {};

		TCFutureVector<void> Results;

		for (auto &CloudManager : mp_CloudManagers)
		{
			if (!CloudManager.m_AppManagerCloudManagerInterface)
				continue;

			CloudManager.m_AppManagerCloudManagerInterface.f_CallActor(&CAppManagerCloudManagerInterface::f_PauseReporting)(mp_PauseReportingForSecondsAtShutdown)
				> Results
			;
		}

		co_await fg_AllDone(Results);

		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_StopApp()
	{
		CLogError LogError("Malterlib/Cloud/AppManager");

		if (mp_RebootScheduleTimerSubscrption)
			co_await fg_Exchange(mp_RebootScheduleTimerSubscrption, nullptr)->f_Destroy().f_Wrap() > LogError.f_Warning("Failed to destroy reboot timer subscription");

		co_await fp_PauseReporting().f_Wrap() > LogError.f_Warning("Failed to pause reporting at shutdown");

		auto CanDestroyFuture = mp_pCanDestroy->f_Future();
		mp_pCanDestroy.f_Clear();
		co_await fg_Move(CanDestroyFuture);

		co_await fp_CancelAllApplicationUpdatesOnStopAppManager();

		{
			TCFutureVector<uint32> ApplicationStops;
			for (auto &pApplication : mp_Applications)
				pApplication->f_Stop(EStopFlag_CloseEncryption) > ApplicationStops;

			co_await fg_AllDone(ApplicationStops).f_Wrap() > LogError.f_Warning("Failed to stop applications");
		}

		for (auto &pApplication : mp_Applications)
		{
			pApplication->f_AbortPendingLaunches();
			pApplication->f_Clear();
		}

		{
			TCFutureVector<void> Destroys;

			for (auto &Launch : mp_LaunchActors)
				fg_Move(Launch).f_Destroy() > Destroys;
			mp_LaunchActors.f_Clear();

			co_await fg_AllDone(Destroys).f_Wrap() > LogError.f_Warning("Failed to destroy app launchers");
		}

		{
			TCFutureVector<void> Destroys;

			for (auto &Subscription : mp_ChangeNotificationSubscriptions)
			{
				if (Subscription.m_fOnChange)
					fg_Move(Subscription.m_fOnChange).f_Destroy() > Destroys;
			}

			mp_ChangeNotificationSubscriptions.f_Clear();

			co_await fg_AllDone(Destroys).f_Wrap() > LogError.f_Warning("Failed to destroy change notification subscriptions");
		}

		{
			TCFutureVector<void> Destroys;

			for (auto &Subscription : mp_UpdateNotificationSubscriptions)
			{
				if (Subscription.m_fOnUpdate)
					fg_Move(Subscription.m_fOnUpdate).f_Destroy() > Destroys;

				fg_Move(Subscription.m_Sequencer).f_Destroy() > Destroys;
			}
			mp_UpdateNotificationSubscriptions.f_Clear();

			co_await fg_AllDone(Destroys).f_Wrap() > LogError.f_Warning("Failed to destroy update notification subscriptions");
		}

		{
			TCFutureVector<void> Destroys;

			for (auto &CloudManager : mp_CloudManagers)
			{
				if (CloudManager.m_AppManagerCloudManagerInterface)
					fg_Move(CloudManager.m_AppManagerCloudManagerInterface).f_Destroy() > Destroys;
				if (CloudManager.m_SensorReporterSubscription)
					fg_Exchange(CloudManager.m_SensorReporterSubscription, nullptr)->f_Destroy() > Destroys;
				if (CloudManager.m_LogReporterSubscription)
					fg_Exchange(CloudManager.m_LogReporterSubscription, nullptr)->f_Destroy() > Destroys;
				if (CloudManager.m_ExpectedOsVersionSubscription)
					fg_Exchange(CloudManager.m_ExpectedOsVersionSubscription, nullptr)->f_Destroy() > Destroys;
			}

			mp_CloudManagers.f_Clear();

			mp_SensorReporterInterface.f_Destroy() > Destroys;
			mp_LogReporterInterface.f_Destroy() > Destroys;

			co_await fg_AllDone(Destroys).f_Wrap() > LogError.f_Warning("Failed to destroy cloud managers or reporter interfaces");
		}

		{
			auto [AppManagerInterfaceDestroyResult, AppManagerCoordinationInterfaceDestroyResult] =
				co_await (mp_AppManagerInterface.f_Destroy() + mp_AppManagerCoordinationInterface.f_Destroy()).f_Wrap()
			;
			if (!AppManagerInterfaceDestroyResult)
				AppManagerInterfaceDestroyResult > LogError.f_Warning("Failed to destroy app manager interface");
			if (!AppManagerCoordinationInterfaceDestroyResult)
				AppManagerCoordinationInterfaceDestroyResult > LogError.f_Warning("Failed to destroy app manager coordination interface");
		}

		co_await mp_RemoteAppManagers.f_Destroy().f_Wrap() > LogError.f_Warning("Failed to destroy remote app managers interface subscription");
		co_await mp_AppInterfaceServer.f_Destroy().f_Wrap() > LogError.f_Warning("Failed to destroy app interface server");

		co_await mp_KeyManagerSubscription.f_Destroy().f_Wrap() > LogError.f_Warning("Failed to destroy key manager interface subscription");
		co_await mp_VersionManagerSubscription.f_Destroy().f_Wrap() > LogError.f_Warning("Failed to destroy version manager interface subscription");
		co_await mp_CloudManagerSubscription.f_Destroy().f_Wrap() > LogError.f_Warning("Failed to destroy cloud manager interface subscription");

		if (mp_MainDirectoryMonitorSubscription)
			co_await fg_Exchange(mp_MainDirectoryMonitorSubscription, nullptr)->f_Destroy();

		if (mp_MainConfigFileMonitorSubscription)
			co_await fg_Exchange(mp_MainConfigFileMonitorSubscription, nullptr)->f_Destroy();

		if (mp_HostMonitor)
			co_await fg_Move(mp_HostMonitor).f_Destroy();

		co_await fg_Move(mp_EncryptionSensorReporterSequencer).f_Destroy().f_Wrap() > LogError.f_Warning("Failed to destroy encryption sensor reporter sequencer");
		co_await fg_Move(mp_EncryptionSensorReporter.m_fReportReadings).f_Destroy().f_Wrap() > LogError.f_Warning("Failed to destroy encryption sensor reporter");

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
