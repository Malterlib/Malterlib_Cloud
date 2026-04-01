// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "Malterlib_Cloud_App_TunnelProxyManager.h"

#include <Mib/Web/WebSocket>
#include <Mib/Network/SSL>
#include <Mib/Network/Sockets/SSL>
#include <Mib/Encoding/JsonShortcuts>

namespace NMib::NCloud::NTunnelProxyManager
{
	void CTunnelProxyManagerApp::fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine)
	{
		CDistributedAppActor::fp_BuildCommandLine(o_CommandLine);

		o_CommandLine.f_SetProgramDescription
			(
				"Malterlib Cloud Manager"
				, "Manages cloud applications."
			)
		;

		auto DefaultSection = o_CommandLine.f_GetDefaultSection();

		DefaultSection.f_RegisterCommand
			(
				{
					"Names"_o= _o["--reload-config"]
					, "Description"_o= "Reload config."
				}
				, [this](CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine) -> TCFuture<uint32>
				{
					co_await mp_State.m_ConfigDatabase.f_Load();
					co_await fp_ReloadConfig
						(
							g_ActorFunctor / [pCommandLine = fg_Move(_pCommandLine)](CStr _Message) -> TCFuture<void>
							{
								*pCommandLine %= "{}\n"_f << _Message;
								co_return {};
							}
						)
					;

					co_return 0;
				}
			)
		;
	}
}
