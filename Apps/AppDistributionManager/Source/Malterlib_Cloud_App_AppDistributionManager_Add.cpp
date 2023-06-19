// Copyright © 2018 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
#include "Malterlib_Cloud_App_AppDistributionManager.h"

namespace NMib::NCloud::NAppDistributionManager
{
	TCFuture<uint32> CAppDistributionManagerActor::fp_CommandLine_DistributionAdd(CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		auto Auditor = f_Auditor();

		CDistributionSettings Settings;

		{
			auto CaptureScope = co_await (g_CaptureExceptions % Auditor);
			fp_ParseSettings(_Params, Settings);
		}

		CStr Name = _Params.f_GetMemberValue("Distribution", Settings.m_VersionManagerApplication).f_String();

		if (!fg_IsValidHostname(Name))
			co_return Auditor.f_Exception("'{}' is not a valid distribution name"_f << Name);

		if (mp_Distributions.f_FindEqual(Name))
			co_return Auditor.f_Exception("Distribution '{}' already exists"_f << Name);

		auto &Distribution = mp_Distributions[Name];

		Distribution.m_Settings = fg_Move(Settings);

		fp_SaveState(Distribution);

		fp_AutoUpdate_Update();

		co_await (mp_State.m_StateDatabase.f_Save() % "[Add distribution] Failed to save state" % Auditor);

		Auditor.f_Info("Added distribution '{}'"_f << Name);

		co_return 0;
	}
}
