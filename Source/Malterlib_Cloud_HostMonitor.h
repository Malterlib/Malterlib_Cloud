// Copyright © 2020 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedAppSensorStoreLocal>
#include <Mib/Concurrency/DistributedAppLogStoreLocal>
#include <Mib/Concurrency/DistributedAppInterface>
#include <Mib/Cloud/CloudManager>
#include <Mib/Container/Registry>

namespace NMib::NCloud::NHostMonitor
{
	struct CConfigFileContents_GeneralText
	{
		template <typename tf_CStream>
		void f_Stream(tf_CStream &_Stream);

		auto operator <=> (CConfigFileContents_GeneralText const &_Right) const = default;

		NStr::CStr m_Parsed;
	};

	struct CConfigFileContents_GeneralBinary
	{
		template <typename tf_CStream>
		void f_Stream(tf_CStream &_Stream);

		auto operator <=> (CConfigFileContents_GeneralBinary const &_Right) const = default;

	};

	struct CConfigFileContents_Registry
	{
		template <typename tf_CStream>
		void f_Stream(tf_CStream &_Stream);

		auto operator <=> (CConfigFileContents_Registry const &_Right) const = default;

		NContainer::CRegistryPreserveAllFull m_Parsed;
	};

	struct CConfigFileContents_Json
	{
		template <typename tf_CStream>
		void f_Stream(tf_CStream &_Stream);

		auto operator <=> (CConfigFileContents_Json const &_Right) const = default;

		NEncoding::CEJSONSorted m_Parsed;
	};

	using CConfigFileContentsParsed = NStorage::TCStreamableVariant
		<
			NConcurrency::CDistributedAppInterfaceServer::EMonitorConfigType
			, NStorage::TCMember<CConfigFileContents_GeneralText, NConcurrency::CDistributedAppInterfaceServer::EMonitorConfigType_GeneralText>
			, NStorage::TCMember<CConfigFileContents_GeneralBinary, NConcurrency::CDistributedAppInterfaceServer::EMonitorConfigType_GeneralBinary>
			, NStorage::TCMember<CConfigFileContents_Registry, NConcurrency::CDistributedAppInterfaceServer::EMonitorConfigType_Registry>
			, NStorage::TCMember<CConfigFileContents_Json, NConcurrency::CDistributedAppInterfaceServer::EMonitorConfigType_Json>
		>
	;

	struct CConfigFileContents
	{
		template <typename tf_CStream>
		void f_Stream(tf_CStream &_Stream);

		auto operator <=> (CConfigFileContents const &_Right) const = default;

		NContainer::CByteVector m_Raw;
		CConfigFileContentsParsed m_Parsed;
	};

	struct CConfigFileVersionKey
	{
		auto operator <=> (CConfigFileVersionKey const &_Right) const = default;

		NStr::CStr m_FileName;
		uint64 m_Sequence = 0;
	};

	struct CConfigFileUniqueProperties
	{
		template <typename tf_CStream>
		void f_Stream(tf_CStream &_Stream);

		auto operator <=> (CConfigFileUniqueProperties const &_Right) const = default;

		NConcurrency::CDistributedAppInterfaceServer::EMonitorConfigType m_ConfigType = NConcurrency::CDistributedAppInterfaceServer::EMonitorConfigType_GeneralText;
		NCryptography::CHashDigest_SHA256 m_Digest;
		NStr::CStr m_ParseError;

		NStr::CStr m_Owner;
		NStr::CStr m_Group;
		uint64 m_Size = 0;
		NFile::EFileAttrib m_Attributes = NFile::EFileAttrib_None;
		bool m_bExists = false;
	};

	struct CConfigFileProperties
	{
		template <typename tf_CStream>
		void f_Stream(tf_CStream &_Stream);

		auto operator <=> (CConfigFileProperties const &_Right) const = default;

		CConfigFileUniqueProperties m_UniqueProperties;
		NTime::CTime m_Timestamp;
	};
}

