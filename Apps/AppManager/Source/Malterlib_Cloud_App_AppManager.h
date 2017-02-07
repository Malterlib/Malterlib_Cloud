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

#include "Malterlib_Cloud_App_AppManager_CoordinationInterface.h"

namespace NMib::NCloud::NAppManager
{
	struct CAppManagerActor : public CDistributedAppActor
	{
		CAppManagerActor();
		
	private:
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
			
			, EApplicationSetting_NeedUpdateSettings
			= EApplicationSetting_Executable
			| EApplicationSetting_ExecutableParameters 
			| EApplicationSetting_RunAsUser 
			| EApplicationSetting_RunAsGroup
			| EApplicationSetting_SelfUpdateSource
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
			
			// Settings that can be updated by app manager (command line or protocol)
			CStr m_VersionManagerApplication;
			CStr m_UpdateGroup;
			TCSet<CStr> m_AutoUpdateTags;
			TCSet<CStr> m_AutoUpdateBranches;
			CUpdateScripts m_UpdateScripts;
			bool m_bAutoUpdate = false;
			bool m_bSelfUpdateSource = false;
			
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
			
			TCDispatchedActorCall<uint32> f_Stop(bool _bCloseEncryption);
			TCDispatchedActorCall<uint32> f_CloseEncryption(uint32 _Status);
			
			CStr f_GetDirectory();
			
			COnScopeExitShared f_SetInProgress();
			
			bool f_NeedsEncryption() const;
			bool f_IsChildApp() const;
			bool f_IsInProgress() const;
			
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

			CVersionManager::CVersionIDAndPlatform m_LastTriedInstalledVersion;
			CVersionManager::CVersionInformation m_LastTriedInstalledVersionInfo;

			CVersionManager::CVersionIDAndPlatform m_WantInstallVersion;
			CAppManagerInterface::EUpdateStage m_UpdateStage = CAppManagerInterface::EUpdateStage_None;
			
			// State
			bool m_bDeleted = false;
			bool m_bStopped = false;
			bool m_bOperationInProgress = false;
			bool m_bEncryptionOpened = false;
			bool m_bLaunching = false;
			bool m_bJustUpdated = false;
			
			TCFunction<void ()> m_fOnRegisterDistributedApp;
			
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
			CStr m_CallingHostID;
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
			bool operator < (CRemoteApplicationKey const &_Right) const
			{
				return fg_TupleReferences(m_Group, m_Application) < fg_TupleReferences(m_Group, m_Application); 
			}

			CStr m_Group;
			CStr m_Application;
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
			
			TCMap<CStr, CRemoteAppManagerApp> m_AppInfos;
			TCMap<CRemoteApplicationKey, TCSet<CStr>> m_KnownApplications;
		};
		
		struct CUpdateApplicationState
		{
			CUpdateApplicationState() = default;
			CUpdateApplicationState(CUpdateApplicationState const &) = delete;
			CUpdateApplicationState(CUpdateApplicationState &&) = delete;
			
			TCSharedPointer<CApplication> m_pApplication;
			TCFunction<void (CStr const &_Info)> m_fOnInfo;
			TCFunction <void ()> m_fUpdateVersionInfo;
			COnScopeExitShared m_pDownloadDirectoryCleanup;
			COnScopeExitShared m_pTemporaryDirectoryCleanup;
			COnScopeExitShared m_pInProgressScope;
			CStr m_SourcePath;
			TCSharedPointer<CApplicationSettings> m_pNewSettings;
			TCSet<CStr> m_AllowSourceExist;
			CVersionManager::CVersionIDAndPlatform m_VersionID;
			TCSet<CStr> m_RequiredTags;
			TCSharedPointer<CVersionManager::CVersionInformation> m_pVersionInfo;
			TCSharedPointer<NTime::CClock> m_pClock;
			TCVector<CStr> m_Files;
			CDistributedAppAuditor m_Auditor;
			bool m_bDryRun = false;
			bool m_bUpdateSettings = true;
			bool m_bUnencrypted = false;
		};
		
