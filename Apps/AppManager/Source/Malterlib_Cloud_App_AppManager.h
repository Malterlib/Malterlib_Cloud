// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedDaemon>
#include <Mib/Process/ProcessLaunchActor>
#include <Mib/Cloud/KeyManager>
#include <Mib/Cloud/VersionManager>
#include <Mib/Concurrency/DistributedAppInterface>
#include <Mib/Concurrency/DistributedAppInterfaceLaunch>
#include <Mib/Cloud/AppManager>
#include <Mib/Cloud/BackupManagerClient>
#include <Mib/Security/UniqueUserGroup>

#include "Malterlib_Cloud_App_AppManager_CoordinationInterface.h"

namespace NMib::NCloud::NAppManager
{
	struct CAppManagerActor : public CDistributedAppActor
	{
		CAppManagerActor();
		
	private:
		using EUpdateStage = CAppManagerInterface::EUpdateStage;
		struct CFirstApplicationUpdate;
		
		enum EUpdateScript
		{
			EUpdateScript_PreUpdate
			, EUpdateScript_PostUpdate 
			, EUpdateScript_PostLaunch
			, EUpdateScript_OnError
		};
		
		struct CUpdateScripts
		{
			CStr m_PreUpdate;
 			CStr m_PostUpdate;
 			CStr m_PostLaunch;
			CStr m_OnError;
			
			CStr const &f_GetScript(EUpdateScript _Script) const;
			CStr f_GetName(EUpdateScript _Script) const;
		};
		
		enum EApplicationSetting
		{
			EApplicationSetting_None = 0
			, EApplicationSetting_Executable = DBit(0)
			, EApplicationSetting_ExecutableParameters = DBit(1)
			, EApplicationSetting_RunAsUser = DBit(2)
			, EApplicationSetting_RunAsGroup = DBit(3)
			, EApplicationSetting_VersionManagerApplication = DBit(4)
			, EApplicationSetting_AutoUpdateTags = DBit(5)
			, EApplicationSetting_AutoUpdateBranches = DBit(6)
			, EApplicationSetting_UpdateScript_PreUpdate = DBit(7)
			, EApplicationSetting_UpdateScript_PostUpdate = DBit(8)
			, EApplicationSetting_UpdateScript_PostLaunch = DBit(9)
			, EApplicationSetting_UpdateScript_OnError = DBit(10)
			, EApplicationSetting_SelfUpdateSource = DBit(11)
			, EApplicationSetting_EncryptionStorage = DBit(12)
			, EApplicationSetting_ParentApplication = DBit(13)
			, EApplicationSetting_EncryptionFileSystem = DBit(14)
			, EApplicationSetting_UpdateGroup = DBit(15)
			, EApplicationSetting_DistributedApp = DBit(16)
			, EApplicationSetting_Dependencies = DBit(17)
			, EApplicationSetting_StopOnDependencyFailure = DBit(18)
			, EApplicationSetting_BackupIncludeWildcards = DBit(19)
			, EApplicationSetting_BackupExcludeWildcards = DBit(20)
			, EApplicationSetting_BackupAddSyncFlagsWildcards = DBit(21)
			, EApplicationSetting_BackupRemoveSyncFlagsWildcards = DBit(22)
			, EApplicationSetting_BackupNewBackupInterval = DBit(23)
			, EApplicationSetting_BackupEnabled = DBit(24)
			, EApplicationSetting_RunAsUserPassword = DBit(25)

			, EApplicationSetting_NeedUpdateSettings
			= EApplicationSetting_Executable
			| EApplicationSetting_ExecutableParameters 
			| EApplicationSetting_RunAsUser 
			| EApplicationSetting_RunAsGroup
			| EApplicationSetting_SelfUpdateSource
		};

		enum EStopFlag
		{
			EStopFlag_None = 0
			, EStopFlag_CloseEncryption = DBit(0)
			, EStopFlag_AutoStart = DBit(1)
			, EStopFlag_PreventLaunchUser = DBit(2)
			, EStopFlag_PreventLaunchUpdate = DBit(3)
		};
		
		struct CApplicationSettings
		{
			// Settings that can on never change
			CStr m_EncryptionStorage;
			CStr m_ParentApplication;
			CStr m_EncryptionFileSystem;

			// Settings that can be updated from version information
			CStr m_Executable; 
			CStr m_RunAsUser; 
			CStr m_RunAsGroup;
			TCVector<CStr> m_ExecutableParameters;
			bool m_bDistributedApp = false;
			
