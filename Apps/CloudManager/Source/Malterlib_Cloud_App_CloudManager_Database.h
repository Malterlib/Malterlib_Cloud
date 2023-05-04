// Copyright © 2019 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Database/DatabaseValue>

#include <Mib/Concurrency/DistributedAppSensorLocalDatabase>

namespace NMib::NCloud::NCloudManagerDatabase
{
	static constexpr uint32 gc_Version = ECloudManagerProtocolVersion_Current;

	struct CCloudManagerGlobalStateKey
	{
		template <typename tf_CStream>
		void f_FeedLexicographic(tf_CStream &_Stream) const;
		template <typename tf_CStream>
		void f_ConsumeLexicographic(tf_CStream &_Stream);

		CEJSON f_ToJson() const;

		static CStr const mc_Prefix;

		CStr m_Prefix = mc_Prefix;
	};

	struct CCloudManagerGlobalStateValue
	{
		template <typename tf_CStream>
		void f_Stream(tf_CStream &_Stream);

		CEJSON f_ToJson() const;

		TCMap<CStr, CStr> m_SensorProblemsSlackThread;
		NTime::CTime m_LastSeenLogTimestamp;
	};

	struct CAppManagerKey
	{
		template <typename tf_CStream>
		void f_FeedLexicographic(tf_CStream &_Stream) const;
		template <typename tf_CStream>
		void f_ConsumeLexicographic(tf_CStream &_Stream);

		CEJSON f_ToJson() const;

		static CStr const mc_Prefix;

		CStr m_Prefix = mc_Prefix;
		CStr m_HostID;
	};

	struct CAppManagerValue
	{
		template <typename tf_CStream>
		void f_Stream(tf_CStream &_Stream);

		CEJSON f_ToJson() const;

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

		CEJSON f_ToJson() const;

		static CStr const mc_Prefix;

		CStr m_Prefix = mc_Prefix;
		CStr m_AppManagerHostID;
		CStr m_Application;
	};

	struct CApplicationValue
	{
		template <typename tf_CStream>
		void f_Stream(tf_CStream &_Stream);

		CEJSON f_ToJson() const;

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

		CEJSON f_ToJson() const;

		fp64 m_Time = 0.0;
	};

	struct CApplicationUpdateStateValue
	{
		template <typename tf_CStream>
		void f_Stream(tf_CStream &_Stream);

		CEJSON f_ToJson() const;

		CStr m_LastUpdateID;
		TCMap<CStr, CStr> m_SlackTimestamps;
		TCMap<CAppManagerInterface::EUpdateStage, CApplicationUpdateStateStage> m_Stages;
		CAppManagerInterface::CUpdateNotification m_LastNotification;
		uint64 m_LastUpdateSequence = 0;
		bool m_bDeferred = false;
	};

	struct CSensorNotificationStateKey : public NSensorStoreLocalDatabase::CSensorKey
	{
		template <typename tf_CStream>
		void f_FeedLexicographic(tf_CStream &_Stream) const;
		template <typename tf_CStream>
		void f_ConsumeLexicographic(tf_CStream &_Stream);

		CSensorKey f_SensorKey() const &;
		CSensorKey f_SensorKey() &&;

		static NStr::CStr const mc_Prefix;
	};

	struct CSensorNotificationStateNotificationStatus
	{
		template <typename tf_CStream>
		void f_Stream(tf_CStream &_Stream);

		CEJSON f_ToJson() const;

		CDistributedAppSensorReporter::EStatusSeverity m_Severity = CDistributedAppSensorReporter::EStatusSeverity_Info;
		CStr m_Message;
	};

	struct CSensorNotificationStateNotification
	{
		template <typename tf_CStream>
		void f_Stream(tf_CStream &_Stream);

		CEJSON f_ToJson() const;

		CSensorNotificationStateNotificationStatus m_Status;
		CSensorNotificationStateNotificationStatus m_OutdatedStatus;
		CSensorNotificationStateNotificationStatus m_CriticalityStatus;
	};

	struct CSensorNotificationStateValue
	{
		template <typename tf_CStream>
		void f_Stream(tf_CStream &_Stream);

		CEJSON f_ToJson() const;

		CSensorNotificationStateNotification m_LastNotification;
		fp64 m_TimeInProblemState = 0.0;
		bool m_bInProblemState = false;
		bool m_bSentAlert = false;
	};

	struct CExpectedOsVersionKey
	{
		template <typename tf_CStream>
		void f_FeedLexicographic(tf_CStream &_Stream) const;
		template <typename tf_CStream>
		void f_ConsumeLexicographic(tf_CStream &_Stream);

		CEJSON f_ToJson() const;

		static CStr const mc_Prefix;

		CStr m_Prefix = mc_Prefix;
		CStr m_OsName;
		CCloudManager::CCurrentVersion m_CurrentVersion;
	};

	struct CExpectedOsVersionValue
	{
		template <typename tf_CStream>
		void f_Stream(tf_CStream &_Stream);

		CEJSON f_ToJson() const;

		CCloudManager::CExpectedVersionRange m_ExpectedVersionRange;
	};

	template <typename tf_CKey>
	tf_CKey fg_GetSensorDatabaseKey(CDistributedAppSensorReporter::CSensorInfoKey const &_SensorInfoKey);

	template <typename tf_CKey>
	tf_CKey fg_GetLogDatabaseKey(CDistributedAppLogReporter::CLogInfoKey const &_LogInfoKey);
}

#include "Malterlib_Cloud_App_CloudManager_Database.hpp"
