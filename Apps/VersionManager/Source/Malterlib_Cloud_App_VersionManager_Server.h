// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedApp>
#include <Mib/Concurrency/ActorSubscription>
#include <Mib/Concurrency/ActorSequencerActor>
#include <Mib/Cloud/VersionManager>
#include <Mib/Daemon/Daemon>
#include <Mib/Database/DatabaseActor>

#include "Malterlib_Cloud_App_VersionManager_Sync.h"

namespace NMib::NCloud::NVersionManager
{
	struct CVersionManagerDaemonActor::CServer : public CActor
	{
		using CActorHolder = CDelegatedActorHolder;

		struct CVersionDownload
		{
			CVersionDownload();
			~CVersionDownload();

			CStr const &f_GetDownloadID() const
			{
				return TCMap<CStr, CVersionDownload>::fs_GetKey(*this);
			}

			TCActor<NCloud::CFileTransferSend> m_FileTransferSend;
			CStr m_Desc;
		};

		struct CVersionUpload
		{
			CVersionUpload();
			~CVersionUpload();

			CStr const &f_GetUploadID() const
			{
				return TCMap<CStr, CVersionUpload>::fs_GetKey(*this);
			}

			TCActor<NCloud::CFileTransferReceive> m_FileTransferReceive;
			CActorSubscription m_DownloadSubscription;

			CStr m_Desc;
		};

		struct CVersion
		{
			CVersionManager::CVersionIDAndPlatform const &f_GetIdentifier() const
			{
				return TCMap<CVersionManager::CVersionIDAndPlatform, CVersion>::fs_GetKey(*this);
			}

			CVersionManager::CVersionInformation m_VersionInfo;
			TCAVLLink<> m_TimeLink;

			struct CCompareTime
			{
				COrdering_Partial operator()(CVersion const &_Left, CVersion const &_Right) const
				{
					if (auto Result = _Left.m_VersionInfo.m_Time.f_IsValid() <=> _Right.m_VersionInfo.m_Time.f_IsValid(); Result != 0)
						return Result;

					return NStorage::fg_TupleReferences(_Left.m_VersionInfo.m_Time, _Left.f_GetIdentifier())
						<=> NStorage::fg_TupleReferences(_Right.m_VersionInfo.m_Time, _Right.f_GetIdentifier())
					;
				}
			};
		};

		struct CApplication
		{
			CStr const &f_GetName() const
			{
				return TCMap<CStr, CApplication>::fs_GetKey(*this);
			}

			TCMap<CVersionManager::CVersionIDAndPlatform, CVersion> m_Versions;
			TCAVLTree<&CVersion::m_TimeLink, CVersion::CCompareTime> m_VersionsByTime;
		};

		struct CVersionManagerImplementation : public CVersionManager
		{
			TCFuture<CListApplications::CResult> f_ListApplications(CListApplications _Params) override;
			TCFuture<CListVersions::CResult> f_ListVersions(CListVersions _Params) override;
			TCFuture<CStartUploadVersion::CResult> f_UploadVersion(CStartUploadVersion _Params) override;
			TCFuture<CStartDownloadVersion::CResult> f_DownloadVersion(CStartDownloadVersion _Params) override;
			TCFuture<CSubscribeToUpdates::CResult> f_SubscribeToUpdates(CSubscribeToUpdates _Params) override;
			TCFuture<CChangeTags::CResult> f_ChangeTags(CChangeTags _Params) override;

			DMibDelegatedActorImplementation(CServer);
		};

		struct CSubscription
		{
			CStr const &f_GetSubscriptionID() const
			{
				return TCMap<CStr, CSubscription>::fs_GetKey(*this);
			}
			uint32 m_nInitial = 0;
			CCallingHostInfo m_CallingHostInfo;
			TCSet<CStr> m_Platforms;
			TCSet<CStr> m_Tags;
			TCActorFunctor<NConcurrency::TCFuture<CVersionManager::CNewVersionNotifications::CResult> (CVersionManager::CNewVersionNotifications _VersionInfo)> m_fOnNewVersions;
		};

