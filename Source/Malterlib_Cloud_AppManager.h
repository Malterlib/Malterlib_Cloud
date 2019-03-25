// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Encoding/EJSON>
#include <Mib/Storage/Optional>
#include <Mib/Cloud/BackupManager>
#include "Malterlib_Cloud_VersionManager.h"

namespace NMib::NCloud
{
	struct CAppManagerInterface : public NConcurrency::CActor
	{
		static constexpr ch8 const *mc_pDefaultNamespace = "com.malterlib/Cloud/AppManager";

		enum : uint32
		{
			EMinProtocolVersion = 0x107
			, EProtocolVersion = 0x109
		};
		
		CAppManagerInterface();
		~CAppManagerInterface();
		
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
		
		struct CVersionIDAndPlatform : public CVersionManager::CVersionIDAndPlatform
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			CVersionIDAndPlatform() = default;
			CVersionIDAndPlatform(CVersionIDAndPlatform const &) = default;
			CVersionIDAndPlatform(CVersionIDAndPlatform &&) = default;
			CVersionIDAndPlatform &operator = (CVersionIDAndPlatform const &) = default;
			CVersionIDAndPlatform &operator = (CVersionIDAndPlatform &&) = default;

			CVersionIDAndPlatform(CVersionManager::CVersionIDAndPlatform const &_Other) : CVersionManager::CVersionIDAndPlatform(_Other) { }
			CVersionIDAndPlatform &operator = (CVersionManager::CVersionIDAndPlatform const &_Right) { static_cast<CVersionManager::CVersionIDAndPlatform &>(*this) = _Right; return *this; }
		};
		