			NContainer::TCMap<NStr::CStr, CDirectoryManifestConfig::CDestination> m_Backup_IncludeWildcards;
			NContainer::TCSet<NStr::CStr> m_Backup_ExcludeWildcards;
			NContainer::TCMap<NStr::CStr, EDirectoryManifestSyncFlag> m_Backup_AddSyncFlagsWildcards;
			NContainer::TCMap<NStr::CStr, EDirectoryManifestSyncFlag> m_Backup_RemoveSyncFlagsWildcards;
			NTime::CTimeSpan m_Backup_NewBackupInterval = NTime::CTimeSpanConvert::fs_CreateDaySpan(1);
			
			// Settings that can be updated by app manager (command line or protocol)
			TCSet<CStr> m_Dependencies;
			CStr m_VersionManagerApplication;
			CStr m_UpdateGroup;
			TCSet<CStr> m_AutoUpdateTags;
			TCSet<CStr> m_AutoUpdateBranches;
			CUpdateScripts m_UpdateScripts;
#ifdef DPlatformFamily_Windows
			CStrSecure m_RunAsUserPassword;
#endif
			bool m_bAutoUpdate = false;
			bool m_bSelfUpdateSource = false;
			bool m_bStopOnDependencyFailure = true;
			bool m_bBackupEnabled = false;

			bool f_ParseSettings(CEJSON const &_Params, EApplicationSetting &o_ChangedSettings, CStr &o_Error, bool _bRelaxed);
			void f_ApplySettings(EApplicationSetting _ChangedSettings, CApplicationSettings const &_Source);
			void f_FromVersionInfo(CVersionManager::CVersionInformation const &_Info, EApplicationSetting &o_ChangedSettings);
			void f_FromInterfaceSettings(CAppManagerInterface::CApplicationSettings const &_Settings, EApplicationSetting &o_ChangedSettings);
			void f_FromInterfaceAdd(CAppManagerInterface::CApplicationAdd const &_Settings, EApplicationSetting &o_ChangedSettings);
			EApplicationSetting f_ChangedSettings(CApplicationSettings const &_Other) const;
			bool f_Validate(CStr &o_Error) const;
		};

		struct CApplication : public TCSharedPointerIntrusiveBase<>
		{
			CApplication(CStr const &_Name, CAppManagerActor *_pThis)
				: m_Name(_Name)
				, m_pThis(_pThis)
			{
			}

			void f_Clear();
			void f_Delete();
			void f_AbortPendingLaunches();
			
			TCDispatchedActorCall<uint32> f_Stop(EStopFlag _Flags);
			TCDispatchedActorCall<uint32> f_CloseEncryption(uint32 _Status);
			
			CStr f_GetDirectory();
			
			COnScopeExitShared f_SetInProgress();
			
			bool f_NeedsEncryption() const;
			bool f_IsChildApp() const;
			bool f_IsInProgress() const;
			bool f_DependenciesSatisfied(CStr &o_State) const;
			bool f_IsLaunched() const;
			
			TCVector<TCSharedPointer<CApplication>> f_GetDependents() const;
			
			EDistributedAppUpdateType f_GetUpdateType() const;
			CAppManagerCoordinationInterface::CAppInfo f_GetRemoteAppInfo() const;

			CStr const m_Name;

			CApplicationSettings m_Settings;
			
			CStr m_AssociatedHostID;
			TCDistributedActorInterface<CDistributedAppInterfaceClient> m_AppInterface;
			mint m_AppInterfaceAssignSequence = 0;
			CDistributedAppInterfaceServer::CRegisterInfo m_RegisterInfo;

			// Specific version state
			TCVector<CStr> m_Files;
			CVersionManager::CVersionIDAndPlatform m_LastInstalledVersion;
			CVersionManager::CVersionInformation m_LastInstalledVersionInfo;

			CVersionManager::CVersionIDAndPlatform m_LastInstalledVersionFinished;
			CVersionManager::CVersionInformation m_LastInstalledVersionInfoFinished;

			CVersionManager::CVersionIDAndPlatform m_LastTriedInstalledVersion;
			CVersionManager::CVersionInformation m_LastTriedInstalledVersionInfo;

			CVersionManager::CVersionIDAndPlatform m_LastFailedInstalledVersion;
			CTime m_LastFailedInstalledVersionTime;
			uint32 m_LastFailedInstalledVersionRetrySequence = 0;

			EUpdateStage m_WantUpdateStage = EUpdateStage::EUpdateStage_None;
			EUpdateStage m_UpdateStage = EUpdateStage::EUpdateStage_None;
			uint64 m_UpdateStartSequence = TCLimitsInt<uint64>::mc_Max;
			
			// State
			bool m_bPreventLaunch_User = false;
			bool m_bPreventLaunch_Update = false;
			bool m_bPreventLaunch_DelayAfterFailure = false;
			bool m_bDeleted = false;
			bool m_bStopped = false;
			bool m_bAutoStart = false;
			bool m_bOperationInProgress = false;
			bool m_bEncryptionOpened = false;
			bool m_bLaunching = false;
			bool m_bLaunched = false;
			bool m_bJustUpdated = false;
			
