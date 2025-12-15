// Copyright © 2025 Unbroken AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

namespace NMib::NCloud::NVersionManagerDatabase
{
	template <typename tf_CStream>
	void CVersionDatabaseKey::f_FeedLexicographic(tf_CStream &_Stream) const
	{
		NDatabase::CDatabaseValue::fs_FeedLexicographic(_Stream, m_Prefix);
		NDatabase::CDatabaseValue::fs_FeedLexicographic(_Stream, m_Application);
		NDatabase::CDatabaseValue::fs_FeedLexicographic(_Stream, m_VersionIDAndPlatform.m_VersionID.m_Branch);
		NDatabase::CDatabaseValue::fs_FeedLexicographic(_Stream, m_VersionIDAndPlatform.m_VersionID.m_Major);
		NDatabase::CDatabaseValue::fs_FeedLexicographic(_Stream, m_VersionIDAndPlatform.m_VersionID.m_Minor);
		NDatabase::CDatabaseValue::fs_FeedLexicographic(_Stream, m_VersionIDAndPlatform.m_VersionID.m_Revision);
		NDatabase::CDatabaseValue::fs_FeedLexicographic(_Stream, m_VersionIDAndPlatform.m_Platform);
	}

	template <typename tf_CStream>
	void CVersionDatabaseKey::f_ConsumeLexicographic(tf_CStream &_Stream)
	{
		NDatabase::CDatabaseValue::fs_ConsumeLexicographic(_Stream, m_Prefix);
		NDatabase::CDatabaseValue::fs_ConsumeLexicographic(_Stream, m_Application);
		NDatabase::CDatabaseValue::fs_ConsumeLexicographic(_Stream, m_VersionIDAndPlatform.m_VersionID.m_Branch);
		NDatabase::CDatabaseValue::fs_ConsumeLexicographic(_Stream, m_VersionIDAndPlatform.m_VersionID.m_Major);
		NDatabase::CDatabaseValue::fs_ConsumeLexicographic(_Stream, m_VersionIDAndPlatform.m_VersionID.m_Minor);
		NDatabase::CDatabaseValue::fs_ConsumeLexicographic(_Stream, m_VersionIDAndPlatform.m_VersionID.m_Revision);
		NDatabase::CDatabaseValue::fs_ConsumeLexicographic(_Stream, m_VersionIDAndPlatform.m_Platform);
	}

	template <typename tf_CStream>
	void CVersionDatabaseValue::f_Stream(tf_CStream &_Stream)
	{
		uint32 Version = gc_Version;
		_Stream % Version;
		DMibBinaryStreamVersion(_Stream, Version);
		_Stream % m_VersionInfo;
	}
}
