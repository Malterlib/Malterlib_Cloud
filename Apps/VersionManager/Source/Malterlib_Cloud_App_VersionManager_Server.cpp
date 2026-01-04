// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/DistributedActorTrustManager>
#include <Mib/Concurrency/DistributedActorTrustManagerDatabases/JsonDirectory>
#include <Mib/Concurrency/LogError>

#include "Malterlib_Cloud_App_VersionManager.h"
#include "Malterlib_Cloud_App_VersionManager_Server.h"

namespace NMib::NCloud::NVersionManager
{
	CVersionManagerDaemonActor::CServer::CServer(CDistributedAppState &_AppState)
		: mp_AppState(_AppState)
	{
#ifdef DPlatformFamily_macOS
		CStr Path = fg_GetSys()->f_GetEnvironmentVariable("PATH");

		CStr OriginalPath = Path;

		if (Path.f_Find("/usr/local/bin") < 0)
			Path = "/usr/local/bin:" + Path;
		if (Path.f_Find("/opt/homebrew/bin") < 0)
			Path = "/opt/homebrew/bin:" + Path;

		if (Path != OriginalPath)
			fg_GetSys()->f_SetEnvironmentVariable("PATH", Path);
#endif
	}

	CVersionManagerDaemonActor::CServer::~CServer()
	{
	}

	TCFuture<void> CVersionManagerDaemonActor::CServer::f_Init()
	{
		co_await fp_SetupDatabase();
		co_await fp_FindVersions();
		co_await fp_SetupPermissions();
		co_await fp_Publish();
		co_await fp_SyncInit();

		co_return {};
	}

	TCSet<CStr> CVersionManagerDaemonActor::CServer::fp_ApplicationSet()
	{
		TCSet<CStr> Return;
		for (auto &Application : mp_Applications)
			Return[Application.f_GetName()];
		return Return;
	}

	TCFuture<void> CVersionManagerDaemonActor::CServer::fp_FindVersions()
	{
		bool bLoaded = co_await fp_LoadVersionsFromDatabase();
		if (bLoaded && mp_Applications.f_GetLen() > 0)
			co_return {};

		// Fallback: scan disk and populate database
		co_await f_RefreshDatabaseFromDisk();

		co_return {};
	}

	TCFuture<void> CVersionManagerDaemonActor::CServer::fp_SetupPermissions()
	{
		TCSet<CStr> Permissions;
		Permissions["Application/ReadAll"];
		Permissions["Application/ListAll"];
		Permissions["Application/WriteAll"];
		Permissions["Application/TagAll"];

		for (auto &Application : mp_Applications)
		{
			Permissions[fg_Format("Application/Read/{}", Application.f_GetName())];
			Permissions[fg_Format("Application/Write/{}", Application.f_GetName())];
		}

		for (auto &Tag : mp_KnownTags)
		{
			Permissions[fg_Format("Application/Tag/{}", Tag)];
		}

		co_await mp_AppState.m_TrustManager(&CDistributedActorTrustManager::f_RegisterPermissions, Permissions);;

		TCVector<CStr> SubscribePermissions;
		SubscribePermissions.f_Insert("Application/*");

		mp_Permissions = co_await mp_AppState.m_TrustManager(&CDistributedActorTrustManager::f_SubscribeToPermissions, SubscribePermissions, fg_ThisActor(this));

		mp_Permissions.f_OnPermissionsAdded
			(
				g_ActorFunctorWeak / [this](CPermissionIdentifiers _Identity, TCMap<CStr, CPermissionRequirements> _AddedPermissions) -> TCFuture<void>
				{
					co_await fp_UpdateSubscriptionsForChangedPermissions(_Identity);

					co_return {};
				}
			)
		;

		mp_Permissions.f_OnPermissionsRemoved
			(
				g_ActorFunctorWeak / [this](CPermissionIdentifiers _Identity, TCSet<CStr> _RemovedPermissions) -> TCFuture<void>
				{
					co_await fp_UpdateSubscriptionsForChangedPermissions(_Identity);

					co_return {};
				}
			)
		;

		co_return {};
	}

	TCFuture<void> CVersionManagerDaemonActor::CServer::fp_Destroy()
	{
		CLogError LogError("Malterlib/Cloud/VersionManager");

		co_await fp_SyncDestroy().f_Wrap() > LogError.f_Warning("Failed to destroy sync");

		{
			TCFutureVector<void> DestroyResults;

			fg_Move(mp_UploadSequencer).f_Destroy() > DestroyResults;
			fg_Move(mp_RefreshSequencer).f_Destroy() > DestroyResults;

			for (auto &VersionDownloads : mp_VersionDownloads)
			{
				if (VersionDownloads.m_FileTransferSend)
					fg_Move(VersionDownloads.m_FileTransferSend).f_Destroy() > DestroyResults;
			}
			mp_VersionDownloads.f_Clear();

			for (auto &VersionUploads : mp_VersionUploads)
			{
				if (VersionUploads.m_FileTransferReceive)
					fg_Move(VersionUploads.m_FileTransferReceive).f_Destroy() > DestroyResults;
				if (VersionUploads.m_DownloadSubscription)
					fg_Exchange(VersionUploads.m_DownloadSubscription, nullptr)->f_Destroy() > DestroyResults;
			}
			mp_VersionUploads.f_Clear();

			auto fDestroySubscriptions = [&](TCMap<CStr, CSubscription> &o_Subscriptions)
				{
					for (auto &Subscription : o_Subscriptions)
					{
						if (Subscription.m_fOnNewVersions)
							fg_Move(Subscription.m_fOnNewVersions).f_Destroy() > DestroyResults;
					}
					o_Subscriptions.f_Clear();
				}
			;

			fDestroySubscriptions(mp_GlobalVersionSubscriptions);

			for (auto &Subscriptions : mp_VersionSubscriptions)
				fDestroySubscriptions(Subscriptions);
			mp_VersionSubscriptions.f_Clear();

			co_await fg_AllDone(DestroyResults).f_Wrap() > LogError.f_Warning("Failed to destroy version manager");
		}

		co_await mp_ProtocolInterface.f_Destroy().f_Wrap() > LogError.f_Warning("Failed to destroy protocol interface");

		{
			auto pCanDestroy = fg_Move(mp_pCanDestroyTracker);
			auto CanDestroyFuture = pCanDestroy->f_Future();
			pCanDestroy.f_Clear();
			co_await fg_Move(CanDestroyFuture);
		}

		if (mp_DatabaseActor)
			co_await fg_Move(mp_DatabaseActor).f_Destroy().f_Wrap() > LogError.f_Warning("Failed to destroy database actor");

		co_return {};
	}
}
