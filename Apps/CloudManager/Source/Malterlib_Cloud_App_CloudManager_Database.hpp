// Copyright © 2019 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

namespace NMib::NCloud::NCloudManagerDatabase
{
	template <typename tf_CStream>
	void CCloudManagerGlobalStateKey::f_FeedLexicographic(tf_CStream &_Stream) const
	{
		CDatabaseValue::fs_FeedLexicographic(_Stream, m_Prefix);
	}

	template <typename tf_CStream>
	void CCloudManagerGlobalStateKey::f_ConsumeLexicographic(tf_CStream &_Stream)
	{
		CDatabaseValue::fs_ConsumeLexicographic(_Stream, m_Prefix);
	}

	template <typename tf_CStream>
	void CCloudManagerGlobalStateValue::f_Stream(tf_CStream &_Stream)
	{
		uint32 Version = gc_Version;
		_Stream % Version;
		DBinaryStreamVersion(_Stream, Version);
		_Stream % m_SensorProblemsSlackThread;

		if (_Stream.f_GetVersion() >= ECloudManagerProtocolVersion_AddLastSeenLogTimestamp)
			_Stream % m_LastSeenLogTimestamp;
	}

	template <typename tf_CStream>
	void CAppManagerKey::f_FeedLexicographic(tf_CStream &_Stream) const
	{
		CDatabaseValue::fs_FeedLexicographic(_Stream, m_Prefix);
		CDatabaseValue::fs_FeedLexicographic(_Stream, m_HostID);
	}

	template <typename tf_CStream>
	void CAppManagerKey::f_ConsumeLexicographic(tf_CStream &_Stream)
	{
		CDatabaseValue::fs_ConsumeLexicographic(_Stream, m_Prefix);
		CDatabaseValue::fs_ConsumeLexicographic(_Stream, m_HostID);
	}

	template <typename tf_CStream>
	void CAppManagerValue::f_Stream(tf_CStream &_Stream)
	{
		uint32 Version = gc_Version;
		_Stream % Version;
		DBinaryStreamVersion(_Stream, Version);
		_Stream % m_Info;
		_Stream % m_LastSeen;
		_Stream % m_LastConnectionError;
		_Stream % m_LastConnectionErrorTime;
		_Stream % m_bActive;
		if (Version >= ECloudManagerProtocolVersion_AddOtherErrors)
			_Stream % m_OtherErrors;
		if (Version >= ECloudManagerProtocolVersion_AddLastSeenUpdateNotificationSequence)
			_Stream % m_LastSeenUpdateNotificationSequence;
		if (Version >= ECloudManagerProtocolVersion_SupportAppManagerCloudManagerInterface)
			_Stream % m_PauseReportingFor;
	}

	template <typename tf_CStream>
	void CApplicationKey::f_FeedLexicographic(tf_CStream &_Stream) const
	{
		CDatabaseValue::fs_FeedLexicographic(_Stream, m_Prefix);
		CDatabaseValue::fs_FeedLexicographic(_Stream, m_AppManagerHostID);
		CDatabaseValue::fs_FeedLexicographic(_Stream, m_Application);
	}

	template <typename tf_CStream>
	void CApplicationKey::f_ConsumeLexicographic(tf_CStream &_Stream)
	{
		CDatabaseValue::fs_ConsumeLexicographic(_Stream, m_Prefix);
		CDatabaseValue::fs_ConsumeLexicographic(_Stream, m_AppManagerHostID);
		CDatabaseValue::fs_ConsumeLexicographic(_Stream, m_Application);
	}

	template <typename tf_CStream>
	void CApplicationValue::f_Stream(tf_CStream &_Stream)
	{
		uint32 Version = gc_Version;
		_Stream % Version;
		DBinaryStreamVersion(_Stream, Version);
		{
			uint32 AppManagerInterfaceVersion = CCloudManager::fs_ProtocolVersion_CloudManagerToAppManager(Version);

			DMibBinaryStreamVersion(_Stream, AppManagerInterfaceVersion);
			_Stream % m_ApplicationInfo;
		}
	}

