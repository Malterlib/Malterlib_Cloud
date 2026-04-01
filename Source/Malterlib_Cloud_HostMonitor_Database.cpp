// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

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
		auto ReadTransaction = co_await m_Database(&CDatabaseActor::f_OpenTransactionRead);

		m_PatchDatabaseState = co_await fg_Move(ReadTransaction).f_BlockingDispatch
			(
				[](CDatabaseActor::CTransactionRead &&_ReadTransaction)
				{
					auto ReadCursor = _ReadTransaction.m_Transaction.f_ReadCursor();

					CPatchStateKey Key;
					CPatchStateValue PatchDatabaseState;

					if (ReadCursor.f_FindEqual(Key))
						PatchDatabaseState = ReadCursor.f_Value<CPatchStateValue>();

					return PatchDatabaseState;
				}
				, "Error reading patch database state"
			)
		;

		co_return {};
	}
}
