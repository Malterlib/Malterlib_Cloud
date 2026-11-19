// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Process/ProcessLaunchActor>
#include <Mib/Concurrency/DistributedAppInProcess>
#include <Mib/Cloud/KeyManager>
#include <Mib/Cloud/VersionManager>
#include <Mib/Concurrency/DistributedAppInterface>
#include <Mib/Concurrency/DistributedAppInterfaceLaunch>
#include <Mib/Cloud/AppManager>
#include <Mib/Cloud/BackupManagerClient>
#include <Mib/Cloud/CloudManager>
#include <Mib/Cloud/HostMonitor>
#include <Mib/Security/UniqueUserGroup>
#include <Mib/Concurrency/ActorSequencerActor>
#include <Mib/Concurrency/DistributedAppSensorStoreLocal>
#include <Mib/Concurrency/DistributedAppLogStoreLocal>
#include <Mib/Virtualization/VirtualMachine>

#include "Malterlib_Cloud_App_AppManagerApp.h"
#include "Malterlib_Cloud_App_AppManager_CoordinationInterface.h"
#include "Malterlib_Cloud_App_AppManager_EnvironmentInterface.h"

namespace NMib::NCloud::NAppManager
{
	struct CAppManagerActor : public CDistributedAppActor
	{
		CAppManagerActor();
		~CAppManagerActor();

		static void fs_ApplyEnvironmentHostName(NStr::CStr const &_HostName);

		static constexpr double mc_DefaultAutoUpdateDelay = 15.0;

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
			CStr const &f_GetScript(EUpdateScript _Script) const;
			CStr f_GetName(EUpdateScript _Script) const;

			CStr m_PreUpdate;
			CStr m_PostUpdate;
			CStr m_PostLaunch;
			CStr m_OnError;
		};

		enum EApplicationSetting
		{
			EApplicationSetting_None                             = 0
			, EApplicationSetting_Executable                     = DBit(0)
			, EApplicationSetting_ExecutableParameters           = DBit(1)
			, EApplicationSetting_RunAsUser                      = DBit(2)
			, EApplicationSetting_RunAsGroup                     = DBit(3)
			, EApplicationSetting_VersionManagerApplication      = DBit(4)
			, EApplicationSetting_UpdateTags                     = DBit(5)
			, EApplicationSetting_UpdateBranches                 = DBit(6)
			, EApplicationSetting_UpdateScript_PreUpdate         = DBit(7)
			, EApplicationSetting_UpdateScript_PostUpdate        = DBit(8)
			, EApplicationSetting_UpdateScript_PostLaunch        = DBit(9)
			, EApplicationSetting_UpdateScript_OnError           = DBit(10)
			, EApplicationSetting_SelfUpdateSource               = DBit(11)
			, EApplicationSetting_EncryptionStorage              = DBit(12)
			, EApplicationSetting_ParentApplication              = DBit(13)
			, EApplicationSetting_EncryptionFileSystem           = DBit(14)
			, EApplicationSetting_UpdateGroup                    = DBit(15)
			, EApplicationSetting_DistributedApp                 = DBit(16)
			, EApplicationSetting_Dependencies                   = DBit(17)
			, EApplicationSetting_StopOnDependencyFailure        = DBit(18)
			, EApplicationSetting_BackupIncludeWildcards         = DBit(19)
			, EApplicationSetting_BackupExcludeWildcards         = DBit(20)
			, EApplicationSetting_BackupAddSyncFlagsWildcards    = DBit(21)
			, EApplicationSetting_BackupRemoveSyncFlagsWildcards = DBit(22)
			, EApplicationSetting_BackupNewBackupInterval        = DBit(23)
			, EApplicationSetting_BackupEnabled                  = DBit(24)
			, EApplicationSetting_RunAsUserPassword              = DBit(25)
			, EApplicationSetting_RunAsUserHasShell              = DBit(26)
			, EApplicationSetting_AutoUpdate                     = DBit(27)
			, EApplicationSetting_LaunchInProcess                = DBit(28)
			, EApplicationSetting_LaunchEnvironment              = DBit(29)
			, EApplicationSetting_OSDependencies                 = DBit(30)
			, EApplicationSetting_NeedUpdateSettings
			= EApplicationSetting_Executable
			| EApplicationSetting_ExecutableParameters
			| EApplicationSetting_RunAsUser
			| EApplicationSetting_RunAsGroup
			| EApplicationSetting_SelfUpdateSource
			| EApplicationSetting_LaunchInProcess
			| EApplicationSetting_LaunchEnvironment
		};

		enum EStopFlag
		{
			EStopFlag_None                  = 0
			, EStopFlag_CloseEncryption     = DBit(0)
			, EStopFlag_AutoStart           = DBit(1)
			, EStopFlag_PreventLaunchUser   = DBit(2)
			, EStopFlag_PreventLaunchUpdate = DBit(3)
		};

		struct CApplicationSettings
		{
			bool f_ParseSettings(CEJsonSorted const &_Params, EApplicationSetting &o_ChangedSettings, CStr &o_Error, bool _bRelaxed);
			void f_ApplySettings(EApplicationSetting _ChangedSettings, CApplicationSettings const &_Source);
			void f_FromVersionInfo(CVersionManager::CVersionInformation const &_Info, EApplicationSetting &o_ChangedSettings);
			void f_FromInterfaceSettings(CAppManagerInterface::CApplicationSettings const &_Settings, EApplicationSetting &o_ChangedSettings);
			void f_FromInterfaceAdd(CAppManagerInterface::CApplicationAdd const &_Settings, EApplicationSetting &o_ChangedSettings);
			EApplicationSetting f_ChangedSettings(CApplicationSettings const &_Other) const;
			bool f_Validate(CStr &o_Error) const;

			// Settings that can on never change
			CStr m_EncryptionStorage;
			CStr m_ParentApplication;
			CStr m_EncryptionFileSystem;

			// Settings that can be updated from version information
			CStr m_Executable;
			CStr m_RunAsUser;
			CStr m_RunAsGroup;
			TCVector<CStr> m_ExecutableParameters;
			bool m_bRunAsUserHasShell = false;
			bool m_bDistributedApp = false;

			NContainer::TCMap<NStr::CStr, CDirectoryManifestConfig::CDestination> m_Backup_IncludeWildcards;
			NContainer::TCSet<NStr::CStr> m_Backup_ExcludeWildcards;
			NContainer::TCMap<NStr::CStr, EDirectoryManifestSyncFlag> m_Backup_AddSyncFlagsWildcards;
			NContainer::TCMap<NStr::CStr, EDirectoryManifestSyncFlag> m_Backup_RemoveSyncFlagsWildcards;
			NTime::CTimeSpan m_Backup_NewBackupInterval = NTime::CTimeSpanConvert::fs_CreateDaySpan(1);
			TCMap<CStr, TCVector<CStr>> m_OSDependencies; /// OS selector to packages installed with the platform package manager before launch

			// Settings that can be updated by app manager (command line or protocol)
			TCSet<CStr> m_Dependencies;
			CStr m_VersionManagerApplication;
			CStr m_UpdateGroup;
			CStr m_LaunchEnvironment;
			TCSet<CStr> m_UpdateTags;
			TCSet<CStr> m_UpdateBranches;
			CUpdateScripts m_UpdateScripts;
#ifdef DPlatformFamily_Windows
			CStrSecure m_RunAsUserPassword;
#endif
			uint32 m_AppManagerVersion = mcp_CurrentAppMangerVersion;
			bool m_bAutoUpdate = false;
			bool m_bSelfUpdateSource = false;
			bool m_bStopOnDependencyFailure = true;
			bool m_bBackupEnabled = false;
			bool m_bLaunchInProcess = false;
		};

		enum EEnvironmentSetting
		{
			EEnvironmentSetting_None                      = 0
			, EEnvironmentSetting_Type                    = DBit(0)
			, EEnvironmentSetting_AgentApplication        = DBit(1)
			, EEnvironmentSetting_AutoStart               = DBit(2)
			, EEnvironmentSetting_ParentApplication       = DBit(3)
			, EEnvironmentSetting_OSDependencies          = DBit(4)
			, EEnvironmentSetting_VMImage                 = DBit(5)
			, EEnvironmentSetting_VMBackend               = DBit(6)
			, EEnvironmentSetting_VMCPUCount              = DBit(7)
			, EEnvironmentSetting_VMMemoryMB              = DBit(8)
			, EEnvironmentSetting_VMDataDiskGB            = DBit(9)
			, EEnvironmentSetting_ContainerRuntime        = DBit(10)
			, EEnvironmentSetting_ContainerImage          = DBit(11)
			, EEnvironmentSetting_ContainerNetwork        = DBit(12)
			, EEnvironmentSetting_ContainerExtraMounts    = DBit(13)
			, EEnvironmentSetting_ContainerExtraArguments = DBit(14)
			, EEnvironmentSetting_MemoryLimit             = DBit(15)
			, EEnvironmentSetting_CPULimit                = DBit(16)
			, EEnvironmentSetting_ContainerReadOnly       = DBit(17)
		};

		struct CEnvironmentSettings
		{
			using EEnvironmentType = CAppManagerInterface::EEnvironmentType;

			bool f_ParseSettings(CEJsonSorted const &_Params, EEnvironmentSetting &o_ChangedSettings, CStr &o_Error);
			void f_ApplySettings(EEnvironmentSetting _ChangedSettings, CEnvironmentSettings const &_Source);
			void f_FromInterfaceSettings(CAppManagerInterface::CEnvironmentSettings const &_Settings, EEnvironmentSetting &o_ChangedSettings);
			EEnvironmentSetting f_ChangedSettings(CEnvironmentSettings const &_Other) const;
			bool f_Validate(CStr &o_Error) const;
			void f_AddPermissionQueries
				(
					NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> &o_Permissions
					, EEnvironmentSetting _ChangedSettings
					, EEnvironmentType _Type
				)
				const
			;

			EEnvironmentType m_Type = CAppManagerInterface::EEnvironmentType_Container;
			CStr m_AgentApplication;
			CStr m_ParentApplication; /// Application whose storage confines the environment storage, supporting encrypted storage

			CStr m_ContainerRuntime;
			CStr m_ContainerImage;
			CStr m_ContainerNetwork;
			TCMap<CStr, CStr> m_ContainerExtraMounts;
			TCVector<CStr> m_ContainerExtraArguments;

			uint64 m_MemoryLimitMB = 0;
			fp64 m_CPULimit = 0.0;
			bool m_bContainerReadOnly = false; /// Makes the container root filesystem read-only, with a writable /tmp tmpfs

			CStr m_VMImage;
			CStr m_VMBackend;
			uint32 m_VMCPUCount = 0;
			uint64 m_VMMemoryMB = 0;
			uint64 m_VMDataDiskGB = 0; /// Initial size of the VM data disk or Colima application storage image; 0 selects the default

			bool m_bAutoStart = true;

			TCVector<CStr> m_OSDependencies; /// Additional packages the agent installs inside the environment, in addition to the agent application's OS dependencies
		};

		struct CEnvironment
		{
			CEnvironment(CStr const &_Name, CAppManagerActor *_pThis);

			void f_SetStatus(CStr const &_Status, CAppManagerInterface::EStatusSeverity _Severity);
			bool f_IsStarted() const;
			bool f_AgentUpdateDefers() const;
			bool f_AgentConfigurationDefers() const;
			void f_SetAgentUpdateInProgress(CStr &&_Description);

