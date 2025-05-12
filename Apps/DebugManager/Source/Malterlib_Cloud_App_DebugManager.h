// Copyright © 2025 Unbroken AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>

#include <Mib/Cloud/DebugManager>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedApp>
#include <Mib/CrashReport/DebugDatabase>
#include <Mib/Daemon/Daemon>

namespace NMib::NCloud::NDebugManager
{
	struct CDebugManagerServer;
	
	struct CDebugManagerApp : public CDistributedAppActor
	{
		CDebugManagerApp();
		~CDebugManagerApp();
		
	private:
		struct CDebugManagerImplementation : public CDebugManager
		{
			TCFuture<CAssetList::CResult> f_Asset_List(CAssetList _Params) override;
			TCFuture<CAssetUpload::CResult> f_Asset_Upload(CAssetUpload _Params) override;
			TCFuture<CAssetDownload::CResult> f_Asset_Download(CAssetDownload _Params) override;
			TCFuture<CAssetDelete::CResult> f_Asset_Delete(CAssetDelete _Params) override;

			TCFuture<CCrashDumpList::CResult> f_CrashDump_List(CCrashDumpList _Params) override;
			TCFuture<CCrashDumpUpload::CResult> f_CrashDump_Upload(CCrashDumpUpload _Params) override;
			TCFuture<CCrashDumpDownload::CResult> f_CrashDump_Download(CCrashDumpDownload _Params) override;
			TCFuture<CCrashDumpDelete::CResult> f_CrashDump_Delete(CCrashDumpDelete _Params) override;

			DMibDelegatedActorImplementation(CDebugManagerApp);
		};

		struct CDownload
		{
			CDownload();
			~CDownload();

			CStr const &f_GetDownloadID() const
			{
				return TCMap<CStr, CDownload>::fs_GetKey(*this);
			}

			TCActor<NCloud::CFileTransferSend> m_FileTransferSend;
		};

		struct CUpload
		{
			CUpload();
			~CUpload();

			CStr const &f_GetUploadID() const
			{
				return TCMap<CStr, CUpload>::fs_GetKey(*this);
			}

			TCActor<NCloud::CFileTransferReceive> m_FileTransferReceive;
		};

		struct CPendingNotification
		{
			TCVector<CStr> m_FileNames;
			CDebugDatabase::CMetadata m_Metadata;
			TCOptional<NStr::CStr> m_ExceptionInfo;
			CClock m_LastAdded;
		};

		TCFuture<void> fp_StartApp(NEncoding::CEJsonSorted const _Params) override;
		TCFuture<void> fp_StopApp() override;
		TCFuture<void> fp_Destroy() override;

		TCFuture<void> fp_DestroyAll();

		void fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine) override;

		TCFuture<void> fp_SetupPermissions();
		TCFuture<void> fp_SetupDatabase();
		TCFuture<void> fp_Publish();

		TCFuture<void> fp_Notify_OpenLogReporter();
		TCFuture<void> fp_Notify_ScheduleProcess();
		TCFuture<void> fp_Notify_CrashDumpAdded(CDebugDatabase::CCrashDumpAdd _CrashDumpAdd);
		TCFuture<void> fp_Notify_Process(bool _bForce);

		constexpr static uint32 mcp_MaxQueueSize = NFile::gc_IdealNetworkQueueSize;

		TCDistributedActorInstance<CDebugManagerImplementation> mp_DebugManagerInterface;

		CTrustedPermissionSubscription mp_Permissions;

		TCMap<CStr, CDownload> mp_Downloads;
		TCMap<CStr, CUpload> mp_Uploads;

		TCActor<CDebugDatabase> mp_DebugDatabase;

		CDebugDatabase::CInitResult mp_DebugDatabaseInitResult;

		TCMap<CStr, CPendingNotification> mp_PendingNotifications;
		CActorSubscription mp_PendingNotificationsTimeout;
		TCOptional<CDistributedAppLogReporter::CLogReporter> mp_NotificationLogReporter;
	};

	struct COptionalFuture
	{
		COptionalFuture(TCFuture<void> &&_Future);
		COptionalFuture(COptionalFuture &&);
		~COptionalFuture();

		TCFuture<void> m_Future;
	};
}
