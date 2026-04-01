// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <Mib/Core/Core>

#include <Mib/Cloud/DebugManager>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedApp>
#include <Mib/Web/HTTPServer>

namespace NMib::NCloud::NDebugManagerClient
{
	struct CDebugManagerClientServer;

	struct CDebugManagerClientApp : public CDistributedAppActor
	{
		CDebugManagerClientApp();
		~CDebugManagerClientApp();

	private:
		TCFuture<void> fp_StartApp(NEncoding::CEJsonSorted const _Params) override;
		TCFuture<void> fp_StopApp() override;
		TCFuture<void> fp_Destroy() override;

		TCFuture<void> fp_StartWebServer();
		TCFuture<void> fp_SubscribeToManagers();

		TCFuture<bool> fp_HandleRequest(NStorage::TCSharedPointer<CHTTPConnection> _pConnection, NStorage::TCSharedPointer<CHTTPRequest> _pRequest);

		void fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine) override;

		CHTTPServer mp_HTTPServer;

		TCTrustedActorSubscription<CDebugManager> mp_DebugManagers;
	};
}