			TCVector<TCFunctionMovable<void ()>> m_OnRegisterDistributedApp;
			TCVector<TCFunctionMovable<void ()>> m_OnStartedDistributedApp;

			TCLinkedList<TCFunction <void (bool _bAborted)>> m_OnLaunchFinished;
			
			CStr m_LaunchState;
			CStr m_LastStdErr;
			CStr m_LastError;
			
			TCActor<CDistributedAppInterfaceLaunchActor> m_ProcessLaunch;
			CActorSubscription m_ProcessLaunchSubscription;
			
			CApplication *m_pParentApplication = nullptr;
			DLinkDS_Link(CApplication, m_ChildrenLink);
			DLinkDS_List(CApplication, m_ChildrenLink) m_Children;
			
			CAppManagerActor *m_pThis;
			
			TCActor<CBackupManagerClient> m_BackupClient;
		};
		
		struct CBashScriptOutput
		{
			CStr m_StdOut;
			CStr m_StdErr;
			uint32 m_Status = 1;
		};

		
		struct CVersionManagerApplication;
		struct CVersionManagerState;
		
		struct CVersionManagerVersion
		{
			struct CCompareApplicationByTime
			{
				bool operator()(CVersionManagerVersion const &_Left, CVersionManagerVersion const &_Right) const;
			};
			
			struct CCompareApplication
			{
				bool operator()(CVersionManagerVersion const &_Left, CVersionManagerVersion const &_Right) const;
				bool operator()(CVersionManager::CVersionIDAndPlatform const &_Left, CVersionManagerVersion const &_Right) const;
				bool operator()(CVersionManagerVersion const &_Left, CVersionManager::CVersionIDAndPlatform const &_Right) const;
			};

			CVersionManagerVersion(CVersionManagerState *_pVersionManager);
			~CVersionManagerVersion();
			void f_SetApplication(CVersionManagerApplication *_pApplication);
			CVersionManager::CVersionIDAndPlatform const &f_GetVersionID() const;
			
			CVersionManagerState *m_pVersionManager;
			CVersionManagerApplication *m_pApplication = nullptr;
			CVersionManager::CVersionInformation m_VersionInfo;
			DIntrusiveLink(CVersionManagerVersion, TCAVLLink<>, m_ApplicationTimeLink);
			DIntrusiveLink(CVersionManagerVersion, TCAVLLink<>, m_ApplicationLink);
		};
		
		struct CVersionManagerApplication
		{
			CVersionManagerApplication(CAppManagerActor &_This);
			CStr const &f_GetApplicationName() const
			{
				return TCMap<CStr, CVersionManagerApplication>::fs_GetKey(*this);
			}
			
			TCAVLTree<CVersionManagerVersion::CLinkTraits_m_ApplicationTimeLink, CVersionManagerVersion::CCompareApplicationByTime> m_VersionsByTime;
			TCAVLTree<CVersionManagerVersion::CLinkTraits_m_ApplicationLink, CVersionManagerVersion::CCompareApplication> m_Versions;
			CAppManagerActor &m_This;
		};
		
		struct CVersionManagerState
		{
			TCDistributedActor<CVersionManager> const &f_GetManager() const
			{
				return TCMap<TCDistributedActor<CVersionManager>, CVersionManagerState>::fs_GetKey(*this); 
			}
			
			TCMap<CStr, TCMap<CVersionManager::CVersionIDAndPlatform, CVersionManagerVersion>> m_Versions;
			
			CTrustedActorInfo m_HostInfo;
			CActorSubscription m_Subscription;
			mint m_SubscribeSequence = 0;
		};
		
		struct CVersionManagerDownloadState
		{
			TCActor<CFileTransferReceive> m_DownloadVersionReceive;
			CActorSubscription m_Subscription;
		};
		
		struct CDistributedAppInterfaceServerImplementation : public CDistributedAppInterfaceServer
		{
			TCContinuation<TCActorSubscriptionWithID<>> f_RegisterDistributedApp
				(
					TCDistributedActorInterfaceWithID<CDistributedAppInterfaceClient> &&_ClientInterface
					, TCDistributedActorInterfaceWithID<CDistributedActorTrustManagerInterface> &&_TrustInterface
					, CRegisterInfo const &_RegisterInfo
				) override
			;

			CAppManagerActor *m_pThis = nullptr;
		};
		
		struct CAppManagerInterfaceImplementation : public CAppManagerInterface
		{
			TCContinuation<CVersionsAvailableForUpdate> f_GetAvailableVersions(CStr const &_Application) override;
			
			TCContinuation<void> f_Add(CStr const &_Name, CApplicationAdd const &_Add, CApplicationSettings const &_Settings) override;
			TCContinuation<void> f_Remove(CStr const &_Name) override;

