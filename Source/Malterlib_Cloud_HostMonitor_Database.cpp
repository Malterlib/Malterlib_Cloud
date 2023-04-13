 // Copyright © 2023 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_HostMonitor.h"
#include "Malterlib_Cloud_HostMonitor_Internal.h"

#include <Mib/CommandLine/TableRenderer>

namespace NMib::NCloud::NHostMonitorDatabase
{
	constexpr NStr::CStr CConfigFileHistoryEntryKey::mc_Prefix = NStr::gc_Str<"HostMonCFHE">;
}

namespace NMib::NCloud
{
	using namespace NHostMonitorDatabase;

	TCFuture<void> CHostMonitor::CInternal::f_SetupDatabase()
	{
		co_return {};
	}
}
