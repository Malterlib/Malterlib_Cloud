// Copyright © 2019 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_App_CloudManager.h"
#include "Malterlib_Cloud_App_CloudManager_Internal.h"
#include "Malterlib_Cloud_App_CloudManager_Database.h"

#include <Mib/Concurrency/ActorSubscription>

namespace NMib::NCloud::NCloudManager
{
	using namespace NCloudManagerDatabase;

	TCFuture<void> CCloudManagerServer::fp_SetupPermissions()
	{
		TCSet<CStr> Permissions
			{
				"CloudManager/RegisterAppManager"
				, "CloudManager/ReportSensorReadings"
				, "CloudManager/ReportSensorReadingsOnBehalfOf/All"
				, "CloudManager/ReportLogEntries"
				, "CloudManager/ReportLogEntriesOnBehalfOf/All"
				, "CloudManager/ReadAll"
				, "CloudManager/ReadSensors"
				, "CloudManager/ReadLogs"
				, "CloudManager/RemoveAppManager"
				, "CloudManager/RemoveSensor"
				, "CloudManager/RemoveLog"
			}
		;
		mp_AppState.m_TrustManager(&CDistributedActorTrustManager::f_RegisterPermissions, Permissions) > fg_DiscardResult();

		TCVector<CStr> SubscribePermissions{"CloudManager/*"};
		mp_Permissions = co_await mp_AppState.m_TrustManager(&CDistributedActorTrustManager::f_SubscribeToPermissions, SubscribePermissions, fg_ThisActor(this));

		co_return {};
	}

	TCFuture<void> CCloudManagerServer::fp_Publish()
	{
		TCActorResultVector<void> PublishResults;
		mp_ProtocolInterface.f_Publish<CCloudManager>(mp_AppState.m_DistributionManager, this) > PublishResults.f_AddResult();

		if (mp_AppState.m_ConfigDatabase.m_Data.f_GetMemberValue("PublishSensorReporter", false).f_Boolean())
			mp_SensorReporterInterface.f_Publish<CDistributedAppSensorReporter>(mp_AppState.m_DistributionManager, this) > PublishResults.f_AddResult();
		else
			mp_SensorReporterInterface.f_Construct(mp_AppState.m_DistributionManager, this);
		mp_SensorReaderInterface.f_Construct(mp_AppState.m_DistributionManager, this);

		if (mp_AppState.m_ConfigDatabase.m_Data.f_GetMemberValue("PublishLogReporter", false).f_Boolean())
			mp_LogReporterInterface.f_Publish<CDistributedAppLogReporter>(mp_AppState.m_DistributionManager, this) > PublishResults.f_AddResult();
		else
			mp_LogReporterInterface.f_Construct(mp_AppState.m_DistributionManager, this);
		mp_LogReaderInterface.f_Construct(mp_AppState.m_DistributionManager, this);

		co_await (co_await PublishResults.f_GetResults() | g_Unwrap);

		co_return {};
	}

	TCFuture<void> CCloudManagerServer::fp_ReportFiltered(CStr const &_AppManagerID, mint _RegisterSequence, bool _bFiltered, bool _bAccessDenied)
	{
		auto OnResume = co_await fg_OnResume
			(
				[this]() -> CExceptionPointer
				{
					if (f_IsDestroyed())
						return DMibErrorInstance("Shutting down");
					return {};
				}
			)
		;

		auto pAppManager = mp_AppManagers.f_FindEqual(_AppManagerID);
		if (!pAppManager || pAppManager->m_RegisterSequence != _RegisterSequence)
			co_return {};

		if (pAppManager->m_bUpdatedOnce && pAppManager->m_bFiltered == _bFiltered && pAppManager->m_bAccessDenied == _bAccessDenied)
			co_return {};

		pAppManager->m_bUpdatedOnce = true;
		pAppManager->m_bFiltered = _bFiltered;
		pAppManager->m_bAccessDenied = _bAccessDenied;

		TCSet<CStr> RemoveErrors;
		TCMap<CStr, CStr> Errors;
		if (_bAccessDenied)
			Errors["Subscribed Notifications"] = "Access denied from AppManager";
		else if (_bFiltered)
			Errors["Subscribed Notifications"] = "Missing application permissions in AppManager. None or only some applications will be monitored.";
		else
			RemoveErrors["Subscribed Notifications"];

		co_await self(&CCloudManagerServer::fp_ChangeOtherErrors, _AppManagerID, _RegisterSequence, fg_Move(RemoveErrors), fg_Move(Errors));

		co_return {};
	}

