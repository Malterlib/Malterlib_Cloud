// Copyright © 2025 Unbroken AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Concurrency/LogError>

#include "Malterlib_Cloud_App_DebugManager.h"

namespace NMib::NCloud::NDebugManager
{
	constexpr uint64 gc_MaxDatabaseSize = constant_uint64(16) * 1024 * 1024 * 1024;

	TCFuture<void> CDebugManagerApp::fp_SetupDatabase()
	{
		uint64 MaxDatabaseSize = mp_State.m_ConfigDatabase.m_Data.f_GetMemberValue("MaxDatabaseSize", gc_MaxDatabaseSize).f_Integer();

		mp_DebugDatabase = fg_Construct
			(
				CDebugDatabase::COptions
				{
					.m_DatabaseRoot = mp_State.m_RootDirectory / "DebugManagerDatabase"
					, .m_MaxDatabaseSize = MaxDatabaseSize
				}
			)
		;

		mp_DebugDatabaseInitResult = co_await mp_DebugDatabase(&CDebugDatabase::f_Init);

		co_return {};
	}
}