	template <typename tf_CStream>
	void CApplicationUpdateStateStage::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Time;
	}

	template <typename tf_CStream>
	void CApplicationUpdateStateValue::f_Stream(tf_CStream &_Stream)
	{
		uint32 Version = gc_Version;
		_Stream % Version;
		DBinaryStreamVersion(_Stream, Version);

		_Stream % m_LastUpdateID;
		_Stream % m_LastUpdateSequence;
		_Stream % m_SlackTimestamps;
		_Stream % m_Stages;
		if (_Stream.f_GetVersion() >= ECloudManagerProtocolVersion_SupportDeferredUpdateNotification)
		{
			_Stream % m_bDeferred;
			{
				uint32 AppManagerInterfaceVersion = CCloudManager::fs_ProtocolVersion_CloudManagerToAppManager(_Stream.f_GetVersion());

				DMibBinaryStreamVersion(_Stream, AppManagerInterfaceVersion);
				_Stream % m_LastNotification;
			}
		}
	}

	template <typename tf_CKey>
	tf_CKey fg_GetSensorDatabaseKey(CDistributedAppSensorReporter::CSensorInfoKey const &_SensorInfoKey)
	{
		tf_CKey Key;
		Key.m_Prefix = tf_CKey::mc_Prefix;
		Key.m_HostID = _SensorInfoKey.m_HostID;
		if (_SensorInfoKey.m_Scope.f_IsOfType<CDistributedAppSensorReporter::CSensorScope_Application>())
			Key.m_ApplicationName = _SensorInfoKey.m_Scope.f_GetAsType<CDistributedAppSensorReporter::CSensorScope_Application>().m_ApplicationName;
		Key.m_Identifier = _SensorInfoKey.m_Identifier;
		Key.m_IdentifierScope = _SensorInfoKey.m_IdentifierScope;

		return Key;
	}

	template <typename tf_CKey>
	tf_CKey fg_GetLogDatabaseKey(CDistributedAppLogReporter::CLogInfoKey const &_LogInfoKey)
	{
		tf_CKey Key;
		Key.m_Prefix = tf_CKey::mc_Prefix;
		Key.m_HostID = _LogInfoKey.m_HostID;
		if (_LogInfoKey.m_Scope.f_IsOfType<CDistributedAppLogReporter::CLogScope_Application>())
			Key.m_ApplicationName = _LogInfoKey.m_Scope.f_GetAsType<CDistributedAppLogReporter::CLogScope_Application>().m_ApplicationName;
		Key.m_Identifier = _LogInfoKey.m_Identifier;
		Key.m_IdentifierScope = _LogInfoKey.m_IdentifierScope;

		return Key;
	}

	template <typename tf_CStream>
	void CSensorNotificationStateKey::f_FeedLexicographic(tf_CStream &_Stream) const
	{
		CSensorKey::f_FeedLexicographic(_Stream);
	}

	template <typename tf_CStream>
	void CSensorNotificationStateKey::f_ConsumeLexicographic(tf_CStream &_Stream)
	{
		CSensorKey::f_ConsumeLexicographic(_Stream);
	}

	template <typename tf_CStream>
	void CSensorNotificationStateNotificationStatus::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Severity;
		_Stream % m_Message;
	}

	template <typename tf_CStream>
	void CSensorNotificationStateNotification::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Status;
		_Stream % m_OutdatedStatus;
		_Stream % m_CriticalityStatus;
	}

	template <typename tf_CStream>
	void CSensorNotificationStateValue::f_Stream(tf_CStream &_Stream)
	{
		uint32 Version = gc_Version;
		_Stream % Version;
		DBinaryStreamVersion(_Stream, Version);

		_Stream % m_LastNotification;
		_Stream % m_TimeInProblemState;
		_Stream % m_bInProblemState;
		_Stream % m_bSentAlert;
	}

	template <typename tf_CStream>
	void CExpectedOsVersionKey::f_FeedLexicographic(tf_CStream &_Stream) const
	{
		CDatabaseValue::fs_FeedLexicographic(_Stream, m_Prefix);
		CDatabaseValue::fs_FeedLexicographic(_Stream, m_OsName);
		CDatabaseValue::fs_FeedLexicographic(_Stream, m_CurrentVersion);
	}

	template <typename tf_CStream>
	void CExpectedOsVersionKey::f_ConsumeLexicographic(tf_CStream &_Stream)
	{
		CDatabaseValue::fs_ConsumeLexicographic(_Stream, m_Prefix);
		CDatabaseValue::fs_ConsumeLexicographic(_Stream, m_OsName);
		CDatabaseValue::fs_ConsumeLexicographic(_Stream, m_CurrentVersion);
	}

	template <typename tf_CStream>
	void CExpectedOsVersionValue::f_Stream(tf_CStream &_Stream)
	{
		uint32 Version = gc_Version;
		_Stream % Version;
		DBinaryStreamVersion(_Stream, Version);

		_Stream % m_ExpectedVersionRange;
	}
}