			CIntrusiveRefCount m_RefCount;

			CStr const m_Name;

			CEnvironmentSettings m_Settings;

			CStr m_Status = "Not started";
			CAppManagerInterface::EStatusSeverity m_StatusSeverity = CAppManagerInterface::EStatusSeverity_Warning;

			CDistributedAppSensorReporter::CSensorReporter m_StatusSensorReporter;
			CSequencer m_StatusSensorReporterSequencer{"EnvironmentStatusSensorReporterSequencer"};
			TCOptional<CDistributedAppSensorReporter::CStatus> m_LastReporterSensorStatus;

			bool m_bDeleted = false;
			bool m_bStoppedManually = false; /// Persisted; while set, only an explicit start or restart, which clear it, storage removal and the VM window boot the environment

			CStr m_ContainerFingerprint;                     /// Run settings of the last recorded container creation attempt, even a failed one; empty means none, a change recreates it
			CStr m_ContainerLaunchID;                        /// Fixed for the lifetime of the container, which keeps its creation-time process environment
			CStr m_ContainerRequestTicketMagic;              /// Fixed for the lifetime of the container, which keeps its creation-time process environment
			NContainer::TCSet<NStr::CStr> m_ContainerMounts; /// Host paths and named volumes of the mount list of the last launch, which keep encrypted storage busy

			uint32 m_ListenPort = 0; /// Host port of the listen address on the shared guest network; persisted to keep the container fingerprint stable

			CStr m_VMMACAddress; /// Persisted so the guest keeps its network identity across restarts

			CStr m_VMAgentLaunchID;           /// Saved after an install so a guest agent that keeps running can reconnect with the same id
			CStr m_VMAgentFingerprint;        /// Fingerprint of the agent files installed in the guest with m_VMAgentLaunchID
			CStr m_VMAgentDesiredFingerprint; /// Not persisted

			// Runtime state
			CStr m_LaunchID;
			CStr m_AgentHostID;
			TCActor<CDistributedAppInterfaceLaunchActor> m_AgentLaunch;
			bool m_bAgentLaunchFinished = false;         /// The agent client failed to launch or exited; there is no process to signal at stop
			TCVector<TCPromise<void>> m_OnAgentLaunchFinished;
			CActorSubscription m_AgentLaunchSubscription;
			CActorSubscription m_AgentTicketSubscription;
			TCActor<NVirtualization::CVirtualMachineActor> m_VMActor;
			umint m_nStorageRemovals = 0;                /// Application storage removals that need the agent; the last one stops a manually stopped environment
			bool m_bWindowOpen = false;                  /// Set while a window session is prepared or runs; it reserves the guest image bundle
			umint m_nPendingInstalls = 0;                /// Adds of applications assigned to the environment that have reached unpacking
			umint m_nPendingInstallGuestWaits = 0;       /// The pending installs that wait for the guest, which stages and applies their files
			umint m_nRunningScripts = 0;                 /// Update script calls to the agent awaiting a reply or timeout; stops and restarts wait for them
			TCDistributedActorInterface<CDistributedAppInterfaceClient> m_AgentAppInterface;
			TCDistributedActorInterface<CAppManagerEnvironmentInterface> m_AgentInterface;
			umint m_AgentAppInterfaceAssignSequence = 0; /// Lets a lost registration of m_AgentAppInterface tell whether a newer one replaced it
			umint m_AgentInterfaceAssignSequence = 0;    /// Lets a lost registration of m_AgentInterface tell whether a newer one replaced it
			TCVector<TCPromise<void>> m_OnAgentConnected;
			TCVector<TCPromise<void>> m_OnStopFinished;
			umint m_nPendingAgentConfigurations = 0;
			umint m_AgentConfigurationSequence = 0;      /// Sequence of the newest agent configuration sent, whose result sets m_AgentConfigurationError
			TCVector<TCPromise<void>> m_OnAgentConfigured;
			CAppManagerGuestDeferral m_AgentConfigurationDeferral;
			fp64 m_LastAgentConfigurationStart = 0.0;    /// CAppManagerGuestDeferral::fs_Now when the newest agent configuration was sent
			CStr m_AgentConfigurationError;              /// Failure of the newest agent configuration; launches retry the configuration
			CStr m_AgentUpdateInProgress;                /// Description of the OS updates or dependency installs the agent reports; see f_AgentUpdateDefers
			CAppManagerGuestDeferral m_AgentUpdateDeferral;
			umint m_nUpdateAdmissionClosed = 0;          /// Stops, restarts and removals that waited for the agent updates refuse new ones until the environment stops
			bool m_bStarting = false;
			bool m_bStartAbortRequested = false;         /// A stop waits for the start, which gives up its retries and its wait for the agent
			bool m_bStartEnsuredContainerSystem = false; /// Set while a start runs on the container system it ensured; a colima restart waits for that start
			bool m_bStarted = false;
			bool m_bStopping = false;
			bool m_bForceStopped = false;                /// The shutdown before a host reboot ran out the guest deferral limit; the agent is refused from then on
			bool m_bRemoving = false;
			bool m_bReplacingContainerAfterPull = false; /// New starts fail and automatic launches are deferred while the container is replaced
			umint m_nStops = 0;                          /// Stops so far, which tell a restart that waits whether another restart came first
			bool m_bRestartWhenIdlePending = false;      /// A restart waits for the environment to become idle, which later requests join

			CAppManagerActor *m_pThis;
		};

		struct CAppManagerEnvironmentInterfaceImplementation : public CAppManagerEnvironmentInterface
		{
			TCFuture<CAgentInfo> f_GetAgentInfo() override;
			TCFuture<CStr> f_LaunchApplication(CEnvironmentLaunch _Launch) override;
			TCFuture<uint32> f_StopApplication(CStr _Name) override;
			TCFuture<void> f_RunScript(CEnvironmentScript _Script) override;
			TCFuture<CApplicationStageResult> f_StageApplicationFiles(CApplicationStage _Stage) override;
			TCFuture<void> f_ApplyApplicationFiles(CApplicationApply _Apply) override;
			TCFuture<void> f_DiscardApplicationStage(CStr _Name, CStr _StageID) override;
			TCFuture<void> f_RemoveApplicationStorage(CStr _Name) override;
			TCFuture<void> f_ConfigureAgent(CAgentConfig _Config) override;
			TCFuture<void> f_ShutdownEnvironment() override;

			DMibDelegatedActorImplementation(CAppManagerActor);
		};

		struct CAppManagerEnvironmentHostInterfaceImplementation : public CAppManagerEnvironmentHostInterface
		{
			auto f_RegisterEnvironmentAgent(CStr _LaunchID, NConcurrency::TCDistributedActorInterfaceWithID<CAppManagerEnvironmentInterface> _Interface)
				-> TCFuture<NConcurrency::TCActorSubscriptionWithID<>> override
			;
			TCFuture<void> f_ReportApplicationState(CAppManagerEnvironmentInterface::CApplicationStateChange _Change) override;
			TCFuture<CStr> f_RequestApplicationConnectionTicket(CStr _LaunchID) override;
			TCFuture<void> f_ReportEnvironmentUpdateState(bool _bUpdating, CStr _Description) override;
			TCFuture<void> f_RequestEnvironmentRestart(CStr _Reason) override;

			DMibDelegatedActorImplementation(CAppManagerActor);
		};

		struct CApplication
		{
			CApplication(CStr const &_Name, CAppManagerActor *_pThis);

			void f_Clear();
			void f_ClearEnvironmentLaunch();
			void f_Delete();
			void f_AbortPendingLaunches();

			TCFuture<uint32> f_Stop(EStopFlag _Flags);
			TCFuture<uint32> f_CloseEncryption(uint32 _Status);

			CStr f_GetDirectory();

			CActorSubscription f_SetInProgress(CStr const &_Description);

			bool f_NeedsEncryption() const;
			bool f_EncryptionOpened() const;
			bool f_EncryptionClosing() const;
			bool f_IsChildApp() const;
			CStr f_AgentDistributionPlatform() const;
			bool f_IsInProgress() const;
			CStr f_InProgressDescription() const;
			fp64 f_InProgressTime() const;
			TCFuture<void> f_InProgressWait();
			bool f_DependenciesSatisfied(CStr &o_State, CAppManagerInterface::EStatusSeverity &o_Severity, bool &o_bNeedsEncryption) const;
			bool f_IsLaunched() const;
			bool f_InProgressWaitsOnlyForGuest() const;

			TCVector<TCSharedPointer<CApplication>> f_GetDependents() const;

			EDistributedAppUpdateType f_GetUpdateType(bool _bBypassCoordination) const;
			CAppManagerCoordinationInterface::CAppInfo f_GetRemoteAppInfo() const;

			CIntrusiveRefCount m_RefCount;

			CStr const m_Name;

			CApplicationSettings m_Settings;

			CStr m_AssociatedHostID;
			CStr m_LaunchID;
			CStr m_EnvironmentTicketID;
			CActorSubscription m_EnvironmentTicketSubscription; /// Keeps the latest unused environment connection ticket alive; a new one replaces it
			TCDistributedActorInterface<CDistributedAppInterfaceClient> m_AppInterface;
			umint m_AppInterfaceAssignSequence = 0;
			CDistributedAppInterfaceServer::CRegisterInfo m_RegisterInfo;

			// Specific version state
			TCVector<CStr> m_Files;
			TCMap<CStr, CStr> m_LowPortExecutablesToGrant; /// Low port executables whose capabilities the install couldn't set, to the SHA-256 of the file it installed
			CVersionManager::CVersionIDAndPlatform m_LastInstalledVersion;
			CVersionManager::CVersionInformation m_LastInstalledVersionInfo;

			CVersionManager::CVersionIDAndPlatform m_LastInstalledVersionFinished;
			CVersionManager::CVersionInformation m_LastInstalledVersionInfoFinished;

			CVersionManager::CVersionIDAndPlatform m_LastMigratedVersion; /// Whose post update script last succeeded, the previous version of the next update
			CVersionManager::CVersionInformation m_LastMigratedVersionInfo;

			CVersionManager::CVersionIDAndPlatform m_LastTriedInstalledVersion;
			CVersionManager::CVersionInformation m_LastTriedInstalledVersionInfo;
			CStr m_LastTriedInstalledVersionError;

			CVersionManager::CVersionIDAndPlatform m_WantVersion;
			CVersionManager::CVersionInformation m_WantVersionInfo;

			CVersionManager::CVersionIDAndPlatform m_NewestUnconditionalVersion;
			CVersionManager::CVersionInformation m_NewestUnconditionalVersionInfo;

			CVersionManager::CVersionIDAndPlatform m_LastFailedInstalledVersion;
			CTime m_LastFailedInstalledVersionTime;
			uint32 m_LastFailedInstalledVersionRetrySequence = 0;
			EUpdateStage m_LastFailedInstalledVersionFailureStage = EUpdateStage::EUpdateStage_None;

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
			bool m_bClosingEncryption = false; /// Blocks new starts of environments confined to the application or its children while its storage closes
			bool m_bLaunching = false;
			bool m_bLaunched = false;
			bool m_bJustUpdated = false;
			bool m_bDistributedStartupFinished = false;
			bool m_bPendingStop = false;

