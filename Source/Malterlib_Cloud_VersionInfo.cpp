// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Encoding/ToJson>

#include "Malterlib_Cloud_VersionInfo.h"
#include "Malterlib_Cloud_VersionManager.h"

namespace NMib::NCloud
{
	constinit NStorage::TCAggregate<CCloudVersionInfo> g_CloudVersion = {DAggregateInit};

	CCloudVersionInfo fg_ParseVersionInfo(NStr::CStr const &_String)
	{
		auto const JSON = NEncoding::CEJSON::fs_FromString(_String);
		CCloudVersionInfo VersionInfo;
		
		NStr::CStr Error;
		CVersionManager::CVersionID VersionID;
		CVersionManager::fs_IsValidVersionIdentifier(JSON["Version"].f_String(), Error, &VersionID);

		VersionInfo.m_Version = VersionID;
		VersionInfo.m_Platform = JSON["Platform"].f_String();
		VersionInfo.m_Configuration = JSON["Configuration"].f_String();
		VersionInfo.m_Application = JSON["Application"].f_String();
		VersionInfo.m_ExtraInfo = JSON["ExtraInfo"];
		
		return VersionInfo;
	}

	NEncoding::CEJSON CCloudVersion::f_ToJson() const
	{
		using namespace NEncoding;
		
		CEJSON Return;
		Return["Branch"] = fg_ToJson(m_Branch);
		Return["Major"] = fg_ToJson(m_Major);
		Return["Minor"] = fg_ToJson(m_Minor);
		Return["Revision"] = fg_ToJson(m_Revision);
		return Return;
	}
}
