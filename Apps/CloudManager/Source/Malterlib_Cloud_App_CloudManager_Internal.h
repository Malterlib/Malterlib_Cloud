// Copyright © 2019 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedAppSensorStoreLocal>
#include <Mib/Concurrency/DistributedAppLogStoreLocal>
#include <Mib/File/ChangeNotificationActor>
#include <Mib/Cloud/CloudManager>
#include <Mib/Cloud/FileTransfer>
#include <Mib/Database/DatabaseActor>
#include <Mib/Web/HttpClient>
#include <Mib/Web/Slack>

#include "Malterlib_Cloud_App_CloudManager.h"
#include "Malterlib_Cloud_App_CloudManager_Database.h"
#include "Malterlib_Cloud_App_CloudManager_Notifications.h"
#include "Malterlib_Cloud_App_CloudManager_Notifications_Updates.h"
#include "Malterlib_Cloud_App_CloudManager_Notifications_Sensors.h"
#include "Malterlib_Cloud_App_CloudManager_Notifications_Logs.h"

namespace NMib::NCloud::NCloudManager
{
	struct CCloudManagerServer : public CActor
	{
		friend struct CNotifications;
		friend struct CUpdateNotifications;
		friend struct CSensorNotifications;
		friend struct CLogNotifications;

		using CActorHolder = CDelegatedActorHolder;

		struct CCloudManagerImplementation : public CCloudManager
		{
			TCFuture<CRegisterAppManagerResult> f_RegisterAppManager(TCDistributedActorInterfaceWithID<CAppManagerInterface> _AppManager, CAppManagerInfo _AppManagerInfo) override;
			TCFuture<TCMap<CStr, CAppManagerDynamicInfo>> f_EnumAppManagers() override;
			TCFuture<TCMap<CApplicationKey, CApplicationInfo>> f_EnumApplications() override;
			TCFuture<CRemoveAppManagerReturn> f_RemoveAppManager(CStr _AppManagerHostID) override;
			TCFuture<uint32> f_RemoveSensor(CRemoveSensor _RemoveSensor) override;
			TCFuture<uint32> f_RemoveLog(CRemoveLog _RemoveLog) override;
			TCFuture<uint32> f_SnoozeSensor(CSnoozeSensor _SnoozeSensor) override;
			TCFuture<TCDistributedActorInterfaceWithID<CDistributedAppSensorReporter>> f_GetSensorReporter() override;
			TCFuture<TCDistributedActorInterfaceWithID<CDistributedAppSensorReader>> f_GetSensorReader() override;
			TCFuture<TCDistributedActorInterfaceWithID<CDistributedAppLogReporter>> f_GetLogReporter() override;
			TCFuture<TCDistributedActorInterfaceWithID<CDistributedAppLogReader>> f_GetLogReader() override;
			TCFuture<TCActorSubscriptionWithID<>> f_SubscribeExpectedOsVersions(CSubscribeExpectedOsVersions _Params) override;
	 		TCFuture<TCMap<CStr, CExpectedVersions>> f_EnumExpectedOsVersions() override;
			TCFuture<void> f_SetExpectedOsVersions(CStr _OsName, CCurrentVersion _CurrentVersion, CExpectedVersionRange _ExpectedRange) override;

			DMibDelegatedActorImplementation(CCloudManagerServer);
		};

		struct CDistributedAppSensorReporterImplementation : public CDistributedAppSensorReporter
		{
			TCFuture<CSensorReporter> f_OpenSensorReporter(CSensorInfo _SensorInfo) override;

			DMibDelegatedActorImplementation(CCloudManagerServer);
		};

		struct CDistributedAppSensorReaderImplementation : public CDistributedAppSensorReader
		{
			TCFuture<TCAsyncGenerator<TCVector<CDistributedAppSensorReporter::CSensorInfo>>> f_GetSensors(CGetSensors _Params) override;
			auto f_GetSensorReadings(CGetSensorReadings _Params) -> TCFuture<TCAsyncGenerator<TCVector<CDistributedAppSensorReader_SensorKeyAndReading>>> override;
			auto f_GetSensorStatus(CGetSensorStatus _Params) -> TCFuture<TCAsyncGenerator<TCVector<CDistributedAppSensorReader_SensorKeyAndReading>>> override;
			auto f_SubscribeSensors(TCVector<CDistributedAppSensorReader_SensorFilter> _Filters, TCActorFunctorWithID<TCFuture<void> (CSensorChange _Change)> _fOnChange)
				-> TCFuture<TCActorSubscriptionWithID<>> override
			;
			auto f_SubscribeSensorReadings
				(
					TCVector<CDistributedAppSensorReader_SensorReadingSubscriptionFilter> _Filters
					, TCActorFunctorWithID<TCFuture<void> (CDistributedAppSensorReader_SensorKeyAndReading _Reading)> _fOnReading
				)
				-> TCFuture<TCActorSubscriptionWithID<>> override
			;
			auto f_SubscribeSensorStatus
				(
					TCVector<CDistributedAppSensorReader_SensorStatusFilter> _Filters
					, TCActorFunctorWithID<TCFuture<void> (CDistributedAppSensorReader_SensorKeyAndReading _Reading)> _fOnReading
				)
				-> TCFuture<TCActorSubscriptionWithID<>> override
			;