			CDistributedAppSensorReporter::ESensorInfoFlag m_PreventRebootSensorFlags = CDistributedAppSensorReporter::ESensorInfoFlag::mc_None;
			CAppManagerGuestDeferral m_PreventRebootDeferral; /// Limits the sensors of an application in a container or VM environment, see fp_ApplicationPreventsReboot
			umint m_nGuestWaits = 0;                          /// Steps of the operation in progress that wait for the container or VM guest; see f_InProgressWaitsOnlyForGuest

			CStr m_OperationInProgressDescription;
			CStopwatch m_OperationInProgressStopwatch;
			TCVector<TCPromise<void>> m_OnOperationInProgressFinished;

			TCVector<TCPromise<void>> m_OnRegisterDistributedApp;
			TCVector<TCPromise<void>> m_OnStartedDistributedApp;

			TCLinkedList<TCFunctionMutable<void (bool _bAborted)>> m_OnLaunchFinished;

			CStr m_LaunchStatus = "Not yet launched";
			CAppManagerInterface::EStatusSeverity m_LaunchStatusSeverity = CAppManagerInterface::EStatusSeverity_Warning;
			CStr m_LastStdErr;
			CStr m_LastError;

			TCVariant<void, TCActor<CDistributedAppInterfaceLaunchActor>, TCActor<CDistributedAppInProcessActor>> m_ProcessLaunch;
			CActorSubscription m_ProcessLaunchSubscription;
			TCVector<TCPromise<void>> m_OnStops;

			// Environment launches (host side)
			TCSharedPointer<CEnvironment> m_pLaunchedEnvironment; /// Set from before the agent launch call, so ticket requests during the launch find the environment
			TCSharedPointer<CEnvironment> m_pStoppingEnvironment; /// Set while a stop runs the application down through the agent, which an environment stop waits for
			TCVector<TCPromise<void>> m_OnEnvironmentStopFinished;

			// Agent side
			CStr m_DirectoryOverride;
			bool m_bEphemeral = false;      /// Application exists only for the lifetime of the environment agent and is never persisted
			CStr m_EnvironmentHostAddress;  /// Host AppManager address the application connects its distributed app interface to
			CStr m_EnvironmentHostLaunchID; /// Launch ID the host assigned to the application, which it registers with

			CApplication *m_pParentApplication = nullptr;
			DLinkDS_Link(CApplication, m_ChildrenLink);
			DLinkDS_List(CApplication, m_ChildrenLink) m_Children;

			CAppManagerActor *m_pThis;

			TCActor<CBackupManagerClient> m_BackupClient;
			CActorSubscription m_DirectoryMonitorSubscription;
			CDistributedAppSensorReporter::CSensorReporter m_StatusSensorReporter;
			CSequencer m_StatusSensorReporterSequencer{"StatusSensorReporterSequencer"};
			TCOptional<CDistributedAppSensorReporter::CStatus> m_LastReporterSensorStatus;
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
				COrdering_Partial operator () (CVersionManagerVersion const &_Left, CVersionManagerVersion const &_Right) const;
			};

			struct CCompareApplication
			{
				COrdering_Partial operator () (CVersionManagerVersion const &_Left, CVersionManagerVersion const &_Right) const;
				COrdering_Partial operator () (CVersionManager::CVersionIDAndPlatform const &_Left, CVersionManagerVersion const &_Right) const;
				COrdering_Partial operator () (CVersionManagerVersion const &_Left, CVersionManager::CVersionIDAndPlatform const &_Right) const;
			};

			CVersionManagerVersion(CVersionManagerState *_pVersionManager);
			~CVersionManagerVersion();
			void f_SetApplication(CVersionManagerApplication *_pApplication);
			CVersionManager::CVersionIDAndPlatform const &f_GetVersionID() const;

