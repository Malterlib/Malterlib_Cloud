// Copyright © 2018 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
#include "Malterlib_Cloud_App_AppDistributionManager.h"

namespace NMib::NCloud::NAppDistributionManager
{
	TCContinuation<uint32> CAppDistributionManagerActor::fp_CommandLine_DistributionAdd(CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		auto Auditor = f_Auditor();

		CDistributionSettings Settings;

		try
		{
			fp_ParseSettings(_Params, Settings);
		}
		catch (CException const &_Exception)
		{
			return Auditor.f_Exception(_Exception.f_GetErrorStr());
		}

		CStr Name = _Params.f_GetMemberValue("Distribution", Settings.m_VersionManagerApplication).f_String();

		if (!fg_IsValidHostname(Name))
			return Auditor.f_Exception("'{}' is not a valid distribution name"_f << Name);

		if (mp_Distributions.f_FindEqual(Name))
			return Auditor.f_Exception("Distribution '{}' already exists"_f << Name);

		auto &Distribution = mp_Distributions[Name];

		Distribution.m_Settings = fg_Move(Settings);

		fp_SaveState(Distribution);

		TCContinuation<uint32> Continuation;
		mp_State.m_StateDatabase.f_Save() > Continuation % "[Add distribution] Failed to save state" % Auditor / [Continuation, Name, Auditor]() mutable
			{
				Auditor.f_Info("Added distribution '{}'"_f << Name);
				Continuation.f_SetResult();
			}
		;

		fp_AutoUpdate_Update();

		return Continuation;
	}
}