			DMibDelegatedActorImplementation(CCloudManagerServer);
		};

		struct CDistributedAppLogReporterImplementation : public CDistributedAppLogReporter
		{
			TCFuture<CLogReporter> f_OpenLogReporter(CLogInfo _LogInfo) override;

			DMibDelegatedActorImplementation(CCloudManagerServer);
		};

		struct CDistributedAppLogReaderImplementation : public CDistributedAppLogReader
		{
			TCFuture<TCAsyncGenerator<TCVector<CDistributedAppLogReporter::CLogInfo>>> f_GetLogs(CGetLogs _Params) override;
			auto f_GetLogEntries(CGetLogEntries _Params) -> TCFuture<TCAsyncGenerator<TCVector<CDistributedAppLogReader_LogKeyAndEntry>>> override;
			auto f_SubscribeLogs(TCVector<CDistributedAppLogReader_LogFilter> _Filter, TCActorFunctorWithID<TCFuture<void> (CLogChange _Change)> _fOnChange)
				-> TCFuture<TCActorSubscriptionWithID<>> override
			;
			auto f_SubscribeLogEntries(CSubscribeLogEntries _Params) -> TCFuture<TCActorSubscriptionWithID<>> override;

			DMibDelegatedActorImplementation(CCloudManagerServer);
		};

		struct CAppManagerCloudManagerInterfaceImplementation : public CAppManagerCloudManagerInterface
		{
			NConcurrency::TCFuture<void> f_PauseReporting(fp32 _SecondsToPauseFor) override;

			DMibDelegatedActorImplementation(CCloudManagerServer);
		};

		CCloudManagerServer(CDistributedAppState &_AppState);
		~CCloudManagerServer();

		TCFuture<void> f_Init();
		TCFuture<void> f_DumpDatabaseEntries(TCSharedPointer<CCommandLineControl> _pCommandLine, CStr _Prefix);

	private:
		struct CAppManagerState
		{
			CStr const &f_AppManagerID() const;
			NCloudManagerDatabase::CAppManagerKey f_DatabaseKey() const;

			TCUnsafeFuture<void> f_Destroy(CCloudManagerServer &_This);

			TCDistributedActorInterfaceWithID<CAppManagerInterface> m_Interface;
			NCloudManagerDatabase::CAppManagerValue m_Data;
			CStr m_UniqueHostID;
			mint m_RegisterSequence = 0;
			CActorSubscription m_ChangeNotificationsSubscription;
			CActorSubscription m_UpdateNotificationsSubscription;
			bool m_bUpdatedOnce = false;;
			bool m_bFiltered = false;
			bool m_bAccessDenied = false;
		};

		struct CExpectedOsVersionSubscriptionKey
		{
			auto operator <=> (CExpectedOsVersionSubscriptionKey const &_Right) const noexcept = default;

			CStr m_OsName;
			mint m_ID = 0;
		};

		struct CExpectedOsVersionSubscription
		{
			TCActorFunctorWithID<TCFuture<void> (CCloudManager::CExpectedVersions _Versions)> m_fVersionRangeChanged;
			TCVector<CCloudManager::CExpectedVersions> m_QueuedNotifications;
		};

		struct CCleanupDatabaseResult
		{
			NDatabase::CDatabaseActor::CTransactionWrite m_Transaction;
			bool m_bMoreWork = false;
		};

		struct CCleanupState
		{
			friend struct CCloudManagerServer;

			void f_LogStartup();
			void f_Log(bool _bProgress);
			void f_Initialize(NDatabase::CDatabaseActor::CTransactionWrite &_WriteTransaction);

			bool m_bForcedCompaction = true;