			CVersionManagerState *m_pVersionManager;
			CVersionManagerApplication *m_pApplication = nullptr;
			CVersionManager::CVersionInformation m_VersionInfo;
			TCAVLLink<> m_ApplicationTimeLink;
			TCAVLLink<> m_ApplicationLink;
		};

		struct CVersionManagerApplication
		{
			CVersionManagerApplication(CAppManagerActor &_This);
			CStr const &f_GetApplicationName() const;

			TCAVLTree<&CVersionManagerVersion::m_ApplicationTimeLink, CVersionManagerVersion::CCompareApplicationByTime> m_VersionsByTime;
			TCAVLTree<&CVersionManagerVersion::m_ApplicationLink, CVersionManagerVersion::CCompareApplication> m_Versions;
			CAppManagerActor &m_This;
		};

		struct CVersionManagerState
		{
			TCDistributedActor<CVersionManager> const &f_GetManager() const;

			TCMap<CStr, TCMap<CVersionManager::CVersionIDAndPlatform, CVersionManagerVersion>> m_Versions;

			CTrustedActorInfo m_HostInfo;
			CActorSubscription m_Subscription;
			umint m_SubscribeSequence = 0;
		};

		struct CVersionManagerDownloadState
		{
			TCActor<CFileTransferReceive> m_DownloadVersionReceive;
			CActorSubscription m_Subscription;
		};

		struct CCloudManagerState
		{
			TCDistributedActor<CCloudManager> const &f_GetManager() const;

			CTrustedActorInfo m_HostInfo;
			TCDistributedActorInterface<CAppManagerCloudManagerInterface> m_AppManagerCloudManagerInterface;
			CActorSubscription m_SensorReporterSubscription;
			CActorSubscription m_LogReporterSubscription;
			CActorSubscription m_ExpectedOsVersionSubscription;
			CCloudManager::CExpectedVersions m_ExpecteOsVersions;
		};

		struct CDistributedAppInterfaceServerImplementation : public CDistributedAppInterfaceServer
		{
			auto f_RegisterDistributedApp
				(
					TCDistributedActorInterfaceWithID<CDistributedAppInterfaceClient> _ClientInterface
					, TCDistributedActorInterfaceWithID<CDistributedActorTrustManagerInterface> _TrustInterface
					, CRegisterInfo _RegisterInfo
				)
				-> TCFuture<TCActorSubscriptionWithID<>> override
			;
			TCFuture<TCActorSubscriptionWithID<>> f_RegisterConfigFiles(CConfigFiles _ConfigFiles) override;

			TCFuture<TCDistributedActorInterfaceWithID<CDistributedAppSensorReporter>> f_GetSensorReporter() override;
			TCFuture<TCDistributedActorInterfaceWithID<CDistributedAppLogReporter>> f_GetLogReporter() override;

			DMibDelegatedActorImplementation(CAppManagerActor);
		};

		struct CAppManagerInterfaceImplementation : public CAppManagerInterface
		{
			TCFuture<CVersionsAvailableForUpdate> f_GetAvailableVersions(CStr _Application) override;

			TCFuture<void> f_Add(CStr _Name, CApplicationAdd _Add, CApplicationSettings _Settings) override;
			TCFuture<void> f_Remove(CStr _Name) override;
			TCFuture<void> f_RemoveWithSettings(CStr _Name, CApplicationRemoveSettings _Settings) override;
			TCFuture<void> f_RemoveInternal(CStr _Name, bool _bForce, bool _bDeleteApplicationData);

			TCFuture<void> f_Update(CStr _Name, CApplicationUpdate _Update) override;

			TCFuture<void> f_Start(CStr _Name) override;
			TCFuture<void> f_Stop(CStr _Name) override;
			TCFuture<void> f_Restart(CStr _Name) override;

			TCFuture<void> f_ChangeSettings(CStr _Name, CApplicationChangeSettings _ChangeSettings, CApplicationSettings _Settings) override;

			TCFuture<TCMap<CStr, CApplicationInfo>> f_GetInstalled() override;

			auto f_SubscribeUpdateNotifications(CSubscribeUpdateNotifications _Params) -> NConcurrency::TCFuture<NConcurrency::TCActorSubscriptionWithID<>> override;
			auto f_SubscribeChangeNotifications(CSubscribeChangeNotifications _Params) -> NConcurrency::TCFuture<NConcurrency::TCActorSubscriptionWithID<>> override;

			TCFuture<void> f_EnvironmentAdd(CStr _Name, CEnvironmentSettings _Settings) override;
			TCFuture<void> f_EnvironmentRemove(CStr _Name) override;
			TCFuture<void> f_EnvironmentChangeSettings(CStr _Name, CEnvironmentSettings _Settings) override;
			TCFuture<void> f_EnvironmentStart(CStr _Name) override;
			TCFuture<void> f_EnvironmentPull(CStr _Name) override;
			TCFuture<void> f_EnvironmentStop(CStr _Name) override;
			TCFuture<void> f_EnvironmentRestart(CStr _Name) override;
			TCFuture<TCMap<CStr, CEnvironmentInfo>> f_GetEnvironments() override;

			DMibDelegatedActorImplementation(CAppManagerActor);
		};

		struct CUpdateNotificationSubscription
		{
			TCActorFunctor<NConcurrency::TCFuture<void> (CAppManagerInterface::CUpdateNotification _Notification)> m_fOnUpdate;
			CCallingHostInfo m_CallingHostInfo;
			CSequencer m_Sequencer{"AppManagerActor UpdateNotificationSubscription {}"_f << TCMap<CStr, CUpdateNotificationSubscription>::fs_GetKey(*this)};
			bool m_bInitialFinished = false;
			bool m_bWaitForResult = true;
		};

		struct CChangeNotificationSubscription
		{
			TCActorFunctor<NConcurrency::TCFuture<void> (CAppManagerInterface::COnChangeNotificationParams _Params)> m_fOnChange;
			CCallingHostInfo m_CallingHostInfo;
			TCSet<CStr> m_Filtered;
			TCVector<TCFunctionMovable<void ()>> m_OnInitialFinished;
			bool m_bInitialFinished = false;
			bool m_bAccessDenied = false;
			bool m_bWaitForResult = true;
		};

		enum EEncryptOperation
		{
			EEncryptOperation_Setup
			, EEncryptOperation_Open
			, EEncryptOperation_Close
		};

		enum EFindVersionFlag
		{
			EFindVersionFlag_None          = 0
			, EFindVersionFlag_RetryFailed = DBit(0)
			, EFindVersionFlag_ForAdd      = DBit(1)
		};

		struct CAppManagerCoordinationInterfaceImplementation : public CAppManagerCoordinationInterface
		{
			TCFuture<void> f_RemoveKnownHost(CStr _Group, CStr _Application, CStr _HostID) override;
			auto f_SubscribeToAppChanges(TCActorFunctorWithID<TCFuture<void> (TCVector<CAppChange> _Changes, bool _bInitial)> _fOnChange) -> TCFuture<TCActorSubscriptionWithID<>> override;

			DMibDelegatedActorImplementation(CAppManagerActor);
		};

		struct CRemoteAppManagerApp
		{
			CAppManagerCoordinationInterface::CAppInfo m_AppInfo;
		};

		struct CRemoteApplicationKey
		{
			CRemoteApplicationKey() = default;
			CRemoteApplicationKey(CAppManagerCoordinationInterface::CAppInfo const &_AppInfo);
			CRemoteApplicationKey(CApplicationSettings const &_Settings);

			auto operator <=> (CRemoteApplicationKey const &_Right) const noexcept = default;

			template <typename tf_CStr>
			void f_Format(tf_CStr &o_Str) const;

			CStr m_Group;
			CStr m_VersionManagerApplication;
		};

		struct CRemoteApplicationWithTypeKey : public CRemoteApplicationKey
		{
			CRemoteApplicationWithTypeKey() = default;
			CRemoteApplicationWithTypeKey(CAppManagerCoordinationInterface::CAppInfo const &_AppInfo, bool _bBypassCoordination);
			CRemoteApplicationWithTypeKey(CApplication const &_Application, bool _bBypassCoordination);

			CRemoteApplicationKey const &f_WithoutType() const;

			auto operator <=> (CRemoteApplicationWithTypeKey const &_Right) const noexcept = default;

			template <typename tf_CStr>
			void f_Format(tf_CStr &o_Str) const;

			EDistributedAppUpdateType m_UpdateType = EDistributedAppUpdateType_Independent;
		};

		struct CRemoteAppManager
		{
			struct CCompareActor
			{
				auto &operator () (CRemoteAppManager const &_Node) const;
			};

			CStr const &f_GetHostID() const;

			void f_Clear();

			TCAVLLink<> m_ByActorLink;

			TCDistributedActor<CAppManagerCoordinationInterface> m_Actor;
			CTrustedActorInfo m_HostInfo;
			TCActorFunctor<TCFuture<void> (TCVector<CAppManagerCoordinationInterface::CAppChange> _Changes, bool _bInitial)> m_fOnChange;
			CActorSubscription m_OnChangeSubscription;
			umint m_iOnChangeSubscriptionSequence = 0;
			bool m_bInitialStateReceived = false;

			TCMap<CStr, CRemoteAppManagerApp> m_AppInfos;
			TCMap<CRemoteApplicationKey, TCSet<CStr>> m_KnownApplications;
		};

		struct CUpdateApplicationState
		{
			CUpdateApplicationState() = default;
			CUpdateApplicationState(CUpdateApplicationState const &_Other) = delete;
			CUpdateApplicationState(CUpdateApplicationState &&_Other) = delete;

			CExceptionPointer f_CheckAbort();
			template <typename tf_CType>
			bool f_CheckAbort(TCPromise<tf_CType> const &o_Error);

			CIntrusiveRefCountWithWeak m_RefCount;
			TCSharedPointer<CApplication> m_pApplication;
			CVersionManager::CVersionIDAndPlatform m_LastInstalledVersion;
			CVersionManager::CVersionInformation m_LastInstalledVersionInfo;
			TCFunction<void (CStr const &_Info)> m_fOnInfo;
			TCFunction<void ()> m_fUpdateVersionInfo;
			CActorSubscription m_DownloadDirectoryCleanup;
			CActorSubscription m_TemporaryDirectoryCleanup;
			CActorSubscription m_AgentStageCleanup;
			CStr m_SourcePath;
			CStr m_TempraryPath;

			TCSharedPointer<CEnvironment> m_pRemoteStorageEnvironment; /// Null when the host manages the application files
			CStr m_AgentStageID;
			TCActor<CFileTransferSend> m_StageSend;                    /// Sends the staged files into the environment; destroyed to abort an in-flight transfer
			TCActorFunctorWithID<TCFuture<void> ()> m_fStageFinish;    /// Dropping this uncalled aborts an in-flight receive
			TCSharedPointer<CApplicationSettings> m_pNewSettings;
			TCSet<CStr> m_AllowSourceExist;
			CAppManagerInterface::CVersionIDAndPlatform m_VersionID;
			CTime m_VersionTime;
			uint32 m_VersionRetrySequence = 0;
			TCSet<CStr> m_RequiredTags;
			TCSharedPointer<CVersionManager::CVersionInformation> m_pVersionInfo;
			TCSharedPointer<NTime::CStopwatch> m_pStopwatch;
			CTime m_StartUpdateTime;
			CStr m_UniqueUpdateID;
			TCVector<CStr> m_Files;
			CDistributedAppAuditor m_Auditor;
			bool m_bDryRun = false;
			bool m_bUpdateSettings = true;
			bool m_bUnencrypted = false;
			bool m_bCancel = false;
			bool m_bCancelOnAppManagerStop = false;
			bool m_bSetLastTried = false;
			bool m_bBypassCoordination = false;
			bool m_bFailureRecorded = false;                           /// The application info shows the failure from here on, also while the error script runs
		};

		struct COnAppUpdateInfoChange
		{
			CActorSubscription m_TimerSubscription;
			CActorSubscription m_DisconnectTimerSubscription;
			TCFunctionMutable<void ()> m_fOnChanged;
		};

		enum EWaitStageFlag
		{
			EWaitStageFlag_None                       = 0
			, EWaitStageFlag_IgnoreFailed             = DBit(0)
			, EWaitStageFlag_DisallowInProgressStates = DBit(1)
		};

		struct CAppLaunchResult
		{
			CStr m_StartupError;
			bool m_bQuitManager = false;
		};

		struct CDistributedAppSensorReporterImplementation : public CDistributedAppSensorReporter
		{
			TCFuture<CSensorReporter> f_OpenSensorReporter(CSensorInfo _SensorInfo) override;

			DMibDelegatedActorImplementation(CAppManagerActor);
		};

		struct CDistributedAppLogReporterImplementation : public CDistributedAppLogReporter
		{
			TCFuture<CLogReporter> f_OpenLogReporter(CLogInfo _LogInfo) override;

			DMibDelegatedActorImplementation(CAppManagerActor);
		};

		struct CPendingSelfupdate
		{
			CStr m_UniqueUpdateID;
			CStr m_Name;
			CTime m_StartUpdateTime;
			CVersionManager::CVersionIDAndPlatform m_VersionID;
			CTime m_VersionTime;
			uint32 m_VersionRetrySequence = 0;
		};

		struct CSensorRebootWatch
		{
			CStr m_Application;
			CDistributedAppSensorReporter::CSensorInfo m_SensorInfo;
			CActorSubscription m_SensorSubscription;
			TCOptional<CDistributedAppSensorReader_SensorKeyAndReading> m_LastReading;
			CDistributedAppSensorReporter::ESensorInfoFlag m_PreventedFlags = CDistributedAppSensorReporter::ESensorInfoFlag::mc_None; /// From the last reading
		};

		struct CSensorRebootPreventionCounts
		{
			umint m_nPreventRebootOnWarning = 0;
			umint m_nPreventRebootOnError = 0;
			umint m_nPreventRebootOnOutdated = 0;
		};

		struct CCheckAndLogPreventedRebootParams
		{
			bool m_bErrorOnPreventReboot = false;
			bool m_bCriticalLog = false;
			bool m_bIgnoreGuestDeferrals = false;
		};

		enum class EPreventedReboot : uint32
		{
			mc_None
			, mc_GuestDeferrals /// Only guests of container or VM environments prevent the reboot, each for a limited time
			, mc_Host
		};

		enum class ERebootResult : uint32
		{
			mc_NotSupported
			, mc_Scheduled
			, mc_AlreadyScheduled
		};

		enum class EEnvironmentStart : uint8
		{
			mc_UnlessStoppedManually
			, mc_EvenIfStoppedManually
		};

		struct CPackageVersionInfo
		{
			CVersionManager::CVersionIDAndPlatform m_VersionID;
			CVersionManager::CVersionInformation m_VersionInfo;
			CStr m_Application;
		};

		struct CApplicationStageReceive
		{
			TCActor<CFileTransferReceive> m_Receive;
			CStr m_Name;
			CStr m_StageDirectory;
		};

		struct CColimaEnvironmentDirectories
		{
			CStr m_StorageDirectory;
			CStr m_AgentDirectory;
			CStr m_DataDirectory;
		};

		void fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine) override;
		void fp_BuildCommandLine_HostMonitor(CDistributedAppCommandLineSpecification &o_CommandLine);

		static CStr fsp_RunTool(CStr const &_Description, CStr const &_Tool, CStr const &_WorkingDir, TCVector<CStr> const &_Arguments);
		auto fp_RunBashScript
			(
				CStr const &_Script
				, CStr const &_Description
				, TCMap<CStr, CStr> const &_Environment
				, TCFunction<void (CStr const &_Output, TCActor<CProcessLaunchActor> const &_LaunchActor)> const &_fOnStdOutput
				, uint32 _WarningErrorStatus = TCLimitsInt<uint32>::mc_Max
			)
			-> TCFuture<CBashScriptOutput>
		;

		TCFuture<void> fp_InitApp();

		TCFuture<void> fp_InitSensor();
		TCFuture<void> fp_InitLog();
		TCFuture<void> fp_InitHostMonitor(NEncoding::CEJsonSorted _AutoUpdateConfig);
		TCFuture<void> fp_ConfigureHostMonitorFromHost(CStr _AutoUpdateConfig);
		void fp_ApplyPendingHostMonitorConfig();
		static CStr fsp_GetOSDependenciesFingerprint(TCMap<CStr, TCVector<CStr>> const &_Dependencies);
		TCFuture<void> fp_InstallOSDependencies(TCMap<CStr, TCVector<CStr>> _Dependencies);
		NConcurrency::TCActorFunctor<NConcurrency::TCFuture<void> ()> fp_HostMonitorRebootNeededFunctor(NEncoding::CEJsonSorted const &_AutoUpdateConfig);

		TCFuture<void> fp_StartApp(NEncoding::CEJsonSorted const _Params) override;
		TCFuture<void> fp_StopApp() override;
		TCFuture<void> fp_Destroy() override;
		TCFuture<void> fp_PauseReporting();
		TCFuture<void> fp_ReadState();
		void fp_InitApplications();
		void fp_OnApplicationAdded(TCSharedPointer<CApplication> const &_pApplication);

		TCFuture<CActorSubscription> fp_SetInProgressWithWait(TCSharedPointer<CApplication> _pApplication, CStr _Description);
		void fp_ReportInProgress(TCSharedPointer<CCommandLineControl> const &_pCommandLine, CStr const &_ApplicationName);

		TCFuture<CAppLaunchResult> fp_LaunchApp(TCSharedPointer<CApplication> _pApplication, bool _bOpenEncryption);
		TCFuture<CAppLaunchResult> fp_LaunchAppInternal(TCSharedPointer<CApplication> _pApplication, bool _bOpenEncryption);
		void fp_DiscardPreviousAppInterface(CApplication &_Application);
		void fp_ScheduleRelaunchApp(TCSharedPointer<CApplication> const &_pApplication);
		static void fsp_CreateApplicationUserGroup
			(
				CApplicationSettings const &_Settings
				, TCFunction<void (CStr const &_Info)> const &_fLogInfo
				, CStr const &_HomeDir
				, TCSharedPointer<CUniqueUserGroup> const &_pUniqueUserGroup
			)
		;
		static auto fsp_UpdateApplicationFilePermissions
			(
				CStr const &_ApplicationDir
				, TCSharedPointer<CApplication> const &_pApplication
				, CApplicationSettings const &_Settings
				, TCVector<CStr> const &_Files
				, TCSharedPointer<CUniqueUserGroup> const &_pUniqueUserGroup
				, TCFunction<void (CStr const &_Info)> const &_fLogInfo
			)
			-> TCMap<CStr, CStr>
		;
		static void fsp_ReassignApplicationFileOwnership(CStr const &_Directory, CStr const &_User, CStr const &_Group);
		static void fsp_SetDirectoryOwnerSkippingLinks(CStr const &_Directory, CStr const &_User, CStr const &_Group);
		static void fsp_ApplyStagedApplicationFiles
			(
				CAppManagerEnvironmentInterface::CApplicationApply const &_Apply
				, CStr const &_Directory
				, TCSharedPointer<CUniqueUserGroup> const &_pUniqueUserGroup
			)
		;
		TCFuture<void> fp_UpdateAppManagerApplicationVersion(TCSharedPointer<CApplication> _pApplication, uint32 _OldVersion);

		TCFuture<void> fp_UpdateApplicationJson(TCSharedPointer<CApplication> _pApplication);
		auto fp_RunUpdateScript
			(
				TCSharedPointer<CApplication> _pApplication
				, EUpdateScript _Script
				, CStr _Param
				, CVersionManager::CVersionIDAndPlatform _VersionID
				, CVersionManager::CVersionInformation const *_pVersionInformation
				, CVersionManager::CVersionIDAndPlatform _PreviousVersionID
				, CVersionManager::CVersionInformation _PreviousVersionInformation
				, fp64 _TimeSinceUpdateStart
				, bool _bOperationWaits
			)
			-> TCFuture<void>
		;
		TCFuture<void> fp_RunBashScript(CAppManagerEnvironmentInterface::CEnvironmentScript _Script, NStr::CStrSecure _RunAsUserPassword = {});
		TCFuture<bool> fp_SelfUpdate(TCSharedPointer<CApplication> _pApplication);
		TCFuture<ERebootResult> fp_Reboot(bool _bErrorOnPreventReboot);
		TCFuture<EPreventedReboot> fp_CheckAndLogPreventedReboot(CCheckAndLogPreventedRebootParams _Params);

		TCFuture<uint32> fp_CommandLine_EnumApplications(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_AddApplication(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_EnableSelfUpdate(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_ChangeApplicationSettings(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_RemoveApplication(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_UpdateApplication(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_StartApplication(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_StopApplication(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_RestartApplication(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);

		void fp_BuildCommandLine_Environments(CDistributedAppCommandLineSpecification &o_CommandLine);
		void fp_ReadEnvironmentsState();
		TCFuture<void> fp_UpdateEnvironmentJson(TCSharedPointer<CEnvironment> _pEnvironment);
		CAppManagerInterface::CEnvironmentInfo fp_GetEnvironmentInfo(CEnvironment const &_Environment);
		TCFuture<void> fp_RegisterEnvironmentPermissions(TCSharedPointer<CEnvironment> _pEnvironment);
		TCFuture<void> fp_UnregisterEnvironmentPermissions(TCSharedPointer<CEnvironment> _pEnvironment);

		auto fp_AddEnvironment
			(
				CStr _Name
				, CEnvironmentSettings _Settings
				, EEnvironmentSetting _ChangedSettings
				, TCFunction<void (CStr const &_Info)> _fOnInfo
				, CCallingHostInfo _CallingHostInfo
			)
			-> TCFuture<void>
		;
		TCFuture<void> fp_RemoveEnvironment(CStr _Name, bool _bDeleteApplicationData, TCFunction<void (CStr const &_Info)> _fOnInfo, CCallingHostInfo _CallingHostInfo);
		auto fp_ChangeEnvironmentSettings
			(
				CStr _Name
				, CEnvironmentSettings _Settings
				, EEnvironmentSetting _ChangedSettings
				, TCFunction<void (CStr const &_Info)> _fOnInfo
				, CCallingHostInfo _CallingHostInfo
			)
			-> TCFuture<void>
		;
		TCFuture<void> fp_StartEnvironment(CStr _Name, CCallingHostInfo _CallingHostInfo);
		TCFuture<void> fp_StopEnvironment(CStr _Name, CCallingHostInfo _CallingHostInfo);
		TCFuture<void> fp_RestartEnvironment(CStr _Name, CCallingHostInfo _CallingHostInfo);

		// Environment agent handling (host side)
		TCSharedPointer<CEnvironment> fp_EnvironmentFromHostID(CStr const &_HostID);
		TCSharedPointer<CApplication> fp_GetEnvironmentParentApplication(CEnvironment const &_Environment);
		TCSharedPointer<CApplication> fp_FindEnvironmentAgentApplication(CEnvironment const &_Environment);
		TCSharedPointer<CApplication> fp_FindAgentApplication(CStr const &_AgentApplication);
		CStr fp_GetEnvironmentStorageDirectory(CEnvironment const &_Environment);
		CStr fp_GetEnvironmentAgentDirectory(CEnvironment const &_Environment);
		CStr fp_GetEnvironmentDataDirectory(CEnvironment const &_Environment);
		TCFuture<void> fp_DeleteEnvironmentStorage(TCSharedPointer<CEnvironment> _pEnvironment);
		CStr fp_CheckLocalEnvironmentAccounts(CEnvironmentSettings const &_Settings);
		CStr fp_CheckLocalEnvironmentApplicationAccount(CStr const &_Application, CApplicationSettings const &_NewSettings);
		CStr fp_FindEnvironmentStoringDataInApplication(CStr const &_Application);
		bool fp_EnvironmentStorageReady(CEnvironment const &_Environment, CStr &o_Error, CAppManagerInterface::EStatusSeverity &o_Severity);
		bool fp_EnvironmentMountsReady(CEnvironment const &_Environment, CStr &o_Error, CAppManagerInterface::EStatusSeverity &o_Severity);
		void fp_AutoStartEnvironments();
		bool fp_EnvironmentAcceptsApplications(CStr const &_Name);
		static CStr fsp_GetEnvironmentGuestPlatform(CAppManagerInterface::EEnvironmentType _Type);
		TCFuture<void> fp_CheckEnvironmentAccess(CStr _Name, CCallingHostInfo _CallingHostInfo);
		TCFuture<void> fp_CheckEnvironmentRestartAccess(TCVector<CStr> _Names, CStr _Description, CCallingHostInfo _CallingHostInfo);
		auto fp_RestartEnvironmentsForSettings(TCVector<TCSharedPointer<CEnvironment>> _Environments, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine, bool _bConnected)
			-> TCFuture<uint32>
		;
		TCFuture<bool> fp_CommandLineProgress(NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine, bool _bConnected, CStr _Output, bool _bError);
		TCFuture<void> fp_EnsureOSDependencies(TCSharedPointer<CApplication> _pApplication);
		TCFuture<void> fp_EnsureEnvironmentStarted(TCSharedPointer<CEnvironment> _pEnvironment, EEnvironmentStart _Start = EEnvironmentStart::mc_UnlessStoppedManually);
		bool fp_EnvironmentUsesAgentSocket(TCSharedPointer<CEnvironment> const &_pEnvironment);
		CAppManagerEnvironmentSocketDirectory fp_GetEnvironmentAgentSocketDirectory(CEnvironment const &_Environment);
		CStr fp_GetEnvironmentAgentSocketPath(CEnvironment const &_Environment);
		void fp_StartAgentHostSocketForwarder();
		TCFuture<NWeb::NHTTP::CURL> fp_EnsureEnvironmentListen(TCSharedPointer<CEnvironment> _pEnvironment);
		TCFuture<void> fp_RemoveEnvironmentListen(TCSharedPointer<CEnvironment> _pEnvironment);
		TCFuture<void> fp_StartEnvironmentInternal(TCSharedPointer<CEnvironment> _pEnvironment, EEnvironmentStart _Start);
		TCFuture<void> fp_StartEnvironmentVM(TCSharedPointer<CEnvironment> _pEnvironment);
		CStr fp_GetEnvironmentVMBundleDirectory(CEnvironment const &_Environment);
		NVirtualization::CVirtualMachineConfig fp_BuildEnvironmentVMConfig(CEnvironment &_Environment);
		TCFuture<NVirtualization::CMacOSGuestProvisioning> fp_LoadVMImageProvisioning(CStr _BundleDirectory);
		CStr fp_GetVMBootstrapAskPassPath(CEnvironment const &_Environment);
		auto fp_BootstrapVMAgentOverSSH(TCSharedPointer<CEnvironment> _pEnvironment, NVirtualization::CMacOSGuestProvisioning _Provisioning, CStr _AgentExecutable, CStr _ConnectSettings)
			-> TCFuture<void>
		;
		TCFuture<CStr> fp_FetchVMAgentGuestLog(TCSharedPointer<CEnvironment> _pEnvironment, NVirtualization::CMacOSGuestProvisioning _Provisioning);
		void fp_RestartEnvironmentsForAgentApplication(CStr const &_ApplicationName);
		TCFuture<void> fp_WaitForEnvironmentIdle(TCSharedPointer<CEnvironment> _pEnvironment, fp64 _GuestDeferralsEnd = fp64::fs_Inf());
		CStr fp_FindApplicationPreventingEnvironmentRestart(CEnvironment const &_Environment);
		TCFuture<void> fp_RestartEnvironmentWhenIdle(TCSharedPointer<CEnvironment> _pEnvironment);
		TCFuture<void> fp_SetEnvironmentSensorStatus(TCSharedPointer<CEnvironment> _pEnvironment, CStr _Status, CAppManagerInterface::EStatusSeverity _Severity);
		TCFuture<void> fp_StopEnvironmentInternal(TCSharedPointer<CEnvironment> _pEnvironment);
		TCFuture<void> fp_ForceStopGuestEnvironmentsAfter(TCFuture<void> _Timeout);
		TCFuture<void> fp_ForceStopGuestEnvironment(TCSharedPointer<CEnvironment> _pEnvironment);
		TCFuture<void> fp_SetEnvironmentStoppedManually(TCSharedPointer<CEnvironment> _pEnvironment, bool _bStoppedManually);
		TCFuture<void> fp_PullEnvironment(CStr _Name, CCallingHostInfo _CallingHostInfo);
		TCFuture<void> fp_StopEnvironmentContainer(TCSharedPointer<CEnvironment> _pEnvironment);
		TCFuture<void> fp_KillEnvironmentContainer(TCSharedPointer<CEnvironment> _pEnvironment);
		TCFuture<bool> fp_EnvironmentContainerExists(TCSharedPointer<CEnvironment> _pEnvironment);
		TCFuture<bool> fp_OwnContainerSystemStopped(TCSharedPointer<CEnvironment> _pEnvironment);
		TCFuture<void> fp_PullEnvironmentContainerImage(TCSharedPointer<CEnvironment> _pEnvironment);
		TCFuture<void> fp_OnEnvironmentAgentConnected(TCSharedPointer<CEnvironment> _pEnvironment, TCDistributedActorInterface<CAppManagerEnvironmentInterface> _Interface);
		void fp_OnEnvironmentAgentDisconnected(TCSharedPointer<CEnvironment> _pEnvironment);
		void fp_AbortPendingDistributedAppWaits(CApplication &_Application, CStr const &_Error);
		TCFuture<void> fp_ConfirmEnvironmentAppStart(TCSharedPointer<CApplication> _pApplication);
		void fp_SendEnvironmentAgentConfig(CEnvironment &_Environment);
		TCFuture<void> fp_WaitForEnvironmentAgentConfiguration(TCSharedPointer<CEnvironment> _pEnvironment);
		TCFuture<void> fp_WaitForPendingAgentConfigurations(TCSharedPointer<CEnvironment> _pEnvironment);
		void fp_OnEnvironmentApplicationStateChange(TCSharedPointer<CEnvironment> const &_pEnvironment, CAppManagerEnvironmentInterface::CApplicationStateChange const &_Change);
		TCFuture<CStr> fp_GetEnvironmentAgentExecutable(TCSharedPointer<CEnvironment> _pEnvironment, TCSharedPointer<CStr> _pError);
		CStr fp_GetContainerRuntimeExecutable(TCSharedPointer<CEnvironment> const &_pEnvironment);
		static CStr fsp_GetContainerRuntimeExecutable(CEnvironmentSettings const &_Settings);
		CStr fp_GetContainerName(TCSharedPointer<CEnvironment> const &_pEnvironment);
		CStr fp_GetEnvironmentHostName(CEnvironment const &_Environment, CStr &o_Error);
		CAppManagerContainerLaunch fp_BuildEnvironmentContainerLaunch(TCSharedPointer<CEnvironment> const &_pEnvironment, CStr const &_AgentExecutableName, CStr &o_Error);
		TCFuture<void> fp_RemoveEnvironmentContainer(TCSharedPointer<CEnvironment> _pEnvironment);
		TCFuture<void> fp_RemoveEnvironmentContainerByName(TCSharedPointer<CEnvironment> _pEnvironment);
		bool fp_UseOwnAppleContainerSystem();
		CStr fp_GetAppleContainerAppRoot();
		void fp_ApplyAppleContainerLaunchEnvironment(CProcessLaunchParams &_LaunchParams);
		CProcessLaunchParams fp_BuildContainerCommandParams(TCSharedPointer<CEnvironment> const &_pEnvironment, TCVector<CStr> &&_Arguments);
		TCFuture<void> fp_EnsureAppleContainerSystem(TCSharedPointer<CEnvironment> _pEnvironment);
		bool fp_UseOwnColimaSystem(TCSharedPointer<CEnvironment> const &_pEnvironment);
		bool fp_EnvironmentUsesHostDataDirectory(CEnvironment const &_Environment);
		CStr fp_GetColimaAppRoot();
		CStr fp_GetColimaUser();
		CStr fp_GetColimaGroup();
		void fp_ApplyColimaLaunchEnvironment(CProcessLaunchParams &_LaunchParams);
		TCFuture<void> fp_EnsureColimaSystem(TCSharedPointer<CEnvironment> _pEnvironment);
		static void fsp_PrepareColimaEnvironmentDirectories(CColimaEnvironmentDirectories const &_Directories, CStr const &_User, CStr const &_Group);
		TCFuture<void> fp_PrepareColimaDataDirectory(TCSharedPointer<CEnvironment> _pEnvironment);
		TCFuture<void> fp_EnsureContainerSystem(TCSharedPointer<CEnvironment> _pEnvironment);
		TCFuture<CAppLaunchResult> fp_LaunchAppInEnvironment(TCSharedPointer<CApplication> _pApplication, TCSharedPointer<CEnvironment> _pEnvironment);
		TCFuture<void> fp_AbortEnvironmentLaunch(TCSharedPointer<CApplication> _pApplication, TCSharedPointer<CEnvironment> _pEnvironment);

		// Application storage managed inside environments (host side)
		bool fp_EnvironmentUsesRemoteStorage(CEnvironment const &_Environment);
		TCSharedPointer<CEnvironment> fp_ApplicationRemoteStorageEnvironment(CApplication const &_Application);
		CStr fp_CheckParentApplicationStorage(CApplicationSettings const &_Settings);
		CStr fp_FindEnvironmentSharingVMImage(CStr const &_Name, CEnvironmentSettings const &_Settings);
		CStr fp_FindApplicationSharingEnvironmentStorage(CStr const &_Name, CApplicationSettings const &_Settings);
		TCSharedPointer<CEnvironment> fp_LaunchEnvironmentRemoteStorage(CStr const &_LaunchEnvironment);
		CStr fp_GetEnvironmentDataVolumeName(CEnvironment const &_Environment);
		TCFuture<bool> fp_EnvironmentDataVolumeExists(TCSharedPointer<CEnvironment> _pEnvironment);
		CStr fp_GetEnvironmentDataVolumeImagePath(CEnvironment const &_Environment);
		CStr fp_GetEnvironmentDataVolumeMountPath(CEnvironment const &_Environment);
		TCFuture<CProcessLaunchActor::CSimpleLaunchResult> fp_RunColimaVMScript(CStr _Script, CStr _LogName);
		TCFuture<CProcessLaunchActor::CSimpleLaunchResult> fp_RunColima(TCVector<CStr> _Arguments, CStr _LogName);
		TCFuture<void> fp_EnsureEnvironmentDataVolume(TCSharedPointer<CEnvironment> _pEnvironment);
		TCFuture<void> fp_RemoveEnvironmentDataVolume(TCSharedPointer<CEnvironment> _pEnvironment);
		TCFuture<void> fp_StageApplicationInEnvironment(TCSharedPointerSupportWeak<CUpdateApplicationState> _pState, TCSharedPointer<CEnvironment> _pEnvironment);
		TCFuture<void> fp_RemoveApplicationEnvironmentStorage(TCSharedPointer<CApplication> _pApplication);

		// Application storage managed inside environments (agent side)
		TCFuture<void> fp_EnsureEnvironmentDataRoot();
		CStr fp_GetAgentApplicationDirectory(CStr const &_Name);

		// Environment agent handling (agent side)
		TCFuture<void> fp_RegisterWithEnvironmentHost();
		void fp_ForwardApplicationStateChange(CAppManagerEnvironmentInterface::CApplicationStateChange _Change);
		TCFuture<void> fp_ReportEnvironmentUpdateStateToHost(bool _bUpdating);
		TCFuture<void> fp_ReportEnvironmentBusyToHost();
		TCFuture<void> fp_RequestEnvironmentRestartFromHost();

		TCFuture<uint32> fp_CommandLine_EnumEnvironments(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_AddEnvironment(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_ChangeEnvironmentSettings(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_RemoveEnvironment(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_StartEnvironment(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_StopEnvironment(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_RestartEnvironment(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_EnvironmentBash(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_ColimaSettings(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_PullEnvironment(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_CreateVMImage(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
#ifdef DPlatformFamily_macOS
		TCFuture<void> fp_EndEnvironmentWindowSession(TCSharedPointer<CEnvironment> _pEnvironment, bool _bWasStarted);
		TCFuture<uint32> fp_CommandLine_EnvironmentWindow(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_VMSettings(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
#endif

		TCFuture<uint32> fp_CommandLine_ListAvailableVersions(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_VersionManagerList(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_CloudManagerList(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);

		TCFuture<uint32> fp_CommandLine_RemoveKnownHost(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);

		TCFuture<uint32> fp_CommandLine_CancelAllUpdates(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);

		TCFuture<uint32> fp_CommandLine_HostMonitorConfigList(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_HostMonitorConfigVersionList(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_HostMonitorConfigContentsGet(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);

		auto fp_AddApplication
			(
				CStr _Name
				, CApplicationSettings _Settings
				, EApplicationSetting _ChangedSettings
				, bool _bForceOverwrite
				, bool _bForceInstall
				, bool _bSettingsFromVersionInfo
				, TCFunction<void (CStr const &_Info)> _fOnInfo
				, CStr _FromLocalFile
				, TCOptional<CVersionManager::CVersionIDAndPlatform> _Version
				, CCallingHostInfo _CallingHostInfo
			)
			-> TCFuture<void>
		;

		auto fp_ChangeApplicationSettings
			(
				CStr _Name
				, CApplicationSettings _Settings
				, EApplicationSetting _ChangedSettings
				, bool _bUpdateFromVersionInfo
				, bool _bForce
				, TCFunction<void (CStr const &_Info)> _fOnInfo
				, CCallingHostInfo _CallingHostInfo
			)
			-> TCFuture<void>
		;

		auto fp_UpdateApplication
			(
				CStr _Name
				, CAppManagerInterface::CApplicationUpdate _Update
				, CVersionManager::CVersionInformation _VersionInfo
				, CStr _FromFileName
				, TCFunction<void (CStr const &_Info)> _fOnInfo
				, CCallingHostInfo _CallingHostInfo
				, bool _bCheckPermissions = true
			)
			-> TCFuture<void>
		;

		TCFuture<void> fp_UpdateApplicationRunProcess(TCSharedPointerSupportWeak<CUpdateApplicationState> _pState);
		TCFuture<void> fp_UpdateApplication_DownloadVersion(TCSharedPointerSupportWeak<CUpdateApplicationState> _pState);
		TCFuture<void> fp_UpdateApplication_Unpack(TCSharedPointerSupportWeak<CUpdateApplicationState> _pState);
		TCFuture<void> fp_UpdateApplication_StopOldApp(TCSharedPointerSupportWeak<CUpdateApplicationState> _pState);
		TCFuture<void> fp_UpdateApplication_PreUpdate(TCSharedPointerSupportWeak<CUpdateApplicationState> _pState);
		TCFuture<void> fp_UpdateApplication_UpdateApplicationFiles(TCSharedPointerSupportWeak<CUpdateApplicationState> _pState);
		TCFuture<void> fp_UpdateApplication_SaveApplicationState(TCSharedPointerSupportWeak<CUpdateApplicationState> _pState);
		TCFuture<void> fp_UpdateApplication_PostUpdate(TCSharedPointerSupportWeak<CUpdateApplicationState> _pState);
		TCFuture<bool> fp_UpdateApplication_StartNewApp(TCSharedPointerSupportWeak<CUpdateApplicationState> _pState);
		TCFuture<void> fp_UpdateApplication_DeferToNextRestart(TCSharedPointerSupportWeak<CUpdateApplicationState> _pState, TCSharedPointer<CCanDestroyTracker> _pCanDestroy);
		TCFuture<void> fp_UpdateApplication_PostLaunch(TCSharedPointerSupportWeak<CUpdateApplicationState> _pState);

		TCFuture<void> fp_CancelAllApplicationUpdatesOnStopAppManager();

		void fp_StartPendingSelfUpdateReporting(CPendingSelfupdate const &_Pending);

		static void fsp_UpdateAttributes(CStr const &_File);
		static auto fsp_UnpackApplication
			(
				CStr const &_ApplicationRootDirectory
				, CStr const &_Source
				, CStr const &_Destination
				, CStr const &_ApplicationName
				, CApplicationSettings const &_Settings
				, TCVector<CStr> &o_Files
				, TCSet<CStr> const &_AllowExist
				, bool _bForceInstall
				, bool _bEnvironmentManagedStorage
				, TCSharedPointer<CUniqueUserGroup> const &_pUniqueUserGroup
			)
			-> CStr
		;
		TCFuture<void> fp_ChangeEncryption(TCSharedPointer<CApplication> _pApplication, EEncryptOperation _Operation, bool _bForceOverwrite);

		CStr fp_GetApplicationStopErrors(TCAsyncResult<uint32> const &_Result, CStr const &_Name);
		TCFuture<void> fp_KeyManagersUpdated();

		TCFuture<void> fp_VersionManagerResubscribeAll();
		TCFuture<void> fp_VersionManagerListPlatformVersions(CStr _Application, CStr _Platform);
		TCFuture<void> fp_VersionManagerListPlatformVersionsFrom(TCDistributedActor<CVersionManager> _Manager, CStr _Application, CStr _Platform);
		TCFuture<void> fp_VersionManagerSubscribe(TCDistributedActor<CVersionManager> _VersionManager);
		TCFuture<void> fp_VersionManagerAdded(TCDistributedActor<CVersionManager> _VersionManager, CTrustedActorInfo _Info);
		void fp_VersionManagerRemoved(TCWeakDistributedActor<CActor> const &_VersionManager);

		auto fp_DownloadApplicationFromManager(TCDistributedActor<CVersionManager> _Manager, CStr _ApplicationName, CVersionManager::CVersionIDAndPlatform _VersionID, CStr _DestinationDir)
			-> TCFuture<CVersionManager::CVersionInformation>
		;

		TCFuture<CVersionManager::CVersionInformation> fp_DownloadApplication(CStr _ApplicationName, CVersionManager::CVersionIDAndPlatform _VersionID, CStr _DestinationDir);

		auto fp_ReadPackageVersionInfo(CStr _Package, TCFunction<void (CStr const &_Info)> _fOnInfo) -> TCFuture<NStorage::TCOptional<CPackageVersionInfo>>;

		void fp_AutoUpdate_Update();

		auto fp_VersionIsNewer
			(
				CVersionManager::CVersionIDAndPlatform const &_LeftVersion
				, CVersionManager::CVersionInformation const &_LeftVersionInfo
				, CVersionManager::CVersionIDAndPlatform const &_RightVersion
				, CVersionManager::CVersionInformation const &_RightVersionInfo
				, TCSet<CStr> const &_RequiredTags
				, TCSet<CStr> const &_AllowedBranches
			)
			-> bool
		;

		auto fp_FindVersion
			(
				TCSharedPointer<CApplication> const &_pApplication
				, TCSet<CStr> const &_RequiredTags
				, TCSet<CStr> const &_AllowedBranches
				, CStr const &_Platform
				, CStr &o_Error
				, EFindVersionFlag _Flags
				, CVersionManager::CVersionInformation &o_VersionInfo
				, CVersionManager::CVersionIDAndPlatform &o_NewestUnconditionalVersion
				, CVersionManager::CVersionInformation &o_NewestUnconditionalVersionInfo
				, bool &o_bNewestUnconditionalVersionChanged
			)
			-> CVersionManager::CVersionIDAndPlatform
		;

		TCFuture<void> fp_SetupAppManagerInterfacePermissions();
		TCFuture<void> fp_SubscribePermissions();
		TCFuture<void> fp_RegisterPermissions();
		TCFuture<void> fp_RegisterApplicationPermissions(TCSharedPointer<CApplication> _pApplication);
		TCFuture<void> fp_UnregisterApplicationPermissions(TCSharedPointer<CApplication> _pApplication);

		TCFuture<void> fp_PublishAppInterface();
		TCFuture<void> fp_PublishAppManagerInterface();

		TCFuture<void> fp_OnUpdateEvent(TCSharedPointerSupportWeak<CUpdateApplicationState> _pState, EUpdateStage _Stage, NStr::CStr _Message);

		TCFuture<void> fp_SendUpdateNotifications(CAppManagerInterface::CUpdateNotification _Notification);
		TCFuture<void> fp_SendMissedUpdateNotifications(CStr _SubscriptionID, uint64 _LastSeenNotification);
		TCFuture<void> fp_SendUpdateNotification(CStr _SubscriptionID, CAppManagerInterface::CUpdateNotification _Notification, bool _bSequence);

		CAppManagerInterface::CApplicationInfo fp_GetApplicationInfo(CApplication const &_Application);
		TCFuture<void> fp_ChangeNotifications_SendChanges(TCVector<CAppManagerInterface::CChangeNotification> _Notifications, CStr _Application);
		TCFuture<void> fp_ChangeNotifications_PermissionsChanged();
		TCFuture<void> fp_ChangeNotifications_SendInitial(CStr _SubscriptionID);
		void fp_SendAppChange_AddedOrChanged(CApplication const &_Application);
		void fp_SendAppChange_Removed(CApplication const &_Application);
		void fp_SendAppChange_Status(CApplication const &_Application);
		TCFuture<void> fp_SendAppChange_Empty(CStr _ApplicationName);

		TCFuture<void> fp_SyncNotifications(CStr _ApplicationName);

		TCFuture<void> fp_PublishCoordinationInterface();
		TCFuture<void> fp_SubscribeCoordinationInterface();
		TCFuture<void> fp_NewRemoteAppManager(CStr _HostID, TCDistributedActor<CAppManagerCoordinationInterface> _Actor);
		void fp_NewRemoteKnownApplication(CRemoteApplicationKey const &_RemoteKey, CStr const &_HostID);
		void fp_SendInitialInfoToRemoteAppManager(CRemoteAppManager &_AppManager);
		void fp_OnAppUpdateInfoChange(TCSharedPointer<CApplication> const &_pApplication);
		void fp_OnAppUpdateInfoChange();
		void fp_SendAppToRemoteAppManagers(TCSharedPointer<CApplication> const &_pApplication);
		void fp_SendRemovedAppToRemoteAppManagers(TCSharedPointer<CApplication> const &_pApplication);
		void fp_BroadcastRemoteAppChange(CAppManagerCoordinationInterface::CAppChange &&_Change);

		void fp_AppLaunchStateChanged(TCSharedPointer<CApplication> const &_pApplication, CStr const &_State, CAppManagerInterface::EStatusSeverity _Severity);
		void fp_AppEncryptionStateChanged(TCSharedPointer<CApplication> const &_pApplication, bool _bEncrypted);
		void fp_SetAppLaunchStatus(TCSharedPointer<CApplication> const &_pApplication, CStr const &_LaunchStatus, CAppManagerInterface::EStatusSeverity _Severity);

		TCFuture<void> fp_UpdateEncryptionSensorStatus(bool _bNeedEncryption);
		TCFuture<void> fp_SetApplicationSensorStatus(TCSharedPointer<CApplication> _pApplication, CStr _LaunchStatus, CAppManagerInterface::EStatusSeverity _Severity);
		TCFuture<void> fp_Coordination_WaitForOurAppsTurnToUpdate(TCSharedPointerSupportWeak<CUpdateApplicationState> _pState);
		TCFuture<void> fp_Coordination_OneAtATime_WaitForOurTurnToUpdate(TCSharedPointerSupportWeak<CUpdateApplicationState> _pState);
		auto fp_Coordination_WaitForAllToReachWantUpdateStage
			(
				TCSharedPointerSupportWeak<CUpdateApplicationState> _pState
				, EUpdateStage _Stage
				, fp64 _Timeout
				, EWaitStageFlag _Flags
				, TCFunctionMutable<bool ()> _fEvalState = {}
			)
			-> TCFuture<void>
		;
		auto fp_Coordination_WaitForState
			(
				TCSharedPointerSupportWeak<CUpdateApplicationState> _pState
				, fp64 _Timeout
				, TCFunctionMutable<bool (TCPromise<void> &_Promise)> _fEvalState
				, CStr _RemoteFailError
				, CStr _TimeoutError
				, CStr _DisconnectedError
				, bool _bIgnoreFailed = false
			)
			-> TCFuture<void>
		;
		static ch8 const *fsp_UpdateTypeToStr(EDistributedAppUpdateType _UpdateType);

		TCFuture<void> fp_SetupLimits();
		void fp_UpdateLimits();

		bool fp_AutoStartApp(TCSharedPointer<CApplication> const &_pApplication, bool &o_bNeedEncryption);
		void fp_UpdateApplicationDependencies();

		void fp_InitialStartupFailed(CExceptionPointer const &_pException);
		TCFuture<void> fp_ClearPreventLaunch(TCSharedPointer<CApplication> _pApplication);

		void fp_ApplicationStartBackup(TCSharedPointer<CApplication> const &_pApplication);
		TCFuture<void> fp_RestartApplicationBackup(TCSharedPointer<CApplication> _pApplication);

		CStr fp_GetRunAsUser(CApplicationSettings const &_Settings) const;
		CStr fp_GetRunAsGroup(CApplicationSettings const &_Settings) const;
		CStr fp_TransformUserGroup(CStr const &_UserName) const;

		TCFuture<void> fp_CloudManagerAdded(TCDistributedActor<CCloudManager> _CloudManager, CTrustedActorInfo _Info);
		TCFuture<void> fp_CloudManagerRemoved(TCWeakDistributedActor<CActor> _CloudManager);

		TCSharedPointer<CApplication> fp_ApplicationFromHostID(CStr const &_HostID);
		TCSharedPointer<CApplication> fp_ApplicationFromLaunchID(CStr const &_LaunchID);

		TCFuture<void> fp_PerformDatabaseCleanup();
		TCFuture<void> fp_SetupDatabaseCleanup();
		TCFuture<void> fp_SetupDatabase();
		TCFuture<NDatabase::CDatabaseActor::CTransactionWrite> fp_CleanupDatabase(NDatabase::CDatabaseActor::CTransactionWrite _WriteTransaction);
		TCFuture<uint64> fp_StoreUpdateNotification(CAppManagerInterface::CUpdateNotification _Notification);
		TCFuture<uint32> fp_CommandLine_StoredUpdateNotificationList(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_StoredUpdateNotificationClear(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);

		static CStr fsp_LimitErrorLogSize(CStr const &_String, umint _ExtraSize);

		TCFuture<void> fp_RebootPrevention_WatchSensor(TCSharedPointer<CApplication> _pApplication, CDistributedAppSensorReporter::CSensorInfo _SensorInfo);
		TCFuture<void> fp_RebootPrevention_RemoveApplication(CStr _Application);
		TCFuture<void> fp_RebootPrevention_WatchInitialSensors();
		TCFuture<void> fp_RebootPrevention_UpdateApplications();
		void fp_RebootPrevention_UpdateWatchFlags(CDistributedAppSensorReporter::CSensorInfoKey const &_Key, CSensorRebootWatch &_Watch, NTime::CTime const &_Now);
		void fp_RebootPrevention_SetWatchFlags
			(
				CDistributedAppSensorReporter::CSensorInfoKey const &_Key
				, CSensorRebootWatch &_Watch
				, CDistributedAppSensorReporter::ESensorInfoFlag _Prevented
			)
		;
		void fp_RebootPrevention_UpdateApplicationFlags(CStr const &_Application);
		void fp_RebootPrevention_RemoveWatch(CDistributedAppSensorReporter::CSensorInfoKey const &_Key);
		void fp_RebootPrevention_SetApplicationFlags(CApplication &_Application, CDistributedAppSensorReporter::ESensorInfoFlag _Prevented);
		bool fp_ApplicationPreventsReboot(CApplication const &_Application, fp64 _Now);
		bool fp_ApplicationRunsInGuest(CApplication const &_Application);
		bool fp_StopWaitsOnlyForGuests(CApplication const &_Application);
		bool fp_StopWaitsOnlyForGuests(CApplication const &_Application, TCSet<CApplication const *> &o_Visited);

		static constexpr uint32 mcp_CurrentAppMangerVersion = 0x101;

		// Directories in the AppManager root and in application directories, which no application can name, since application names don't start
		// with a dot, so a child application can't take them
		static constexpr ch8 const *mcp_pEnvironmentStorageDirectory = ".Environment";
		static constexpr ch8 const *mcp_pVMImagesDirectory = ".VMImages";

		static constexpr ch8 const *mcp_pEnvironmentDataVolumeName = "MalterlibData";
		static constexpr ch8 const *mcp_pEnvironmentDataRootVM = "/Volumes/MalterlibData";
		static constexpr ch8 const *mcp_pEnvironmentDataRootContainer = "/opt/Malterlib/Data";
		static constexpr ch8 const *mcp_pEnvironmentAgentRootContainer = "/opt/Malterlib/Agent";
		static constexpr uint32 mcp_VMAgentHostPort = 19788; /// Virtio socket port of the host that VM guests reach the environment listen through

#ifdef DPlatformFamily_Windows
		TCSharedPointer<CUniqueUserGroup> mp_pUniqueUserGroup = fg_Construct("C:/M", CDistributedAppActor::mp_State.m_RootDirectory);
#else
		TCSharedPointer<CUniqueUserGroup> mp_pUniqueUserGroup = fg_Construct("/M", CDistributedAppActor::mp_State.m_RootDirectory);
#endif

		TCMap<CStr, TCSharedPointer<CApplication>> mp_Applications;
		TCSet<CStr> mp_AddingApplicationNames;                                     /// Names of the applications being added
		TCMap<CStr, TCSharedPointer<CApplication>> mp_AddingApplicationsInStorage; /// Applications being added that use the environment storage they checked
		TCMap<CStr, TCSharedPointer<CEnvironment>> mp_Environments;
		TCTrustedActorSubscription<CKeyManager> mp_KeyManagerSubscription;
		TCTrustedActorSubscription<CVersionManager> mp_VersionManagerSubscription;
		TCTrustedActorSubscription<CCloudManager> mp_CloudManagerSubscription;
		TCSet<CStr> mp_KnownPlatforms;
		TCSet<CStr> mp_CreatingVMImageBundles;                                     /// Lower case bundle directories of the VM images being created, which environments don't boot yet

		fp32 mp_PauseReportingForSecondsAtShutdown = 60.0;

		bool mp_bPendingAutoUpdate = false;
		bool mp_bLogLaunchesToStdErr = false;
		bool mp_bPendingSelfUpdateInProgress = false;
		bool mp_bEnableApplicationStatusSensors = true;
		bool mp_bEnableEncryptionStatusSensors = true;
		bool mp_bAppleContainerSystemReady = false;
		bool mp_bAppleContainerSystemStarting = false;
		TCVector<TCPromise<void>> mp_OnAppleContainerSystemReady;
		bool mp_bColimaSystemReady = false;
		bool mp_bColimaSystemStarting = false;
		CStr mp_ColimaConfigFingerprint;
		TCVector<TCPromise<void>> mp_OnColimaSystemReady;
		uint32 mp_ColimaCPUCount = 0; /// 0 leaves the value to the colima profile
		uint64 mp_ColimaMemoryMB = 0; /// 0 leaves the value to the colima profile
		uint64 mp_ColimaDiskGB = 0;   /// 0 leaves the value to the colima profile

		bool mp_bVMWindow = false; /// Applies at VM start: hosts the VM in a window showing the guest display when a graphical session is available

		fp64 mp_AutoUpdateDelay = mc_DefaultAutoUpdateDelay;

		TCActor<CDatabaseActor> mp_DatabaseActor;

		CStr mp_DatabaseUniqueKey;
		uint64 mp_LastUpdateSequence = 0;

		CActorSubscription mp_CleanupTimerSubscription;
		uint64 mp_RetentionDays = 0;

		DMibActorEntries(Owner) TCMap<CStr, CVersionManagerDownloadState> mp_Downloads;

		TCMap<CStr, CVersionManagerApplication> mp_VersionManagerApplications;
		TCMap<TCDistributedActor<CVersionManager>, CVersionManagerState> mp_VersionManagers;

		TCMap<TCDistributedActor<CCloudManager>, CCloudManagerState> mp_CloudManagers;

		TCDistributedActorInstance<CDistributedAppInterfaceServerImplementation> mp_AppInterfaceServer;
		TCDistributedActorInstance<CAppManagerInterfaceImplementation> mp_AppManagerInterface;
		TCDistributedActorInstance<CAppManagerCoordinationInterfaceImplementation> mp_AppManagerCoordinationInterface;
		TCDistributedActorInstance<CAppManagerEnvironmentInterfaceImplementation> mp_EnvironmentInterface;
		TCDistributedActorInstance<CAppManagerEnvironmentHostInterfaceImplementation> mp_EnvironmentHostInterface;
		TCTrustedActorSubscription<CAppManagerEnvironmentHostInterface> mp_EnvironmentHostActors;
		TCDistributedActor<CAppManagerEnvironmentHostInterface> mp_EnvironmentHostActor;
		CActorSubscription mp_EnvironmentHostRegistration;
		bool mp_bEnvironmentAgent = false;
		bool mp_bStoppingApp = false;                  /// The shutdown waits for environment starts, so long start retries give up
		bool mp_bEnvironmentContainer = false;         /// Agent side: the environment is a container, so the kernel belongs to the host/VM outside it
		CStr mp_EnvironmentHostID;                     /// Agent side: host id of the launching AppManager, from the deployment settings
		CStr mp_EnvironmentDataRoot;                   /// Agent side: root of the environment-managed application storage, from the deployment settings
		bool mp_bEnvironmentPatching = false;          /// Agent side: OS updates are installing; reported again to a host that reconnects
		umint mp_nEnvironmentOSDependencyInstalls = 0; /// Running OS dependency installations; prevent reboots, and agents report them like OS updates
		bool mp_bEnvironmentDataRootReady = false;
		bool mp_bEnvironmentDataRootPreparing = false;
		TCVector<TCPromise<void>> mp_OnEnvironmentDataRootReady;

		NStorage::TCUniquePointer<NVirtualization::CGuestHostSocketForwarder> mp_pAgentHostSocketForwarder; /// Agent side in a VM guest: its way to the environment listen

		TCMap<CStr, CApplicationStageReceive> mp_ApplicationStageReceives; /// Agent side, by stage id; dropping the finish functor aborts an in-flight receive

		CTrustedPermissionSubscription mp_Permissions;

		TCMap<CStr, CUpdateNotificationSubscription> mp_UpdateNotificationSubscriptions;
		TCTrustedActorSubscription<CAppManagerCoordinationInterface> mp_RemoteAppManagers;

		TCMap<CStr, CChangeNotificationSubscription> mp_ChangeNotificationSubscriptions;
		CSequencer mp_ChangeNotificationsPermissionsChangedSequencer{"AppManagerActor ChangeNotificationsPermissionsChangedSequencer"};

		TCMap<CStr, CRemoteAppManager> mp_RemoteAppManagerState;
		TCAVLTree<&CRemoteAppManager::m_ByActorLink, CRemoteAppManager::CCompareActor> mp_RemoteAppManagerStateByActor;
		TCMap<CRemoteApplicationKey, TCSet<CStr>> mp_KnownRemoteApplications;
		TCSet<TCSharedPointerSupportWeak<COnAppUpdateInfoChange>> mp_OnAppUpdateInfoChange;

		TCSharedPointer<CCanDestroyTracker> mp_pCanDestroy = fg_Construct();
		TCVector<TCActor<CProcessLaunchActor>> mp_LaunchActors;

		TCPromise<void> mp_InitialStartupResult;
		TCFuture<void> mp_InitialStartupResultFuture;
		umint mp_PendingAutoLaunches = 0;

		uint64 mp_AppStageChangeSequence = 0;

		TCSet<TCWeakPointer<CUpdateApplicationState>> mp_RunningUpdates;

		TCPromise<void> mp_CancelRunningUpdatesOnStopAppManagerPromise;

		TCActor<CDistributedAppSensorStoreLocal> mp_SensorStore;
		TCDistributedActorInstance<CDistributedAppSensorReporterImplementation> mp_SensorReporterInterface;

		TCActor<CDistributedAppLogStoreLocal> mp_LogStore;
		TCDistributedActorInstance<CDistributedAppLogReporterImplementation> mp_LogReporterInterface;

		fp64 mp_HostMonitorInterval = CHostMonitor::mc_DefaultHostMonitorInterval;
		fp64 mp_HostMonitorPatchInterval = CHostMonitor::mc_DefaultHostMonitorPatchInterval;
		fp64 mp_HostMonitorMemoryInterval = CHostMonitor::mc_DefaultHostMonitorMemoryInterval;

		TCActor<CHostMonitor> mp_HostMonitor;
		CStr mp_AppliedHostMonitorConfig;                                                             /// Agent side: the configuration of the last successful host monitor initialization
		TCOptional<CStr> mp_PendingHostMonitorConfig;                                                 /// Agent side: a host configuration that waits for the patching to end
		CSequencer mp_HostMonitorConfigureSequencer{"AppManagerActor HostMonitorConfigureSequencer"}; /// Agent side: one host configuration replaces the monitor at a time
		CSequencer mp_OSDependenciesSequencer{"AppManagerActor OSDependencies"};                      /// Package managers hold exclusive locks, so installs run one at a time
		CActorSubscription mp_PatchingOSDependenciesSequence;                                         /// The turn of mp_OSDependenciesSequencer that automatic patching holds
		umint mp_HostMonitorGeneration = 0;                                                           /// Agent side: advanced for each host monitor; a replaced one's functors give up
		TCSet<CStr> mp_InstalledOSDependenciesFingerprints;                                           /// Fingerprints of the installed OS dependencies; not persisted
		CActorSubscription mp_MainDirectoryMonitorSubscription;
		CActorSubscription mp_EnvironmentDataRootMonitorSubscription;                                 /// Agent side: monitors the environment-managed application storage volume, if any
		CActorSubscription mp_MainConfigFileMonitorSubscription;
		CStr mp_OsName;

		CDistributedAppSensorReporter::CSensorReporter mp_EncryptionSensorReporter;
		CSequencer mp_EncryptionSensorReporterSequencer{"EncryptionSensorReporter"};
		TCOptional<CDistributedAppSensorReporter::CStatus> mp_LastEncryptionReporterSensorStatus;

		CActorSubscription mp_RebootScheduleTimerSubscrption;
		bool mp_bRebooting = false;
		bool mp_bRebootScheduled = false;

		CStr mp_LastPreventRebootDescription;
		TCMap<CDistributedAppSensorReporter::CSensorInfoKey, CSensorRebootWatch> mp_SensorRebootPreventionWatch;
		TCMap<CStr, TCSet<CDistributedAppSensorReporter::CSensorInfoKey>> mp_SensorRebootPreventionWatchByApplication; /// The keys of mp_SensorRebootPreventionWatch
		TCMap<CStr, TCMap<CStr, CSensorRebootPreventionCounts>> mp_SensorRebootPreventionCounts;                       /// Per application and reporting host ID, the watches per flag
		CSequencer mp_SensorRebootPreventionWatchSequencer{"AppManagerActor SensorRebootPreventionWatchSequencer"};
		CActorSubscription mp_SensorRebootPreventionTimerSubscription;
	};

	CStr fg_ConcatOutput(CStr const &_StdOut, CStr const &_StdErr);

	bool fg_AppManager_PathsOverlap(CStr const &_Left, CStr const &_Right);

#ifdef DPlatformFamily_macOS
	CEJsonSorted fg_AppManager_BuildVMWindowConfig(NVirtualization::CVirtualMachineConfig const &_Config, CStr const &_Backend, CStr const &_Title);
	NVirtualization::CVirtualMachineConfig fg_AppManager_ParseVMWindowConfig(CEJsonSorted const &_Params, NVirtualization::EVirtualizationBackend &o_Backend, CStr &o_Title);
	TCFuture<CEJsonSorted> fg_AppManager_HandleEnvironmentWindowRequest(CEJsonSorted _Params, CEJsonSorted _Request, NStorage::TCSharedPointer<CRunLoop> _pRunLoop);

	uint32 fg_AppManager_RunVMWindowHost(CStr const &_ConfigPath);
#endif
}

#include "Malterlib_Cloud_App_AppManager.hpp"
