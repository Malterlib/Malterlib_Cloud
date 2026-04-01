// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <Mib/Database/DatabaseActor>
#include <Mib/Cloud/CloudManager>
#include <Mib/Concurrency/ActorSequencerActor>
#include <Mib/Concurrency/DistributedAppInterface>
#include <Mib/File/ChangeNotificationActor>
#include <Mib/Memory/Platform>

namespace NMib::NCloud
{
	using namespace NConcurrency;
	using namespace NContainer;
	using namespace NCryptography;
	using namespace NDatabase;
	using namespace NEncoding;
	using namespace NException;
	using namespace NFile;
	using namespace NStorage;
	using namespace NStr;
	using namespace NTime;
}

#include "Malterlib_Cloud_HostMonitor_Database.h"

namespace NMib::NCloud
{
	struct CHostMonitor::CInternal : public CActorInternal
	{
		enum ESensorType
		{
			ESensorType_Free
			, ESensorType_FreePercent
			, ESensorType_Total
		};

		struct CMonitoredPath
		{
			TCUnsafeFuture<void> f_Destroy();

			CMonitorPathOptions m_Options;
			umint m_RefCount = 0;
			TCOptional<CDistributedAppSensorReporter::CSensorReporter> m_FreeReporter;
			TCOptional<CDistributedAppSensorReporter::CSensorReporter> m_FreePercentReporter;
			TCOptional<CDistributedAppSensorReporter::CSensorReporter> m_TotalReporter;
		};

		struct CAutomaticMount
		{
			CActorSubscription m_MonitorPathSubscription;
		};

		struct CMonitoredConfigFile
		{
			CDistributedAppInterfaceServer::CConfigFile m_Options;
			CActorSubscription m_FileChangeSubscription;
			CSequencer m_UpdateSequencer{"HostMonitor MonitoredConfigFile UpdateSequencer {}"_f << TCMap<CStr, CMonitoredConfigFile>::fs_GetKey(*this)};
		};

		struct CMonitoredConfig
		{
			TCMap<CStr, CMonitoredConfigFile> m_Files;
		};

		struct CConfigFileKeyValue
		{
			NHostMonitorDatabase::CConfigFileHistoryEntryKey m_Key;
			NHostMonitorDatabase::CConfigFileHistoryEntryValue m_Value;
		};

		struct CMemoryStatistic
		{
			NMemory::CSystemMemoryStatisticCharacteristics m_Characteristics;
			TCOptional<CDistributedAppSensorReporter::CSensorReporter> m_SensorReporter;
		};

		struct CPatchesNeeded
		{
			uint32 m_nSecurityPatches = 0;
			uint32 m_nNormalPatches = 0;
		};

		TCFuture<CDistributedAppSensorReporter::CVersion> f_GetOsNameAndVersion();

		TCFuture<void> f_SetupDatabase();

		TCFuture<void> f_PeriodicUpdate();
		TCFuture<void> f_PeriodicUpdate_Diskspace(bool _bCanSkip);
		TCFuture<void> f_PeriodicUpdate_Diskspace_UpdateMounts();
		TCFuture<void> f_PeriodicUpdate_Patch(bool _bCanSkip);
		TCFuture<bool> f_PeriodicUpdate_Patch_OsVersion();
		TCFuture<bool> f_PeriodicUpdate_Patch_ExpectedOsVersion();
		TCFuture<bool> f_PeriodicUpdate_Patch_PatchStatus();
		TCFuture<void> f_PeriodicUpdate_Memory(bool _bCanSkip);

		TCFuture<bool> f_Patch_RebootNeeded();
		TCFuture<CPatchesNeeded> f_Patch_PatchesNeeded();
		TCFuture<void> f_Patch_InstallPatches();

		CExceptionPointer f_ConfigFile_CheckFilePrerequisites(CMonitoredConfigFile * &o_pConfigFile, CStr _ConfigID, CStr _FileName);

		TCFuture<void> f_ConfigFile_Changed(CStr _ConfigID, CStr _FileName);
		TCFuture<void> f_ConfigFile_ValueChanged(CStr _ConfigID, CStr _FileName, NHostMonitorDatabase::CConfigFileHistoryEntryValue _Value);
		TCFuture<void> f_ConfigFile_LogChanges
			(
				CStr _ConfigID
				, CStr _FileName
				, NHostMonitorDatabase::CConfigFileHistoryEntryValue _PreviousValue
				, NHostMonitorDatabase::CConfigFileHistoryEntryValue _NewValue
			)
		;

		TCFuture<void> f_OpenConfigLogReporter();

		CInternal
			(
				CHostMonitor *_pThis
				, TCActor<CDistributedAppSensorStoreLocal> const &_SensorStore
				, TCActor<CDistributedAppLogStoreLocal> const &_LogStore
				, TCActor<CDatabaseActor> const &_Database
 				, TCMap<CStr, CEJsonSorted> const &_SensorMetadata
				, TCMap<CStr, CEJsonSorted> const &_LogMetadata
			)
		;

		CHostMonitor *m_pThis;

		TCActor<CDistributedAppSensorStoreLocal> m_SensorStore;
		TCActor<CDistributedAppLogStoreLocal> m_LogStore;
		TCActor<CDatabaseActor> m_Database;
		CActorSubscription m_UpdateTimerSubscription;
		TCMap<CStr, CMonitoredPath> m_MonitoredPaths;
		TCMap<CStr, CAutomaticMount> m_AutomaticMounts;
		CSequencer m_UpdatePeriodicSequencer{"HostMonitor UpdatePeriodicSequencer"};
		CSequencer m_UpdatePeriodicDiskSpaceSequencer{"HostMonitor UpdatePeriodicDiskSpaceSequencer"};
		TCVector<TCPromise<void>> m_UpdatePeriodicWaitList;
		CConfig m_Config;
		TCMap<CStr, CEJsonSorted> m_SensorMetadata;
		TCMap<CStr, CEJsonSorted> m_LogMetadata;

		TCMap<CStr, CMonitoredConfig> m_MonitoredConfigs;
		TCActor<CFileChangeNotificationActor> m_FileChangeNotificationsActor{fg_Construct()};
		TCOptional<CDistributedAppLogReporter::CLogReporter> m_ConfigLogReporter;

		CSequencer m_UpdatePeriodicPatch{"HostMonitor UpdatePeriodicPatch"};
		NHostMonitorDatabase::CPatchStateValue m_PatchDatabaseState;
		TCOptional<CDistributedAppSensorReporter::CSensorReporter> m_OsVersionReporter;
		TCOptional<CDistributedAppSensorReporter::CSensorReporter> m_OsVersionStatusReporter;
		TCOptional<CDistributedAppSensorReporter::CSensorReporter> m_OsPatchStatusReporter;
		TCOptional<CStopwatch> m_PatchStopwatch;
		CCloudManager::CExpectedVersions m_ExpectedOsVersions;
		CDistributedAppSensorReporter::CVersion m_CurrentOsVersion;

		CSequencer m_UpdatePeriodicMemory{"HostMonitor UpdatePeriodicMemory"};
		TCOptional<CStopwatch> m_MemoryStopwatch;
		TCMap<CStr, CMemoryStatistic> m_MemoryReporters;

		bool m_bInitialized = false;
	};
}
