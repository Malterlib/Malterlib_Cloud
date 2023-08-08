// Copyright © 2019 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Database/DatabaseValue>

namespace NMib::NCloud
{
	template <typename tf_CStream>
	void CCloudManager::CAppManagerInfo::f_Stream(tf_CStream &_Stream)
	{
		if (_Stream.f_GetVersion() >= ECloudManagerProtocolVersion_AddEnvironment)
			_Stream % m_Environment;

		_Stream % m_HostName;
		_Stream % m_ProgramDirectory;

		if (_Stream.f_GetVersion() >= ECloudManagerProtocolVersion_AddVersionInfo)
		{
			_Stream % m_Version;
			_Stream % m_Platform;
			_Stream % m_PlatformFamily;
		}
		if (_Stream.f_GetVersion() >= ECloudManagerProtocolVersion_AddVersionDate)
			_Stream % m_VersionDate;
	}

	template <typename tf_CStream>
	void CCloudManager::CAppManagerDynamicInfo::f_Stream(tf_CStream &_Stream)
	{
		CCloudManager::CAppManagerInfo::f_Stream(_Stream);

		_Stream % m_LastSeen;
		_Stream % m_LastConnectionError;
		_Stream % m_LastConnectionErrorTime;
		_Stream % m_bActive;
		if (_Stream.f_GetVersion() >= ECloudManagerProtocolVersion_AddOtherErrors)
			_Stream % m_OtherErrors;
	}

	template <typename tf_CStream>
	void CCloudManager::CApplicationKey::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_AppManagerID;
		_Stream % m_Name;
	}

	template <typename tf_CStream>
	void CCloudManager::CApplicationInfo::f_Stream(tf_CStream &_Stream)
	{
		{
			DMibBinaryStreamVersion(_Stream, fs_ProtocolVersion_CloudManagerToAppManager(_Stream.f_GetVersion()));
			_Stream % m_ApplicationInfo;
		}
	}

	template <typename tf_CStream>
	void CCloudManager::CVersion::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Major;
		_Stream % m_Minor;
		_Stream % m_Revision;
	}

	template <typename tf_CStr>
	void CCloudManager::CVersion::f_Format(tf_CStr &o_Str) const
	{
		o_Str += typename tf_CStr::CFormat("{}.{}.{}") << m_Major << m_Minor << m_Revision;
	}

	template <typename tf_CStream>
	void CCloudManager::CCurrentVersion::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Major;
		_Stream % m_Minor;
	}

	template <typename tf_CStream>
	void CCloudManager::CCurrentVersion::f_FeedLexicographic(tf_CStream &_Stream) const
	{
		NDatabase::CDatabaseValue::fs_FeedLexicographic(_Stream, m_Major);
		NDatabase::CDatabaseValue::fs_FeedLexicographic(_Stream, m_Minor);
	}

	template <typename tf_CStream>
	void CCloudManager::CCurrentVersion::f_ConsumeLexicographic(tf_CStream &_Stream)
	{
		NDatabase::CDatabaseValue::fs_ConsumeLexicographic(_Stream, m_Major);
		NDatabase::CDatabaseValue::fs_ConsumeLexicographic(_Stream, m_Minor);
	}

	template <typename tf_CStr>
	void CCloudManager::CCurrentVersion::f_Format(tf_CStr &o_Str) const
	{
		o_Str += typename tf_CStr::CFormat("{}.{}") << m_Major << m_Minor;
	}

	template <typename tf_CStr>
	void CCloudManager::CExpectedVersionRange::f_Format(tf_CStr &o_Str) const
	{
		o_Str += typename tf_CStr::CFormat("Min: {} Max: {}") << m_Min << m_Max;
	}

	template <typename tf_CStream>
	void CCloudManager::CExpectedVersionRange::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Min;
		_Stream % m_Max;
	}

	template <typename tf_CStream>
	void CCloudManager::CExpectedVersions::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Versions;
	}

	template <typename tf_CStream>
	void CCloudManager::CSubscribeExpectedOsVersions::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_OsName;
		_Stream % fg_Move(m_fVersionRangeChanged);
	}

	template <typename tf_CStream>
	void CCloudManager::CRemoveSensor::f_Stream(tf_CStream &_Stream)
	{
		if (_Stream.f_GetVersion() >= ECloudManagerProtocolVersion_SupportFilterInRemoveSensorAndLog)
			_Stream % m_Filter;
		else
		{
			if constexpr (tf_CStream::mc_bConsume)
			{
				NConcurrency::CDistributedAppSensorReporter::CSensorInfoKey Key;
				_Stream >> Key;
				
				m_Filter.m_HostID = Key.m_HostID;
				m_Filter.m_Scope = Key.m_Scope;
				m_Filter.m_Identifier = Key.m_Identifier;
				m_Filter.m_IdentifierScope = Key.m_IdentifierScope;
			}
			else
			{
				NConcurrency::CDistributedAppSensorReporter::CSensorInfoKey Key;

				if (m_Filter.m_HostID)
					Key.m_HostID = *m_Filter.m_HostID;

				if (m_Filter.m_Scope)
					Key.m_Scope = *m_Filter.m_Scope;

				if (m_Filter.m_Identifier)
					Key.m_Identifier = *m_Filter.m_Identifier;

				if (m_Filter.m_IdentifierScope)
					Key.m_IdentifierScope = *m_Filter.m_IdentifierScope;

				_Stream << Key;
			}
		}
	}

	template <typename tf_CStream>
	void CCloudManager::CRemoveLog::f_Stream(tf_CStream &_Stream)
	{
		if (_Stream.f_GetVersion() >= ECloudManagerProtocolVersion_SupportFilterInRemoveSensorAndLog)
			_Stream % m_Filter;
		else
		{
			if constexpr (tf_CStream::mc_bConsume)
			{
				NConcurrency::CDistributedAppLogReporter::CLogInfoKey Key;
				_Stream >> Key;

				m_Filter.m_HostID = Key.m_HostID;
				m_Filter.m_Scope = Key.m_Scope;
				m_Filter.m_Identifier = Key.m_Identifier;
				m_Filter.m_IdentifierScope = Key.m_IdentifierScope;
			}
			else
			{
				NConcurrency::CDistributedAppLogReporter::CLogInfoKey Key;

				if (m_Filter.m_HostID)
					Key.m_HostID = *m_Filter.m_HostID;

				if (m_Filter.m_Scope)
					Key.m_Scope = *m_Filter.m_Scope;

				if (m_Filter.m_Identifier)
					Key.m_Identifier = *m_Filter.m_Identifier;

				if (m_Filter.m_IdentifierScope)
					Key.m_IdentifierScope = *m_Filter.m_IdentifierScope;

				_Stream << Key;
			}
		}
	}
}
