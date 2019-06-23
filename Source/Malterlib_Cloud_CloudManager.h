// Copyright © 2019 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/ActorInterface>
#include <Mib/Cloud/AppManager>

namespace NMib::NCloud
{
	enum
	{
		ECloudManagerMinProtocolVersion = 0x101
		, ECloudManagerProtocolVersion = 0x101
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

			NStr::CStr m_HostName;
			NStr::CStr m_ProgramDirectory;
		};

		struct CAppManagerDynamicInfo : public CAppManagerInfo
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			using CAppManagerInfo::operator =;

			NTime::CTime m_LastSeen;
			NStr::CStr m_LastConnectionError;
			NTime::CTime m_LastConnectionErrorTime;
			bool m_bActive = false;
		};

		virtual NConcurrency::TCFuture<NConcurrency::TCActorSubscriptionWithID<>> f_RegisterAppManager
			(
			 	NConcurrency::TCDistributedActorInterfaceWithID<CAppManagerInterface> &&_AppManager
			 	, CAppManagerInfo &&_AppManagerInfo
			)
			= 0
		;
		virtual NConcurrency::TCFuture<NContainer::TCMap<NStr::CStr, CAppManagerDynamicInfo>> f_EnumAppManagers() = 0;

		CCloudManager();
		~CCloudManager();
	};
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NCloud;
#endif

#include "Malterlib_Cloud_CloudManager.hpp"
