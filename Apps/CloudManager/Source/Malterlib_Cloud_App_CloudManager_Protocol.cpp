// Copyright © 2019 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_App_CloudManager.h"
#include "Malterlib_Cloud_App_CloudManager_Internal.h"
#include "Malterlib_Cloud_App_CloudManager_Database.h"

#include <Mib/Concurrency/ActorSubscription>
#include <Mib/Concurrency/LogError>

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
				, "CloudManager/SnoozeSensor"
				, "CloudManager/SetExpectedOsVersion"
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

		mp_AppManagerCloudManagerInterface.f_Construct(mp_AppState.m_DistributionManager, this);

		co_await (co_await PublishResults.f_GetResults() | g_Unwrap);

		co_return {};
	}

	TCFuture<void> CCloudManagerServer::fp_ReportFiltered(CStr const &_AppManagerID, mint _RegisterSequence, bool _bFiltered, bool _bAccessDenied)
	{
		auto OnResume = co_await f_CheckDestroyedOnResume();

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
		auto OnResume = co_await f_CheckDestroyedOnResume();

		auto Result = co_await mp_DatabaseActor
			(
				&CDatabaseActor::f_WriteWithCompaction
				, g_ActorFunctorWeak / [ThisActor = fg_ThisActor(this), pThis = this, _AppManagerID, _RegisterSequence, _Remove, _Add]
				(CDatabaseActor::CTransactionWrite &&_Transaction, bool _bCompacting) -> TCFuture<CDatabaseActor::CTransactionWrite>
				{
					co_await ECoroutineFlag_CaptureMalterlibExceptions;

					auto WriteTransaction = fg_Move(_Transaction);
					if (_bCompacting)
						WriteTransaction = fg_Move((co_await ThisActor(&CCloudManagerServer::fp_CleanupDatabase, fg_Move(WriteTransaction), fg_Construct())).m_Transaction);

					NCloudManagerDatabase::CAppManagerValue AppManagerData;
					{
						auto pAppManager = pThis->mp_AppManagers.f_FindEqual(_AppManagerID);

						if (!pAppManager || pAppManager->m_RegisterSequence != _RegisterSequence)
							co_return CDatabaseActor::CTransactionWrite::fs_Empty();

						pAppManager->m_Data.m_OtherErrors -= _Remove;
						for (auto &Error : _Add)
							pAppManager->m_Data.m_OtherErrors[_Add.fs_GetKey(Error)] = Error;

						AppManagerData = pAppManager->m_Data;
					}

					co_await fg_ContinueRunningOnActor(WriteTransaction.f_Checkout());

					auto WriteCursor = WriteTransaction.m_Transaction.f_WriteCursor();

					CAppManagerKey Key{.m_HostID = _AppManagerID};
					WriteCursor.f_Upsert(Key, AppManagerData);

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
		auto OnResume = co_await f_CheckDestroyedOnResume();

		struct CHostChanges
		{
			TCSet<CStr> m_RemovedHosts;
			TCMap<CStr, CTime> m_SeenHostsLog;
			TCMap<CStr, CDistributedAppSensorStoreLocal::CSeenHost> m_SeenHostsSensor;
		};

		TCSharedPointer<CHostChanges> pHostChanges = fg_Construct();

		auto Result = co_await mp_DatabaseActor
			(
				&CDatabaseActor::f_WriteWithCompaction
				, g_ActorFunctorWeak / [ThisActor = fg_ThisActor(this), Params = fg_Move(_Params), _AppManagerID, pHostChanges]
				(CDatabaseActor::CTransactionWrite &&_Transaction, bool _bCompacting) -> TCFuture<CDatabaseActor::CTransactionWrite>
				{
					co_await ECoroutineFlag_CaptureMalterlibExceptions;

					CTime Now = CTime::fs_NowUTC();

					auto WriteTransaction = fg_Move(_Transaction);
					if (_bCompacting)
						WriteTransaction = fg_Move((co_await ThisActor(&CCloudManagerServer::fp_CleanupDatabase, fg_Move(WriteTransaction), fg_Construct())).m_Transaction);

					co_await fg_ContinueRunningOnActor(WriteTransaction.f_Checkout());

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
						{
							pHostChanges->m_SeenHostsLog[Value.m_ApplicationInfo.m_HostID] = Now;
							pHostChanges->m_SeenHostsSensor[Value.m_ApplicationInfo.m_HostID].m_TimeSeen = Now;
						}

						WriteCursor.f_Upsert(Key, Value);
					}

					co_return fg_Move(WriteTransaction);
				}
			)
			.f_Wrap()
		;

		if (!pHostChanges->m_RemovedHosts.f_IsEmpty())
		{
			co_await mp_AppSensorStore(&CDistributedAppSensorStoreLocal::f_RemoveHosts, pHostChanges->m_RemovedHosts);
			co_await mp_AppLogStore(&CDistributedAppLogStoreLocal::f_RemoveHosts, pHostChanges->m_RemovedHosts);
		}

		if (!pHostChanges->m_SeenHostsSensor.f_IsEmpty())
			co_await mp_AppSensorStore(&CDistributedAppSensorStoreLocal::f_SeenHosts, pHostChanges->m_SeenHostsSensor);

		if (!pHostChanges->m_SeenHostsLog.f_IsEmpty())
			co_await mp_AppLogStore(&CDistributedAppLogStoreLocal::f_SeenHosts, pHostChanges->m_SeenHostsLog);

		if (!Result)
		{
			DMibLogWithCategory(CloudManager, Critical, "Error saving app manager data to database: {}", Result.f_GetExceptionStr());
			co_return Result.f_GetException();
		}

		co_return {};
	}

	TCFuture<void> CCloudManagerServer::CAppManagerState::f_Destroy(CCloudManagerServer &_This)
	{
		co_await ECoroutineFlag_AllowReferences;

		CAppManagerKey DatabaseKey{.m_HostID = f_AppManagerID()};

		TCDistributedActorInterfaceWithID<CAppManagerInterface> Interface;
		NCloudManagerDatabase::CAppManagerValue Data;
		TCActorResultVector<void> DestroyResults;
		{
			if (m_ChangeNotificationsSubscription)
				fg_Exchange(m_ChangeNotificationsSubscription, nullptr)->f_Destroy() > DestroyResults.f_AddResult();

			if (m_UpdateNotificationsSubscription)
				fg_Exchange(m_UpdateNotificationsSubscription, nullptr)->f_Destroy() > DestroyResults.f_AddResult();

			Data = fg_Move(m_Data);
			Data.m_LastSeen = CTime::fs_NowUTC();
			Data.m_bActive = false;

			Interface = fg_Move(m_Interface);
		}

		auto [InterfaceDestroyResult, SaveAppManagerDataDestroyResult, SubscriptionDestroyResult] = co_await
			(Interface.f_Destroy() + _This.fp_SaveAppManagerData(DatabaseKey, fg_Move(Data)) + DestroyResults.f_GetUnwrappedResults()).f_Wrap()
		;

		if (!SubscriptionDestroyResult)
			SubscriptionDestroyResult > fg_LogWarning("CloudManager", "Failed to destroy registered app manager subscriptions");

		if (!InterfaceDestroyResult)
			InterfaceDestroyResult > fg_LogWarning("CloudManager", "Failed to destroy registered app manager interface");

		if (!SaveAppManagerDataDestroyResult)
			SaveAppManagerDataDestroyResult > fg_LogWarning("CloudManager", "Failed to store app manager data in subscription cleanup");

		co_return {};
	}

	TCFuture<CCloudManager::CRegisterAppManagerResult> CCloudManagerServer::CCloudManagerImplementation::f_RegisterAppManager
		(
			TCDistributedActorInterfaceWithID<CAppManagerInterface> &&_AppManager
			, CAppManagerInfo &&_AppManagerInfo
		)
	{
		if (!_AppManager)
			co_return DMibErrorInstance("Invalid app manager");

		auto pThis = m_pThis;
		auto OnResume = co_await pThis->f_CheckDestroyedOnResume();

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
			auto DestroyFuture = pAppManager->f_Destroy(*pThis);

			pThis->mp_AppManagers.f_Remove(pAppManager); // Remove any old manager

			co_await fg_Move(DestroyFuture).f_Timeout(30.0, "Timed out waiting for old app manager destruction").f_Wrap()
				> fg_LogWarning("CloudManager", "Failed to destroy old app manager registration")
			;
		}
		else
		{
			auto ReadTransaction = co_await (pThis->mp_DatabaseActor(&CDatabaseActor::f_OpenTransactionRead) % Auditor);
			Data = co_await fg_Move(ReadTransaction).f_BlockingDispatch
				(
					[DatabaseKey](CDatabaseActor::CTransactionRead &&_ReadTransaction)
					{
						CAppManagerValue Data;
						_ReadTransaction.m_Transaction.f_Get(DatabaseKey, Data); // If available use old data
						return Data;
					}
				)
			;
		}

		auto RegisterSequence = ++pThis->mp_AppManagerRegisterSequence;

		Data.m_Info = fg_Move(_AppManagerInfo);
		Data.m_LastSeen = CTime::fs_NowUTC();
		Data.m_bActive = true;
		Data.m_PauseReportingFor = fp32::fs_QNan();

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

		co_return
			{
				.m_AppManagerCloudManagerInterface = TCDistributedActorInterfaceWithID<CAppManagerCloudManagerInterface>
				(
					pThis->mp_AppManagerCloudManagerInterface.m_Actor->f_ShareInterface<CAppManagerCloudManagerInterface>()
					, g_ActorSubscription / [pThis, RegisterSequence, AppManagerID, DatabaseKey]() -> TCFuture<void>
					{
						CAppManagerState *pAppManager = pThis->mp_AppManagers.f_FindEqual(AppManagerID);
						if (!pAppManager || pAppManager->m_RegisterSequence != RegisterSequence)
							co_return DMibErrorInstance("App Manager was removed before subscription finished");

						auto DestroyFuture = pAppManager->f_Destroy(*pThis);
						pThis->mp_AppManagers.f_Remove(AppManagerID);

						co_await fg_Move(DestroyFuture);

						co_return {};
					}
				)
			}
		;
	}

	NConcurrency::TCFuture<void> CCloudManagerServer::CAppManagerCloudManagerInterfaceImplementation::f_PauseReporting(fp32 _SecondsToPause)
	{
		co_await ECoroutineFlag_AllowReferences;

		auto pThis = m_pThis;

		auto CallingHostInfo = fg_GetCallingHostInfo();
		auto Auditor = pThis->mp_AppState.f_Auditor({}, CallingHostInfo);
		CStr AppManagerID = CallingHostInfo.f_GetRealHostID();

		CAppManagerState *pAppManagerState;

		auto OnResume = co_await fg_OnResume
			(
				[&]() -> CExceptionPointer
				{
					pAppManagerState = pThis->mp_AppManagers.f_FindEqual(AppManagerID);
					if (!pAppManagerState)
						return DMibErrorInstance("App manager no longer registered");

					return {};
				}
			)
		;

		CAppManagerKey DatabaseKey{.m_HostID = AppManagerID};

		pAppManagerState->m_Data.m_LastSeen = CTime::fs_NowUTC();
		pAppManagerState->m_Data.m_PauseReportingFor = _SecondsToPause;

		TCMap<CStr, CDistributedAppSensorStoreLocal::CSeenHost> AppManagerSeenHosts;
		{
			auto ReadTransaction = co_await (pThis->mp_DatabaseActor(&CDatabaseActor::f_OpenTransactionRead) % Auditor);

			AppManagerSeenHosts = co_await fg_Move(ReadTransaction).f_BlockingDispatch
				(
					[AppManagerID, _SecondsToPause](CDatabaseActor::CTransactionRead &&_ReadTransaction)
					{
						TCMap<CStr, CDistributedAppSensorStoreLocal::CSeenHost> AppManagerSeenHosts;

						for (auto ApplicationCursor = _ReadTransaction.m_Transaction.f_ReadCursor(CApplicationKey::mc_Prefix, AppManagerID); ApplicationCursor; ++ApplicationCursor)
						{
							auto Application = ApplicationCursor.f_Value<CApplicationValue>();
							if (Application.m_ApplicationInfo.m_HostID)
								AppManagerSeenHosts[Application.m_ApplicationInfo.m_HostID].m_PauseReportingFor = _SecondsToPause;
						}

						return AppManagerSeenHosts;
					}
					, "Failed to read apps from database"
				)
			;
		}

		if (!AppManagerSeenHosts.f_IsEmpty())
		{
			co_await pThis->mp_AppSensorStore
				(
					&CDistributedAppSensorStoreLocal::f_SeenHosts
					, fg_Move(AppManagerSeenHosts)
				)
			;
		}

		auto SaveAppManagerDataResult = co_await pThis->fp_SaveAppManagerData(DatabaseKey, pAppManagerState->m_Data).f_Wrap();

		if (!SaveAppManagerDataResult)
			SaveAppManagerDataResult > fg_LogWarning("CloudManager", "Failed to store app manager data when pausing reporting");

		Auditor.f_Info("Paused reporting for {}"_f << fg_SecondsDurationToHumanReadable(_SecondsToPause));

		co_return {};
	}

	auto CCloudManagerServer::CCloudManagerImplementation::f_EnumAppManagers() -> TCFuture<TCMap<CStr, CAppManagerDynamicInfo>>
	{
		auto pThis = m_pThis;
		auto OnResume = co_await pThis->f_CheckDestroyedOnResume();

		auto Auditor = pThis->mp_AppState.f_Auditor();

		NContainer::TCVector<NStr::CStr> Permissions = {"CloudManager/ReadAll"};

		if (!co_await pThis->mp_Permissions.f_HasPermission("Enum app managers", Permissions))
			co_return Auditor.f_AccessDenied("(Enum app managers)", Permissions);

		co_await (pThis->fp_UpdateAppManagerState() % Auditor);

		TCMap<CStr, CAppManagerDynamicInfo> Return;
		{
			auto ReadTransaction = co_await (pThis->mp_DatabaseActor(&CDatabaseActor::f_OpenTransactionRead) % Auditor);

			Return = co_await fg_Move(ReadTransaction).f_BlockingDispatch
				(
					[](CDatabaseActor::CTransactionRead &&_ReadTransaction)
					{
						TCMap<CStr, CAppManagerDynamicInfo> Return;

						for (auto AppManagers = _ReadTransaction.m_Transaction.f_ReadCursor(CAppManagerKey::mc_Prefix); AppManagers; ++AppManagers)
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
							OutAppManager.m_PauseReportingFor = Value.m_PauseReportingFor;
						}

						return Return;
					}
					, "Failed to read app managers from database"
				)
			;
		}

		Auditor.f_Info("Enum app managers");

		co_return fg_Move(Return);
	}

	auto CCloudManagerServer::CCloudManagerImplementation::f_EnumApplications() -> TCFuture<TCMap<CApplicationKey, CApplicationInfo>>
	{
		auto pThis = m_pThis;
		auto OnResume = co_await pThis->f_CheckDestroyedOnResume();

		auto Auditor = pThis->mp_AppState.f_Auditor();

		NContainer::TCVector<NStr::CStr> Permissions = {"CloudManager/ReadAll"};

		if (!co_await pThis->mp_Permissions.f_HasPermission("Enum applications", Permissions))
			co_return Auditor.f_AccessDenied("(Enum applications)", Permissions);

		TCMap<CApplicationKey, CApplicationInfo> Return;
		{
			auto ReadTransaction = co_await (pThis->mp_DatabaseActor(&CDatabaseActor::f_OpenTransactionRead) % Auditor);

			Return = co_await fg_Move(ReadTransaction).f_BlockingDispatch
				(
					[](CDatabaseActor::CTransactionRead &&_ReadTransaction)
					{
						TCMap<CApplicationKey, CApplicationInfo> Return;

						for (auto Applications = _ReadTransaction.m_Transaction.f_ReadCursor(NCloudManagerDatabase::CApplicationKey::mc_Prefix); Applications; ++Applications)
						{
							auto Key = Applications.f_Key<NCloudManagerDatabase::CApplicationKey>();
							auto Value = Applications.f_Value<CApplicationValue>();

							CApplicationKey ApplicationKey{Key.m_AppManagerHostID, Key.m_Application};

							auto &OutApplication = Return[ApplicationKey];
							OutApplication.m_ApplicationInfo = Value.m_ApplicationInfo;
						}

						return Return;
					}
					, "Failed to read applications from database"
				)
			;
		}

#if !DMibConfig_Tests_Enable // This is used for polling in tests, and we don't want extra log traffic
		Auditor.f_Info("Enum applications");
#endif
		co_return fg_Move(Return);
	}

	TCFuture<void> CCloudManagerServer::CCloudManagerImplementation::f_RemoveAppManager(NStr::CStr const &_AppManagerHostID)
	{
		auto pThis = m_pThis;
		auto OnResume = co_await pThis->f_CheckDestroyedOnResume();

		auto Auditor = pThis->mp_AppState.f_Auditor();

		NContainer::TCVector<NStr::CStr> Permissions = {"CloudManager/RemoveAppManager"};

		if (!co_await pThis->mp_Permissions.f_HasPermission("Remove app manager", Permissions))
			co_return Auditor.f_AccessDenied("(Remove app manager)", Permissions);

		CLogError LogError("CloudManager");

		auto *pAppManager = pThis->mp_AppManagers.f_FindEqual(_AppManagerHostID);
		if (pAppManager)
		{
			auto ChangeSubscription = fg_Move(pAppManager->m_ChangeNotificationsSubscription);
			auto UpdateSubscription = fg_Move(pAppManager->m_UpdateNotificationsSubscription);
			auto Interface = fg_Move(pAppManager->m_Interface);

			pThis->mp_AppManagers.f_Remove(pAppManager);

			if (ChangeSubscription)
				co_await ChangeSubscription->f_Destroy().f_Wrap() > LogError.f_Warning("Failed to destroy app manager change subscription");

			if (UpdateSubscription)
				co_await UpdateSubscription->f_Destroy().f_Wrap() > LogError.f_Warning("Failed to destroy app manager update subscription");

			if (Interface)
				co_await Interface.f_Destroy().f_Wrap() > LogError.f_Warning("Failed to destroy app manager interface subscription");
		}

		co_await (pThis->self(&CCloudManagerServer::fp_RemoveAppManagerData, _AppManagerHostID) % Auditor);

		Auditor.f_Info("Remove App Manager");

		co_return {};
	}

	TCFuture<uint32> CCloudManagerServer::CCloudManagerImplementation::f_RemoveSensor(CRemoveSensor &&_RemoveSensor)
	{
		auto pThis = m_pThis;
		auto OnResume = co_await pThis->f_CheckDestroyedOnResume();

		auto Auditor = pThis->mp_AppState.f_Auditor();

		NContainer::TCVector<NStr::CStr> Permissions = {"CloudManager/RemoveSensor"};

		if (!co_await pThis->mp_Permissions.f_HasPermission("Remove sensor", Permissions))
			co_return Auditor.f_AccessDenied("(Remove sensor)", Permissions);

		CDistributedAppSensorReader::CGetSensors GetSensors;
		GetSensors.m_Filters.f_Insert(fg_Move(_RemoveSensor.m_Filter));
		auto Sensors = co_await pThis->mp_AppSensorStore(&CDistributedAppSensorStoreLocal::f_GetSensors, fg_Move(GetSensors));

		TCSet<CDistributedAppSensorReporter::CSensorInfoKey> SensorsToRemove;

		for (auto iSensor = co_await fg_Move(Sensors).f_GetIterator(); iSensor; co_await ++iSensor)
		{
			for (auto &Sensor : *iSensor)
				SensorsToRemove[Sensor.f_Key()];
		}

		auto nRemoved = co_await pThis->mp_AppSensorStore(&CDistributedAppSensorStoreLocal::f_RemoveSensors, fg_Move(SensorsToRemove));

		Auditor.f_Info("Remove Sensor");

		co_return nRemoved;
	}

	TCFuture<uint32> CCloudManagerServer::CCloudManagerImplementation::f_RemoveLog(CRemoveLog &&_RemoveLog)
	{
		auto pThis = m_pThis;
		auto OnResume = co_await pThis->f_CheckDestroyedOnResume();

		auto Auditor = pThis->mp_AppState.f_Auditor();

		NContainer::TCVector<NStr::CStr> Permissions = {"CloudManager/RemoveLog"};

		if (!co_await pThis->mp_Permissions.f_HasPermission("Remove log", Permissions))
			co_return Auditor.f_AccessDenied("(Remove log)", Permissions);

		CDistributedAppLogReader::CGetLogs GetLogs;
		GetLogs.m_Filters.f_Insert(fg_Move(_RemoveLog.m_Filter));
		auto Logs = co_await pThis->mp_AppLogStore(&CDistributedAppLogStoreLocal::f_GetLogs, fg_Move(GetLogs));

		TCSet<CDistributedAppLogReporter::CLogInfoKey> LogsToRemove;

		for (auto iLog = co_await fg_Move(Logs).f_GetIterator(); iLog; co_await ++iLog)
		{
			for (auto &Log : *iLog)
				LogsToRemove[Log.f_Key()];
		}

		auto nRemoved = co_await pThis->mp_AppLogStore(&CDistributedAppLogStoreLocal::f_RemoveLogs, fg_Move(LogsToRemove));

		Auditor.f_Info("Remove Log");

		co_return nRemoved;
	}

	TCFuture<uint32> CCloudManagerServer::CCloudManagerImplementation::f_SnoozeSensor(CSnoozeSensor &&_SnoozeSensor)
	{
		auto pThis = m_pThis;
		auto OnResume = co_await pThis->f_CheckDestroyedOnResume();

		auto Auditor = pThis->mp_AppState.f_Auditor();

		NContainer::TCVector<NStr::CStr> Permissions = {"CloudManager/SnoozeSensor"};

		if (!co_await pThis->mp_Permissions.f_HasPermission("Snooze sensor", Permissions))
			co_return Auditor.f_AccessDenied("(Snooze sensor)", Permissions);

		CDistributedAppSensorReader::CGetSensorStatus GetSensorStatus;
		auto &Filter = GetSensorStatus.m_Filters.f_Insert();
		Filter.m_SensorFilter = fg_Move(_SnoozeSensor.m_Filter);
		if (_SnoozeSensor.m_SnoozeDuration.f_IsValid())
			Filter.m_Flags = CDistributedAppSensorReader_SensorReadingFilter::ESensorReadingsFlag_OnlyProblems;

		auto SensorStatus = co_await pThis->mp_AppSensorStore(&CDistributedAppSensorStoreLocal::f_GetSensorStatus, fg_Move(GetSensorStatus));

		TCSet<CDistributedAppSensorReporter::CSensorInfoKey> SensorsToSnooze;

		for (auto iSensorStatus = co_await fg_Move(SensorStatus).f_GetIterator(); iSensorStatus; co_await ++iSensorStatus)
		{
			for (auto &SensorStatus : *iSensorStatus)
				SensorsToSnooze[SensorStatus.m_SensorInfoKey];
		}

		auto nChanged = co_await pThis->mp_AppSensorStore(&CDistributedAppSensorStoreLocal::f_SnoozeSensors, fg_Move(SensorsToSnooze), _SnoozeSensor.m_SnoozeDuration);

		if (_SnoozeSensor.m_SnoozeDuration.f_IsValid())
			Auditor.f_Info("Snoozed {} sensors"_f << nChanged);
		else
			Auditor.f_Info("Un-snoozed {} sensors"_f << nChanged);

		co_return nChanged;
	}

	TCFuture<TCActorSubscriptionWithID<>> CCloudManagerServer::CCloudManagerImplementation::f_SubscribeExpectedOsVersions(CSubscribeExpectedOsVersions &&_Params)
	{
		auto pThis = m_pThis;
		auto OnResume = co_await pThis->f_CheckDestroyedOnResume();

		auto Auditor = pThis->mp_AppState.f_Auditor();

		if (!_Params.m_fVersionRangeChanged)
			co_return Auditor.f_Exception("Invalid notification functor");

		NContainer::TCVector<NStr::CStr> Permissions = {"CloudManager/SubscribeExpectedOsVersions", "CloudManager/RegisterAppManager"};

		if (!co_await pThis->mp_Permissions.f_HasPermission("Subscribe expected OS versions", Permissions))
			co_return Auditor.f_AccessDenied("(Subscribe expected os versions)", Permissions);

		CExpectedOsVersionSubscriptionKey SubscriptionKey{.m_OsName = _Params.m_OsName, .m_ID = ++pThis->mp_ExpectedOsVersionSubscriptionNextID};

		auto pSubscription = &pThis->mp_ExpectedOsVersionSubscriptions[SubscriptionKey];

		auto OnResume2 = co_await fg_OnResume
			(
				[&]() -> CExceptionPointer
				{
					pSubscription = pThis->mp_ExpectedOsVersionSubscriptions.f_FindEqual(SubscriptionKey);
					if (!pSubscription)
						return DMibErrorInstance("Subscription removed");
					return {};
				}
			)
		;

		CCloudManager::CExpectedVersions InitialExpectedVersions;
		{
			auto ReadTransaction = co_await (pThis->mp_DatabaseActor(&CDatabaseActor::f_OpenTransactionRead) % Auditor);

			InitialExpectedVersions = co_await fg_Move(ReadTransaction).f_BlockingDispatch
				(
					[OsName = _Params.m_OsName](CDatabaseActor::CTransactionRead &&_ReadTransaction)
					{
						CCloudManager::CExpectedVersions InitialExpectedVersions;

						for (auto Applications = _ReadTransaction.m_Transaction.f_ReadCursor(CExpectedOsVersionKey::mc_Prefix, OsName); Applications; ++Applications)
						{
							auto Key = Applications.f_Key<CExpectedOsVersionKey>();
							auto Value = Applications.f_Value<CExpectedOsVersionValue>();

							InitialExpectedVersions.m_Versions[fg_Move(Key.m_CurrentVersion)] = fg_Move(Value.m_ExpectedVersionRange);
						}

						return InitialExpectedVersions;
					}
					, "Failed to read expected OS versions from database"
					, Auditor
				)
			;
		}

		for (auto &Notification : pSubscription->m_QueuedNotifications)
			InitialExpectedVersions.f_ApplyChanges(Notification);
		pSubscription->m_QueuedNotifications.f_Clear();
		pSubscription->m_fVersionRangeChanged = fg_Move(_Params.m_fVersionRangeChanged);

		(void)co_await (pSubscription->m_fVersionRangeChanged(fg_Move(fg_Move(InitialExpectedVersions))) % "Error sending initial expected OsVersion" % Auditor).f_Wrap();

		Auditor.f_Info("Subscribe expected OS versions for '{}'"_f << _Params.m_OsName);

		co_return {};
	}

	auto CCloudManagerServer::CCloudManagerImplementation::f_EnumExpectedOsVersions() -> TCFuture<TCMap<CStr, CExpectedVersions>>
	{
		auto pThis = m_pThis;
		auto OnResume = co_await pThis->f_CheckDestroyedOnResume();

		auto Auditor = pThis->mp_AppState.f_Auditor();

		NContainer::TCVector<NStr::CStr> Permissions = {"CloudManager/ReadAll"};

		if (!co_await pThis->mp_Permissions.f_HasPermission("Enum expected OS versions", Permissions))
			co_return Auditor.f_AccessDenied("(Enum expected os versions)", Permissions);

		TCMap<CStr, CExpectedVersions> Return;
		{
			auto ReadTransaction = co_await (pThis->mp_DatabaseActor(&CDatabaseActor::f_OpenTransactionRead) % Auditor);

			Return = co_await fg_Move(ReadTransaction).f_BlockingDispatch
				(
					[](CDatabaseActor::CTransactionRead &&_ReadTransaction)
					{
						TCMap<CStr, CExpectedVersions> Return;

						for (auto Applications = _ReadTransaction.m_Transaction.f_ReadCursor(CExpectedOsVersionKey::mc_Prefix); Applications; ++Applications)
						{
							auto Key = Applications.f_Key<CExpectedOsVersionKey>();
							auto Value = Applications.f_Value<CExpectedOsVersionValue>();

							Return[Key.m_OsName].m_Versions[Key.m_CurrentVersion] = fg_Move(Value.m_ExpectedVersionRange);
						}

						return Return;
					}
					, "Failed to read expected OS versions from database"
					, Auditor
				)
			;
		}

		Auditor.f_Info("Enum expected OS versions");

		co_return fg_Move(Return);
	}

	TCFuture<void> CCloudManagerServer::CCloudManagerImplementation::f_SetExpectedOsVersions(CStr &&_OsName, CCurrentVersion &&_CurrentVersion, CExpectedVersionRange &&_ExpectedRange)
	{
		auto pThis = m_pThis;
		auto OnResume = co_await pThis->f_CheckDestroyedOnResume();

		auto Auditor = pThis->mp_AppState.f_Auditor();

		NContainer::TCVector<NStr::CStr> Permissions = {"CloudManager/SetExpectedOsVersion"};

		if (!co_await pThis->mp_Permissions.f_HasPermission("Set expected OS version", Permissions))
			co_return Auditor.f_AccessDenied("(Set expected OS version)", Permissions);

		TCPromise<bool> ChangedPromise;

		co_await
			(
				pThis->mp_DatabaseActor
				(
					&CDatabaseActor::f_WriteWithCompaction
					, g_ActorFunctorWeak / [ThisActor = fg_ThisActor(pThis), _OsName, _CurrentVersion, _ExpectedRange, ChangedPromise]
					(CDatabaseActor::CTransactionWrite &&_Transaction, bool _bCompacting) -> TCFuture<CDatabaseActor::CTransactionWrite>
					{
						co_await ECoroutineFlag_CaptureMalterlibExceptions;

						auto WriteTransaction = fg_Move(_Transaction);
						if (_bCompacting)
							WriteTransaction = fg_Move((co_await ThisActor(&CCloudManagerServer::fp_CleanupDatabase, fg_Move(WriteTransaction), fg_Construct())).m_Transaction);

						co_await fg_ContinueRunningOnActor(WriteTransaction.f_Checkout());

						CExpectedOsVersionKey Key{.m_OsName = _OsName, .m_CurrentVersion = _CurrentVersion};

						bool bChanged = false;

						if (!_ExpectedRange.f_IsSet())
						{
							if (WriteTransaction.m_Transaction.f_Exists(Key))
							{
								WriteTransaction.m_Transaction.f_Delete(Key);
								bChanged = true;
							}
						}
						else
						{
							CExpectedOsVersionValue NewValue{.m_ExpectedVersionRange = _ExpectedRange};
							CExpectedOsVersionValue OldValue;
							if (WriteTransaction.m_Transaction.f_Get(Key, OldValue))
							{
								if (OldValue.m_ExpectedVersionRange != NewValue.m_ExpectedVersionRange)
									bChanged = true;
							}

							WriteTransaction.m_Transaction.f_Upsert(Key, NewValue);
						}

						ChangedPromise.f_SetResult(bChanged);

						co_return fg_Move(WriteTransaction);
					}
				)
				% "Error saving expected OS version to database"
				% Auditor("Internal error saving to database, see logs")
			)
		;

		Auditor.f_Info("Set expected OS version");

		auto bChanged = co_await ChangedPromise.f_MoveFuture();

		if (bChanged)
		{
			CCloudManager::CExpectedVersions ChangeData;
			ChangeData.m_Versions[_CurrentVersion] = _ExpectedRange;

			TCActorResultVector<void> NotificationResults;
			CExpectedOsVersionSubscriptionKey Key{.m_OsName = _OsName};
			for
				(
					auto iSubscription = pThis->mp_ExpectedOsVersionSubscriptions.f_GetIterator_SmallestGreaterThanEqual(Key)
					; iSubscription && iSubscription.f_GetKey().m_OsName == _OsName
					; ++iSubscription
				)
			{
				auto &Subscription = *iSubscription;
				if (Subscription.m_fVersionRangeChanged)
					Subscription.m_fVersionRangeChanged(ChangeData) > NotificationResults.f_AddResult();
				else
					Subscription.m_QueuedNotifications.f_Insert(ChangeData);
			}

			co_await NotificationResults.f_GetUnwrappedResults().f_Wrap() > fg_LogError("CloudManager", "Error sending expected OS version notifications");
		}

		co_return {};
	}

	TCFuture<TCDistributedActorInterfaceWithID<CDistributedAppSensorReporter>> CCloudManagerServer::CCloudManagerImplementation::f_GetSensorReporter()
	{
		auto pThis = m_pThis;
		auto OnResume = co_await pThis->f_CheckDestroyedOnResume();

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
		auto OnResume = co_await pThis->f_CheckDestroyedOnResume();

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
		auto OnResume = co_await pThis->f_CheckDestroyedOnResume();

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
		auto OnResume = co_await pThis->f_CheckDestroyedOnResume();

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
