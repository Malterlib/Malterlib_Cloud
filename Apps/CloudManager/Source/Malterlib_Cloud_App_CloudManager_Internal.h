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
#include <Mib/Web/Curl>
#include <Mib/Web/Slack>

#include "Malterlib_Cloud_App_CloudManager.h"
#include "Malterlib_Cloud_App_CloudManager_Database.h"
#include "Malterlib_Cloud_App_CloudManager_Notifications.h"
#include "Malterlib_Cloud_App_CloudManager_Notifications_Updates.h"
#include "Malterlib_Cloud_App_CloudManager_Notifications_Sensors.h"

namespace NMib::NCloud::NCloudManager
{
	struct CCloudManagerServer : public CActor
	{
		friend struct CNotifications;
		friend struct CUpdateNotifications;
		friend struct CSensorNotifications;

		using CActorHolder = CDelegatedActorHolder;

		CCloudManagerServer(CDistributedAppState &_AppState);
		~CCloudManagerServer();

		struct CCloudManagerImplementation : public CCloudManager
		{
			TCFuture<TCActorSubscriptionWithID<>> f_RegisterAppManager(TCDistributedActorInterfaceWithID<CAppManagerInterface> &&_AppManager, CAppManagerInfo &&_AppManagerInfo) override;
			TCFuture<TCMap<CStr, CAppManagerDynamicInfo>> f_EnumAppManagers() override;
			TCFuture<TCMap<CApplicationKey, CApplicationInfo>> f_EnumApplications() override;
			TCFuture<void> f_RemoveAppManager(CStr const &_AppManagerHostID) override;
			TCFuture<uint32> f_RemoveSensor(CDistributedAppSensorReporter::CSensorInfoKey &&_SensorInfoKey) override;
			TCFuture<TCDistributedActorInterfaceWithID<CDistributedAppSensorReporter>> f_GetSensorReporter() override;
			TCFuture<TCDistributedActorInterfaceWithID<CDistributedAppSensorReader>> f_GetSensorReader() override;
			TCFuture<TCDistributedActorInterfaceWithID<CDistributedAppLogReporter>> f_GetLogReporter() override;
			TCFuture<TCDistributedActorInterfaceWithID<CDistributedAppLogReader>> f_GetLogReader() override;

			DMibDelegatedActorImplementation(CCloudManagerServer);
		};

		struct CDistributedAppSensorReporterImplementation : public CDistributedAppSensorReporter
		{
			TCFuture<CSensorReporter> f_OpenSensorReporter(CSensorInfo &&_SensorInfo) override;

			DMibDelegatedActorImplementation(CCloudManagerServer);
		};

		struct CDistributedAppSensorReaderImplementation : public CDistributedAppSensorReader
		{
			TCFuture<TCAsyncGenerator<TCVector<CDistributedAppSensorReporter::CSensorInfo>>> f_GetSensors(CGetSensors &&_Params) override;
			auto f_GetSensorReadings(CGetSensorReadings &&_Params) -> TCFuture<TCAsyncGenerator<TCVector<CDistributedAppSensorReader_SensorKeyAndReading>>> override;
			auto f_GetSensorStatus(CGetSensorStatus &&_Params) -> TCFuture<TCAsyncGenerator<TCVector<CDistributedAppSensorReader_SensorKeyAndReading>>> override;
			auto f_SubscribeSensors(TCVector<CDistributedAppSensorReader_SensorFilter> &&_Filters, TCActorFunctorWithID<TCFuture<void> (CSensorChange &&_Change)> &&_fOnChange)
				-> TCFuture<TCActorSubscriptionWithID<>> override
			;
			auto f_SubscribeSensorReadings
				(
					TCVector<CDistributedAppSensorReader_SensorReadingSubscriptionFilter> &&_Filters
					, TCActorFunctorWithID<TCFuture<void> (CDistributedAppSensorReader_SensorKeyAndReading &&_Reading)> &&_fOnReading
				)
				-> TCFuture<TCActorSubscriptionWithID<>> override
			;
			auto f_SubscribeSensorStatus
				(
					TCVector<CDistributedAppSensorReader_SensorStatusFilter> &&_Filters
					, TCActorFunctorWithID<TCFuture<void> (CDistributedAppSensorReader_SensorKeyAndReading &&_Reading)> &&_fOnReading
				)
				-> TCFuture<TCActorSubscriptionWithID<>> override
			;

