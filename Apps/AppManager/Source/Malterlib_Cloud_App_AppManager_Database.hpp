// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

namespace NMib::NCloud::NAppManagerDatabase
{
	template <typename tf_CStream>
	void CRootInfoKey::f_FeedLexicographic(tf_CStream &_Stream) const
	{
		CDatabaseValue::fs_FeedLexicographic(_Stream, m_Prefix);
	}

	template <typename tf_CStream>
	void CRootInfoKey::f_ConsumeLexicographic(tf_CStream &_Stream)
	{
		CDatabaseValue::fs_ConsumeLexicographic(_Stream, m_Prefix);
	}

	template <typename tf_CStream>
	void CRootInfoValue::f_Stream(tf_CStream &_Stream)
	{
		uint32 Version = gc_Version;
		_Stream % Version;
		DBinaryStreamVersion(_Stream, Version);

		_Stream % m_UniqueKey;
		_Stream % m_LastUpdateSequenceAtCleanup;
	}

	template <typename tf_CStream>
	void CUpdateNotificationKey::f_FeedLexicographic(tf_CStream &_Stream) const
	{
		CDatabaseValue::fs_FeedLexicographic(_Stream, m_Prefix);
		CDatabaseValue::fs_FeedLexicographic(_Stream, m_UniqueSequence);
	}

	template <typename tf_CStream>
	void CUpdateNotificationKey::f_ConsumeLexicographic(tf_CStream &_Stream)
	{
		CDatabaseValue::fs_ConsumeLexicographic(_Stream, m_Prefix);
		CDatabaseValue::fs_ConsumeLexicographic(_Stream, m_UniqueSequence);
	}

	template <typename tf_CStream>
	void CUpdateNotificationValue::f_Stream(tf_CStream &_Stream)
	{
		uint32 Version = gc_Version;
		_Stream % Version;
		DBinaryStreamVersion(_Stream, Version);

		_Stream % m_UniqueKey;

		{
			uint32 AppManagerInterfaceVersion = CAppManagerInterface::EProtocolVersion_ResumableUpdateNotifications;

			static_assert
				(
					CAppManagerInterface::EProtocolVersion_Current == CAppManagerInterface::EProtocolVersion_ResumableUpdateNotifications
					, "Add a new version mapping if m_Notification streaming changed"
				)
			;

			DMibBinaryStreamVersion(_Stream, AppManagerInterfaceVersion);
			_Stream % m_Notification;
		}
	}
}
