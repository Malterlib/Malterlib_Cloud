// Copyright © 2018 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
#include "Malterlib_Cloud_App_AppDistributionManager.h"

namespace NMib::NCloud::NAppDistributionManager
{
	CStr CAppDistributionManagerActor::fsp_DeployDestinationToString(EDeployDestination _Type)
	{
		switch (_Type)
		{
			case EDeployDestination_FileSystem: return "FileSystem";
		}
		DMibNeverGetHere;
		return "";
	}

	auto CAppDistributionManagerActor::fsp_DeployDestinationFromString(CStr const &_Type) -> EDeployDestination
	{
		if (_Type == "FileSystem")
			return EDeployDestination_FileSystem;
		else
			DMibError("Invalid deploy destination: {}"_f << _Type);
	}

	void CAppDistributionManagerActor::fp_ParseSettings(CEJSON const &_Params, CDistributionSettings &o_Settings)
	{
		if (auto pValue = _Params.f_GetMember("VersionManagerApplication"))
		{
			if (!CVersionManager::fs_IsValidApplicationName(pValue->f_String()))
				DMibError("'{}' is not a valid version manager application name"_f << pValue->f_String());
			o_Settings.m_VersionManagerApplication = pValue->f_String();
		}

		if (auto pValue = _Params.f_GetMember("Tags"))
		{
			for (auto &Tag : pValue->f_Array())
			{
				if (!CVersionManager::fs_IsValidTag(Tag.f_String()))
					DMibError("'{}' is not a valid tag"_f << pValue->f_String());
				o_Settings.m_Tags[Tag.f_String()];
			}
		}

		if (auto pValue = _Params.f_GetMember("Platforms"))
		{
			for (auto &Platform : pValue->f_Array())
			{
				if (!CVersionManager::fs_IsValidPlatform(Platform.f_String()))
					DMibError("'{}' is not a valid platform"_f << pValue->f_String());
				o_Settings.m_Platforms[Platform.f_String()];
			}
		}

		if (auto pValue = _Params.f_GetMember("BranchWildcards"))
		{
			for (auto &Branch : pValue->f_Array())
			{
				if (!CVersionManager::fs_IsValidBranch(Branch.f_String(), true))
					DMibError("'{}' is not a valid branch"_f << pValue->f_String());
				o_Settings.m_BranchWildcards[Branch.f_String()];
			}
		}

		if (auto pValue = _Params.f_GetMember("RenameTemplate"))
			o_Settings.m_RenameTemplate = pValue->f_String();

		if (auto pValue = _Params.f_GetMember("DeployDestinations"))
		{
			for (auto &Type : pValue->f_Array())
				o_Settings.m_DeployDestinations[fsp_DeployDestinationFromString(Type.f_String())];
		}
	}

	CEJSON CAppDistributionManagerActor::fp_SaveSettings(CDistributionSettings const &_Settings)
	{
		CEJSON Distribution;

		Distribution["VersionManagerApplication"] = _Settings.m_VersionManagerApplication;
		Distribution["RenameTemplate"] = _Settings.m_RenameTemplate;
		{
			auto &BranchWildcards = Distribution["BranchWildcards"];
			BranchWildcards.f_Array();
			for (auto &Wildcard : _Settings.m_BranchWildcards)
				BranchWildcards.f_Insert(Wildcard);
		}
		{
			auto &Tags = Distribution["Tags"];
			Tags.f_Array();
			for (auto &Tag : _Settings.m_Tags)
				Tags.f_Insert(Tag);
		}
		{
			auto &Platforms = Distribution["Platforms"];
			Platforms.f_Array();
			for (auto &Platform : _Settings.m_Platforms)
				Platforms.f_Insert(Platform);
		}
		{
			auto &DeployDestinations = Distribution["DeployDestinations"];
			DeployDestinations.f_Array();
			for (auto &Type : _Settings.m_DeployDestinations)
				DeployDestinations.f_Insert(fsp_DeployDestinationToString(Type));
		}

		return Distribution;
	}

	void CAppDistributionManagerActor::fp_SaveState(CDistribution const &_Distribution)
	{
		auto &Distributions = mp_State.m_StateDatabase.m_Data["Distributions"];

		Distributions.f_RemoveMember(_Distribution.f_GetName());
		auto &Distribution = Distributions[_Distribution.f_GetName()];
		Distribution["Settings"] = fp_SaveSettings(_Distribution.m_Settings);
		{
			auto &DeployedVersionsJSON = Distribution["DeployedVersions"];
			DeployedVersionsJSON.f_Array();
			for (auto &VersionInfo : _Distribution.m_DeployedVersions)
			{
				auto &VersionID = _Distribution.m_DeployedVersions.fs_GetKey(VersionInfo);
				auto &DistributedVersionJSON = DeployedVersionsJSON.f_Insert();
				DistributedVersionJSON["Version"] = VersionID.f_ToJSON();
				DistributedVersionJSON["Time"] = VersionInfo.m_Time;
				DistributedVersionJSON["RetrySequence"] = VersionInfo.m_RetrySequence;
			}
		}
	}

	TCFuture<void> CAppDistributionManagerActor::fp_ReadState()
	{
		return TCFuture<void>::fs_RunProtected() / [&]
			{
				auto pDistributions = mp_State.m_StateDatabase.m_Data.f_GetMember("Distributions");
				if (!pDistributions)
					return;

				for (auto &DistributionObject : pDistributions->f_Object())
				{
					auto &Name = DistributionObject.f_Name();
					if (!fg_IsValidHostname(Name))
						DMibError("'{}' is not a valid distribution name"_f << Name);

					auto &DistributionJSON = DistributionObject.f_Value();

					CDistributionSettings Settings;
					fp_ParseSettings(DistributionJSON["Settings"], Settings);

					TCMap<CVersionManager::CVersionIDAndPlatform, CDeployedVersionInfo> DeployedVersions;
					if (auto *pDeployedVersions = DistributionJSON.f_GetMember("DeployedVersions"))
					{
						for (auto &DistributedVersionJSON : pDeployedVersions->f_Array())
						{
							auto Version = CVersionManager::CVersionIDAndPlatform::fs_FromJSON(DistributedVersionJSON["Version"]);
							DeployedVersions[Version].m_Time = DistributedVersionJSON["Time"].f_Date();
							DeployedVersions[Version].m_RetrySequence = DistributedVersionJSON["RetrySequence"].f_Integer();
						}
					}

					auto &Distribution = mp_Distributions[Name];
					Distribution.m_Settings = fg_Move(Settings);
					Distribution.m_DeployedVersions = fg_Move(DeployedVersions);
				}
			}
		;
	}
}