		struct CVersionID : public CVersionManager::CVersionID
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);
			
			CVersionID() = default;
			CVersionID(CVersionID const &) = default;
			CVersionID(CVersionID &&) = default;
			CVersionID &operator = (CVersionID const &) = default;
			CVersionID &operator = (CVersionID &&) = default;

			CVersionID(CVersionManager::CVersionID const &_Other) : CVersionManager::CVersionID(_Other) { }
			CVersionID &operator = (CVersionManager::CVersionID const &_Right) { static_cast<CVersionManager::CVersionID &>(*this) = _Right; return *this; }
		};
		
		struct CVersionInformation : public CVersionManager::CVersionInformation
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);
			
			CVersionInformation() = default;
			CVersionInformation(CVersionInformation const &) = default;
			CVersionInformation(CVersionInformation &&) = default;
			CVersionInformation &operator = (CVersionInformation const &) = default;
			CVersionInformation &operator = (CVersionInformation &&) = default;

			CVersionInformation(CVersionManager::CVersionInformation const &_Other) : CVersionManager::CVersionInformation(_Other) { }
			CVersionInformation &operator = (CVersionManager::CVersionInformation const &_Right) { static_cast<CVersionManager::CVersionInformation &>(*this) = _Right; return *this; }
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
			
			NStorage::TCOptional<NContainer::TCSet<NStr::CStr>> m_AutoUpdateTags;
			NStorage::TCOptional<NContainer::TCSet<NStr::CStr>> m_AutoUpdateBranches; // Are wild cards
			NStorage::TCOptional<NStr::CStr> m_UpdateScriptPreUpdate;
			NStorage::TCOptional<NStr::CStr> m_UpdateScriptPostUpdate;
			NStorage::TCOptional<NStr::CStr> m_UpdateScriptPostLaunch;
			NStorage::TCOptional<NStr::CStr> m_UpdateScriptOnError;
			NStorage::TCOptional<NContainer::TCSet<NStr::CStr>> m_Dependencies;
			
			NStorage::TCOptional<bool> m_bDistributedApp;
			NStorage::TCOptional<bool> m_bSelfUpdateSource;
			NStorage::TCOptional<bool> m_bStopOnDependencyFailure;
			NStorage::TCOptional<bool> m_bBackupEnabled;
		};
		
		struct CApplicationInfo
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);
			
			NStr::CStr const &f_GetName()
			{
				return NContainer::TCMap<NStr::CStr, CApplicationInfo>::fs_GetKey(*this);
			}
			
			// State
			NStr::CStr m_Status;

			// Immutable
			NStr::CStr m_EncryptionStorage;
			NStr::CStr m_EncryptionFileSystem;
			NStr::CStr m_ParentApplication;

			// Updatable
			CVersionIDAndPlatform m_Version;
			CVersionInformation m_VersionInfo;
			
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
			
			NContainer::TCSet<NStr::CStr> m_AutoUpdateTags;
			NContainer::TCSet<NStr::CStr> m_AutoUpdateBranches;
			NStr::CStr m_UpdateScriptPreUpdate;
			NStr::CStr m_UpdateScriptPostUpdate;
			NStr::CStr m_UpdateScriptPostLaunch;
			NStr::CStr m_UpdateScriptOnError;
			NContainer::TCSet<NStr::CStr> m_Dependencies;
			
			bool m_bSelfUpdateSource = false;
			bool m_bDistributedApp = false;
			bool m_bStopOnDependencyFailure = true;
			bool m_bBackupEnabled = false;
		};
		
		struct CApplicationVersion
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);
			
			CVersionIDAndPlatform m_VersionID;
			CVersionInformation m_VersionInfo;
		};
		
		struct CUpdateNotification
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);
			
			NStr::CStr m_Application;
			NStr::CStr m_Message; // Currently only for EUpdateStage_Failed
			CVersionIDAndPlatform m_VersionID;
			NTime::CTime m_VersionTime;
			EUpdateStage m_Stage = EUpdateStage_Failed;
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
		};
		
		struct CApplicationChangeSettings
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			bool m_bUpdateFromVersionInfo = false; // Update settings from the last installed version manager application info.
			bool m_bForce = false; // Force running the update process even if no settings are changed.
		};
		
		using CVersionsAvailableForUpdate = NContainer::TCMap<NStr::CStr, NContainer::TCVector<CApplicationVersion>>;
		
		virtual NConcurrency::TCFuture<CVersionsAvailableForUpdate> f_GetAvailableVersions
			(
				NStr::CStr const &_Application	/// Leave empty to list versions for all version manager applications know by the AppManager. By default app manager will only subscribe to 
												/// applications with the same platform as it's running under and all platforms of any application it has installed.
			) = 0
		;
		
		virtual NConcurrency::TCFuture<void> f_Add(NStr::CStr const &_Name, CApplicationAdd const &_Add, CApplicationSettings const &_Settings) = 0;
		virtual NConcurrency::TCFuture<void> f_Remove(NStr::CStr const &_Name) = 0;

		virtual NConcurrency::TCFuture<void> f_Update(NStr::CStr const &_Name, CApplicationUpdate const &_Update) = 0;
		
		virtual NConcurrency::TCFuture<void> f_Start(NStr::CStr const &_Name) = 0;
		virtual NConcurrency::TCFuture<void> f_Stop(NStr::CStr const &_Name) = 0;
		virtual NConcurrency::TCFuture<void> f_Restart(NStr::CStr const &_Name) = 0;

		virtual NConcurrency::TCFuture<void> f_ChangeSettings
			(
				NStr::CStr const &_Name
				, CApplicationChangeSettings const &_ChangeSettings
				, CApplicationSettings const &_Settings
			) = 0
		;

		virtual NConcurrency::TCFuture<NContainer::TCMap<NStr::CStr, CApplicationInfo>> f_GetInstalled() = 0;
		virtual auto f_SubscribeUpdateNotifications(NConcurrency::TCActorFunctorWithID<NConcurrency::TCFuture<void> (CUpdateNotification const &_Notification)> &&_fOnNotification)
			-> NConcurrency::TCFuture<NConcurrency::TCActorSubscriptionWithID<>> = 0
		; 
	};
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NCloud;
#endif
