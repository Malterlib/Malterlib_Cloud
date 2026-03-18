// Copyright © 2018 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_App_AppDistributionManager.h"

namespace NMib::NCloud::NAppDistributionManager
{
	bool CDistributionSettings::operator == (CDistributionSettings const &_Right) const noexcept
	{
		return fg_TupleReferences(m_VersionManagerApplication, m_RenameTemplate, m_BranchWildcards, m_Tags, m_Platforms, m_DeployDestinations)
			== fg_TupleReferences(_Right.m_VersionManagerApplication, _Right.m_RenameTemplate, _Right.m_BranchWildcards, _Right.m_Tags, _Right.m_Platforms, _Right.m_DeployDestinations)
		;
	}

	TCFuture<uint32> CAppDistributionManagerActor::fp_CommandLine_DistributionChangeSettings(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		auto Auditor = f_Auditor();

		CStr Name = _Params["Distribution"].f_String();

		if (!fg_IsValidHostname(Name))
			co_return Auditor.f_Exception("'{}' is not a valid distribution name"_f << Name);

		if (!mp_Distributions.f_FindEqual(Name))
			co_return Auditor.f_Exception("Distribution '{}' does not exist"_f << Name);

		auto &Distribution = mp_Distributions[Name];

		CDistributionSettings Settings = Distribution.m_Settings;

		{
			auto CaptureScope = co_await (g_CaptureExceptions % Auditor);
			fp_ParseSettings(_Params, Settings);
		}

		if (Distribution.m_Settings == Settings)
			co_return Auditor.f_Exception("No setting changed");

		Distribution.m_Settings = fg_Move(Settings);

		fp_SaveState(Distribution);

		fp_AutoUpdate_Update();

		co_await (mp_State.m_StateDatabase.f_Save() % "[Change distribution settings] Failed to save state" % Auditor);

		Auditor.f_Info("Changed distribution settings '{}'"_f << Name);

		co_return 0;
	}
}
