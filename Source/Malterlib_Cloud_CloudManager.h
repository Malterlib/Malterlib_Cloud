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
		, ECloudManagerProtocolVersion_AddLastSeenLogTimestamp = 0x115
		, ECloudManagerProtocolVersion_SupportExpectedOsVersions = 0x116
		, ECloudManagerProtocolVersion_SupportDeferredUpdateNotification = 0x117

		, ECloudManagerProtocolVersion_Current = 0x117
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

			NEncoding::CEJSON f_ToJson() const;

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

		struct CVersion
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);
			template <typename tf_CStr>
			void f_Format(tf_CStr &o_Str) const;

			NEncoding::CEJSON f_ToJson() const;

			static NConcurrency::TCFuture<CVersion> fs_ParseVersion(NStr::CStr _Version);

			auto operator <=> (CVersion const &_Right) const = default;

			uint32 m_Major = 0;
			uint32 m_Minor = 0;
			uint32 m_Revision = 0;
		};

		struct CCurrentVersion
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);
			template <typename tf_CStream>
			void f_FeedLexicographic(tf_CStream &_Stream) const;
			template <typename tf_CStream>
			void f_ConsumeLexicographic(tf_CStream &_Stream);
			template <typename tf_CStr>
			void f_Format(tf_CStr &o_Str) const;

			NEncoding::CEJSON f_ToJson() const;

			auto operator <=> (CCurrentVersion const &_Right) const = default;

			NStorage::TCOptional<uint32> m_Major;
			NStorage::TCOptional<uint32> m_Minor;
		};

		struct CExpectedVersionRange
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);
			template <typename tf_CStr>
			void f_Format(tf_CStr &o_Str) const;

			NEncoding::CEJSON f_ToJson() const;

			auto operator <=> (CExpectedVersionRange const &_Right) const = default;

			bool f_IsSet() const;
			bool f_IsDeprecated() const;
			void f_SetDeprecated();

			NStorage::TCOptional<CVersion> m_Min;
			NStorage::TCOptional<CVersion> m_Max;
		};

		struct CExpectedVersions
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			auto operator <=> (CExpectedVersions const &_Right) const = default;

			void f_ApplyChanges(CExpectedVersions const &_Changes);

			NContainer::TCMap<CCurrentVersion, CExpectedVersionRange> m_Versions;
		};

		struct CSubscribeExpectedOsVersions
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NStr::CStr m_OsName;
			NConcurrency::TCActorFunctorWithID<NConcurrency::TCFuture<void> (CExpectedVersions &&_Versions)> m_fVersionRangeChanged;
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
		virtual NConcurrency::TCFuture<NConcurrency::TCActorSubscriptionWithID<>> f_SubscribeExpectedOsVersions(CSubscribeExpectedOsVersions &&_Params) = 0;
 		virtual NConcurrency::TCFuture<NContainer::TCMap<NStr::CStr, CExpectedVersions>> f_EnumExpectedOsVersions() = 0;
 		virtual NConcurrency::TCFuture<void> f_SetExpectedOsVersions(NStr::CStr &&_OsName, CCurrentVersion &&_CurrentVersion, CExpectedVersionRange &&_ExpectedRange) = 0;

		CCloudManager();
		~CCloudManager();
	};
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NCloud;
#endif

#include "Malterlib_Cloud_CloudManager.hpp"
