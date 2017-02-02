// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedApp>
#include <Mib/Concurrency/ActorSubscription>
#include <Mib/Cloud/VersionManager>
#include <Mib/Daemon/Daemon>

namespace NMib::NCloud::NVersionManager
{
	struct CVersionManagerDaemonActor::CServer : public CActor
	{
	public:
		using CActorHolder = CDelegatedActorHolder;
		
		CServer(CDistributedAppState &_AppState);
		~CServer();
		void f_Construct() override;
		TCContinuation<void> f_Destroy() override;

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
			
			TCActor<CSeparateThreadActor> m_UploadFileAccess;
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
			DIntrusiveLink(CVersion, TCAVLLink<>, m_TimeLink);
			
			struct CCompareTime
			{
				bool operator()(CVersion const &_Left, CVersion const &_Right) const
				{
					if (!_Left.m_VersionInfo.m_Time.f_IsValid() && _Right.m_VersionInfo.m_Time.f_IsValid())
						return true;
					else if (_Left.m_VersionInfo.m_Time.f_IsValid() && !_Right.m_VersionInfo.m_Time.f_IsValid())
						return false;					
					return NContainer::fg_TupleReferences(_Left.m_VersionInfo.m_Time, _Left.f_GetIdentifier()) 
						< NContainer::fg_TupleReferences(_Right.m_VersionInfo.m_Time, _Right.f_GetIdentifier())
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
			TCAVLTree<CVersion::CLinkTraits_m_TimeLink, CVersion::CCompareTime> m_VersionsByTime;
		};
		
		struct CVersionManagerImplementation : public CVersionManager
		{
			TCContinuation<CListApplications::CResult> f_ListApplications(CListApplications &&_Params) override;
			TCContinuation<CListVersions::CResult> f_ListVersions(CListVersions &&_Params) override;
			TCContinuation<CStartUploadVersion::CResult> f_UploadVersion(CStartUploadVersion &&_Params) override;
			TCContinuation<CStartDownloadVersion::CResult> f_DownloadVersion(CStartDownloadVersion &&_Params) override;
			TCContinuation<CSubscribeToUpdates::CResult> f_SubscribeToUpdates(CSubscribeToUpdates &&_Params) override;
			TCContinuation<CChangeTags::CResult> f_ChangeTags(CChangeTags &&_Params) override;
			
			CServer *m_pThis = nullptr;
		};
		
		struct CSubscription
		{
			CStr const &f_GetSubscriptionID() const
			{
				return TCMap<CStr, CSubscription>::fs_GetKey(*this);
			}
			uint32 m_nInitial = 0;
			CStr m_HostID;
			TCSet<CStr> m_Platforms;
			TCSet<CStr> m_Tags;
			TCActor<> m_DispatchActor;
			TCFunctionMutable<NConcurrency::TCContinuation<CVersionManager::CNewVersionNotifications::CResult> (CVersionManager::CNewVersionNotifications &&_VersionInfo)> 
				m_fOnNewVersions
			;
			
			void f_SendVersions(CVersionManager::CNewVersionNotifications const &_NewVersionNotification) const;
		};
		
		struct CSizeInfo
		{
			uint32 m_nFiles = 0;
			uint64 m_nBytes = 0;
		};
		
	private:
		void fp_Init();
		void fp_Publish();
		TCContinuation<void> fp_SetupPermissions();
		TCContinuation<void> fp_FindVersions();

		void fp_SendSubscriptionInitial(CStr const &_Application, CSubscription const &_Subscription, bool _bPermissionsChanged);
		void fp_UpdateSubscriptionsForChangedPermissions(CStr const &_HostID);
		
		TCSet<CStr> fp_FilterApplicationsByPermissions(CStr const &_CallingHostID, TCSet<CStr> const &_Applications);
		TCContinuation<TCSet<CStr>> fp_EnumApplications();
		TCSet<CStr> fp_ApplicationSet();
		TCSet<CStr> fp_FilterTags(CStr const &_HostID, TCSet<CStr> const &_Tags, TCSet<CStr> &o_DeniedTags);
		void fp_NewTagsKnown(TCSet<CStr> const &_Tags);
		void fp_NewVersion(CStr const &_ApplicationName, CVersion const &_Version);
		TCContinuation<CSizeInfo> fp_SaveVersionInfo(TCActor<> const &_FileActor, CStr const &_VersionPath, CVersionManager::CVersionInformation const &_VersionInfo);
		bool fp_VersionMatchesSubscription(CSubscription const &_Subscription, CVersion const &_Version);
		
		TCActor<CSeparateThreadActor> const &fp_GetQueryFileActor();
		
		TCSharedPointer<CCanDestroyTracker> mp_pCanDestroyTracker;

		TCDelegatedActorInterface<CVersionManagerImplementation> mp_ProtocolInterface;
		
		CDistributedAppState &mp_AppState;
		
		CTrustedPermissionSubscription mp_Permissions;
		
		TCSet<CStr> mp_KnownTags;
		
		TCMap<CStr, CVersionDownload> mp_VersionDownloads;
		TCMap<CStr, CVersionUpload> mp_VersionUploads;
		
		TCActor<CSeparateThreadActor> mp_QueryFileActor;
		
		TCMap<CStr, CApplication> mp_Applications;
		
		TCMap<CStr, CSubscription> mp_GlobalVersionSubscriptions;
		TCMap<CStr, TCMap<CStr, CSubscription>> mp_VersionSubscriptions; 
	};
}
