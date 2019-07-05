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

		template <typename tf_CStream>
		void f_Stream(tf_CStream &_Stream);

		template <typename tf_CStr>
		void f_Format(tf_CStr &o_String) const;

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

		template <typename tf_CStream>
		void f_Stream(tf_CStream &_Stream);
	};
	
	CCloudVersionInfo fg_ParseVersionInfo(NStr::CStr const &_String);
	extern NStorage::TCAggregate<CCloudVersionInfo> g_CloudVersion;
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NCloud;
#endif

#include "Malterlib_Cloud_VersionInfo.hpp"