			DMibDelegatedActorImplementation(CCloudManagerServer);
		};

		struct CDistributedAppLogReporterImplementation : public CDistributedAppLogReporter
		{
			TCFuture<CLogReporter> f_OpenLogReporter(CLogInfo &&_LogInfo) override;

			DMibDelegatedActorImplementation(CCloudManagerServer);
		};

		struct CDistributedAppLogReaderImplementation : public CDistributedAppLogReader
		{
			TCFuture<TCAsyncGenerator<TCVector<CDistributedAppLogReporter::CLogInfo>>> f_GetLogs(CGetLogs &&_Params) override;
			auto f_GetLogEntries(CGetLogEntries &&_Params) -> TCFuture<TCAsyncGenerator<TCVector<CDistributedAppLogReader_LogKeyAndEntry>>> override;
			auto f_SubscribeLogs(TCVector<CDistributedAppLogReader_LogFilter> &&_Filter, TCActorFunctorWithID<TCFuture<void> (CLogChange &&_Change)> &&_fOnChange)
				-> TCFuture<TCActorSubscriptionWithID<>> override
			;
			auto f_SubscribeLogEntries
				(
					TCVector<CDistributedAppLogReader_LogEntrySubscriptionFilter> &&_Filters
					, TCActorFunctorWithID<TCFuture<void> (CDistributedAppLogReader_LogKeyAndEntry &&_Entry)> &&_fOnEntry
				)
				-> TCFuture<TCActorSubscriptionWithID<>> override
			;

			DMibDelegatedActorImplementation(CCloudManagerServer);
		};

		TCFuture<void> f_Init();

	private:
		struct CAppManagerState
		{
			CStr const &f_AppManagerID() const;
			NCloudManagerDatabase::CAppManagerKey f_DatabaseKey() const;

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

		TCFuture<void> fp_Destroy() override;
		TCFuture<void> fp_Publish();
		TCFuture<void> fp_SetupDatabase();
		TCFuture<void> fp_SetupPermissions();
		TCFuture<void> fp_SetupMonitor();
		TCFuture<void> fp_PerformCleanup();
		TCFuture<void> fp_SetupCleanup();
		TCFuture<void> fp_SetupSensorStore();
		TCFuture<void> fp_SetupLogStore();
		TCFuture<NDatabase::CDatabaseActor::CTransactionWrite> fp_CleanupDatabase(NDatabase::CDatabaseActor::CTransactionWrite &&_WriteTransaction);
		TCFuture<CDatabaseActor::CTransactionWrite> fp_SaveGlobalState(CDatabaseActor::CTransactionWrite &&_Transaction);
		TCFuture<void> fp_UpdateAppManagerState();
		TCFuture<void> fp_SaveAppManagerData(NCloudManagerDatabase::CAppManagerKey _Key, NCloudManagerDatabase::CAppManagerValue _Data);
		TCFuture<void> fp_RemoveAppManagerData(CStr const &_HostID);
		TCFuture<void> fp_ProcessApplicationChanges(CStr const &_AppManagerID, CAppManagerInterface::COnChangeNotificationParams &&_Params);
		TCFuture<void> fp_ChangeOtherErrors(CStr const &_AppManagerID, mint _RegisterSequence, TCSet<CStr> const &_Remove, TCMap<CStr, CStr> const &_Add);
		TCFuture<void> fp_ReportFiltered(CStr const &_AppManagerID, mint _RegisterSequence, bool _bFiltered, bool _bAccessDenied);

		static TCVector<CStr> fsp_SensorReadPermissions();
		static TCVector<CStr> fsp_LogReadPermissions();

		static CStr const mc_DatabasePrefixLog;
		static CStr const mc_DatabasePrefixSensor;

		TCDistributedActorInstance<CCloudManagerImplementation> mp_ProtocolInterface;
		TCDistributedActorInstance<CDistributedAppSensorReporterImplementation> mp_SensorReporterInterface;
		TCDistributedActorInstance<CDistributedAppSensorReaderImplementation> mp_SensorReaderInterface;
		TCDistributedActorInstance<CDistributedAppLogReporterImplementation> mp_LogReporterInterface;
		TCDistributedActorInstance<CDistributedAppLogReaderImplementation> mp_LogReaderInterface;
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
	};
}
