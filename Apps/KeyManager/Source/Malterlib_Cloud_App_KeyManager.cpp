// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Cloud/KeyManagerServer>
#include <Mib/Cloud/KeyManagerDatabases/EncryptedFile>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/DistributedActorTrustManager>
#include <Mib/Concurrency/DistributedActorTrustManagerDatabases/JSONDirectory>

#include "Malterlib_Cloud_App_KeyManager.h"

namespace NMib::NCloud::NKeyManager
{
	CKeyManagerDaemonActor::CKeyManagerDaemonActor()
		: CDistributedAppActor(CDistributedAppActor_Settings{"KeyManager"})
	{
	}

	CKeyManagerDaemonActor::~CKeyManagerDaemonActor()
	{
	}

	TCFuture<void> CKeyManagerDaemonActor::fp_StartApp(NEncoding::CEJSON const &_Params)
	{
		TCPromise<void> Promise;
		DMibLogWithCategory(Mib/Cloud/KeyManager/Daemon, Warning, "Waiting for user to provide password");
		Promise.f_SetResult();
		return Promise.f_MoveFuture();
	}

	void CKeyManagerDaemonActor::fp_DatabaseDecrypted()
	{
		DMibLogWithCategory(Mib/Cloud/KeyManager/Daemon, Info, "Password provided, starting up key manager");
		CKeyManagerServerConfig Config;
		Config.m_DatabaseActor = mp_DatabaseActor;
		mp_ServerActor = fg_ConstructActor<CKeyManagerServer>(Config, mp_State.m_DistributionManager);
	}

	TCFuture<void> CKeyManagerDaemonActor::fp_StopApp()
	{
		TCSharedPointer<CCanDestroyTracker> pCanDestroy = fg_Construct();

		if (mp_ServerActor || mp_DatabaseActor)
			DMibLogWithCategory(Mib/Cloud/KeyManager/Daemon, Info, "Shutting down");

		if (mp_ServerActor)
		{
			DMibLogWithCategory(Mib/Cloud/KeyManager/Daemon, Info, "Shutting down key server");
			mp_ServerActor->f_Destroy() > [this, pCanDestroy](TCAsyncResult<void> &&_Result)
				{
					DMibLogWithCategory(Mib/Cloud/KeyManager/Daemon, Info, "Key server shut down");
					if (mp_DatabaseActor)
					{
						DMibLogWithCategory(Mib/Cloud/KeyManager/Daemon, Info, "Shutting down key server database");
						mp_DatabaseActor->f_Destroy() > [pCanDestroy](TCAsyncResult<void> &&_Result)
							{
								DMibLogWithCategory(Mib/Cloud/KeyManager/Daemon, Info, "Key server database shut down");
							}
						;
						mp_DatabaseActor = nullptr;
					}
				}
			;
			mp_ServerActor = nullptr;
		}
		else if (mp_DatabaseActor)
		{
			DMibLogWithCategory(Mib/Cloud/KeyManager/Daemon, Info, "Shutting down key server database");
			mp_DatabaseActor->f_Destroy() > pCanDestroy->f_Track();
			mp_DatabaseActor = nullptr;
		}

		return pCanDestroy->f_Future();
	}
}

namespace NMib::NCloud
{
	TCActor<CDistributedAppActor> fg_ConstructApp_KeyManager()
	{
		return fg_Construct<NKeyManager::CKeyManagerDaemonActor>();
	}
}
