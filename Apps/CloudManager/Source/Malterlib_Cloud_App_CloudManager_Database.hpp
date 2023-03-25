// Copyright © 2019 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

namespace NMib::NCloud::NCloudManagerDatabase
{
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
}
