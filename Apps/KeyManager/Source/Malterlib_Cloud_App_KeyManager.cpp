// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Cloud/KeyManagerServer>
#include <Mib/Cloud/KeyManagerDatabases/EncryptedFile>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/DistributedActorTrustManager>
#include <Mib/Concurrency/DistributedActorTrustManagerDatabases/JSONDirectory>
#include <Mib/Concurrency/LogError>

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
		DMibLogWithCategory(Mib/Cloud/KeyManager/Daemon, Critical, "Waiting for user to provide password");
		co_return {};
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
		CLogError LogError("Mib/Cloud/KeyManager/Daemon");

		if (mp_ServerActor || mp_DatabaseActor)
			DMibLogWithCategory(Mib/Cloud/KeyManager/Daemon, Info, "Shutting down");

		if (mp_ServerActor)
		{
			DMibLogWithCategory(Mib/Cloud/KeyManager/Daemon, Info, "Shutting down key server");
			co_await fg_Move(mp_ServerActor).f_Destroy().f_Wrap() > LogError.f_Warning("Failed to destroy server actor");
			DMibLogWithCategory(Mib/Cloud/KeyManager/Daemon, Info, "Key server shut down");
		}

		if (mp_DatabaseActor)
		{
			DMibLogWithCategory(Mib/Cloud/KeyManager/Daemon, Info, "Shutting down key server database");
			co_await fg_Move(mp_DatabaseActor).f_Destroy().f_Wrap() > LogError.f_Warning("Failed to destroy database actor");
			DMibLogWithCategory(Mib/Cloud/KeyManager/Daemon, Info, "Key server database shut down");
		}

		co_return {};
	}
}

namespace NMib::NCloud
{
	TCActor<CDistributedAppActor> fg_ConstructApp_KeyManager()
	{
		return fg_Construct<NKeyManager::CKeyManagerDaemonActor>();
	}
}
