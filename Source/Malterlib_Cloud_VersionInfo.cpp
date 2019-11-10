// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

#include "Malterlib_Cloud_VersionInfo.h"
#include "Malterlib_Cloud_VersionManager.h"

namespace NMib::NCloud
{
	constinit NStorage::TCAggregate<CCloudVersionInfo> g_CloudVersion = {DAggregateInit};

	bool CCloudVersion::operator == (CCloudVersion const &_Right) const
	{
		return NStorage::fg_TupleReferences(m_Branch, m_Major, m_Minor, m_Revision) 
			== NStorage::fg_TupleReferences(_Right.m_Branch, _Right.m_Major, _Right.m_Minor, _Right.m_Revision)
		;
	}
	
	bool CCloudVersion::operator < (CCloudVersion const &_Right) const
	{
		return NStorage::fg_TupleReferences(m_Branch, m_Major, m_Minor, m_Revision) 
			< NStorage::fg_TupleReferences(_Right.m_Branch, _Right.m_Major, _Right.m_Minor, _Right.m_Revision)
		;
	}
	
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
}