		private:
			CStopwatch mp_StatsStopwatch{true};
			CDatabaseSizeStatistics mp_OriginalStats;
			CDatabaseSizeStatistics mp_CurrentStats;
			CDatabaseOffset mp_TargetSize;
			CDatabaseOffset mp_MaxFreedLimit;

			NTime::CTime mp_StartTime;
			NTime::CTime mp_EndTime;
			NTime::CTimeSpan mp_UtcOffset;

			mint mp_nReadingsDeletedSensor = 0;
			mint mp_nReadingsDeletedLog = 0;
			mint mp_nYields = 0;
			mint mp_nDatabaseYields = 0;

			bool mp_bLoggedStart = false;
			bool mp_bInitialized = false;
		};

		TCFuture<void> fp_Destroy() override;
		TCFuture<void> fp_Publish();
		TCFuture<void> fp_SetupDatabase();
		TCFuture<void> fp_SetupPermissions();
		TCFuture<void> fp_SetupMonitor();
		TCFuture<void> fp_PerformCleanup();
		TCFuture<void> fp_SetupCleanup();
		TCFuture<void> fp_SetupSensorStore();
		TCFuture<void> fp_SetupLogStore();
		TCFuture<CCleanupDatabaseResult> fp_CleanupDatabase(NDatabase::CDatabaseActor::CTransactionWrite _WriteTransaction, TCSharedPointer<CCleanupState> _pState);
		TCFuture<CDatabaseActor::CTransactionWrite> fp_SaveGlobalState(CDatabaseActor::CTransactionWrite _Transaction);
		TCFuture<void> fp_SaveGlobalStateWithoutTransaction();
		TCFuture<void> fp_UpdateAppManagerState();
		TCFuture<void> fp_SaveAppManagerData(NCloudManagerDatabase::CAppManagerKey _Key, NCloudManagerDatabase::CAppManagerValue _Data);
		TCFuture<CCloudManager::CRemoveAppManagerReturn> fp_RemoveAppManagerData(CStr _HostID);
		TCFuture<void> fp_ProcessApplicationChanges(CStr _AppManagerID, CAppManagerInterface::COnChangeNotificationParams _Params);
		TCFuture<void> fp_ChangeOtherErrors(CStr _AppManagerID, mint _RegisterSequence, TCSet<CStr> _Remove, TCMap<CStr, CStr> _Add);
		TCFuture<void> fp_ReportFiltered(CStr _AppManagerID, mint _RegisterSequence, bool _bFiltered, bool _bAccessDenied);

		static TCVector<CStr> fsp_SensorReadPermissions();
		static TCVector<CStr> fsp_LogReadPermissions();

		static CStr const mcp_DatabasePrefixLog;
		static CStr const mcp_DatabasePrefixSensor;

		TCDistributedActorInstance<CCloudManagerImplementation> mp_ProtocolInterface;
		TCDistributedActorInstance<CDistributedAppSensorReporterImplementation> mp_SensorReporterInterface;
		TCDistributedActorInstance<CDistributedAppSensorReaderImplementation> mp_SensorReaderInterface;
		TCDistributedActorInstance<CDistributedAppLogReporterImplementation> mp_LogReporterInterface;
		TCDistributedActorInstance<CDistributedAppLogReaderImplementation> mp_LogReaderInterface;
		TCDistributedActorInstance<CAppManagerCloudManagerInterfaceImplementation> mp_AppManagerCloudManagerInterface;

		CDistributedAppState &mp_AppState;

		NCloudManagerDatabase::CCloudManagerGlobalStateValue mp_GlobalState;

		CTrustedPermissionSubscription mp_Permissions;

		TCActor<CDatabaseActor> mp_DatabaseActor;
		mint mp_AppManagerRegisterSequence = 0;
		TCMap<CStr, CAppManagerState> mp_AppManagers;

		CActorSubscription mp_MonitorTimerSubscription;

		CActorSubscription mp_CleanupTimerSubscription;
		uint64 mp_RetentionDays = 0;

		TCActor<CDistributedAppSensorStoreLocal> mp_AppSensorStore;
		TCActor<CDistributedAppLogStoreLocal> mp_AppLogStore;

		CNotifications mp_Notifications{*this};
		CUpdateNotifications mp_UpdateNotifications{*this};
		CSensorNotifications mp_SensorNotifications{*this};
		CLogNotifications mp_LogNotifications{*this};

		TCMap<CExpectedOsVersionSubscriptionKey, CExpectedOsVersionSubscription> mp_ExpectedOsVersionSubscriptions;
		mint mp_ExpectedOsVersionSubscriptionNextID = 0;

		bool mp_bDoingCleanup = false;
	};
}