		struct CSizeInfo
		{
			uint32 m_nFiles = 0;
			uint64 m_nBytes = 0;
		};

		struct CRefreshResult
		{
			umint m_nAdded = 0;
			umint m_nUpdated = 0;
			umint m_nRemoved = 0;
		};

		CServer(CDistributedAppState &_AppState);
		~CServer();

		TCFuture<CRefreshResult> f_RefreshDatabaseFromDisk();
		TCFuture<void> f_Init();

	private:
		struct CFilteredTagsResult
		{
			TCSet<CStr> m_DeniedTags;
			TCSet<CStr> m_TagsAdded;
			TCSet<CStr> m_TagsRemoved;
		};

		struct CMatchingConfig
		{
			CStr m_Name;
			CSyncSourceConfig const *m_pConfig;
		};

		TCFuture<void> fp_Destroy() override;

		TCFuture<void> fp_Publish();
		TCFuture<void> fp_SetupPermissions();
		TCFuture<void> fp_FindVersions();

		// Database operations
		TCFuture<void> fp_SetupDatabase();
		TCFuture<bool> fp_LoadVersionsFromDatabase();
		TCFuture<void> fp_SaveVersionToDatabase(CStr _Application, CVersionManager::CVersionIDAndPlatform _VersionID, CVersionManager::CVersionInformation _VersionInfo);
		TCFuture<void> fp_RemoveVersionFromDatabase(CStr _Application, CVersionManager::CVersionIDAndPlatform _VersionID);
		void fp_NotifyUploadsEmpty();

		TCFuture<void> fp_SendSubscriptionInitial(CStr _Application, CSubscription const *_pSubscription);
		TCFuture<void> fp_UpdateSubscriptionsForChangedPermissions(CPermissionIdentifiers _Identity);

		TCFuture<TCSet<CStr>> fp_FilterApplicationsByPermissions(CStr _Description, TCSet<CStr> _Applications);
		TCSet<CStr> fp_ApplicationSet();
		TCFuture<CFilteredTagsResult> fp_FilterTags(CStr _HostID, TCSet<CStr> _TagsAdded, TCSet<CStr> _TagsRemoved);
		void fp_NewTagsKnown(TCSet<CStr> const &_Tags);
		TCFuture<void> fp_NewVersion(CStr _ApplicationName, CVersionManager::CVersionIDAndPlatform _VersionID, CVersionManager::CVersionInformation _VersionInfo, CStr _OriginID);
		TCFuture<CSizeInfo> fp_SaveVersionInfo(CStr _VersionPath, CVersionManager::CVersionInformation _VersionInfo);
		bool fp_VersionMatchesSubscription(CSubscription const &_Subscription, CVersion const &_Version);
		CSubscription const *fp_GetSubscription(CStr const &_ApplicationName, CStr const &_SubscriptionID) const;

		// Sync lifecycle
		TCFuture<void> fp_SyncInit();
		TCFuture<void> fp_SyncDestroy();

		// Sync sensor functions
		TCFuture<void> fp_SyncRegisterSensors();
		TCFuture<void> fp_SyncUpdateSensorStatus(CStr _ConfigName);
		TCFuture<void> fp_SyncSetHostError(CStr _ConfigName, CStr _HostID, CStr _ErrorMessage);
		TCFuture<void> fp_SyncClearHostError(CStr _ConfigName, CStr _HostID);

		// Sync callbacks
		TCFuture<void> fp_SyncOnVersionManagerAdded(TCDistributedActor<CVersionManager> _Manager, CTrustedActorInfo _Info);
		TCFuture<void> fp_SyncOnVersionManagerRemoved(TCWeakDistributedActor<CActor> _Manager, CTrustedActorInfo _Info);
		TCFuture<void> fp_SyncOnNewVersions(CStr _HostID, TCWeakDistributedActor<CVersionManager> _Manager, CVersionManager::CNewVersionNotifications _Notifications);

