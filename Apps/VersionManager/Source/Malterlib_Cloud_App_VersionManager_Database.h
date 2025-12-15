// Copyright © 2025 Unbroken AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Database/DatabaseValue>

#include <Mib/Cloud/VersionManager>

namespace NMib::NCloud::NVersionManagerDatabase
{
	static constexpr uint32 gc_Version = CVersionManager::EProtocolVersion_Current;

	struct CVersionDatabaseKey
	{
		template <typename tf_CStream>
		void f_FeedLexicographic(tf_CStream &_Stream) const;
		template <typename tf_CStream>
		void f_ConsumeLexicographic(tf_CStream &_Stream);

		NEncoding::CEJsonSorted f_ToJson() const;

		static NStr::CStr const mc_Prefix;

		NStr::CStr m_Prefix = mc_Prefix;
		NStr::CStr m_Application;
		CVersionManager::CVersionIDAndPlatform m_VersionIDAndPlatform;
	};

	struct CVersionDatabaseValue
	{
		template <typename tf_CStream>
		void f_Stream(tf_CStream &_Stream);

		NEncoding::CEJsonSorted f_ToJson() const;

		CVersionManager::CVersionInformation m_VersionInfo;
	};
}

#include "Malterlib_Cloud_App_VersionManager_Database.hpp"
