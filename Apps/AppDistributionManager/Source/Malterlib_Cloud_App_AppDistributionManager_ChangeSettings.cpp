// Copyright © 2018 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_App_AppDistributionManager.h"

namespace NMib::NCloud::NAppDistributionManager
{
	bool CDistributionSettings::operator == (CDistributionSettings const &_Right) const
	{
		return fg_TupleReferences(m_VersionManagerApplication, m_RenameTemplate, m_BranchWildcards, m_Tags, m_Platforms, m_DeployDestinations)
			== fg_TupleReferences(_Right.m_VersionManagerApplication, _Right.m_RenameTemplate, _Right.m_BranchWildcards, _Right.m_Tags, _Right.m_Platforms, _Right.m_DeployDestinations)
		;
	}

	TCContinuation<uint32> CAppDistributionManagerActor::fp_CommandLine_DistributionChangeSettings(CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		auto Auditor = f_Auditor();

		CStr Name = _Params["Distribution"].f_String();

		if (!fg_IsValidHostname(Name))
			return Auditor.f_Exception("'{}' is not a valid distribution name"_f << Name);

		if (!mp_Distributions.f_FindEqual(Name))
			return Auditor.f_Exception("Distribution '{}' does not exist"_f << Name);

		auto &Distribution = mp_Distributions[Name];

		CDistributionSettings Settings = Distribution.m_Settings;

		try
		{
			fp_ParseSettings(_Params, Settings);
		}
		catch (CException const &_Exception)
		{
			return Auditor.f_Exception(_Exception.f_GetErrorStr());
		}

		if (Distribution.m_Settings == Settings)
			return Auditor.f_Exception("No setting changed");

		Distribution.m_Settings = fg_Move(Settings);

		fp_SaveState(Distribution);

		TCContinuation<uint32> Continuation;
		mp_State.m_StateDatabase.f_Save() > Continuation % "[Change distribution settings] Failed to save state" % Auditor / [Continuation, Name, Auditor]() mutable
			{
				Auditor.f_Info("Changed distribution settings '{}'"_f << Name);
				Continuation.f_SetResult();
			}
		;

		fp_AutoUpdate_Update();

		return Continuation;
	}
}
