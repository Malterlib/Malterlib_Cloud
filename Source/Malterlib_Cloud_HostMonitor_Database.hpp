// Copyright © 2023 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

namespace NMib::NCloud::NHostMonitorDatabase
{
	template <typename tf_CStream>
	void CConfigFileHistoryEntryKey::f_FeedLexicographic(tf_CStream &_Stream) const
	{
		NDatabase::CDatabaseValue::fs_FeedLexicographic(_Stream, m_Prefix);
		NDatabase::CDatabaseValue::fs_FeedLexicographic(_Stream, m_VersionKey.m_FileName);
		NDatabase::CDatabaseValue::fs_FeedLexicographic(_Stream, m_VersionKey.m_Sequence);
	}

	template <typename tf_CStream>
	void CConfigFileHistoryEntryKey::f_ConsumeLexicographic(tf_CStream &_Stream)
	{
		NDatabase::CDatabaseValue::fs_ConsumeLexicographic(_Stream, m_Prefix);
		NDatabase::CDatabaseValue::fs_ConsumeLexicographic(_Stream, m_VersionKey.m_FileName);
		NDatabase::CDatabaseValue::fs_ConsumeLexicographic(_Stream, m_VersionKey.m_Sequence);
	}

	template <typename tf_CStream>
	void CConfigFileHistoryEntryValue::f_Stream(tf_CStream &_Stream)
	{
		uint32 Version = gc_Version;
		_Stream % Version;
		DMibBinaryStreamVersion(_Stream, Version);

		_Stream % m_Properties;
		_Stream % m_Contents;
	}
}