			TCContinuation<void> f_Update(CStr const &_Name, CApplicationUpdate const &_Update) override;
			
			TCContinuation<void> f_Start(CStr const &_Name) override;
			TCContinuation<void> f_Stop(CStr const &_Name) override;
			TCContinuation<void> f_Restart(CStr const &_Name) override;

			TCContinuation<void> f_ChangeSettings
				(
					CStr const &_Name
					, CApplicationChangeSettings const &_ChangeSettings
					, CApplicationSettings const &_Settings
				) override
			;

			TCContinuation<TCMap<CStr, CApplicationInfo>> f_GetInstalled() override;
			
			auto f_SubscribeUpdateNotifications(NConcurrency::TCActorFunctorWithID<NConcurrency::TCContinuation<void> (CUpdateNotification const &_Notification)> &&_fOnNotification) 
				-> NConcurrency::TCContinuation<NConcurrency::TCActorSubscriptionWithID<>> override
			; 

			CAppManagerActor *m_pThis = nullptr;
		};
		
		struct CUpdateNotificationSubscription
		{
			TCActorFunctor<NConcurrency::TCContinuation<void> (CAppManagerInterface::CUpdateNotification const &_Notification)> m_fOnUpdate;
			CCallingHostInfo m_CallingHostInfo;
		};
		
		enum EEncryptOperation
		{
			EEncryptOperation_Setup
			, EEncryptOperation_Open
			, EEncryptOperation_Close
		};
		
		enum EFindVersionFlag
		{
			EFindVersionFlag_None = 0
			, EFindVersionFlag_RetryFailed = DBit(0)
			, EFindVersionFlag_ForAdd = DBit(1)
		};
		
		struct CAppManagerCoordinationInterfaceImplementation : public CAppManagerCoordinationInterface
		{
			TCContinuation<void> f_RemoveKnownHost(CStr const &_Group, CStr const &_Application, CStr const &_HostID) override;
			auto f_SubscribeToAppChanges(TCActorFunctorWithID<TCContinuation<void> (TCVector<CAppChange> const &_Changes, bool _bInitial)> &&_fOnChange)
				-> TCContinuation<TCActorSubscriptionWithID<>> override
			;

			CAppManagerActor *m_pThis = nullptr;
		};
		
		struct CRemoteAppManagerApp
		{
			CAppManagerCoordinationInterface::CAppInfo m_AppInfo;
		};
		
		struct CRemoteApplicationKey
		{
			CRemoteApplicationKey() = default;
			
			CRemoteApplicationKey(CAppManagerCoordinationInterface::CAppInfo const &_AppInfo)
				: m_Group(_AppInfo.m_Group)
				, m_VersionManagerApplication(_AppInfo.m_VersionManagerApplication)
			{
			}
			
			CRemoteApplicationKey(CApplicationSettings const &_Settings)
				: m_Group(_Settings.m_UpdateGroup)
				, m_VersionManagerApplication(_Settings.m_VersionManagerApplication)
			{
			}
			
			bool operator < (CRemoteApplicationKey const &_Right) const
			{
				return fg_TupleReferences(m_Group, m_VersionManagerApplication) < fg_TupleReferences(_Right.m_Group, _Right.m_VersionManagerApplication); 
			}

			bool operator == (CRemoteApplicationKey const &_Right) const
			{
				return m_Group == _Right.m_Group && m_VersionManagerApplication == _Right.m_VersionManagerApplication;
			}
			
			template <typename tf_CStr>
			void f_Format(tf_CStr &o_Str) const
			{
				o_Str += typename tf_CStr::CFormat("{}/{}") << m_Group << m_VersionManagerApplication;
			}

			CStr m_Group;
			CStr m_VersionManagerApplication;
		};
		
		struct CRemoteAppManager
		{
			struct CCompareActor
			{
				auto &operator () (CRemoteAppManager const &_Node) const
				{
					return _Node.m_Actor;
				}
			};
			
			CStr const &f_GetHostID() const
			{
				return TCMap<CStr, CRemoteAppManager>::fs_GetKey(*this);
			}

			void f_Clear();
			
			DIntrusiveLink(CRemoteAppManager, TCAVLLink<>, m_ByActorLink);
			
			TCDistributedActor<CAppManagerCoordinationInterface> m_Actor;
			CTrustedActorInfo m_HostInfo;
			TCActorFunctor<TCContinuation<void> (TCVector<CAppManagerCoordinationInterface::CAppChange> const &_Changes, bool _bInitial)> m_fOnChange;
			CActorSubscription m_OnChangeSubscription;
			mint m_iOnChangeSubscriptionSequence = 0;
			bool m_bInitialStateReceived = false;
			
			TCMap<CStr, CRemoteAppManagerApp> m_AppInfos;
			TCMap<CRemoteApplicationKey, TCSet<CStr>> m_KnownApplications;
		};
		