	TCFuture<void> CCloudManagerServer::fp_ChangeOtherErrors(CStr const &_AppManagerID, mint _RegisterSequence, TCSet<CStr> const &_Remove, TCMap<CStr, CStr> const &_Add)
	{
		auto OnResume = co_await fg_OnResume
			(
				[this]() -> CExceptionPointer
				{
					if (f_IsDestroyed())
						return DMibErrorInstance("Shutting down");
					return {};
				}
			)
		;

		auto Result = co_await mp_DatabaseActor
			(
				&CDatabaseActor::f_WriteWithCompaction
				, g_ActorFunctorWeak / [this, _AppManagerID, _RegisterSequence, _Remove, _Add]
				(CDatabaseActor::CTransactionWrite &&_Transaction, bool _bCompacting) -> TCFuture<CDatabaseActor::CTransactionWrite>
				{
					co_await ECoroutineFlag_CaptureMalterlibExceptions;

					auto WriteTransaction = fg_Move(_Transaction);
					if (_bCompacting)
						WriteTransaction = co_await self(&CCloudManagerServer::fp_CleanupDatabase, fg_Move(WriteTransaction));

					{
						auto pAppManager = mp_AppManagers.f_FindEqual(_AppManagerID);

						if (!pAppManager || pAppManager->m_RegisterSequence != _RegisterSequence)
							co_return CDatabaseActor::CTransactionWrite::fs_Empty();

						auto WriteCursor = WriteTransaction.m_Transaction.f_WriteCursor();
						CAppManagerKey Key{.m_HostID = _AppManagerID};

						pAppManager->m_Data.m_OtherErrors -= _Remove;
						for (auto &Error : _Add)
							pAppManager->m_Data.m_OtherErrors[_Add.fs_GetKey(Error)] = Error;

						WriteCursor.f_Upsert(Key, pAppManager->m_Data);
					}
					co_return fg_Move(WriteTransaction);
				}
			)
			.f_Wrap()
		;

		if (!Result)
		{
			DMibLogWithCategory(CloudManager, Critical, "Error saving app manager data to database: {}", Result.f_GetExceptionStr());
			co_return Result.f_GetException();
		}

		co_return {};
	}

