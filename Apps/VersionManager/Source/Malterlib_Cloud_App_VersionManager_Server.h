// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedApp>
#include <Mib/Cloud/VersionManager>
#include <Mib/Daemon/Daemon>

namespace NMib::NCloud::NVersionManager
{
	struct CVersionManagerDaemonActor::CServer : public CActor
	{
	public:
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
		
		struct CVersionManagerImplementation : public CVersionManager
		{
			CVersionManagerImplementation(TCActor<CVersionManagerDaemonActor::CServer> &&_Server);
			
			TCContinuation<CListApplications::CResult> f_ListApplications(CListApplications &&_Params) override;
			TCContinuation<CListVersions::CResult> f_ListVersions(CListVersions &&_Params) override;
			TCContinuation<CStartUploadVersion::CResult> f_UploadVersion(CStartUploadVersion &&_Params) override;
			TCContinuation<CStartDownloadVersion::CResult> f_DownloadVersion(CStartDownloadVersion &&_Params) override;
			
		private:
			TCWeakActor<CVersionManagerDaemonActor::CServer> mp_Server;
		};
		
	private:
		void fp_Init();
		void fp_Publish();
		TCContinuation<void> fp_SetupPermissions();

		TCContinuation<CVersionManager::CListApplications::CResult> fp_Protocol_ListApplications(CCallingHostInfo const &_CallingHostInfo, CVersionManager::CListApplications &&_Params);
		TCContinuation<CVersionManager::CListVersions::CResult> fp_Protocol_ListVersions(CCallingHostInfo const &_CallingHostInfo, CVersionManager::CListVersions &&_Params);
		TCContinuation<CVersionManager::CStartUploadVersion::CResult> fp_Protocol_UploadVersion(CCallingHostInfo const &_CallingHostInfo, CVersionManager::CStartUploadVersion &&_Params);
		TCContinuation<CVersionManager::CStartDownloadVersion::CResult> fp_Protocol_DownloadVersion(CCallingHostInfo const &_CallingHostInfo, CVersionManager::CStartDownloadVersion &&_Params);
		
		CException fp_AccessDenied(CCallingHostInfo const &_CallingHostInfo, CStr const &_Description);
		TCSet<CStr> fp_FilterApplicationsByPermissions(CCallingHostInfo const &_CallingHostInfo, TCSet<CStr> const &_Applications);
		TCContinuation<TCSet<CStr>> fp_EnumApplications();
		
		static void fsp_LogActivityInfo(CCallingHostInfo const &_CallingHostInfo, CStr const &_Info);
		static void fsp_LogActivityError(CCallingHostInfo const &_CallingHostInfo, CStr const &_Error);
		static void fsp_LogActivityWarning(CCallingHostInfo const &_CallingHostInfo, CStr const &_Error);
		
		TCActor<CSeparateThreadActor> const &fp_GetQueryFileActor();
		
		TCSharedPointer<CCanDestroyTracker> mp_pCanDestroyTracker;
		
		TCDistributedActor<CVersionManagerImplementation> mp_ProtocolImplementation;
		CDistributedActorPublication mp_ProtocolPublication;
		CDistributedAppState mp_AppState;
		
		CTrustedPermissionSubscription mp_Permissions;
		
		TCMap<CStr, CVersionDownload> mp_VersionDownloads;
		TCMap<CStr, CVersionUpload> mp_VersionUploads;
		
		TCActor<CSeparateThreadActor> mp_QueryFileActor;
	};
}
