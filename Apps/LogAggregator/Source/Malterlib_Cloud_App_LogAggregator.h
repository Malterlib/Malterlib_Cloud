// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedApp>
#include <Mib/Concurrency/DistributedLogAggregator>

namespace NMib::NCloud::NLogAggregator
{
	struct CLogAggregatorServer : public CActor
	{
	public:
		using CActorHolder = CDelegatedActorHolder;
		
		CLogAggregatorServer(CDistributedAppState &_AppState);
		~CLogAggregatorServer();
		
		struct CLogAggregatorImplementation : public CLogAggregator
		{
			CLogAggregatorServer *m_pThis;
		};

		TCContinuation<void> f_Init();
		
	private:
		TCContinuation<void> fp_Destroy() override;

		void fp_Publish();
		TCContinuation<void> fp_SetupPermissions();

		TCDelegatedActorInterface<CLogAggregatorImplementation> mp_ProtocolInterface;
		
		CDistributedAppState &mp_AppState;
		
		CTrustedPermissionSubscription mp_Permissions;
	};
}
