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
}