	TCFuture<void> CCloudManagerServer::fp_ProcessApplicationChanges(CStr const &_AppManagerID, CAppManagerInterface::COnChangeNotificationParams &&_Params)
	{
		auto OnResume = co_await fg_OnResume
			(
				[this]() -> CExceptionPointer
				{
					if (f_IsDestroyed())
						return DMibErrorInstance("Shutting down");
					return {};
				}
			)
		;

		struct CHostChanges
		{
			TCSet<CStr> m_RemovedHosts;
			TCMap<CStr, CTime> m_SeenHosts;
		};

		TCSharedPointer<CHostChanges> pHostChanges = fg_Construct();

		auto Result = co_await mp_DatabaseActor
			(
				&CDatabaseActor::f_WriteWithCompaction
				, g_ActorFunctorWeak / [this, Params = fg_Move(_Params), _AppManagerID, pHostChanges]
				(CDatabaseActor::CTransactionWrite &&_Transaction, bool _bCompacting) -> TCFuture<CDatabaseActor::CTransactionWrite>
				{
					co_await ECoroutineFlag_CaptureMalterlibExceptions;

					CTime Now = CTime::fs_NowUTC();

					auto WriteTransaction = fg_Move(_Transaction);
					if (_bCompacting)
						WriteTransaction = co_await self(&CCloudManagerServer::fp_CleanupDatabase, fg_Move(WriteTransaction));

					if (Params.m_bInitial)
					{
						for (auto ApplicationCursor = WriteTransaction.m_Transaction.f_WriteCursor(CApplicationKey::mc_Prefix, _AppManagerID); ApplicationCursor;)
						{
							if (Params.m_bFiltered || Params.m_bAccessDenied)
							{
								CApplicationValue Value;
								Value = ApplicationCursor.f_Value<CApplicationValue>();

								Value.m_ApplicationInfo.m_Status = "Indeterminate status, fix permissions in app manager";
								Value.m_ApplicationInfo.m_StatusSeverity = CAppManagerInterface::EStatusSeverity_Error;

								ApplicationCursor.f_SetValue(Value);
								++ApplicationCursor;
							}
							else
							{
								auto Value = ApplicationCursor.f_Value<CApplicationValue>();
								if (Value.m_ApplicationInfo.m_HostID)
									pHostChanges->m_RemovedHosts[Value.m_ApplicationInfo.m_HostID];
								ApplicationCursor.f_Delete();
							}
						}
					}

					auto WriteCursor = WriteTransaction.m_Transaction.f_WriteCursor();
					for (auto &Change : Params.m_Changes)
					{
						CApplicationKey Key{.m_AppManagerHostID = _AppManagerID, .m_Application = Change.m_Application};
						if (Change.m_Change.f_IsOfType<CAppManagerInterface::CApplicationChange_Remove>())
						{
							if (WriteCursor.f_FindEqual(Key))
								WriteCursor.f_Delete();
							continue;
						}

						CApplicationValue Value;
						if (WriteCursor.f_FindEqual(Key))
							Value = WriteCursor.f_Value<CApplicationValue>();

						if (Change.m_Change.f_IsOfType<CAppManagerInterface::CApplicationChange_AddOrChangeInfo>())
							Value.m_ApplicationInfo = Change.m_Change.f_GetAsType<CAppManagerInterface::CApplicationChange_AddOrChangeInfo>().m_Info;
						else if (Change.m_Change.f_IsOfType<CAppManagerInterface::CApplicationChange_Status>())
						{
							auto &ChangeValue = Change.m_Change.f_GetAsType<CAppManagerInterface::CApplicationChange_Status>();
							Value.m_ApplicationInfo.m_Status = ChangeValue.m_Status;
							Value.m_ApplicationInfo.m_StatusSeverity = ChangeValue.m_StatusSeverity;
						}
						else
						{
							DMibNeverGetHere;
						}

						if (Value.m_ApplicationInfo.m_HostID)
							pHostChanges->m_SeenHosts[Value.m_ApplicationInfo.m_HostID] = Now;

						WriteCursor.f_Upsert(Key, Value);
					}

					co_return fg_Move(WriteTransaction);
				}
			)
			.f_Wrap()
		;

		if (!pHostChanges->m_RemovedHosts.f_IsEmpty())
			co_await mp_AppSensorStore(&CDistributedAppSensorStoreLocal::f_RemoveHosts, fg_Move(pHostChanges->m_RemovedHosts));

		if (!pHostChanges->m_SeenHosts.f_IsEmpty())
			co_await mp_AppSensorStore(&CDistributedAppSensorStoreLocal::f_SeenHosts, fg_Move(pHostChanges->m_SeenHosts));

		if (!Result)
		{
			DMibLogWithCategory(CloudManager, Critical, "Error saving app manager data to database: {}", Result.f_GetExceptionStr());
			co_return Result.f_GetException();
		}

		co_return {};
	}

