// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedApp>
#include <Mib/Cloud/SecretsManager>

namespace NMib::NCloud::NSecretsManager
{
	struct CSecretsManagerDaemonActor::CServer : public CActor
	{
	public:
		using CActorHolder = CDelegatedActorHolder;
		
		CServer(CDistributedAppState &_AppState);
		~CServer();

		struct CSecretsManagerImplementation : public CSecretsManager
		{
			CServer *m_pThis;
		};
		
	private:
		TCContinuation<void> fp_Destroy() override;
		void fp_Init();
		void fp_Publish();
		
		TCContinuation<void> fp_SetupPermissions();

		TCSharedPointer<CCanDestroyTracker> mp_pCanDestroyTracker;

		TCDelegatedActorInterface<CSecretsManagerImplementation> mp_ProtocolInterface;
		
		CDistributedAppState &mp_AppState;
		
		CTrustedPermissionSubscription mp_Permissions;
	};
}
