// Copyright © 2019 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

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
}