		// Sync operations
		TCVector<CMatchingConfig> fp_SyncGetMatchingConfigs
			(
				CStr const &_HostID
				, CVersionManager::CNewVersionNotification const &_Notification
				, TCMap<CStr, TCSet<CSyncVersionKey>> const &_DateBypassByConfig
			)
		;
		TCFuture<void> fp_SyncProcessVersion
			(
				CStr _HostID
				, TCDistributedActor<CVersionManager> _SourceManager
				, CVersionManager::CNewVersionNotification _Notification
				, TCVector<CMatchingConfig> _MatchingConfigs
				, CStr _OriginID
			)
		;
		TCFuture<void> fp_SyncDownloadAndStoreVersion
			(
				CStr _SyncSourceName
				, CSyncSourceConfig _Config
				, TCDistributedActor<CVersionManager> _SourceManager
				, CStr _Application
				, CVersionManager::CVersionIDAndPlatform _VersionID
				, CVersionManager::CVersionInformation _VersionInfo
				, TCSet<CStr> _CombinedTags
				, bool _bSyncRetrySequence
				, CStr _OriginID
			)
		;
		TCFuture<void> fp_SyncUpdateLocalTags
			(
				CStr _Application
				, CVersionManager::CVersionIDAndPlatform _VersionID
				, TCSet<CStr> _TransformedTags
				, bool _bSyncRetrySequence
				, uint32 _RemoteRetrySequence
				, CStr _OriginID
			)
		;

		// Sync helpers
		static bool fs_SyncMatchesFilter(CStr const &_Value, TCVector<CStr> const &_Filters);
		static bool fs_SyncMatchesTagFilter(TCSet<CStr> const &_Tags, TCVector<CStr> const &_Filters);
		static bool fs_SyncNotificationMatchesConfigFilters(CVersionManager::CNewVersionNotification const &_Notification, CSyncSourceConfig const &_Config);
		static TCSet<CStr> fs_SyncGetConfigOwnedTags(TCMap<CStr, CStr> const &_CopyTagMappings);
		static TCSet<CStr> fs_SyncGetPresentTags(TCSet<CStr> const &_SourceTags, TCMap<CStr, CStr> const &_CopyTagMappings);

		constexpr static uint32 mcp_MaxQueueSize = NFile::gc_IdealNetworkQueueSize;

		TCDistributedActorInstance<CVersionManagerImplementation> mp_ProtocolInterface;

		CDistributedAppState &mp_AppState;

		CTrustedPermissionSubscription mp_Permissions;

		TCSet<CStr> mp_KnownTags;

		TCMap<CStr, CVersionDownload> mp_VersionDownloads;
		TCMap<CStr, CVersionUpload> mp_VersionUploads;

		TCMap<CStr, CApplication> mp_Applications;

		TCMap<CStr, CSubscription> mp_GlobalVersionSubscriptions;
		TCMap<CStr, TCMap<CStr, CSubscription>> mp_VersionSubscriptions;

		TCActor<NDatabase::CDatabaseActor> mp_DatabaseActor;
		TCSharedPointer<CCanDestroyTracker> mp_pCanDestroyTracker = fg_Construct();

		CSequencer mp_UploadSequencer{"VersionManagerUpload", 8};

		// Synchronization for refresh/upload mutual exclusion
		CSequencer mp_RefreshSequencer{"VersionManagerRefreshSequencer"};
		TCVector<TCPromise<void>> mp_UploadsEmptyWaiters;
		umint mp_nInProgressUploads = 0;
		bool mp_bRefreshInProgress = false;

		// Sync configuration and state
		TCMap<CStr, CSyncSourceState> mp_SyncSources;
		TCMap<CStr, CSyncHostSubscription> mp_SyncHostSubscriptions; // Keyed by host ID
		TCMap<CSyncVersionKey, CSyncVersionState> mp_SyncVersionStates;
		TCTrustedActorSubscription<CVersionManager> mp_SyncVersionManagerSubscription;
	};
}
