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

	private:
		TCContinuation<void> fp_Destroy() override;
		void fp_Init();
		void fp_KeyManagerAvailable(TCDistributedActor<CKeyManager> const &_KeyManager);
		
		NConcurrency::TCActor<CSecretsManagerDaemonActor::CServer> mp_ServerActor;
		TCTrustedActorSubscription<CKeyManager> mp_KeyManagerSubscription;
		TCSet<TCActor<CSecretsManagerServerDatabase>> mp_PendingDatabases;
		CDistributedAppState &mp_AppState;
		TCActor<> mp_Delegator;
	};
}