		void fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine) override;
		
		static CStr fsp_RunTool
			(
				CStr const &_Description
				, CStr const &_Tool
				, CStr const &_WorkingDir
				, TCVector<CStr> const &_Arguments
				, CStr const &_Home
				, CStr const &_User
				, TCMap<CStr, CStr> const &_Environment = fg_Default()
				, bool _bQuiet = false
			)
		;
		TCContinuation<CBashScriptOutput> fp_RunBashScript
			(
				CStr const &_Script
				, CStr const &_Description
				, CStr const &_Home
				, CStr const &_User
				, TCMap<CStr, CStr> const &_Environment
				, TCFunction<void (CStr const &_Output, TCActor<CProcessLaunchActor> const &_LaunchActor)> const &_fOnStdOutput
			)
		;
		
		TCContinuation<void> fp_StartApp(NEncoding::CEJSON const &_Params) override;
		TCContinuation<void> fp_StopApp() override;
		TCContinuation<void> fp_ReadState();
		void fp_InitApplications();
		void fp_OnApplicationAdded(TCSharedPointer<CApplication> const &_pApplication);
		void fp_DoInitialApplicationLaunch(TCSharedPointer<CApplication> const &_pApplication);

		void fp_LaunchNormalApps();
		void fp_LaunchEncryptedApps();
		TCContinuation<void> fp_LaunchApp(TCSharedPointer<CApplication> const &_pApplication, bool _bOpenEncryption);
		TCContinuation<void> fp_LaunchAppInternal(TCSharedPointer<CApplication> const &_pApplication, bool _bOpenEncryption);
		void fp_ScheduleRelaunchApp(TCSharedPointer<CApplication> const &_pApplication);
		static void fsp_CreateApplicationUserGroup(CApplicationSettings const &_Settings, TCFunction<void (CStr const &_Info)> const &_fLogInfo, CStr const &_HomeDir);
		static void fsp_UpdateApplicationFiles(CStr const &_ApplicationDir, TCSharedPointer<CApplication> const &_pApplication, TCVector<CStr> const &_Files);
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
		void fp_SelfUpdate(TCSharedPointer<CApplication> const &_pApplication);
		
		TCContinuation<CDistributedAppCommandLineResults> fp_CommandLine_EnumApplications(CEJSON const &_Params);
		TCContinuation<CDistributedAppCommandLineResults> fp_CommandLine_AddApplication(CEJSON const &_Params);
		TCContinuation<CDistributedAppCommandLineResults> fp_CommandLine_ChangeApplicationSettings(CEJSON const &_Params);
		TCContinuation<CDistributedAppCommandLineResults> fp_CommandLine_RemoveApplication(CEJSON const &_Params);
		TCContinuation<CDistributedAppCommandLineResults> fp_CommandLine_UpdateApplication(CEJSON const &_Params);
		TCContinuation<CDistributedAppCommandLineResults> fp_CommandLine_StartApplication(CEJSON const &_Params);
		TCContinuation<CDistributedAppCommandLineResults> fp_CommandLine_StopApplication(CEJSON const &_Params);
		TCContinuation<CDistributedAppCommandLineResults> fp_CommandLine_RestartApplication(CEJSON const &_Params);
		
		TCContinuation<CDistributedAppCommandLineResults> fp_CommandLine_ListAvailableVersions(CEJSON const &_Params);

		TCContinuation<CDistributedAppCommandLineResults> fp_CommandLine_RemoveKnownHost(CEJSON const &_Params);
		
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
				, CStr const &_FromFileName
				, TCFunction<void (CStr const &_Info)> &&_fOnInfo
				, bool _bCheckPermissions = true
			)
		;

		TCContinuation<void> fp_UpdateApplicationRunProcess(TCSharedPointer<CUpdateApplicationState> const &_pState);
		TCContinuation<void> fp_UpdateApplication_DownloadVersion(TCSharedPointer<CUpdateApplicationState> const &_pState);
		TCContinuation<void> fp_UpdateApplication_Unpack(TCSharedPointer<CUpdateApplicationState> const &_pState);
		TCContinuation<void> fp_UpdateApplication_StopOldApp(TCSharedPointer<CUpdateApplicationState> const &_pState);
		TCContinuation<void> fp_UpdateApplication_PreUpdate(TCSharedPointer<CUpdateApplicationState> const &_pState);
		TCContinuation<void> fp_UpdateApplication_UpdateApplicationFiles(TCSharedPointer<CUpdateApplicationState> const &_pState);
		TCContinuation<void> fp_UpdateApplication_SaveApplicationState(TCSharedPointer<CUpdateApplicationState> const &_pState);
		TCContinuation<void> fp_UpdateApplication_PostUpdate(TCSharedPointer<CUpdateApplicationState> const &_pState);
		TCContinuation<void> fp_UpdateApplication_StartNewApp(TCSharedPointer<CUpdateApplicationState> const &_pState);
		TCContinuation<void> fp_UpdateApplication_PostLaunch(TCSharedPointer<CUpdateApplicationState> const &_pState);
		
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
				TCSharedPointer<CApplication> const &_pApplication
				, CAppManagerInterface::EUpdateStage _Stage
				, CAppManagerInterface::CVersionIDAndPlatform const &_VersionID
				, NStr::CStr const &_Message
			)
		;

		TCContinuation<void> fp_PublishCoordinationInterface();
		TCContinuation<void> fp_SubscribeCoordinationInterface();
		void fp_NewRemoteAppManager(CRemoteAppManager &_AppManager);
		void fp_NewRemoteKnownApplication(CStr const &_Group, CStr const &_Application, CStr const &_HostID);
		void fp_SendInitialInfoToRemoteAppManager(CRemoteAppManager &_AppManager);
		void fp_RemoteAppInfoChanged(TCSharedPointer<CApplication> const &_pApplication);
		void fp_SendAddedAppToRemoteAppManagers(TCSharedPointer<CApplication> const &_pApplication);
		void fp_SendRemovedAppToRemoteAppManagers(TCSharedPointer<CApplication> const &_pApplication);
		void fp_BroadcastRemoteAppChange(CAppManagerCoordinationInterface::CAppChange &&_Change);
		
		TCMap<CStr, TCSharedPointer<CApplication>> mp_Applications;
		TCActor<CSeparateThreadActor> mp_FileActor;
		TCTrustedActorSubscription<CKeyManager> mp_KeyManagerSubscription;
		TCTrustedActorSubscription<CVersionManager> mp_VersionManagerSubscription;
		TCSet<CStr> mp_KnownPlatforms;
		bool mp_bAppsEncryptedLaunched = false;
		bool mp_bPendingAutoUpdate = false;
		bool mp_bLogLaunchesToStdErr = false;
		
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
	};

	CStr fg_ConcatOutput(CStr const &_StdOut, CStr const &_StdErr);
}
