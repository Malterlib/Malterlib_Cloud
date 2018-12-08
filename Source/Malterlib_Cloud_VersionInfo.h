// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Encoding/EJSON>

namespace NMib::NCloud
{
	struct CCloudVersion
	{
		NStr::CStr m_Branch;
		uint32 m_Major = 0;
		uint32 m_Minor = 0;
		uint32 m_Revision = 0;

		bool operator == (CCloudVersion const &_Right) const;
		bool operator < (CCloudVersion const &_Right) const;
	};
	
	struct CCloudVersionInfo
	{
		NStr::CStr m_Application;
		CCloudVersion m_Version;
		NStr::CStr m_Platform;
		NStr::CStr m_Configuration;
		NMib::NEncoding::CEJSON m_ExtraInfo;
	};
	
	CCloudVersionInfo fg_ParseVersionInfo(NStr::CStr const &_String);
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NCloud;
#endif