		struct CUpdateApplicationState : public TCSharedPointerIntrusiveBase<ESharedPointerOption_SupportWeakPointer>
		{
			CUpdateApplicationState() = default;
			CUpdateApplicationState(CUpdateApplicationState const &) = delete;
			CUpdateApplicationState(CUpdateApplicationState &&) = delete;
			
			template <typename tf_CType>
			bool f_CheckAbort(TCContinuation<tf_CType> const &o_Error)
			{
				if (m_pApplication->m_bDeleted)
				{
					if (!o_Error.f_IsSet())
						o_Error.f_SetException(DMibErrorInstance("Application has been deleted, aborting"));
					return true;
				}
				if (m_bCancel)
				{
					if (!o_Error.f_IsSet())
						o_Error.f_SetException(DMibErrorInstance("Update was cancelled"));
					return true;
				}
				if (m_bCancelOnAppManagerStop)
				{
					if (!o_Error.f_IsSet())
						o_Error.f_SetException(DMibErrorInstance("AppManager stopped"));
					return true;
				}
				return false;
			}
			
			TCSharedPointer<CApplication> m_pApplication;
			TCFunction<void (CStr const &_Info)> m_fOnInfo;
			TCFunction <void ()> m_fUpdateVersionInfo;
			COnScopeExitShared m_pDownloadDirectoryCleanup;
			COnScopeExitShared m_pTemporaryDirectoryCleanup;
			COnScopeExitShared m_pInProgressScope;
			COnScopeExitShared m_pCleanupStateMap;
			CStr m_SourcePath;
			TCSharedPointer<CApplicationSettings> m_pNewSettings;
			TCSet<CStr> m_AllowSourceExist;
			CAppManagerInterface::CVersionIDAndPlatform m_VersionID;
			CTime m_VersionTime;
			uint32 m_VersionRetrySequence = 0;
			TCSet<CStr> m_RequiredTags;
			TCSharedPointer<CVersionManager::CVersionInformation> m_pVersionInfo;
			TCSharedPointer<NTime::CClock> m_pClock;
			TCVector<CStr> m_Files;
			CDistributedAppAuditor m_Auditor;
			bool m_bDryRun = false;
			bool m_bUpdateSettings = true;
			bool m_bUnencrypted = false;
			bool m_bCancel = false;
			bool m_bCancelOnAppManagerStop = false;
			bool m_bSetLastTried = false;
		};

		struct COnAppUpdateInfoChange
		{
			CActorSubscription m_TimerSubscription;
			CActorSubscription m_DisconnectTimerSubscription;
			TCFunctionMutable<void ()> m_fOnChanged;
		};
		
		enum EWaitStageFlag
		{
			EWaitStageFlag_None = 0
			, EWaitStageFlag_IgnoreFailed = DBit(0)
			, EWaitStageFlag_DisallowInProgressStates = DBit(1)
		};

		void fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine) override;
		
		static CStr fsp_RunTool
			(
				CStr const &_Description
				, CStr const &_Tool
				, CStr const &_WorkingDir
				, TCVector<CStr> const &_Arguments
			)
		;
		TCContinuation<CBashScriptOutput> fp_RunBashScript
			(
				CStr const &_Script
				, CStr const &_Description
				, TCMap<CStr, CStr> const &_Environment
				, TCFunction<void (CStr const &_Output, TCActor<CProcessLaunchActor> const &_LaunchActor)> const &_fOnStdOutput
			)
		;
		
		TCContinuation<void> fp_StartApp(NEncoding::CEJSON const &_Params) override;
		TCContinuation<void> fp_StopApp() override;
		TCContinuation<void> fp_ReadState();
		void fp_InitApplications();
		void fp_OnApplicationAdded(TCSharedPointer<CApplication> const &_pApplication);

		TCContinuation<bool> fp_LaunchApp(TCSharedPointer<CApplication> const &_pApplication, bool _bOpenEncryption);
		TCContinuation<bool> fp_LaunchAppInternal(TCSharedPointer<CApplication> const &_pApplication, bool _bOpenEncryption);
		void fp_ScheduleRelaunchApp(TCSharedPointer<CApplication> const &_pApplication);
		static void fsp_CreateApplicationUserGroup
			(
			 	CApplicationSettings const &_Settings
			 	, TCFunction<void (CStr const &_Info)> const &_fLogInfo
			 	, CStr const &_HomeDir
			 	, TCSharedPointer<CUniqueUserGroup> const &_pUniqueUserGroup
			)
		;
		static void fsp_UpdateApplicationFiles
			(
			 	CStr const &_ApplicationDir
			 	, TCSharedPointer<CApplication> const &_pApplication
			 	, TCVector<CStr> const &_Files
			 	, TCSharedPointer<CUniqueUserGroup> const &_pUniqueUserGroup
			)
		;
		TCContinuation<void> fp_UpdateApplicationJSON(TCSharedPointer<CApplication> const &_pApplication);
		TCContinuation<void> fp_RunUpdateScript
			(
				TCSharedPointer<CApplication> const &_pApplication
				, EUpdateScript _Script
				, CStr const &_Param
				, CVersionManager::CVersionIDAndPlatform const &_VersionID
				, CVersionManager::CVersionInformation *_pVersionInformation
				, fp64 _TimeSinceUpdateStart 
			)
		;
		TCContinuation<bool> fp_SelfUpdate(TCSharedPointer<CApplication> const &_pApplication);
		
