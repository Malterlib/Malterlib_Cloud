// Copyright © 2023 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_HostMonitor.h"
#include "Malterlib_Cloud_HostMonitor_Internal.h"

#include <Mib/Concurrency/LogError>

namespace NMib::NCloud
{
	using namespace NHostMonitorDatabase;

	TCFuture<void> CHostMonitor::CInternal::f_PeriodicUpdate_Patch(bool _bCanSkip)
	{
		if (!m_Config.m_PatchInterval)
			co_return {};

		auto OnResume = co_await m_pThis->f_CheckDestroyedOnResume();

		auto SequenceSubscription = co_await m_UpdatePeriodicPatch.f_Sequence();

		bool bIsOverTime = m_PatchClock && (*m_PatchClock).f_GetTime() >= m_Config.m_PatchInterval;

		if (!m_PatchClock)
			m_PatchClock = CClock{true};
		else if (_bCanSkip && !bIsOverTime)
			co_return {};

		if (bIsOverTime)
			(*m_PatchClock).f_AddOffset(m_Config.m_PatchInterval);

		CLogError LogError("Malterlib/Cloud/HostMonitor");

		bool bUpdateDatabase = co_await f_PeriodicUpdate_Patch_OsVersion();
		bUpdateDatabase = bUpdateDatabase || co_await f_PeriodicUpdate_Patch_ExpectedOsVersion();
		bUpdateDatabase = bUpdateDatabase || co_await f_PeriodicUpdate_Patch_PatchStatus();

		if (bUpdateDatabase)
		{
			co_await m_Database
				(
					&CDatabaseActor::f_WriteWithCompaction
					, g_ActorFunctorWeak / [pThis = this](CDatabaseActor::CTransactionWrite _Transaction, bool _bCompacting) -> TCFuture<CDatabaseActor::CTransactionWrite>
					{
						auto CaptureScope = co_await (g_CaptureExceptions % "Error saving patch state to database");

						// TODO: Handle _bCompacting

						auto WriteTransaction = fg_Move(_Transaction);
						auto PatchDatabaseState = pThis->m_PatchDatabaseState;

						co_await fg_ContinueRunningOnActor(WriteTransaction.f_Checkout());

						CPatchStateKey Key;
						WriteTransaction.m_Transaction.f_Upsert(Key, PatchDatabaseState);

						co_return fg_Move(WriteTransaction);
					}
				)
				.f_Wrap() > LogError("Error saving patch state to database")
			;
		}

		co_return {};
	}
}
