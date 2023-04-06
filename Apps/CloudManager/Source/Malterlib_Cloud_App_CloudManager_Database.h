// Copyright © 2019 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Database/DatabaseValue>

namespace NMib::NCloud::NCloudManagerDatabase
{
	static constexpr uint32 gc_Version = ECloudManagerProtocolVersion_Current;

	struct CAppManagerKey
	{
		template <typename tf_CStream>
		void f_FeedLexicographic(tf_CStream &_Stream) const;
		template <typename tf_CStream>
		void f_ConsumeLexicographic(tf_CStream &_Stream);

		static CStr const mc_Prefix;

		CStr m_Prefix = mc_Prefix;
		CStr m_HostID;
	};

	struct CAppManagerValue
	{
		template <typename tf_CStream>
		void f_Stream(tf_CStream &_Stream);

		CCloudManager::CAppManagerInfo m_Info;
		CTime m_LastSeen;
		CStr m_LastConnectionError;
		CTime m_LastConnectionErrorTime;
		TCMap<CStr, CStr> m_OtherErrors;
		uint64 m_LastSeenUpdateNotificationSequence = TCLimitsInt<uint64>::mc_Max;
		bool m_bActive = false;
	};

	struct CApplicationKey
	{
		template <typename tf_CStream>
		void f_FeedLexicographic(tf_CStream &_Stream) const;
		template <typename tf_CStream>
		void f_ConsumeLexicographic(tf_CStream &_Stream);

		static CStr const mc_Prefix;

		CStr m_Prefix = mc_Prefix;
		CStr m_AppManagerHostID;
		CStr m_Application;
	};

	struct CApplicationValue
	{
		template <typename tf_CStream>
		void f_Stream(tf_CStream &_Stream);

		CAppManagerInterface::CApplicationInfo m_ApplicationInfo;
	};

	struct CApplicationUpdateStateKey : public CApplicationKey
	{
		CApplicationUpdateStateKey()
			: CApplicationKey{.m_Prefix = mc_Prefix}
		{
		}

		static CStr const mc_Prefix;
	};

	struct CApplicationUpdateStateStage
	{
		template <typename tf_CStream>
		void f_Stream(tf_CStream &_Stream);

		fp64 m_Time = 0.0;
	};

	struct CApplicationUpdateStateValue
	{
		template <typename tf_CStream>
		void f_Stream(tf_CStream &_Stream);

		CStr m_LastUpdateID;
		TCMap<CStr, CStr> m_SlackTimestamps;
		TCMap<CAppManagerInterface::EUpdateStage, CApplicationUpdateStateStage> m_Stages;
		uint64 m_LastUpdateSequence = 0;
	};
}

#include "Malterlib_Cloud_App_CloudManager_Database.hpp"
