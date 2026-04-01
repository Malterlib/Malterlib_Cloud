// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Encoding/JsonShortcuts>
#include <Mib/Cryptography/RandomID>
#include "Malterlib_Cloud_App_AppDistributionManager.h"

namespace NMib::NCloud::NAppDistributionManager
{
	TCFuture<uint32> CAppDistributionManagerActor::fp_CommandLine_DistributionRemove(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		auto Auditor = f_Auditor();

		CStr Name = _Params["Distribution"].f_String();

		if (!mp_Distributions.f_FindEqual(Name))
			co_return Auditor.f_Exception(fg_Format("No such distribution '{}'", Name));

		mp_Distributions.f_Remove(Name);

		if (auto *pDistributionState = mp_State.m_StateDatabase.m_Data.f_GetMember("Distributions"))
		{
			if (pDistributionState->f_GetMember(Name))
				pDistributionState->f_RemoveMember(Name);
		}

		fp_AutoUpdate_Update();

		co_await (mp_State.m_StateDatabase.f_Save() % "[Remove distribution] Failed to save state" % Auditor);

		Auditor.f_Info(fg_Format("Removed distribution '{}'", Name));

		co_return 0;
	}
}