	TCFuture<TCActorSubscriptionWithID<>> CCloudManagerServer::CCloudManagerImplementation::f_RegisterAppManager
		(
			TCDistributedActorInterfaceWithID<CAppManagerInterface> &&_AppManager
			, CAppManagerInfo &&_AppManagerInfo
		)
	{
		if (!_AppManager)
			co_return DMibErrorInstance("Invalid app manager");

		auto pThis = m_pThis;
		auto OnResume = co_await fg_OnResume
			(
				[pThis]() -> CExceptionPointer
				{
					if (pThis->f_IsDestroyed())
						return DMibErrorInstance("Shutting down");
					return {};
				}
			)
		;

		auto CallingHostInfo = fg_GetCallingHostInfo();
		auto Auditor = pThis->mp_AppState.f_Auditor({}, CallingHostInfo);

		CStr UniqueHostID = CallingHostInfo.f_GetUniqueHostID();

		NContainer::TCVector<NStr::CStr> Permissions = {"CloudManager/RegisterAppManager"};

		if (!co_await pThis->mp_Permissions.f_HasPermission("Register as app manager", Permissions))
			co_return Auditor.f_AccessDenied("(Register as app manager)", Permissions);

		CStr AppManagerID = CallingHostInfo.f_GetRealHostID();
		CAppManagerKey DatabaseKey{.m_HostID = AppManagerID};

		CAppManagerValue Data;

		if (auto pAppManager = pThis->mp_AppManagers.f_FindEqual(AppManagerID))
		{
			Data = pAppManager->m_Data;
			pThis->mp_AppManagers.f_Remove(pAppManager); // Remove any old manager
		}
		else
		{
			auto ReadTransaction = co_await (pThis->mp_DatabaseActor(&CDatabaseActor::f_OpenTransactionRead) % Auditor);
			ReadTransaction.m_Transaction.f_Get(DatabaseKey, Data); // If available use old data
		}

		auto RegisterSequence = ++pThis->mp_AppManagerRegisterSequence;

		Data.m_Info = fg_Move(_AppManagerInfo);
		Data.m_LastSeen = CTime::fs_NowUTC();
		Data.m_bActive = true;

		co_await (pThis->fp_SaveAppManagerData(DatabaseKey, Data) % Auditor);

		auto &AppManager = pThis->mp_AppManagers[AppManagerID];
		AppManager.m_Interface = fg_Move(_AppManager);
		AppManager.m_Data = fg_Move(Data);
		AppManager.m_UniqueHostID = UniqueHostID;
		AppManager.m_RegisterSequence = RegisterSequence;

		auto [ChangeNotificationsSubscription, UpdateNotificationsSubscription] = co_await
			(
				AppManager.m_Interface.f_CallActor(&CAppManagerInterface::f_SubscribeChangeNotifications)
				(
					CAppManagerInterface::CSubscribeChangeNotifications
					{
						.m_fOnNotification = g_ActorFunctor / [pThis, RegisterSequence, AppManagerID, AllowDestroy = g_AllowWrongThreadDestroy]
						(CAppManagerInterface::COnChangeNotificationParams &&_Params) -> TCFuture<void>
						{
							auto pAppManager = pThis->mp_AppManagers.f_FindEqual(AppManagerID);
							if (!pAppManager || pAppManager->m_RegisterSequence != RegisterSequence)
								co_return {};

							co_await pThis->self(&CCloudManagerServer::fp_ReportFiltered, AppManagerID, RegisterSequence, _Params.m_bFiltered, _Params.m_bAccessDenied);
							co_await pThis->self(&CCloudManagerServer::fp_ProcessApplicationChanges, AppManagerID, fg_Move(_Params));

							co_return {};
						}
						, .m_bWaitForNotification = false
					}
				)
				+ AppManager.m_Interface.f_CallActor(&CAppManagerInterface::f_SubscribeUpdateNotifications)
				(
					CAppManagerInterface::CSubscribeUpdateNotifications
					{
						.m_fOnNotification = g_ActorFunctor / [pThis, RegisterSequence, AppManagerID, AllowDestroy = g_AllowWrongThreadDestroy]
						(CAppManagerInterface::CUpdateNotification const &_Notification) -> TCFuture<void>
						{
							auto pAppManager = pThis->mp_AppManagers.f_FindEqual(AppManagerID);
							if (!pAppManager || pAppManager->m_RegisterSequence != RegisterSequence)
								co_return {};

							co_await pThis->mp_UpdateNotifications.f_ProcessApplicationUpdateNotification(AppManagerID, _Notification);

							co_return {};
						}
						, .m_LastSeenUniqueSequence = AppManager.m_Data.m_LastSeenUpdateNotificationSequence
						, .m_bWaitForNotification = false
					}
				)
			)
			.f_Wrap()
		;

		auto pAppManager = pThis->mp_AppManagers.f_FindEqual(AppManagerID);
		if (!pAppManager || pAppManager->m_RegisterSequence != RegisterSequence)
			co_return {};

		if (ChangeNotificationsSubscription)
		{
			pAppManager->m_ChangeNotificationsSubscription = fg_Move(*ChangeNotificationsSubscription);
			co_await pThis->self(&CCloudManagerServer::fp_ChangeOtherErrors, AppManagerID, RegisterSequence, TCSet<CStr>{"Subscribe Notifications"}, TCMap<CStr, CStr>{});
		}
		else
		{
			co_await pThis->self
				(
					&CCloudManagerServer::fp_ChangeOtherErrors
					, AppManagerID
					, RegisterSequence
					, TCSet<CStr>{}
					, TCMap<CStr, CStr>{{"Subscribe Notifications", ChangeNotificationsSubscription.f_GetExceptionStr()}}
				)
			;
		}

		if (UpdateNotificationsSubscription)
		{
			pAppManager->m_UpdateNotificationsSubscription = fg_Move(*UpdateNotificationsSubscription);
			co_await pThis->self(&CCloudManagerServer::fp_ChangeOtherErrors, AppManagerID, RegisterSequence, TCSet<CStr>{"Subscribe Update Notifications"}, TCMap<CStr, CStr>{});
		}
		else
		{
			co_await pThis->self
				(
					&CCloudManagerServer::fp_ChangeOtherErrors
					, AppManagerID
					, RegisterSequence
					, TCSet<CStr>{}
					, TCMap<CStr, CStr>{{"Subscribe Update Notifications", UpdateNotificationsSubscription.f_GetExceptionStr()}}
				)
			;
		}

		Auditor.f_Info("App manager registered");

		co_return g_ActorSubscription / [pThis, RegisterSequence, AppManagerID, DatabaseKey]() -> TCFuture<void>
			{
				auto pAppManager = pThis->mp_AppManagers.f_FindEqual(AppManagerID);

				if (pAppManager && pAppManager->m_ChangeNotificationsSubscription)
					co_await pAppManager->m_ChangeNotificationsSubscription->f_Destroy();

				pAppManager = pThis->mp_AppManagers.f_FindEqual(AppManagerID);
				if (!pAppManager || pAppManager->m_RegisterSequence != RegisterSequence)
					co_return {};

				if (pAppManager && pAppManager->m_UpdateNotificationsSubscription)
					co_await pAppManager->m_ChangeNotificationsSubscription->f_Destroy();

				pAppManager = pThis->mp_AppManagers.f_FindEqual(AppManagerID);
				if (!pAppManager || pAppManager->m_RegisterSequence != RegisterSequence)
					co_return {};

				auto Data = fg_Move(pAppManager->m_Data);
				Data.m_LastSeen = CTime::fs_NowUTC();
				Data.m_bActive = false;

				auto Interface = fg_Move(pAppManager->m_Interface);
				pThis->mp_AppManagers.f_Remove(AppManagerID);

				co_await (Interface.f_Destroy() + pThis->fp_SaveAppManagerData(DatabaseKey, Data)).f_Wrap();

				co_return {};
			}
		;
	}

