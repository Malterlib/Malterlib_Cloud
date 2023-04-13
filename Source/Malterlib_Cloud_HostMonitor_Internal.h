// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/File/ChangeNotificationActor>
#include <Mib/Database/DatabaseActor>
#include <Mib/Concurrency/ActorSequencerActor>
#include <Mib/Concurrency/DistributedAppInterface>

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
			TCFuture<void> f_Destroy();

			CMonitorPathOptions m_Options;
			mint m_RefCount = 0;
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
			TCActor<TCActorSequencerActor<void>> m_UpdateSequencer{fg_Construct()};
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

		TCFuture<void> f_SetupDatabase();

		TCFuture<void> f_PeriodicUpdate();
		TCFuture<void> f_PeriodicUpdate_Diskspace(bool _bCanSkip);
		TCFuture<void> f_PeriodicUpdate_Diskspace_UpdateMounts();

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
			)
		;

		CHostMonitor *m_pThis;

		TCActor<CDistributedAppSensorStoreLocal> m_SensorStore;
		TCActor<CDistributedAppLogStoreLocal> m_LogStore;
		TCActor<CDatabaseActor> m_Database;
		TCActor<CSeparateThreadActor> m_FileActor;
		CActorSubscription m_UpdateTimerSubscription;
		TCMap<CStr, CMonitoredPath> m_MonitoredPaths;
		TCMap<CStr, CAutomaticMount> m_AutomaticMounts;
		TCActorSequencerAsync<void> m_UpdatePeriodicSequencer;
		TCActorSequencerAsync<void> m_UpdatePeriodicDiskSpaceSequencer;
		TCVector<TCPromise<void>> m_UpdatePeriodicWaitList;
		fp64 m_HostMonitorInterval = mc_DefaultHostMonitorInterval;
		mint m_FileActorSequence = 0;
		EInitFlag m_Flags = EInitFlag_None;

		TCMap<CStr, CMonitoredConfig> m_MonitoredConfigs;
		TCActor<CFileChangeNotificationActor> m_FileChangeNotificationsActor{fg_Construct()};
		TCOptional<CDistributedAppLogReporter::CLogReporter> m_ConfigLogReporter;
	};
}
