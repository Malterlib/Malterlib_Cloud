// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedApp>
#include <Mib/Cloud/SecretsManager>
#include <Mib/Container/Map>

#include "Malterlib_Cloud_App_SecretsManager_Server.h"
#include "Malterlib_Cloud_App_SecretsManager_Database.h"

namespace NMib::NCloud::NSecretsManager
{
	struct CSecretsManagerDaemonActor::CServerController : public CActor
	{
	public:
		using CActorHolder = CDelegatedActorHolder;
		
		CServerController(TCActor<> const &_Delegator, CDistributedAppState &_AppState);
		~CServerController();

		TCFuture<void> f_Init();

#if DMibConfig_Tests_Enable
		TCFuture<CEJSONSorted> f_Test_Command(CStr _Command, CEJSONSorted const _Params);
#endif

	private:
		TCFuture<void> fp_Destroy() override;
		TCFuture<void> fp_KeyManagerAvailable(TCDistributedActor<CKeyManager> _KeyManager);
		
		TCActor<CSecretsManagerDaemonActor::CServer> mp_ServerActor;
		TCTrustedActorSubscription<CKeyManager> mp_KeyManagerSubscription;
		TCSet<TCActor<CSecretsManagerServerDatabase>> mp_PendingDatabases;
		CDistributedAppState &mp_AppState;
		TCActor<> mp_Delegator;
	};
}
