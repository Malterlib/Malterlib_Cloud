// Copyright © 2019 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/ActorInterface>
#include <Mib/Concurrency/DistributedAppSensorReporter>
#include <Mib/Concurrency/DistributedAppSensorReader>
#include <Mib/Concurrency/DistributedAppLogReporter>
#include <Mib/Concurrency/DistributedAppLogReader>
#include <Mib/Cloud/AppManager>
#include <Mib/Cloud/VersionInfo>

namespace NMib::NCloud
{
	enum
	{
		ECloudManagerProtocolVersion_Min = 0x101
		, ECloudManagerProtocolVersion_AddEnvironment = 0x102
		, ECloudManagerProtocolVersion_AddVersionInfo = 0x103
		, ECloudManagerProtocolVersion_AddVersionDate = 0x105
		, ECloudManagerProtocolVersion_AddOtherErrors = 0x106
		, ECloudManagerProtocolVersion_AppManagerVersionIncreased1 = 0x109
		, ECloudManagerProtocolVersion_AppManagerVersionIncreased2 = 0x110
		, ECloudManagerProtocolVersion_SupportLogs = 0x111
		, ECloudManagerProtocolVersion_AppManagerVersionIncreased3 = 0x112
		, ECloudManagerProtocolVersion_AppManagerVersionIncreased4 = 0x113
		, ECloudManagerProtocolVersion_AddLastSeenUpdateNotificationSequence = 0x114
		, ECloudManagerProtocolVersion_Current = 0x114
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
			EProtocolVersion_Min = ECloudManagerProtocolVersion_Min
			, EProtocolVersion_Current = ECloudManagerProtocolVersion_Current
		};

		struct CAppManagerInfo
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			auto operator <=> (CAppManagerInfo const &_Right) const = default;

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

			auto operator <=> (CApplicationKey const &_Right) const = default;

			NStr::CStr m_AppManagerID;
			NStr::CStr m_Name;
		};

		struct CApplicationInfo
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			CAppManagerInterface::CApplicationInfo m_ApplicationInfo;
		};

		static uint32 fs_ProtocolVersion_CloudManagerToAppManager(uint32 _CloudManagerVersion);

		virtual NConcurrency::TCFuture<NConcurrency::TCActorSubscriptionWithID<>> f_RegisterAppManager
			(
				NConcurrency::TCDistributedActorInterfaceWithID<CAppManagerInterface> &&_AppManager
				, CAppManagerInfo &&_AppManagerInfo
			)
			= 0
		;
		virtual NConcurrency::TCFuture<NContainer::TCMap<NStr::CStr, CAppManagerDynamicInfo>> f_EnumAppManagers() = 0;
		virtual NConcurrency::TCFuture<NContainer::TCMap<CApplicationKey, CApplicationInfo>> f_EnumApplications() = 0;
		virtual NConcurrency::TCFuture<void> f_RemoveAppManager(NStr::CStr const &_AppManagerHostID) = 0;
		virtual NConcurrency::TCFuture<uint32> f_RemoveSensor(NConcurrency::CDistributedAppSensorReporter::CSensorInfoKey &&_SensorInfoKey) = 0;
		virtual NConcurrency::TCFuture<uint32> f_RemoveLog(NConcurrency::CDistributedAppLogReporter::CLogInfoKey &&_LogInfoKey) = 0;
		virtual NConcurrency::TCFuture<NConcurrency::TCDistributedActorInterfaceWithID<NConcurrency::CDistributedAppSensorReporter>> f_GetSensorReporter() = 0;
		virtual NConcurrency::TCFuture<NConcurrency::TCDistributedActorInterfaceWithID<NConcurrency::CDistributedAppSensorReader>> f_GetSensorReader() = 0;
		virtual NConcurrency::TCFuture<NConcurrency::TCDistributedActorInterfaceWithID<NConcurrency::CDistributedAppLogReporter>> f_GetLogReporter() = 0;
		virtual NConcurrency::TCFuture<NConcurrency::TCDistributedActorInterfaceWithID<NConcurrency::CDistributedAppLogReader>> f_GetLogReader() = 0;

		CCloudManager();
		~CCloudManager();
	};
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NCloud;
#endif

#include "Malterlib_Cloud_CloudManager.hpp"