	auto CCloudManagerServer::CCloudManagerImplementation::f_EnumAppManagers() -> TCFuture<TCMap<CStr, CAppManagerDynamicInfo>>
	{
		auto pThis = m_pThis;
		auto OnResume = co_await fg_OnResume
			(
				[pThis]() -> CExceptionPointer
				{
					if (pThis->f_IsDestroyed())
						return DMibErrorInstance("Shutting down");
					return {};
				}
			)
		;

		auto Auditor = pThis->mp_AppState.f_Auditor();

		NContainer::TCVector<NStr::CStr> Permissions = {"CloudManager/ReadAll"};

		if (!co_await pThis->mp_Permissions.f_HasPermission("Enum app managers", Permissions))
			co_return Auditor.f_AccessDenied("(Enum app managers)", Permissions);

		TCMap<CStr, CAppManagerDynamicInfo> Return;

		co_await (pThis->fp_UpdateAppManagerState() % Auditor);

		{
			auto CaptureScope = co_await (g_CaptureExceptions % "Failed to read app managers from database");

			auto ReadTransaction = co_await (pThis->mp_DatabaseActor(&CDatabaseActor::f_OpenTransactionRead) % Auditor);

			for (auto AppManagers = ReadTransaction.m_Transaction.f_ReadCursor(CAppManagerKey::mc_Prefix); AppManagers; ++AppManagers)
			{
				auto Key = AppManagers.f_Key<CAppManagerKey>();
				auto Value = AppManagers.f_Value<CAppManagerValue>();

				auto &OutAppManager = Return[Key.m_HostID];
				OutAppManager = Value.m_Info;
				OutAppManager.m_LastSeen = Value.m_LastSeen;
				OutAppManager.m_LastConnectionError = Value.m_LastConnectionError;
				OutAppManager.m_LastConnectionErrorTime = Value.m_LastConnectionErrorTime;
				OutAppManager.m_OtherErrors = Value.m_OtherErrors;
				OutAppManager.m_bActive = Value.m_bActive;
			}
		}

		Auditor.f_Info("Enum app managers");

		co_return fg_Move(Return);
	}

