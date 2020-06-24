// Copyright © 2019 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

namespace NMib::NCloud
{
	template <typename tf_CStream>
	void CCloudManager::CAppManagerInfo::f_Stream(tf_CStream &_Stream)
	{
		if (_Stream.f_GetVersion() >= 0x102)
			_Stream % m_Environment;

		_Stream % m_HostName;
		_Stream % m_ProgramDirectory;

		if (_Stream.f_GetVersion() >= 0x103)
		{
			_Stream % m_Version;
			_Stream % m_Platform;
			_Stream % m_PlatformFamily;
		}
		if (_Stream.f_GetVersion() >= 0x105)
			_Stream % m_VersionDate;
	}

	inline auto CCloudManager::CAppManagerInfo::f_Tuple() const
	{
		return NStorage::fg_TupleReferences(m_Environment, m_HostName, m_ProgramDirectory, m_VersionDate, m_Version, m_Platform, m_PlatformFamily);
	}

	template <typename tf_CStream>
	void CCloudManager::CAppManagerDynamicInfo::f_Stream(tf_CStream &_Stream)
	{
		CCloudManager::CAppManagerInfo::f_Stream(_Stream);

		_Stream % m_LastSeen;
		_Stream % m_LastConnectionError;
		_Stream % m_LastConnectionErrorTime;
		_Stream % m_bActive;
		if (_Stream.f_GetVersion() >= 0x106)
			_Stream % m_OtherErrors;
	}

	template <typename tf_CStream>
	void CCloudManager::CApplicationKey::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_AppManagerID;
		_Stream % m_Name;
	}

	inline auto CCloudManager::CApplicationKey::f_Tuple() const
	{
		return NStorage::fg_TupleReferences(m_AppManagerID, m_Name);
	}

	template <typename tf_CStream>
	void CCloudManager::CApplicationInfo::f_Stream(tf_CStream &_Stream)
	{
		uint32 AppManagerInterfaceVersion = 0x110;
		static_assert(CAppManagerInterface::EProtocolVersion == 0x116, "Add a new version mapping if streaming of m_ApplicationInfo changed");

		if (_Stream.f_GetVersion() >= 0x109)
			AppManagerInterfaceVersion = 0x114;

		if (_Stream.f_GetVersion() >= 0x110)
			AppManagerInterfaceVersion = 0x116;

		{
			DMibBinaryStreamVersion(_Stream, AppManagerInterfaceVersion);
			_Stream % m_ApplicationInfo;
		}
	}
}
