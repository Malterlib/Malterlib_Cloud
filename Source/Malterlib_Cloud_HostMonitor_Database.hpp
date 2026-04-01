// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

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

	template <typename tf_CStream>
	void CPatchStateKey::f_FeedLexicographic(tf_CStream &_Stream) const
	{
		NDatabase::CDatabaseValue::fs_FeedLexicographic(_Stream, m_Prefix);
	}

	template <typename tf_CStream>
	void CPatchStateKey::f_ConsumeLexicographic(tf_CStream &_Stream)
	{
		NDatabase::CDatabaseValue::fs_ConsumeLexicographic(_Stream, m_Prefix);
	}

	template <typename tf_CStream>
	void CPatchStateValue::f_Stream(tf_CStream &_Stream)
	{
		uint32 Version = gc_Version;
		_Stream % Version;
		DMibBinaryStreamVersion(_Stream, Version);

		_Stream % m_ProblemStart_ExpectedOsVersionError;
		_Stream % m_ProblemStart_RebootRequired;
		_Stream % m_ProblemStart_SecurityPatches;
	}
}