	auto CCloudManagerServer::CCloudManagerImplementation::f_EnumApplications() -> TCFuture<TCMap<CApplicationKey, CApplicationInfo>>
	{
		auto pThis = m_pThis;
		auto OnResume = co_await fg_OnResume
			(
				[pThis]() -> CExceptionPointer
				{
					if (pThis->f_IsDestroyed())
						return DMibErrorInstance("Shutting down");
					return {};
				}
			)
		;

		auto Auditor = pThis->mp_AppState.f_Auditor();

		NContainer::TCVector<NStr::CStr> Permissions = {"CloudManager/ReadAll"};

		if (!co_await pThis->mp_Permissions.f_HasPermission("Enum applications", Permissions))
			co_return Auditor.f_AccessDenied("(Enum applications)", Permissions);

		TCMap<CApplicationKey, CApplicationInfo> Return;

		{
			auto CaptureScope = co_await (g_CaptureExceptions % "Failed to read applications from database");

			auto ReadTransaction = co_await (pThis->mp_DatabaseActor(&CDatabaseActor::f_OpenTransactionRead) % Auditor);

			for (auto Applications = ReadTransaction.m_Transaction.f_ReadCursor(NCloudManagerDatabase::CApplicationKey::mc_Prefix); Applications; ++Applications)
			{
				auto Key = Applications.f_Key<NCloudManagerDatabase::CApplicationKey>();
				auto Value = Applications.f_Value<CApplicationValue>();

				CApplicationKey ApplicationKey{Key.m_AppManagerHostID, Key.m_Application};

				auto &OutApplication = Return[ApplicationKey];
				OutApplication.m_ApplicationInfo = Value.m_ApplicationInfo;
			}
		}

		Auditor.f_Info("Enum applications");

		co_return fg_Move(Return);
	}

