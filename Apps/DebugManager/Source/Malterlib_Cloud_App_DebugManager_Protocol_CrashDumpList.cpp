// Copyright © 2025 Unbroken AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Concurrency/LogError>
#include <Mib/Cryptography/UUID>

#include "Malterlib_Cloud_App_DebugManager.h"
#include "Malterlib_Cloud_App_DebugManager_Protocol_Conversion.hpp"

namespace NMib::NCloud::NDebugManager
{
	auto CDebugManagerApp::CDebugManagerImplementation::f_CrashDump_List(CCrashDumpList _Params) -> TCFuture<CCrashDumpList::CResult>
	{
		auto pThis = m_pThis;

		auto CallingHostInfo = fg_GetCallingHostInfo();
		auto OnResume = co_await pThis->f_CheckDestroyedOnResume();
		auto Auditor = pThis->mp_State.f_Auditor({}, CallingHostInfo);

		TCVector<CStr> Permissions{"DebugManager/ReadAll", "DebugManager/ReadCrashDump", "DebugManager/ListAll", "DebugManager/ListCrashDump"};

		auto bHasPermissions = co_await (pThis->mp_Permissions.f_HasPermission("List crash dumps", Permissions) % "Permission denied listing crash dumps" % Auditor);
		if (!bHasPermissions)
			co_return Auditor.f_AccessDenied("(Upload crash dump)", Permissions);

		auto CrashDumpGenerator = co_await pThis->mp_DebugDatabase
			(
				&CDebugDatabase::f_CrashDump_List
				, fg_ConvertToDebugDatabase<CDebugDatabase::CCrashDumpFilter>(fg_Move(_Params.m_Filter))
			)
		;

		co_return CCrashDumpList::CResult
			{
				.m_CrashDumpsGenerator = fg_CallSafe
				(
					[CrashDumpGenerator = fg_Move(CrashDumpGenerator), Auditor]() mutable -> NConcurrency::TCAsyncGenerator<NContainer::TCVector<CCrashDumpList::CCrashDump>>
					{
						umint nCrashDumps = 0;
						bool bDone = false;

						auto Cleanup = g_OnScopeExit / [&]
							{
								Auditor.f_Info("Listed {} crash dumps{}"_f << nCrashDumps << (bDone ? "" : " (aborted)"));
							}
						;

						for (auto iCrashDump = co_await fg_Move(CrashDumpGenerator).f_GetPipelinedIterator(); iCrashDump; co_await ++iCrashDump)
						{
							NContainer::TCVector<CCrashDumpList::CCrashDump> CrashDumps;
							for (auto &SourceCrashDump : *iCrashDump)
								CrashDumps.f_Insert(fg_ConvertToDebugDatabase<CCrashDumpList::CCrashDump>(fg_Move(SourceCrashDump)));

							nCrashDumps += CrashDumps.f_GetLen();

							co_yield fg_Move(CrashDumps);
						}

						bDone = true;

						co_return {};
					}
				)
			}
		;

		Auditor.f_Info("Started listing of crash dumps");

		co_return {};
	}
}
