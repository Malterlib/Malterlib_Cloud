// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Concurrency/LogError>
#include <Mib/CommandLine/TableRenderer>

#include "Malterlib_Cloud_App_AppManager.h"
#include "Malterlib_Cloud_App_AppManager_NetworkTunnelManager.h"

namespace NMib::NCloud
{
	CNetworkTunnelManager::CNetworkTunnelManager()
	{
	}
}

namespace NMib::NCloud::NAppManager
{
	TCFuture<uint32> CAppManagerActor::fp_CommandLine_NetworkTunnelSubscriptionList(CEJSONSorted _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		CTableRenderHelper TableRenderer = _pCommandLine->f_TableRenderer();
		auto AnsiEncoding= _pCommandLine->f_AnsiEncoding();
		TableRenderer.f_AddHeadings("Host");
		TableRenderer.f_SetOptions(CTableRenderHelper::EOption_Rounded | CTableRenderHelper::EOption_AvoidRowSeparators);

		//for (auto &Manager : mp_CloudManagers)
		//	TableRenderer.f_AddRow(Manager.m_HostInfo.m_HostInfo.f_GetDescColored(AnsiEncoding.f_Flags()));

		TableRenderer.f_Output(_Params);

		co_return 0;
	}
}
