// Copyright © 2018 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Cryptography/RandomID>
#include "Malterlib_Cloud_App_AppDistributionManager.h"

namespace NMib::NCloud::NAppDistributionManager
{
	TCContinuation<uint32> CAppDistributionManagerActor::fp_CommandLine_DistributionRemove(CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		auto Auditor = f_Auditor();

		CStr Name = _Params["Distribution"].f_String();
		
		if (!mp_Distributions.f_FindEqual(Name))
			return Auditor.f_Exception(fg_Format("No such distribution '{}'", Name));

		mp_Distributions.f_Remove(Name);

		if (auto *pDistributionState = mp_State.m_StateDatabase.m_Data.f_GetMember("Distributions"))
		{
			if (pDistributionState->f_GetMember(Name))
				pDistributionState->f_RemoveMember(Name);
		}

		TCContinuation<uint32> Continuation;

		mp_State.m_StateDatabase.f_Save() > Continuation % "[Remove distribution] Failed to save state" % Auditor / [Continuation, Name, Auditor]() mutable
			{
				Auditor.f_Info(fg_Format("Removed distribution '{}'", Name));
				Continuation.f_SetResult();
			}
		;

		fp_AutoUpdate_Update();

		return Continuation;
	}
}
