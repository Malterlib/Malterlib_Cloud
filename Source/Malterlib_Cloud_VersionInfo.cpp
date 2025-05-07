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
		auto const Json = NEncoding::CEJsonSorted::fs_FromString(_String);
		CCloudVersionInfo VersionInfo;
		
		NStr::CStr Error;
		CVersionManager::CVersionID VersionID;
		CVersionManager::fs_IsValidVersionIdentifier(Json["Version"].f_String(), Error, &VersionID);

		VersionInfo.m_Version = VersionID;
		VersionInfo.m_Platform = Json["Platform"].f_String();
		VersionInfo.m_Configuration = Json["Configuration"].f_String();
		VersionInfo.m_Application = Json["Application"].f_String();
		VersionInfo.m_ExtraInfo = Json["ExtraInfo"];
		
		return VersionInfo;
	}

	NEncoding::CEJsonSorted CCloudVersion::f_ToJson() const
	{
		using namespace NEncoding;
		
		CEJsonSorted Return;
		Return["Branch"] = fg_ToJson(m_Branch);
		Return["Major"] = fg_ToJson(m_Major);
		Return["Minor"] = fg_ToJson(m_Minor);
		Return["Revision"] = fg_ToJson(m_Revision);
		return Return;
	}
}