		TCContinuation<uint32> fp_CommandLine_EnumApplications(CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCContinuation<uint32> fp_CommandLine_AddApplication(CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCContinuation<uint32> fp_CommandLine_ChangeApplicationSettings(CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCContinuation<uint32> fp_CommandLine_RemoveApplication(CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCContinuation<uint32> fp_CommandLine_UpdateApplication(CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCContinuation<uint32> fp_CommandLine_StartApplication(CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCContinuation<uint32> fp_CommandLine_StopApplication(CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCContinuation<uint32> fp_CommandLine_RestartApplication(CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		
		TCContinuation<uint32> fp_CommandLine_ListAvailableVersions(CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine);

		TCContinuation<uint32> fp_CommandLine_RemoveKnownHost(CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine);

		TCContinuation<uint32> fp_CommandLine_CancelAllUpdates(CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		
		TCContinuation<void> fp_AddApplication
			(
				CStr const &_Name
				, CApplicationSettings const &_Settings
				, EApplicationSetting _ChangedSettings
				, bool _bForceOverwrite
				, bool _bForceInstall
				, bool _bSettingsFromVersionInfo
				, TCFunction<void (CStr const &_Info)> &&_fOnInfo
				, CStr const &_FromLocalFile
				, TCOptional<CVersionManager::CVersionIDAndPlatform> const &_Version 
			)
		;
		
		TCContinuation<void> fp_ChangeApplicationSettings
			(
				CStr const &_Name
				, CApplicationSettings const &_Settings
				, EApplicationSetting _ChangedSettings
				, bool _bUpdateFromVersionInfo
				, bool _bForce
				, TCFunction<void (CStr const &_Info)> &&_fOnInfo
			)
		;
		
		TCContinuation<void> fp_UpdateApplication
			(
				CStr const &_Name
				, CAppManagerInterface::CApplicationUpdate const &_Update
		 		, CVersionManager::CVersionInformation const &_VersionInfo
				, CStr const &_FromFileName
				, TCFunction<void (CStr const &_Info)> &&_fOnInfo
				, bool _bCheckPermissions = true
			)
		;

		TCContinuation<void> fp_UpdateApplicationRunProcess(TCSharedPointerSupportWeak<CUpdateApplicationState> const &_pState);
		TCContinuation<void> fp_UpdateApplication_DownloadVersion(TCSharedPointerSupportWeak<CUpdateApplicationState> const &_pState);
		TCContinuation<void> fp_UpdateApplication_Unpack(TCSharedPointerSupportWeak<CUpdateApplicationState> const &_pState);
		TCContinuation<void> fp_UpdateApplication_StopOldApp(TCSharedPointerSupportWeak<CUpdateApplicationState> const &_pState);
		TCContinuation<void> fp_UpdateApplication_PreUpdate(TCSharedPointerSupportWeak<CUpdateApplicationState> const &_pState);
		TCContinuation<void> fp_UpdateApplication_UpdateApplicationFiles(TCSharedPointerSupportWeak<CUpdateApplicationState> const &_pState);
		TCContinuation<void> fp_UpdateApplication_SaveApplicationState(TCSharedPointerSupportWeak<CUpdateApplicationState> const &_pState);
		TCContinuation<void> fp_UpdateApplication_PostUpdate(TCSharedPointerSupportWeak<CUpdateApplicationState> const &_pState);
		TCContinuation<bool> fp_UpdateApplication_StartNewApp(TCSharedPointerSupportWeak<CUpdateApplicationState> const &_pState);
		TCContinuation<void> fp_UpdateApplication_DeferToNextRestart
			(
				 TCSharedPointerSupportWeak<CUpdateApplicationState> const &_pState
				 , TCSharedPointer<CCanDestroyTracker> const &_pCanDestroy
			)
		;
		TCContinuation<void> fp_UpdateApplication_PostLaunch(TCSharedPointerSupportWeak<CUpdateApplicationState> const &_pState);
		
		TCContinuation<void> fp_CancelAllApplicationUpdatesOnStopAppManager();
		
		void fp_StartPendingSelfUpdateReporting(CStr const &_Name, CVersionManager::CVersionIDAndPlatform const &_VersionID, CTime const &_VersionTime, uint32 _VersionRetrySequence);
		
		static void fsp_UpdateAttributes(CStr const &_File);
		static CStr fsp_UnpackApplication
			(
				CStr const &_Source
				, CStr const &_Destination
				, CStr const &_ApplicationName
				, CApplicationSettings const &_Settings 
				, TCVector<CStr> &o_Files
				, TCSet<CStr> const &_AllowExist
				, bool _bForceInstall
			 	, TCSharedPointer<CUniqueUserGroup> const &_pUniqueUserGroup
			)
		;
		TCContinuation<void> fp_ChangeEncryption(TCSharedPointer<CApplication> const &_pApplication, EEncryptOperation _Operation, bool _bForceOverwrite);
		
		CStr fp_GetApplicationStopErrors(TCAsyncResult<uint32> const &_Result, CStr const &_Name);
		void fp_KeyManagerAvailable();
		
		void fp_VersionManagerResubscribeAll();
		void fp_VersionManagerSubscribe(CVersionManagerState &_VersionManagerState);
		void fp_VersionManagerAdded(TCDistributedActor<CVersionManager> const &_VersionManager, CTrustedActorInfo const &_Info);
		void fp_VersionManagerRemoved(TCWeakDistributedActor<CActor> const &_VersionManager);
		
		TCContinuation<CVersionManager::CVersionInformation> fp_DownloadApplicationFromManager
			(
				TCDistributedActor<CVersionManager> const &_Manager
				, CStr const &_ApplicationName
				, CVersionManager::CVersionIDAndPlatform const &_VersionID
				, CStr const &_DestinationDir
			)
		;
		
		TCContinuation<CVersionManager::CVersionInformation> fp_DownloadApplication
			(
				CStr const &_ApplicationName
				, CVersionManager::CVersionIDAndPlatform const &_VersionID
				, CStr const &_DestinationDir
			)
		;
		
		void fp_AutoUpdate_Update();

		CVersionManager::CVersionIDAndPlatform fp_FindVersion
			(
				TCSharedPointer<CApplication> const &_pApplication
				, TCSet<CStr> const &_RequiredTags
				, TCSet<CStr> const &_AllowedBranches
				, CStr const &_Platform 
				, CStr &o_Error
				, EFindVersionFlag _Flags
				, CVersionManager::CVersionInformation &o_VersionInfo
			)
		;

		TCContinuation<void> fp_SetupAppManagerInterfacePermissions();
		TCContinuation<void> fp_SubscribePermissions();
		TCContinuation<void> fp_RegisterPermissions();		
		TCContinuation<void> fp_RegisterApplicationPermissions(TCSharedPointer<CApplication> const &_pApplication);		
		TCContinuation<void> fp_UnregisterApplicationPermissions(TCSharedPointer<CApplication> const &_pApplication);
		
		TCContinuation<void> fp_PublishAppInterface();		
		TCContinuation<void> fp_PublishAppManagerInterface();
		
		TCContinuation<void> fp_OnUpdateEvent
			(
				TCSharedPointerSupportWeak<CUpdateApplicationState> const &_pState
				, EUpdateStage _Stage
				, NStr::CStr const &_Message
			)
		;

		TCContinuation<void> fp_PublishCoordinationInterface();
		TCContinuation<void> fp_SubscribeCoordinationInterface();
		void fp_NewRemoteAppManager(CRemoteAppManager &_AppManager);
		void fp_NewRemoteKnownApplication(CRemoteApplicationKey const &_RemoteKey, CStr const &_HostID);
		void fp_SendInitialInfoToRemoteAppManager(CRemoteAppManager &_AppManager);
		void fp_OnAppUpdateInfoChange(TCSharedPointer<CApplication> const &_pApplication);
		void fp_OnAppUpdateInfoChange();
		void fp_SendAppToRemoteAppManagers(TCSharedPointer<CApplication> const &_pApplication);
		void fp_SendRemovedAppToRemoteAppManagers(TCSharedPointer<CApplication> const &_pApplication);
		void fp_BroadcastRemoteAppChange(CAppManagerCoordinationInterface::CAppChange &&_Change);
		
		void fp_AppLaunchStateChanged(TCSharedPointer<CApplication> const &_pApplication, CStr const &_State);
		void fp_AppEncryptionStateChanged(TCSharedPointer<CApplication> const &_pApplication, bool _bEncrypted);
		
		TCContinuation<void> fp_Coordination_WaitForOurAppsTurnToUpdate(TCSharedPointerSupportWeak<CUpdateApplicationState> const &_pState);
		TCContinuation<void> fp_Coordination_OneAtATime_WaitForOurTurnToUpdate(TCSharedPointerSupportWeak<CUpdateApplicationState> const &_pState);
		TCContinuation<void> fp_Coordination_WaitForAllToReachWantUpdateStage
			(
				TCSharedPointerSupportWeak<CUpdateApplicationState> const &_pState
				, EUpdateStage _Stage
				, fp64 _Timeout
				, EWaitStageFlag _Flags
				, TCFunctionMutable<bool ()> &&_fEvalState = {}
			)
		;
		TCContinuation<void> fp_Coordination_WaitForState
			(
				TCSharedPointerSupportWeak<CUpdateApplicationState> const &_pState
				, fp64 _Timeout
				, TCFunctionMutable<bool (TCContinuation<void> &_Continuation)> &&_fEvalState
				, CStr const &_RemoteFailError
				, CStr const &_TimeoutError
				, CStr const &_DisconnectedError
				, bool _bIgnoreFailed = false
			)
		;
		static ch8 const *fsp_UpdateStageToStr(EUpdateStage _Stage);
		static ch8 const *fsp_UpdateTypeToStr(EDistributedAppUpdateType _UpdateType);

		TCContinuation<void> fp_SetupLimits();
		void fp_UpdateLimits();
		
		bool fp_AutoStartApp(TCSharedPointer<CApplication> const &_pApplication);
		void fp_UpdateApplicationDependencies();
		
		void fp_InitialStartupFailed(CExceptionPointer const &_pException);
		TCContinuation<void> fp_ClearPreventLaunch(TCSharedPointer<CApplication> const &_pApplication);
		
		void fp_ApplicationStartBackup(TCSharedPointer<CApplication> const &_pApplication);

		CStr fp_GetRunAsUser(CApplicationSettings const &_Settings) const;
		CStr fp_GetRunAsGroup(CApplicationSettings const &_Settings) const;
		CStr fp_TransformUserGroup(CStr const &_UserName) const;

#ifdef DPlatformFamily_Windows
		TCSharedPointer<CUniqueUserGroup> mp_pUniqueUserGroup = fg_Construct("C:/M");
#else
		TCSharedPointer<CUniqueUserGroup> mp_pUniqueUserGroup = fg_Construct("/M");
#endif

		TCMap<CStr, TCSharedPointer<CApplication>> mp_Applications;
		TCActor<CSeparateThreadActor> mp_FileActor;
		TCTrustedActorSubscription<CKeyManager> mp_KeyManagerSubscription;
		TCTrustedActorSubscription<CVersionManager> mp_VersionManagerSubscription;
		TCSet<CStr> mp_KnownPlatforms;
		bool mp_bPendingAutoUpdate = false;
		bool mp_bLogLaunchesToStdErr = false;
		bool mp_bPendingSelfUpdateInProgress = false;

		TCLinkedList<CVersionManagerDownloadState> mp_Downloads;
		
		TCMap<CStr, CVersionManagerApplication> mp_VersionManagerApplications;
		TCMap<TCDistributedActor<CVersionManager>, CVersionManagerState> mp_VersionManagers;
		
		TCDelegatedActorInterface<CDistributedAppInterfaceServerImplementation> mp_AppInterfaceServer;
		TCDelegatedActorInterface<CAppManagerInterfaceImplementation> mp_AppManagerInterface;
		TCDelegatedActorInterface<CAppManagerCoordinationInterfaceImplementation> mp_AppManagerCoordinationInterface;
		
		CTrustedPermissionSubscription mp_Permissions;
		
		TCMap<CStr, CUpdateNotificationSubscription> mp_UpdateNotificationSubscriptions;
		TCTrustedActorSubscription<CAppManagerCoordinationInterface> mp_RemoteAppManagers;

		TCMap<CStr, CRemoteAppManager> mp_RemoteAppManagerState;
		TCAVLTree<CRemoteAppManager::CLinkTraits_m_ByActorLink, CRemoteAppManager::CCompareActor> mp_RemoteAppManagerStateByActor;
		TCMap<CRemoteApplicationKey, TCSet<CStr>> mp_KnownRemoteApplications;
		TCSet<TCSharedPointerSupportWeak<COnAppUpdateInfoChange>> mp_OnAppUpdateInfoChange;
		
		TCSharedPointer<CCanDestroyTracker> mp_pCanDestroy = fg_Construct();
		TCVector<TCActor<CProcessLaunchActor>> mp_LaunchActors;

		TCContinuation<void> mp_InitialStartupResult;
		mint mp_PendingAutoLaunches = 0;
		
		uint64 mp_AppStageChangeSequence = 0;
		
		TCSet<TCWeakPointer<CUpdateApplicationState>> mp_RunningUpdates;
		
		TCContinuation<void> mp_CancelRunningUpdatesOnStopAppManagerContinuation;
	};

	CStr fg_ConcatOutput(CStr const &_StdOut, CStr const &_StdErr);
}
