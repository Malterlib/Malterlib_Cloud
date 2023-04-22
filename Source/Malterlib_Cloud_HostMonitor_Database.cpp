 // Copyright © 2023 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_HostMonitor.h"
#include "Malterlib_Cloud_HostMonitor_Internal.h"

#include <Mib/CommandLine/TableRenderer>

namespace NMib::NCloud::NHostMonitorDatabase
{
	constexpr CStr CConfigFileHistoryEntryKey::mc_Prefix = gc_Str<"HostMonCFHE">;
	constexpr CStr CPatchStateKey::mc_Prefix = gc_Str<"HostMonPS">;
}

namespace NMib::NCloud
{
	using namespace NHostMonitorDatabase;

	TCFuture<void> CHostMonitor::CInternal::f_SetupDatabase()
	{
		{
			auto Capture = co_await (g_CaptureExceptions % "Error reading patch database state");
			auto ReadTransaction = co_await m_Database(&CDatabaseActor::f_OpenTransactionRead);
			auto ReadCursor = ReadTransaction.m_Transaction.f_ReadCursor();

			CPatchStateKey Key;

			if (ReadCursor.f_FindEqual(Key))
				m_PatchDatabaseState = ReadCursor.f_Value<CPatchStateValue>();
		}

		co_return {};
	}
}
