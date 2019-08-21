// Copyright © 2019 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/ActorInterface>
#include <Mib/Cloud/AppManager>
#include <Mib/Cloud/VersionInfo>

namespace NMib::NCloud
{
	enum
	{
		ECloudManagerMinProtocolVersion = 0x101
		, ECloudManagerProtocolVersion = 0x109
	};

#	if defined(DMibCloudCloudManagerDebug)
#		define DMibCloudCloudManagerDebugOut DMibConOut2
#	else
#		define DMibCloudCloudManagerDebugOut(...)  (void)0
#	endif

	struct CCloudManager : public NConcurrency::CActor
	{
		static constexpr ch8 const *mc_pDefaultNamespace = "com.malterlib/Cloud/CloudManager";
		
		enum : uint32
		{
			EMinProtocolVersion = ECloudManagerMinProtocolVersion
			, EProtocolVersion = ECloudManagerProtocolVersion
		};

		struct CAppManagerInfo
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			auto f_Tuple() const;
			bool operator == (CAppManagerInfo const &_Right) const;

			NStr::CStr m_Environment;
			NStr::CStr m_HostName;
			NStr::CStr m_ProgramDirectory;
			CCloudVersion m_Version;
			NTime::CTime m_VersionDate;
			NStr::CStr m_Platform;
			NStr::CStr m_PlatformFamily;
		};

		struct CAppManagerDynamicInfo : public CAppManagerInfo
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			bool f_HasErrors() const;
			using CAppManagerInfo::operator =;

			NTime::CTime m_LastSeen;
			NStr::CStr m_LastConnectionError;
			NTime::CTime m_LastConnectionErrorTime;
			NContainer::TCMap<NStr::CStr, NStr::CStr> m_OtherErrors;
			bool m_bActive = false;
		};

		struct CApplicationKey
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			auto f_Tuple() const;
			bool operator < (CApplicationKey const &_Right) const;

			NStr::CStr m_AppManagerID;
			NStr::CStr m_Name;
		};

		struct CApplicationInfo
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			CAppManagerInterface::CApplicationInfo m_ApplicationInfo;
		};

		virtual NConcurrency::TCFuture<NConcurrency::TCActorSubscriptionWithID<>> f_RegisterAppManager
			(
			 	NConcurrency::TCDistributedActorInterfaceWithID<CAppManagerInterface> &&_AppManager
			 	, CAppManagerInfo &&_AppManagerInfo
			)
			= 0
		;
		virtual NConcurrency::TCFuture<NContainer::TCMap<NStr::CStr, CAppManagerDynamicInfo>> f_EnumAppManagers() = 0;
		virtual NConcurrency::TCFuture<NContainer::TCMap<CApplicationKey, CApplicationInfo>> f_EnumApplications() = 0;

		CCloudManager();
		~CCloudManager();
	};
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NCloud;
#endif

#include "Malterlib_Cloud_CloudManager.hpp"
