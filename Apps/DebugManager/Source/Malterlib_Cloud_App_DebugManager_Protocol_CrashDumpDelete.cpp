// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>
#include <Mib/Concurrency/LogError>
#include <Mib/Cryptography/UUID>

#include "Malterlib_Cloud_App_DebugManager.h"
#include "Malterlib_Cloud_App_DebugManager_Protocol_Conversion.hpp"

namespace NMib::NCloud::NDebugManager
{
	auto CDebugManagerApp::CDebugManagerImplementation::f_CrashDump_Delete(CCrashDumpDelete _Params) -> TCFuture<CCrashDumpDelete::CResult>
	{
		auto pThis = m_pThis;

		auto CallingHostInfo = fg_GetCallingHostInfo();
		auto OnResume = co_await pThis->f_CheckDestroyedOnResume();
		auto Auditor = pThis->mp_State.f_Auditor({}, CallingHostInfo);

		TCVector<CStr> Permissions{"DebugManager/DeleteAll", "DebugManager/DeleteCrashDump"};

		auto bHasPermissions = co_await (pThis->mp_Permissions.f_HasPermission("Delete crash dumps", Permissions) % "Permission denied deleting dumps" % Auditor);
		if (!bHasPermissions)
			co_return Auditor.f_AccessDenied("(Delete crash dump)", Permissions);

		auto Result = co_await
			(
				pThis->mp_DebugDatabase.f_Bind<&CDebugDatabase::f_CrashDump_Delete>
				(
					fg_ConvertToDebugDatabase<CDebugDatabase::CCrashDumpFilter>(fg_Move(_Params.m_Filter))
					, _Params.m_nMaxToDelete
					, _Params.m_bPretend
				)
				% "Failed to delete crash dumps in database" % Auditor
			)
		;

		Auditor.f_Info
			(
				"{} assests resulted in {} crash dumps deleted, {} files deleted and {ns } bytes deleted"_f
				<< (_Params.m_bPretend ? "Pretending to delete" : "Deleting")
				<< Result.m_nItemsDeleted
				<< Result.m_nFilesDeleted
				<< Result.m_nBytesDeleted
			)
		;

		co_return CCrashDumpDelete::CResult
			{
				.m_nCrashDumpsDeleted = Result.m_nItemsDeleted
				, .m_nFilesDeleted = Result.m_nFilesDeleted
				, .m_nBytesDeleted = Result.m_nBytesDeleted
			}
		;
	}
}
