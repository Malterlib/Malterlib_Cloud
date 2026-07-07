// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Encoding/EJson>
#include <Mib/Storage/Optional>
#include <Mib/Storage/Variant>
#include <Mib/Cloud/BackupManager>
#include "Malterlib_Cloud_VersionManager.h"

namespace NMib::NCloud
{
	struct CAppManagerInterface : public NConcurrency::CActor
	{
		static constexpr ch8 const *mc_pDefaultNamespace = "com.malterlib/Cloud/AppManager";

		enum : uint32
		{
			EProtocolVersion_Min = 0x107
			, EProtocolVersion_AddRunAsUserHasShell = 0x108
			, EProtocolVersion_AddCoordinatedWait = 0x109
			, EProtocolVersion_AddStatusSeverity = 0x110
			, EProtocolVersion_AddFiltered = 0x111
			, EProtocolVersion_AddAccessDenied = 0x112
			, EProtocolVersion_AddAutoUpdateAndExtendAppInfo = 0x114
			, EProtocolVersion_AddBypassCoordination = 0x115
			, EProtocolVersion_AddLaunchInProcess = 0x116
			, EProtocolVersion_ExtendUpdateNotification = 0x117
			, EProtocolVersion_HostIDInApplicationInfo = 0x118
			, EProtocolVersion_OptionalWaitForNotificationResult = 0x119
			, EProtocolVersion_ResumableUpdateNotifications = 0x119
			, EProtocolVersion_AddLaunchEnvironment = 0x11a
			, EProtocolVersion_Current = 0x11a
		};

		enum EUpdateStage : uint32
		{
			EUpdateStage_Failed						= 0x70000000
			, EUpdateStage_None						= 0
			, EUpdateStage_SyncStart				= 0x10000
			, EUpdateStage_ChangeEncryption			= 0x20000
			, EUpdateStage_DownloadVersion			= 0x30000
			, EUpdateStage_Unpack					= 0x40000
			, EUpdateStage_StopOldApp				= 0x50000
			, EUpdateStage_PreUpdateScript			= 0x60000
			, EUpdateStage_UpdateApplicationFiles	= 0x70000
			, EUpdateStage_SaveApplicationState		= 0x80000
			, EUpdateStage_PostUpdateScript			= 0x90000
			, EUpdateStage_StartNewApp				= 0xa0000
			, EUpdateStage_PostLaunch				= 0xb0000
			, EUpdateStage_Finished					= 0xc0000
		};

		enum EStatusSeverity
		{
			EStatusSeverity_None
			, EStatusSeverity_Warning
			, EStatusSeverity_Error
		};

		enum EApplicationChange
		{
			EApplicationChange_AddOrChangeInfo
			, EApplicationChange_Remove
			, EApplicationChange_Status
		};

		enum EEnvironmentType : uint32
		{
			EEnvironmentType_Local		/// Agent runs as a plain child process on the host (no isolation, mainly for tests)
			, EEnvironmentType_Container	/// Agent runs as the main process of a Linux container
			, EEnvironmentType_VM		/// Agent runs as a daemon inside a virtual machine
		};

		struct CVersionIDAndPlatform : public CVersionManager::CVersionIDAndPlatform
		{
			CVersionIDAndPlatform() = default;
			CVersionIDAndPlatform(CVersionIDAndPlatform const &) = default;
			CVersionIDAndPlatform(CVersionIDAndPlatform &&) = default;
			CVersionIDAndPlatform &operator = (CVersionIDAndPlatform const &) = default;
			CVersionIDAndPlatform &operator = (CVersionIDAndPlatform &&) = default;

			CVersionIDAndPlatform(CVersionManager::CVersionIDAndPlatform const &_Other) : CVersionManager::CVersionIDAndPlatform(_Other) { }
			CVersionIDAndPlatform &operator = (CVersionManager::CVersionIDAndPlatform const &_Right) { static_cast<CVersionManager::CVersionIDAndPlatform &>(*this) = _Right; return *this; }

			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);
		};

		struct CVersionID : public CVersionManager::CVersionID
		{
			CVersionID() = default;
			CVersionID(CVersionID const &) = default;
			CVersionID(CVersionID &&) = default;
			CVersionID &operator = (CVersionID const &) = default;
			CVersionID &operator = (CVersionID &&) = default;

