
#include "Malterlib_Cloud_App_BackupManager_Internal.h"
#include "Malterlib_Cloud_App_BackupManager.h"

#include <Mib/Web/WebSocket>
#include <Mib/Network/SSL>
#include <Mib/Network/Sockets/SSL>
#include <Mib/Process/Platform>

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
#ifdef DPlatformFamily_OSX
		CStr Path = fg_GetSys()->f_GetEnvironmentVariable("PATH");
		if (Path.f_Find("/opt/local/bin") < 0)
			fg_GetSys()->f_SetEnvironmentVariable("PATH", "/opt/local/bin:" + Path);
#endif
	}
	
	CBackupManagerServer::~CBackupManagerServer()
	{
	}
	
	TCContinuation<void> CBackupManagerServer::f_Init()
	{
		TCContinuation<void> Continuation;
		fp_SetupPermissions() > Continuation % "Failed to setup permissions" / [=]()
			{
				fp_Publish() > Continuation % "Failed to publish";
			}
		;

		return Continuation;
	}
	
	TCContinuation<void> CBackupManagerServer::fp_SetupPermissions()
	{
		TCSet<CStr> Permissions;
		Permissions["Backup/WriteSelf"];
		Permissions["Backup/ReadSelf"];
		Permissions["Backup/ReadAll"];
		Permissions["Backup/ListAll"];
		
		mp_AppState.m_TrustManager(&CDistributedActorTrustManager::f_RegisterPermissions, Permissions) > fg_DiscardResult();

		TCVector<CStr> SubscribePermissions;
		SubscribePermissions.f_Insert("Backup/*");
	
		TCContinuation<void> Continuation;
		mp_AppState.m_TrustManager(&CDistributedActorTrustManager::f_SubscribeToPermissions, SubscribePermissions, fg_ThisActor(this)) 
			> Continuation / [this, Continuation](CTrustedPermissionSubscription &&_Subscription)
			{
				mp_Permissions = fg_Move(_Subscription);
				auto fOnPermissionsChange = [this](NStr::CStr const &_HostID, NContainer::TCSet<NStr::CStr> const &_Permissions)
					{
						if (!mp_ProtocolInterface.m_Publication.f_IsValid())
							return;
						if (!_Permissions.f_FindEqual("Backup/WriteSelf"))
							return;
						mp_ProtocolInterface.m_Publication.f_Republish(_HostID); // Force clients to reconnect
					}
				;
				mp_Permissions.f_OnPermissionsAdded(fOnPermissionsChange);
				mp_Permissions.f_OnPermissionsRemoved(fOnPermissionsChange);
				
				Continuation.f_SetResult();
			}
		;
		
		return Continuation;
	}
	
	TCContinuation<void> CBackupManagerServer::fp_Destroy()
	{
		for (auto &BackupInstance : mp_BackupInstances)
			fp_DestroyBackupInstance(BackupInstance.f_GetKey(), BackupInstance.m_OwningHost, true, "Backup Manager shutting down");
		
		auto pCanDestroy = fg_Move(mp_pCanDestroyTracker);
		
		for (auto &Download : mp_BackupDownloads)
			Download.f_Destroy() > pCanDestroy->f_Track();
		
		mp_ProtocolInterface.f_Destroy() > pCanDestroy->f_Track();
		
		if (mp_QueryFileActor)
			mp_QueryFileActor->f_Destroy() > pCanDestroy->f_Track();
		
		return pCanDestroy->m_Continuation;
	}
	
	TCActor<CSeparateThreadActor> const &CBackupManagerServer::fp_GetQueryFileActor()
	{
		if (mp_QueryFileActor)
			return mp_QueryFileActor;
		
		mp_QueryFileActor = fg_ConstructActor<CSeparateThreadActor>(fg_Construct("Backup manager query file actor"));
		return mp_QueryFileActor;
	}
}
