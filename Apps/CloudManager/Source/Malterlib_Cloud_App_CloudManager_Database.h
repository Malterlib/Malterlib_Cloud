// Copyright © 2019 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Database/DatabaseValue>

namespace NMib::NCloud::NCloudManagerDatabase
{
	static constexpr uint32 gc_Version = ECloudManagerProtocolVersion;

	struct CAppManagerKey
	{
		static constexpr ch8 mc_Prefix[] = "AppMgr";

		CStr m_Prefix;
		CStr m_HostID;

		template <typename tf_CStream>
		void f_FeedLexicographic(tf_CStream &_Stream) const;
		template <typename tf_CStream>
		void f_ConsumeLexicographic(tf_CStream &_Stream);
	};

	struct CAppManagerValue
	{
		CCloudManager::CAppManagerInfo m_Info;
		CTime m_LastSeen;
		CStr m_LastConnectionError;
		CTime m_LastConnectionErrorTime;
		TCMap<CStr, CStr> m_OtherErrors;
		bool m_bActive = false;

		template <typename tf_CStream>
		void f_Stream(tf_CStream &_Stream);
	};

	struct CApplicationKey
	{
		static constexpr ch8 mc_Prefix[] = "App";

		CStr m_Prefix;
		CStr m_AppManagerHostID;
		CStr m_Application;

		template <typename tf_CStream>
		void f_FeedLexicographic(tf_CStream &_Stream) const;
		template <typename tf_CStream>
		void f_ConsumeLexicographic(tf_CStream &_Stream);
	};

	struct CApplicationValue
	{
		CAppManagerInterface::CApplicationInfo m_ApplicationInfo;

		template <typename tf_CStream>
		void f_Stream(tf_CStream &_Stream);
	};
}

#include "Malterlib_Cloud_App_CloudManager_Database.hpp"