	TCFuture<void> CCloudManagerServer::CCloudManagerImplementation::f_RemoveAppManager(NStr::CStr const &_AppManagerHostID)
	{
		auto pThis = m_pThis;
		auto OnResume = co_await fg_OnResume
			(
				[pThis]() -> CExceptionPointer
				{
					if (pThis->f_IsDestroyed())
						return DMibErrorInstance("Shutting down");
					return {};
				}
			)
		;

		auto Auditor = pThis->mp_AppState.f_Auditor();

		NContainer::TCVector<NStr::CStr> Permissions = {"CloudManager/RemoveAppManager"};

		if (!co_await pThis->mp_Permissions.f_HasPermission("Remove app manager", Permissions))
			co_return Auditor.f_AccessDenied("(Remove app manager)", Permissions);

		auto *pAppManager = pThis->mp_AppManagers.f_FindEqual(_AppManagerHostID);
		if (pAppManager)
		{
			auto ChangeSubscription = fg_Move(pAppManager->m_ChangeNotificationsSubscription);
			auto UpdateSubscription = fg_Move(pAppManager->m_UpdateNotificationsSubscription);
			auto Interface = fg_Move(pAppManager->m_Interface);
			pThis->mp_AppManagers.f_Remove(pAppManager);

			if (ChangeSubscription)
				co_await ChangeSubscription->f_Destroy().f_Wrap();

			if (UpdateSubscription)
				co_await UpdateSubscription->f_Destroy().f_Wrap();

			if (Interface)
				co_await Interface.f_Destroy().f_Wrap();
		}

		co_await (pThis->self(&CCloudManagerServer::fp_RemoveAppManagerData, _AppManagerHostID) % Auditor);

		Auditor.f_Info("Remove App Manager");

		co_return {};
	}

	TCFuture<uint32> CCloudManagerServer::CCloudManagerImplementation::f_RemoveSensor(CDistributedAppSensorReporter::CSensorInfoKey &&_SensorInfoKey)
	{
		auto pThis = m_pThis;
		auto OnResume = co_await fg_OnResume
			(
				[pThis]() -> CExceptionPointer
				{
					if (pThis->f_IsDestroyed())
						return DMibErrorInstance("Shutting down");
					return {};
				}
			)
		;

		auto Auditor = pThis->mp_AppState.f_Auditor();

		NContainer::TCVector<NStr::CStr> Permissions = {"CloudManager/RemoveSensor"};

		if (!co_await pThis->mp_Permissions.f_HasPermission("Remove sensor", Permissions))
			co_return Auditor.f_AccessDenied("(Remove sensor)", Permissions);

		auto nRemoved = co_await pThis->mp_AppSensorStore(&CDistributedAppSensorStoreLocal::f_RemoveSensors, TCSet<CDistributedAppSensorReporter::CSensorInfoKey>{fg_Move(_SensorInfoKey)});

		Auditor.f_Info("Remove Sensor");

		co_return nRemoved;
	}

	TCFuture<uint32> CCloudManagerServer::CCloudManagerImplementation::f_RemoveLog(CDistributedAppLogReporter::CLogInfoKey &&_LogInfoKey)
	{
		auto pThis = m_pThis;
		auto OnResume = co_await fg_OnResume
			(
				[pThis]() -> CExceptionPointer
				{
					if (pThis->f_IsDestroyed())
						return DMibErrorInstance("Shutting down");
					return {};
				}
			)
		;

		auto Auditor = pThis->mp_AppState.f_Auditor();

		NContainer::TCVector<NStr::CStr> Permissions = {"CloudManager/RemoveLog"};

		if (!co_await pThis->mp_Permissions.f_HasPermission("Remove sensor", Permissions))
			co_return Auditor.f_AccessDenied("(Remove sensor)", Permissions);

		auto nRemoved = co_await pThis->mp_AppLogStore(&CDistributedAppLogStoreLocal::f_RemoveLogs, TCSet<CDistributedAppLogReporter::CLogInfoKey>{fg_Move(_LogInfoKey)});

		Auditor.f_Info("Remove Log");

		co_return nRemoved;
	}