namespace NMib::NCloud
{
	struct CHostMonitor : public NConcurrency::CActor
	{
		enum EInitFlag
		{
			EInitFlag_None = 0
			, EInitFlag_MonitorAllMounts = DMibBit(0)
		};

		struct CMonitorPathOptions
		{
			auto f_Tuple() const;
			bool operator == (CMonitorPathOptions const &_Other) const;

			NStr::CStr m_Path;
			uint64 m_WarnFree = TCLimitsInt<uint64>::mc_Max;
			uint64 m_CriticalFree = TCLimitsInt<uint64>::mc_Max;
			fp32 m_WarnFreePercent = 5.0; ///< Set to fp32::fs_Inf() to disable
			fp32 m_CriticalFreePercent = 1.0; ///< Set to fp32::fs_Inf() to disable
		};

		struct CConfig
		{
			EInitFlag m_Flags = EInitFlag_None;
			fp64 m_Interval = mc_DefaultHostMonitorInterval;
			fp64 m_PatchInterval = mc_DefaultHostMonitorPatchInterval;

			fp64 m_ReportErrorAfter_OsVersion = 7_days;

			fp64 m_ReportWarningAfter_SecurityPatch = 1_days;
			fp64 m_ReportErrorAfter_SecurityPatch = 2_days;

			fp64 m_ReportWarningAfter_RebootRequired = 1_weeks;
			fp64 m_ReportErrorAfter_RebootRequired = 2_weeks;
		};

		struct [[nodiscard]] CInitResult
		{
			NStr::CStr m_OsName;
		};

		CHostMonitor
			(
				NConcurrency::TCActor<NConcurrency::CDistributedAppSensorStoreLocal> const &_SensorStore
				, NConcurrency::TCActor<NConcurrency::CDistributedAppLogStoreLocal> const &_LogStore
				, NConcurrency::TCActor<NDatabase::CDatabaseActor> const &_Database
				, NContainer::TCMap<NStr::CStr, NEncoding::CEJSONSorted> const &_SensorMetaData
				, NContainer::TCMap<NStr::CStr, NEncoding::CEJSONSorted> const &_LogMetaData
			)
		;
		~CHostMonitor();

		NConcurrency::TCFuture<CInitResult> f_Init(CConfig &&_Config);
		NConcurrency::TCFuture<NConcurrency::CActorSubscription> f_MonitorPath(CMonitorPathOptions const &_Options);
		NConcurrency::TCFuture<NConcurrency::CActorSubscription> f_MonitorConfigs(NConcurrency::CDistributedAppInterfaceServer::CConfigFiles &&_ConfigFiles);
		NConcurrency::TCFuture<void> f_SetExpectedOsVersions(CCloudManager::CExpectedVersions &&_ExpectedOsVersions);

		NConcurrency::TCFuture<NContainer::TCSet<NStr::CStr>> f_EnumConfigFiles();
		NConcurrency::TCFuture<NContainer::TCMap<NHostMonitor::CConfigFileVersionKey, NHostMonitor::CConfigFileProperties>> f_EnumConfigFileVersions(NStr::CStr &&_File);
		NConcurrency::TCFuture<NHostMonitor::CConfigFileContents> f_GetConfigFileContents(NHostMonitor::CConfigFileVersionKey &&_Key);

		static constexpr pfp64 mc_MinimumHostMonitorInterval = 10_seconds;
		static constexpr pfp64 mc_DefaultHostMonitorInterval = 60_seconds;

		static constexpr pfp64 mc_MinimumHostMonitorPatchInterval = 5_minutes;
		static constexpr pfp64 mc_DefaultHostMonitorPatchInterval = 12_hours;

	private:
		NConcurrency::TCFuture<void> fp_Destroy();

		struct CInternal;

		NStorage::TCUniquePointer<CInternal> mp_pInternal;
	};
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NCloud;
#endif

#include "Malterlib_Cloud_HostMonitor.hpp"
