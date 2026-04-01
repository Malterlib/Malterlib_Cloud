// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "Malterlib_Cloud_App_BackupManager_Internal.h"
#include "Malterlib_Cloud_App_BackupManager.h"

#include <Mib/Web/WebSocket>
#include <Mib/Network/SSL>
#include <Mib/Network/Sockets/SSL>
#include <Mib/Process/Platform>
#include <Mib/Concurrency/LogError>

namespace NMib::NCloud::NBackupManager
{
	CStr CBackupManagerServer::CBackupKey::f_GetDesc() const
	{
		return fg_Format("{}/{tst.}-{}", m_BackupName, m_BackupTime, m_BackupID);
	}

	CBackupManagerServer::CBackupManagerServer(CDistributedAppState &_AppState)
		: mp_AppState(_AppState)
		, mp_pCanDestroyTracker(fg_Construct())
		, mp_FriendlyName{NProcess::NPlatform::fg_Process_GetComputerName()}
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

	CBackupManagerServer::~CBackupManagerServer()
	{
	}

	TCFuture<void> CBackupManagerServer::f_Init()
	{
		auto [Permission, BackupSources] = co_await ((fp_SetupPermissions() + fp_EnumBackupSourcesFromDisk()) % "Failed to setup permissions or enum backup sources");

		for (auto &BackupSource : BackupSources)
			fp_CreateBackupSource(BackupSource);

		co_await (fp_Publish() % "Failed to publish");

		co_return {};
	}

	TCFuture<void> CBackupManagerServer::fp_SetupPermissions()
	{
		TCSet<CStr> Permissions;
		Permissions["Backup/WriteSelf"];
		Permissions["Backup/ReadSelf"];
		Permissions["Backup/ReadAll"];
		Permissions["Backup/ListAll"];

		co_await mp_AppState.m_TrustManager(&CDistributedActorTrustManager::f_RegisterPermissions, Permissions);

		TCVector<CStr> SubscribePermissions;
		SubscribePermissions.f_Insert("Backup/*");

		mp_Permissions = co_await mp_AppState.m_TrustManager(&CDistributedActorTrustManager::f_SubscribeToPermissions, SubscribePermissions, fg_ThisActor(this));

		mp_Permissions.f_OnPermissionsAdded
			(
				g_ActorFunctorWeak / [this](CPermissionIdentifiers _Identity, NContainer::TCMap<NStr::CStr, CPermissionRequirements> _Permissions) -> TCFuture<void>
				{
					if (!mp_ProtocolInterface.m_Publication.f_IsValid())
						co_return {};

					if (!_Permissions.f_FindEqual("Backup/WriteSelf"))
						co_return {};

					co_await mp_ProtocolInterface.m_Publication.f_Republish(_Identity.f_GetHostID()); // Force clients to reconnect

					co_return {};
				}
			)
		;

		mp_Permissions.f_OnPermissionsRemoved
			(
				g_ActorFunctorWeak / [this](CPermissionIdentifiers _Identity, NContainer::TCSet<NStr::CStr> _Permissions) -> TCFuture<void>
				{
					if (!mp_ProtocolInterface.m_Publication.f_IsValid())
						co_return {};

					if (!_Permissions.f_FindEqual("Backup/WriteSelf"))
						co_return {};

					co_await mp_ProtocolInterface.m_Publication.f_Republish(_Identity.f_GetHostID()); // Force clients to reconnect

					co_return {};
				}
			)
		;

		co_return {};
	}

	TCFuture<void> CBackupManagerServer::fp_Destroy()
	{
		CLogError LogError("Mib/Cloud/BackupManager");

		auto CanDestroyFuture = mp_pCanDestroyTracker->f_Future();
		mp_pCanDestroyTracker.f_Clear();

		TCFutureVector<void> Destroys;
		fg_Move(CanDestroyFuture) > Destroys;

		for (auto &BackupInstance : mp_BackupInstances)
			fp_DestroyBackupInstance(BackupInstance.f_GetKey(), BackupInstance.m_OwningHost, true, "Backup Manager shutting down") > Destroys;

		for (auto &Download : mp_BackupDownloads)
			Download.f_Destroy() > Destroys;

		co_await fg_AllDone(Destroys).f_Wrap() > LogError.f_Warning("Failed to destroy backup manager server");;

		co_await mp_ProtocolInterface.f_Destroy().f_Wrap() > LogError.f_Warning("Failed to destroy protocol interface");

		co_return {};
	}
}