	TCFuture<TCDistributedActorInterfaceWithID<CDistributedAppSensorReporter>> CCloudManagerServer::CCloudManagerImplementation::f_GetSensorReporter()
	{
		auto pThis = m_pThis;
		auto OnResume = co_await fg_OnResume
			(
				[pThis]() -> CExceptionPointer
				{
					if (pThis->f_IsDestroyed())
						return DMibErrorInstance("Shutting down");
					return {};
				}
			)
		;

		auto Auditor = pThis->mp_AppState.f_Auditor();

		NContainer::TCVector<NStr::CStr> Permissions = {"CloudManager/ReportSensorReadings"};

		if (!co_await pThis->mp_Permissions.f_HasPermission("Get sensor reporter", Permissions))
			co_return Auditor.f_AccessDenied("(Get sensor reporter)", Permissions);

		Auditor.f_Info("Get sensor reporter");

		co_return TCDistributedActorInterfaceWithID<CDistributedAppSensorReporter>
			(
				pThis->mp_SensorReporterInterface.m_Actor->f_ShareInterface<CDistributedAppSensorReporter>()
				, g_ActorSubscription / []
				{
				}
			)
		;
	}

	TCFuture<TCDistributedActorInterfaceWithID<CDistributedAppSensorReader>> CCloudManagerServer::CCloudManagerImplementation::f_GetSensorReader()
	{
		auto pThis = m_pThis;
		auto OnResume = co_await fg_OnResume
			(
				[pThis]() -> CExceptionPointer
				{
					if (pThis->f_IsDestroyed())
						return DMibErrorInstance("Shutting down");
					return {};
				}
			)
		;

		auto Auditor = pThis->mp_AppState.f_Auditor();

		auto Permissions = fsp_SensorReadPermissions();

		if (!co_await pThis->mp_Permissions.f_HasPermission("Get sensor reader", Permissions))
			co_return Auditor.f_AccessDenied("(Get sensor reader)", Permissions);

		Auditor.f_Info("Get sensor reader");

		co_return TCDistributedActorInterfaceWithID<CDistributedAppSensorReader>
			(
				pThis->mp_SensorReaderInterface.m_Actor->f_ShareInterface<CDistributedAppSensorReader>()
				, g_ActorSubscription / []
				{
				}
			)
		;
	}

	TCFuture<TCDistributedActorInterfaceWithID<CDistributedAppLogReporter>> CCloudManagerServer::CCloudManagerImplementation::f_GetLogReporter()
	{
		auto pThis = m_pThis;
		auto OnResume = co_await fg_OnResume
			(
				[pThis]() -> CExceptionPointer
				{
					if (pThis->f_IsDestroyed())
						return DMibErrorInstance("Shutting down");
					return {};
				}
			)
		;

		auto Auditor = pThis->mp_AppState.f_Auditor();

		NContainer::TCVector<NStr::CStr> Permissions = {"CloudManager/ReportLogEntries"};

		if (!co_await pThis->mp_Permissions.f_HasPermission("Get log reporter", Permissions))
			co_return Auditor.f_AccessDenied("(Get log reporter)", Permissions);

		Auditor.f_Info("Get log reporter");

		co_return TCDistributedActorInterfaceWithID<CDistributedAppLogReporter>
			(
				pThis->mp_LogReporterInterface.m_Actor->f_ShareInterface<CDistributedAppLogReporter>()
				, g_ActorSubscription / []
				{
				}
			)
		;
	}

	TCFuture<TCDistributedActorInterfaceWithID<CDistributedAppLogReader>> CCloudManagerServer::CCloudManagerImplementation::f_GetLogReader()
	{
		auto pThis = m_pThis;
		auto OnResume = co_await fg_OnResume
			(
				[pThis]() -> CExceptionPointer
				{
					if (pThis->f_IsDestroyed())
						return DMibErrorInstance("Shutting down");
					return {};
				}
			)
		;

		auto Auditor = pThis->mp_AppState.f_Auditor();

		auto Permissions = fsp_LogReadPermissions();

		if (!co_await pThis->mp_Permissions.f_HasPermission("Get log reader", Permissions))
			co_return Auditor.f_AccessDenied("(Get log reader)", Permissions);

		Auditor.f_Info("Get log reader");

		co_return TCDistributedActorInterfaceWithID<CDistributedAppLogReader>
			(
				pThis->mp_LogReaderInterface.m_Actor->f_ShareInterface<CDistributedAppLogReader>()
				, g_ActorSubscription / []
				{
				}
			)
		;
	}
}
