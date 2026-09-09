// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "Malterlib_Cloud_App_DebugManagerClient.h"

#include <Mib/Web/HTTPServer>
#include <Mib/Concurrency/AsyncDestroy>
#include <Mib/Network/ResolveActor>

namespace NMib::NCloud::NDebugManagerClient
{
	TCFuture<void> CDebugManagerClientApp::fp_StartWebServer()
	{
		CHTTPServerOptions HTTPOptions;

		CStr BindIP = fg_GetSys()->f_GetEnvironmentVariable("BIND_IP_MALTERLIB", fg_GetSys()->f_GetEnvironmentVariable("BIND_IP", "127.0.0.1"));
		DMibLog(Info, "BindIP: {}", BindIP);

		uint16 Port = fg_GetSys()->f_GetEnvironmentVariable("PORT", "8080").f_ToInt(uint16(8080));
		uint32 PortConcurrency = fg_GetSys()->f_GetEnvironmentVariable("PORT_CONCURRENCY", "1").f_ToInt(uint32(1));

		TCActor<CResolveActor> ResolveActor = fg_Construct();
		auto Cleanup = co_await fg_AsyncDestroy(ResolveActor);

		TCFutureVector<CResolveActor::CLookup> LookupFutures;
		if (BindIP.f_StartsWith("UNIX("))
		{
			for (uint16 iPort = Port; iPort < Port + PortConcurrency; ++iPort)
				ResolveActor(&CResolveActor::f_Resolve, CStr("{}.{}"_f << BindIP << iPort), NNetwork::ENetAddressType_None) > LookupFutures;
		}
		else
		{
			for (uint16 iPort = Port; iPort < Port + PortConcurrency; ++iPort)
				ResolveActor(&CResolveActor::f_Resolve, CStr("{}:{}"_f << BindIP << iPort), NNetwork::ENetAddressType_None) > LookupFutures;
		}

		auto Lookups = co_await fg_AllDone(LookupFutures);
		TCFutureVector<CResolveActor::CAddresses> ListenAddresses;
		for (auto &Lookup : Lookups)
			fg_Move(Lookup.m_Result) > ListenAddresses;

		HTTPOptions.m_FastCGIListenStartPort = Port;
		auto ResolvedAddresses = co_await fg_AllDone(ListenAddresses);
		for (auto &Addresses : ResolvedAddresses)
			HTTPOptions.m_FastCGIListenAddresses.f_Insert(fg_Move(Addresses[0]));
		HTTPOptions.m_bUseNginx = false;
		HTTPOptions.m_nMaxThreads = PortConcurrency;

		co_await mp_HTTPServer.f_AddHandlerActorForPathAsync
			(
				"/"
				, g_ActorFunctorWeak / [this](NStorage::TCSharedPointer<CHTTPConnection> _pConnection, NStorage::TCSharedPointer<CHTTPRequest> _pRequest) -> TCFuture<bool>
				{
					co_return co_await fp_HandleRequest(fg_Move(_pConnection), fg_Move(_pRequest));
				}
				, 1
			)
		;

		if (!mp_HTTPServer.f_Run(HTTPOptions))
			co_return DMibErrorInstance("Failed to start HTTP server");

		co_return {};
	}
}
