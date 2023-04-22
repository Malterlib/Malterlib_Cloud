// Copyright © 2023 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Database/DatabaseValue>

namespace NMib::NCloud::NHostMonitorDatabase
{
	enum
	{
		EHostMonitorDatabaseVersion_Current = 0x101
	};

	static constexpr uint32 gc_Version = EHostMonitorDatabaseVersion_Current;

	struct CPatchStateKey
	{
		template <typename tf_CStream>
		void f_FeedLexicographic(tf_CStream &_Stream) const;
		template <typename tf_CStream>
		void f_ConsumeLexicographic(tf_CStream &_Stream);

		static CStr const mc_Prefix;

		CStr m_Prefix = mc_Prefix;
	};

	struct CPatchStateValue
	{
		template <typename tf_CStream>
		void f_Stream(tf_CStream &_Stream);

		NTime::CTime m_ProblemStart_ExpectedOsVersionError;
		NTime::CTime m_ProblemStart_RebootRequired;
		NTime::CTime m_ProblemStart_SecurityPatches;
	};

	struct CConfigFileHistoryEntryKey
	{
		template <typename tf_CStream>
		void f_FeedLexicographic(tf_CStream &_Stream) const;
		template <typename tf_CStream>
		void f_ConsumeLexicographic(tf_CStream &_Stream);

		static CStr const mc_Prefix;

		CStr m_Prefix = mc_Prefix;
		NHostMonitor::CConfigFileVersionKey m_VersionKey;
	};

	struct CConfigFileHistoryEntryValue
	{
		template <typename tf_CStream>
		void f_Stream(tf_CStream &_Stream);

		NHostMonitor::CConfigFileProperties m_Properties;
		NHostMonitor::CConfigFileContents m_Contents;
	};
}

#include "Malterlib_Cloud_HostMonitor_Database.hpp"
