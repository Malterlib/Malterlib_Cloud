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
	}

	inline auto CCloudManager::CAppManagerInfo::f_Tuple() const
	{
		return NStorage::fg_TupleReferences(m_Environment, m_HostName, m_ProgramDirectory);
	}

	template <typename tf_CStream>
	void CCloudManager::CAppManagerDynamicInfo::f_Stream(tf_CStream &_Stream)
	{
		CCloudManager::CAppManagerInfo::f_Stream(_Stream);

		_Stream % m_LastSeen;
		_Stream % m_LastConnectionError;
		_Stream % m_LastConnectionErrorTime;
		_Stream % m_bActive;
	}
}