			CVersionID(CVersionManager::CVersionID const &_Other) : CVersionManager::CVersionID(_Other) { }
			CVersionID &operator = (CVersionManager::CVersionID const &_Right) { static_cast<CVersionManager::CVersionID &>(*this) = _Right; return *this; }

			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);
		};

		struct CVersionInformation : public CVersionManager::CVersionInformation
		{
			CVersionInformation() = default;
			CVersionInformation(CVersionInformation const &) = default;
			CVersionInformation(CVersionInformation &&) = default;
			CVersionInformation &operator = (CVersionInformation const &) = default;
			CVersionInformation &operator = (CVersionInformation &&) = default;

			CVersionInformation(CVersionManager::CVersionInformation const &_Other) : CVersionManager::CVersionInformation(_Other) { }
			CVersionInformation &operator = (CVersionManager::CVersionInformation const &_Right) { static_cast<CVersionManager::CVersionInformation &>(*this) = _Right; return *this; }

			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);
		};

		struct CApplicationSettings
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NStorage::TCOptional<NStr::CStr> m_VersionManagerApplication;	/// If left empty when adding an application an null application is added.
																			/// Useful for using as encrypted parent application
			NStorage::TCOptional<NStr::CStr> m_UpdateGroup;
			NStorage::TCOptional<NStr::CStr> m_Executable;
			NStorage::TCOptional<NContainer::TCVector<NStr::CStr>> m_ExecutableParameters;
			NStorage::TCOptional<NStr::CStr> m_RunAsUser;
			NStorage::TCOptional<NStr::CStr> m_RunAsGroup;
			NStorage::TCOptional<bool> m_bRunAsUserHasShell;

			NStorage::TCOptional<NContainer::TCMap<NStr::CStr, NFile::CDirectoryManifestConfig::CDestination>> m_Backup_IncludeWildcards;
			NStorage::TCOptional<NContainer::TCSet<NStr::CStr>> m_Backup_ExcludeWildcards;
			NStorage::TCOptional<NContainer::TCMap<NStr::CStr, NFile::EDirectoryManifestSyncFlag>> m_Backup_AddSyncFlagsWildcards;
			NStorage::TCOptional<NContainer::TCMap<NStr::CStr, NFile::EDirectoryManifestSyncFlag>> m_Backup_RemoveSyncFlagsWildcards;
			NStorage::TCOptional<NTime::CTimeSpan> m_Backup_NewBackupInterval;

			NStorage::TCOptional<bool> m_bAutoUpdate;
			NStorage::TCOptional<NContainer::TCSet<NStr::CStr>> m_UpdateTags;
			NStorage::TCOptional<NContainer::TCSet<NStr::CStr>> m_UpdateBranches; // Are wild cards
			NStorage::TCOptional<NStr::CStr> m_UpdateScriptPreUpdate;
			NStorage::TCOptional<NStr::CStr> m_UpdateScriptPostUpdate;
			NStorage::TCOptional<NStr::CStr> m_UpdateScriptPostLaunch;
			NStorage::TCOptional<NStr::CStr> m_UpdateScriptOnError;
			NStorage::TCOptional<NContainer::TCSet<NStr::CStr>> m_Dependencies;

			NStorage::TCOptional<NStr::CStr> m_LaunchEnvironment; /// Name of the environment to launch the application in. Empty means launching directly on the host.

			NStorage::TCOptional<bool> m_bDistributedApp;
			NStorage::TCOptional<bool> m_bSelfUpdateSource;
			NStorage::TCOptional<bool> m_bStopOnDependencyFailure;
			NStorage::TCOptional<bool> m_bBackupEnabled;
			NStorage::TCOptional<bool> m_bLaunchInProcess;
		};

		struct CApplicationInfo
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NEncoding::CEJsonSorted f_ToJson() const;

			NStr::CStr const &f_GetName()
			{
				return NContainer::TCMap<NStr::CStr, CApplicationInfo>::fs_GetKey(*this);
			}

			// State
			NStr::CStr m_Status;
			EStatusSeverity m_StatusSeverity = EStatusSeverity_None;

			// Immutable
			NStr::CStr m_EncryptionStorage;
			NStr::CStr m_EncryptionFileSystem;
			NStr::CStr m_ParentApplication;

			// Updatable
			NStr::CStr m_HostID;

			CVersionIDAndPlatform m_Version;
			CVersionInformation m_VersionInfo;

			CVersionIDAndPlatform m_FailedVersion;
			CVersionInformation m_FailedVersionInfo;
			NStr::CStr m_FailedVersionError;

			CVersionIDAndPlatform m_WantVersion;
			CVersionInformation m_WantVersionInfo;

			CVersionIDAndPlatform m_NewestUnconditionalVersion;
			CVersionInformation m_NewestUnconditionalVersionInfo;

			// Changable
			NStr::CStr m_VersionManagerApplication;
			NStr::CStr m_UpdateGroup;
			NStr::CStr m_Executable;
			NContainer::TCVector<NStr::CStr> m_Parameters;
			NStr::CStr m_RunAsUser;
			NStr::CStr m_RunAsGroup;
			bool m_bRunAsUserHasShell = false;

			NContainer::TCMap<NStr::CStr, NFile::CDirectoryManifestConfig::CDestination> m_Backup_IncludeWildcards;
			NContainer::TCSet<NStr::CStr> m_Backup_ExcludeWildcards;
			NContainer::TCMap<NStr::CStr, NFile::EDirectoryManifestSyncFlag> m_Backup_AddSyncFlagsWildcards;
			NContainer::TCMap<NStr::CStr, NFile::EDirectoryManifestSyncFlag> m_Backup_RemoveSyncFlagsWildcards;
			NTime::CTimeSpan m_Backup_NewBackupInterval;

			bool m_bAutoUpdate = false;
			NContainer::TCSet<NStr::CStr> m_UpdateTags;
			NContainer::TCSet<NStr::CStr> m_UpdateBranches;
			NStr::CStr m_UpdateScriptPreUpdate;
			NStr::CStr m_UpdateScriptPostUpdate;
			NStr::CStr m_UpdateScriptPostLaunch;
			NStr::CStr m_UpdateScriptOnError;
			NContainer::TCSet<NStr::CStr> m_Dependencies;

			NStr::CStr m_LaunchEnvironment;

			bool m_bSelfUpdateSource = false;
			bool m_bDistributedApp = false;
			bool m_bStopOnDependencyFailure = true;
			bool m_bBackupEnabled = false;
			bool m_bLaunchInProcess = false;
		};

		struct CApplicationVersion
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			CVersionIDAndPlatform m_VersionID;
			CVersionInformation m_VersionInfo;
		};

		struct CEnvironmentSettings
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NStorage::TCOptional<EEnvironmentType> m_Type;
			NStorage::TCOptional<NStr::CStr> m_AgentApplication; /// Application that provides the agent executable for the environment
			NStorage::TCOptional<NStr::CStr> m_ParentApplication; /// Application whose storage confines the environment storage, supporting encrypted storage
			NStorage::TCOptional<bool> m_bAutoStart;

			NStorage::TCOptional<NStr::CStr> m_ContainerRuntime; /// Empty selects the default runtime for the host platform
			NStorage::TCOptional<NStr::CStr> m_ContainerImage;
			NStorage::TCOptional<NStr::CStr> m_ContainerNetwork;
			NStorage::TCOptional<NContainer::TCMap<NStr::CStr, NStr::CStr>> m_ContainerExtraMounts;
			NStorage::TCOptional<NContainer::TCVector<NStr::CStr>> m_ContainerExtraArguments;
			NStorage::TCOptional<bool> m_bContainerReadOnly;

			NStorage::TCOptional<NStr::CStr> m_MemoryLimit;
			NStorage::TCOptional<fp64> m_CPULimit;

			NStorage::TCOptional<NStr::CStr> m_VMImage;
			NStorage::TCOptional<NStr::CStr> m_VMBackend; /// Empty selects the default virtualization backend for the host platform
			NStorage::TCOptional<uint32> m_VMCPUCount;
			NStorage::TCOptional<uint32> m_VMMemoryMB;
		};

		struct CEnvironmentInfo
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NEncoding::CEJsonSorted f_ToJson() const;

			// State
			NStr::CStr m_Status;
			EStatusSeverity m_StatusSeverity = EStatusSeverity_None;
			NStr::CStr m_HostID; /// HostID of the connected agent when the environment is running

			// Settings
			EEnvironmentType m_Type = EEnvironmentType_Container;
			NStr::CStr m_AgentApplication;
			NStr::CStr m_ParentApplication;
			bool m_bAutoStart = true;

			NStr::CStr m_ContainerRuntime;
			NStr::CStr m_ContainerImage;
			NStr::CStr m_ContainerNetwork;
			NContainer::TCMap<NStr::CStr, NStr::CStr> m_ContainerExtraMounts;
			NContainer::TCVector<NStr::CStr> m_ContainerExtraArguments;
			bool m_bContainerReadOnly = false;

			NStr::CStr m_MemoryLimit;
			fp64 m_CPULimit = 0.0;

			NStr::CStr m_VMImage;
			NStr::CStr m_VMBackend;
			uint32 m_VMCPUCount = 0;
			uint32 m_VMMemoryMB = 0;

			NContainer::TCSet<NStr::CStr> m_Applications; /// Applications configured to launch in this environment
		};

		struct CUpdateNotification
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NEncoding::CEJsonSorted f_ToJson() const;
			bool f_IsDone() const;

			uint64 m_UniqueSequence = 0;

			NStr::CStr m_UpdateID;
			NStr::CStr m_Application;
			NStr::CStr m_Message; // Currently only for EUpdateStage_Failed
			CVersionIDAndPlatform m_VersionID;
			NTime::CTime m_VersionTime;
			NTime::CTime m_StartUpdateTime;
			EUpdateStage m_Stage = EUpdateStage_Failed;
			fp64 m_UpdateTime = 0.0;
			bool m_bCoordinateWait = false; // When set the stage has not yet been reached, the group is coordinating the update
		};

		struct CApplicationAdd
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NStr::CStr m_ParentApplication;
			NStr::CStr m_EncryptionStorage;
			NStr::CStr m_EncryptionFileSystem;
			NStorage::TCOptional<CVersionIDAndPlatform> m_Version; // If not specified the latest known version will be used

			bool m_bForceOverwriteEncryption = false; // If an encrypted volume is found to be used already, force it to be overwritten
			bool m_bForceInstall = false; // Force application install even if application directory already exists
			bool m_bSettingsFromVersionInfo = true; // Get settings from version downloaded
		};

		struct CApplicationUpdate
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NStorage::TCOptional<NContainer::TCSet<NStr::CStr>> m_RequireTags; // Defaults to tags in application settings
			NStorage::TCOptional<NStr::CStr> m_Platform; // Defaults to same as last installed version
			NStorage::TCOptional<CVersionID> m_Version; // Defaults to newest version available

			bool m_bUpdateSettings = true; // Update settings from from downloaded version info
			bool m_bDryRun = false; // Just download and extract application, don't actually update
			bool m_bBypassCoordination = false; // Behave as if application is has independent update type
		};

		struct CApplicationChangeSettings
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			bool m_bUpdateFromVersionInfo = false; // Update settings from the last installed version manager application info.
			bool m_bForce = false; // Force running the update process even if no settings are changed.
		};

		using CVersionsAvailableForUpdate = NContainer::TCMap<NStr::CStr, NContainer::TCVector<CApplicationVersion>>;

		struct CApplicationChange_AddOrChangeInfo
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			CApplicationInfo m_Info;
		};

		struct CApplicationChange_Remove
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);
		};

		struct CApplicationChange_Status
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NStr::CStr m_Status;
			EStatusSeverity m_StatusSeverity = EStatusSeverity_None;
		};

		using CApplicationChange = NStorage::TCStreamableVariant
			<
				EApplicationChange
				, NStorage::TCMember<CApplicationChange_AddOrChangeInfo, EApplicationChange_AddOrChangeInfo>
				, NStorage::TCMember<CApplicationChange_Remove, EApplicationChange_Remove>
				, NStorage::TCMember<CApplicationChange_Status, EApplicationChange_Status>
			>
		;

		struct CChangeNotification
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NStr::CStr m_Application;
			CApplicationChange m_Change;
		};

		struct COnChangeNotificationParams
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NContainer::TCVector<CChangeNotification> m_Changes;
			bool m_bInitial = false;
			bool m_bAccessDenied = false;
			bool m_bFiltered = false;
		};

		struct CSubscribeUpdateNotifications
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NConcurrency::TCActorFunctorWithID<NConcurrency::TCFuture<void> (CUpdateNotification _Notification)> m_fOnNotification;
			uint64 m_LastSeenUniqueSequence = TCLimitsInt<uint64>::mc_Max;
			bool m_bWaitForNotification = true;
		};

		struct CSubscribeChangeNotifications
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NConcurrency::TCActorFunctorWithID<NConcurrency::TCFuture<void> (COnChangeNotificationParams _Params)> m_fOnNotification;
			bool m_bWaitForNotification = true;
		};

		CAppManagerInterface();
		~CAppManagerInterface();

		static NStr::CStr fs_UpdateStageToStr(EUpdateStage _Stage);
		static NStr::CStr fs_EnvironmentTypeToStr(EEnvironmentType _Type);
		static NStorage::TCOptional<EEnvironmentType> fs_EnvironmentTypeFromStr(NStr::CStr const &_Type);

		virtual NConcurrency::TCFuture<CVersionsAvailableForUpdate> f_GetAvailableVersions
			(
				NStr::CStr _Application	/// Leave empty to list versions for all version manager applications know by the AppManager. By default app manager will only subscribe to
										/// applications with the same platform as it's running under and all platforms of any application it has installed.
			) = 0
		;

		virtual NConcurrency::TCFuture<void> f_Add(NStr::CStr _Name, CApplicationAdd _Add, CApplicationSettings _Settings) = 0;
		virtual NConcurrency::TCFuture<void> f_Remove(NStr::CStr _Name) = 0;

		virtual NConcurrency::TCFuture<void> f_Update(NStr::CStr _Name, CApplicationUpdate _Update) = 0;

		virtual NConcurrency::TCFuture<void> f_Start(NStr::CStr _Name) = 0;
		virtual NConcurrency::TCFuture<void> f_Stop(NStr::CStr _Name) = 0;
		virtual NConcurrency::TCFuture<void> f_Restart(NStr::CStr _Name) = 0;

		virtual NConcurrency::TCFuture<void> f_ChangeSettings
			(
				NStr::CStr _Name
				, CApplicationChangeSettings _ChangeSettings
				, CApplicationSettings _Settings
			) = 0
		;

		virtual NConcurrency::TCFuture<NContainer::TCMap<NStr::CStr, CApplicationInfo>> f_GetInstalled() = 0;
		virtual auto f_SubscribeUpdateNotifications(CSubscribeUpdateNotifications _Params) -> NConcurrency::TCFuture<NConcurrency::TCActorSubscriptionWithID<>> = 0;
		virtual auto f_SubscribeChangeNotifications(CSubscribeChangeNotifications _Params) -> NConcurrency::TCFuture<NConcurrency::TCActorSubscriptionWithID<>> = 0;

		// Environments (requires EProtocolVersion_AddLaunchEnvironment)
		virtual NConcurrency::TCFuture<void> f_EnvironmentAdd(NStr::CStr _Name, CEnvironmentSettings _Settings) = 0;
		virtual NConcurrency::TCFuture<void> f_EnvironmentRemove(NStr::CStr _Name) = 0;
		virtual NConcurrency::TCFuture<void> f_EnvironmentChangeSettings(NStr::CStr _Name, CEnvironmentSettings _Settings) = 0;
		virtual NConcurrency::TCFuture<void> f_EnvironmentStart(NStr::CStr _Name) = 0;
		virtual NConcurrency::TCFuture<void> f_EnvironmentStop(NStr::CStr _Name) = 0;
		virtual NConcurrency::TCFuture<void> f_EnvironmentRestart(NStr::CStr _Name) = 0;
		virtual NConcurrency::TCFuture<NContainer::TCMap<NStr::CStr, CEnvironmentInfo>> f_GetEnvironments() = 0;
	};
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NCloud;
#endif

#include "Malterlib_Cloud_AppManager.hpp"
