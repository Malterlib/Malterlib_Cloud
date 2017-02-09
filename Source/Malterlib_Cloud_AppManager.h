// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Encoding/EJSON>
#include <Mib/Storage/Optional>
#include "Malterlib_Cloud_VersionManager.h"

namespace NMib::NCloud
{
	struct CAppManagerInterface : public NConcurrency::CActor
	{
		static constexpr ch8 const *mc_pDefaultNamespace = "com.malterlib/Cloud/AppManager";

		enum 
		{
			EMinProtocolVersion = 0x101
			, EProtocolVersion = 0x102
		};
		
		CAppManagerInterface();
		~CAppManagerInterface();
		
		enum EUpdateStage
		{
			EUpdateStage_Failed = 0x70000000
			, EUpdateStage_None = 0
			, EUpdateStage_ChangeEncryption = 1
			, EUpdateStage_DownloadVersion = 2
			, EUpdateStage_Unpack = 3
			, EUpdateStage_StopOldApp = 4
			, EUpdateStage_PreUpdateScript = 5
			, EUpdateStage_UpdateApplicationFiles = 6
			, EUpdateStage_SaveApplicationState = 7
			, EUpdateStage_PostUpdateScript = 8
			, EUpdateStage_StartNewApp = 9
			, EUpdateStage_PostLaunch = 10
			, EUpdateStage_Finished = 11
		};
		
		struct CVersionIDAndPlatform : public CVersionManager::CVersionIDAndPlatform
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			using CVersionManager::CVersionIDAndPlatform::CVersionIDAndPlatform;
			using CVersionManager::CVersionIDAndPlatform::operator =;
		};
		
		struct CVersionID : public CVersionManager::CVersionID
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);
			
			using CVersionManager::CVersionID::CVersionID;
			using CVersionManager::CVersionID::operator =;
		};
		
		struct CVersionInformation : public CVersionManager::CVersionInformation
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);
			
			using CVersionManager::CVersionInformation::CVersionInformation;
			using CVersionManager::CVersionInformation::operator =;
		};
		
		struct CApplicationSettings
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NStorage::TCOptional<NStr::CStr> m_VersionManagerApplication; // Must be set when adding application
			NStorage::TCOptional<NStr::CStr> m_UpdateGroup;
			NStorage::TCOptional<NStr::CStr> m_Executable;
			NStorage::TCOptional<NContainer::TCVector<NStr::CStr>> m_ExecutableParameters;
			NStorage::TCOptional<NStr::CStr> m_RunAsUser;
			NStorage::TCOptional<NStr::CStr> m_RunAsGroup;
			NStorage::TCOptional<bool> m_bDistributedApp;
			NStorage::TCOptional<NContainer::TCSet<NStr::CStr>> m_AutoUpdateTags;
			NStorage::TCOptional<NContainer::TCSet<NStr::CStr>> m_AutoUpdateBranches; // Are wild cards
			NStorage::TCOptional<NStr::CStr> m_UpdateScriptPreUpdate;
			NStorage::TCOptional<NStr::CStr> m_UpdateScriptPostUpdate;
			NStorage::TCOptional<NStr::CStr> m_UpdateScriptPostLaunch;
			NStorage::TCOptional<NStr::CStr> m_UpdateScriptOnError;
			NStorage::TCOptional<bool> m_bSelfUpdateSource;
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
			NContainer::TCSet<NStr::CStr> m_AutoUpdateTags;
			NContainer::TCSet<NStr::CStr> m_AutoUpdateBranches;
			NStr::CStr m_UpdateScriptPreUpdate;
			NStr::CStr m_UpdateScriptPostUpdate;
			NStr::CStr m_UpdateScriptPostLaunch;
			NStr::CStr m_UpdateScriptOnError;
			bool m_bSelfUpdateSource = false;
			bool m_bDistributedApp = false;
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
			EUpdateStage m_Stage = EUpdateStage_Failed;
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
		
		virtual NConcurrency::TCContinuation<CVersionsAvailableForUpdate> f_GetAvailableVersions
			(
				NStr::CStr const &_Application	/// Leave empty to list versions for all version manager applications know by the AppManager. By default app manager will only subscribe to 
												/// applications with the same platform as it's running under and all platforms of any application it has installed.
			) = 0
		;
		
		virtual NConcurrency::TCContinuation<void> f_Add(NStr::CStr const &_Name, CApplicationAdd const &_Add, CApplicationSettings const &_Settings) = 0;
		virtual NConcurrency::TCContinuation<void> f_Remove(NStr::CStr const &_Name) = 0;

		virtual NConcurrency::TCContinuation<void> f_Update(NStr::CStr const &_Name, CApplicationUpdate const &_Update) = 0;
		
		virtual NConcurrency::TCContinuation<void> f_Start(NStr::CStr const &_Name) = 0;
		virtual NConcurrency::TCContinuation<void> f_Stop(NStr::CStr const &_Name) = 0;
		virtual NConcurrency::TCContinuation<void> f_Restart(NStr::CStr const &_Name) = 0;

		virtual NConcurrency::TCContinuation<void> f_ChangeSettings
			(
				NStr::CStr const &_Name
				, CApplicationChangeSettings const &_ChangeSettings
				, CApplicationSettings const &_Settings
			) = 0
		;

		virtual NConcurrency::TCContinuation<NContainer::TCMap<NStr::CStr, CApplicationInfo>> f_GetInstalled() = 0;
		virtual auto f_SubscribeUpdateNotifications(NConcurrency::TCActorFunctorWithID<NConcurrency::TCContinuation<void> (CUpdateNotification const &_Notification)> &&_fOnNotification) 
			-> NConcurrency::TCContinuation<NConcurrency::TCActorSubscriptionWithID<>> = 0
		; 
	};
}
