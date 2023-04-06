// Copyright © 2023 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Database/DatabaseValue>

namespace NMib::NCloud::NAppManagerDatabase
{
	enum
	{
		EAppManagerDatabaseVersion_Current = 0x101
	};

	static constexpr uint32 gc_Version = EAppManagerDatabaseVersion_Current;

	struct CRootInfoKey
	{
		template <typename tf_CStream>
		void f_FeedLexicographic(tf_CStream &_Stream) const;
		template <typename tf_CStream>
		void f_ConsumeLexicographic(tf_CStream &_Stream);

		static CStr const mc_Prefix;

		CStr m_Prefix = mc_Prefix;
	};

	struct CRootInfoValue
	{
		template <typename tf_CStream>
		void f_Stream(tf_CStream &_Stream);

		NStr::CStr m_UniqueKey;
		uint64 m_LastUpdateSequenceAtCleanup = 0;
	};

	struct CUpdateNotificationKey
	{
		template <typename tf_CStream>
		void f_FeedLexicographic(tf_CStream &_Stream) const;
		template <typename tf_CStream>
		void f_ConsumeLexicographic(tf_CStream &_Stream);

		static CStr const mc_Prefix;

		CStr m_Prefix = mc_Prefix;
		uint64 m_UniqueSequence = 0;
	};

	struct CUpdateNotificationValue
	{
		template <typename tf_CStream>
		void f_Stream(tf_CStream &_Stream);

		CStr m_UniqueKey;
		CAppManagerInterface::CUpdateNotification m_Notification;
	};
}

#include "Malterlib_Cloud_App_AppManager_Database.hpp"
