// Copyright © 2019 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

namespace NMib::NCloud::NCloudManagerDatabase
{
	template <typename tf_CStream>
	void CAppManagerKey::f_FeedLexicograpic(tf_CStream &_Stream) const
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
		if (Version >= 0x106)
			_Stream % m_OtherErrors;
	}

	template <typename tf_CStream>
	void CApplicationKey::f_FeedLexicograpic(tf_CStream &_Stream) const
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
			uint32 AppManagerInterfaceVersion = 0;
			if (Version >= 0x104)
				AppManagerInterfaceVersion = 0x110;

			if (Version >= 0x109)
				AppManagerInterfaceVersion = 0x114;

			if (Version >= 0x110)
				AppManagerInterfaceVersion = 0x116;

			static_assert(CAppManagerInterface::EProtocolVersion == 0x116, "Add a new version mapping if m_ApplicationInfo streaming changed");

			DMibBinaryStreamVersion(_Stream, AppManagerInterfaceVersion);
			_Stream % m_ApplicationInfo;
		}
	}
}
