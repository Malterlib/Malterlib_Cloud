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
		TCSet<CStr> Permissions{"CloudManager/RegisterAppManager", "CloudManager/ReadAll"};
		mp_AppState.m_TrustManager(&CDistributedActorTrustManager::f_RegisterPermissions, Permissions) > fg_DiscardResult();

		TCVector<CStr> SubscribePermissions{"CloudManager/*"};
		mp_Permissions = co_await mp_AppState.m_TrustManager(&CDistributedActorTrustManager::f_SubscribeToPermissions, SubscribePermissions, fg_ThisActor(this));

		co_return {};
	}

	TCFuture<void> CCloudManagerServer::fp_Publish()
	{
		return mp_ProtocolInterface.f_Publish<CCloudManager>(mp_AppState.m_DistributionManager, this, CCloudManager::mc_pDefaultNamespace);
	}

	TCFuture<void> CCloudManagerServer::fp_ProcessApplicationChanges(CStr const &_AppManagerID, TCVector<CAppManagerInterface::CChangeNotification> &&_Changes, bool _bInitial)
	{
		try
		{
			auto WriteTransaction = co_await mp_DatabaseActor(&CDatabaseActor::f_OpenTransactionWrite);
			{
				if (_bInitial)
				{
					for (auto ApplicationCursor = WriteTransaction.m_Transaction.f_WriteCursor(CApplicationKey::mc_Prefix, _AppManagerID); ApplicationCursor;)
						ApplicationCursor.f_Delete();
				}

				auto WriteCursor = WriteTransaction.m_Transaction.f_WriteCursor();
				for (auto &Change : _Changes)
				{
					CApplicationKey Key{CApplicationKey::mc_Prefix, _AppManagerID, Change.m_Application};
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

					WriteCursor.f_Upsert(Key, Value);
				}
			}
			co_await mp_DatabaseActor(&CDatabaseActor::f_CommitWriteTransaction, fg_Move(WriteTransaction));
		}
		catch (CException const &_Exception)
		{
			DMibLogWithCategory(CloudManager, Critical, "Error saving app manager data to database: {}", _Exception);
			co_return _Exception.f_ExceptionPointer();
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
		auto CallingHostInfo = fg_GetCallingHostInfo();
		auto Auditor = pThis->mp_AppState.f_Auditor(CallingHostInfo);

		CStr UniqueHostID = CallingHostInfo.f_GetUniqueHostID();

		if (!co_await pThis->mp_Permissions.f_HasPermission("Register as app manager", {"CloudManager/RegisterAppManager"}))
			co_return Auditor.f_AccessDenied("(Register as app manager)");

		CStr AppManagerID = CallingHostInfo.f_GetRealHostID();
		CAppManagerKey DatabaseKey{CAppManagerKey::mc_Prefix, AppManagerID};

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

		auto ChangeNotificationsSubscription = co_await AppManager.m_Interface.f_CallActor(&CAppManagerInterface::f_SubscribeChangeNotifications)
			(
			 	g_ActorFunctor / [pThis, RegisterSequence, AppManagerID, AllowDestroy = g_AllowWrongThreadDestroy]
			 	(TCVector<CAppManagerInterface::CChangeNotification> &&_Changes, bool _bInitial) -> NConcurrency::TCFuture<void>
				{
					auto pAppManager = pThis->mp_AppManagers.f_FindEqual(AppManagerID);
					if (!pAppManager || pAppManager->m_RegisterSequence != RegisterSequence)
						co_return {};

					co_await pThis->self(&CCloudManagerServer::fp_ProcessApplicationChanges, AppManagerID, fg_Move(_Changes), _bInitial);

					co_return {};
				}
			)
		;

		auto pAppManager = pThis->mp_AppManagers.f_FindEqual(AppManagerID);
		if (!pAppManager || pAppManager->m_RegisterSequence != RegisterSequence)
			co_return {};

		pAppManager->m_ChangeNotificationsSubscription = fg_Move(ChangeNotificationsSubscription);

		Auditor.f_Info("App manager registered");

		co_return g_ActorSubscription / [pThis, RegisterSequence, AppManagerID, DatabaseKey]() -> TCFuture<void>
			{
				auto pAppManager = pThis->mp_AppManagers.f_FindEqual(AppManagerID);

				if (pAppManager && pAppManager->m_ChangeNotificationsSubscription)
					co_await pAppManager->m_ChangeNotificationsSubscription->f_Destroy();

				if (!pAppManager || pAppManager->m_RegisterSequence != RegisterSequence)
					co_return {};

				auto Data = fg_Move(pAppManager->m_Data);
				Data.m_LastSeen = CTime::fs_NowUTC();
				Data.m_bActive = false;

				auto Interface = fg_Move(pAppManager->m_Interface);
				pThis->mp_AppManagers.f_Remove(pAppManager);

				co_await (Interface.f_Destroy() + pThis->fp_SaveAppManagerData(DatabaseKey, Data)).f_Wrap();

				co_return {};
			}
		;
	}

	auto CCloudManagerServer::CCloudManagerImplementation::f_EnumAppManagers() -> TCFuture<TCMap<CStr, CAppManagerDynamicInfo>>
	{
		auto pThis = m_pThis;
		auto Auditor = pThis->mp_AppState.f_Auditor();

		if (!co_await pThis->mp_Permissions.f_HasPermission("Enum app managers", {"CloudManager/ReadAll"}))
			co_return Auditor.f_AccessDenied("(Enum app managers)");

		TCMap<CStr, CAppManagerDynamicInfo> Return;

		co_await (pThis->fp_UpdateAppManagerState() % Auditor);

		try
		{
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
				OutAppManager.m_bActive = Value.m_bActive;
			}
		}
		catch (CException const &_Exception)
		{
			co_return _Exception.f_ExceptionPointer();
		}

		Auditor.f_Info("Enum app managers");

		co_return fg_Move(Return);
	}

	auto CCloudManagerServer::CCloudManagerImplementation::f_EnumApplications() -> TCFuture<TCMap<CApplicationKey, CApplicationInfo>>
	{
		auto pThis = m_pThis;
		auto Auditor = pThis->mp_AppState.f_Auditor();

		if (!co_await pThis->mp_Permissions.f_HasPermission("Enum applications", {"CloudManager/ReadAll"}))
			co_return Auditor.f_AccessDenied("(Enum applications)");

		TCMap<CApplicationKey, CApplicationInfo> Return;

		try
		{
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
		catch (CException const &_Exception)
		{
			co_return _Exception.f_ExceptionPointer();
		}

		Auditor.f_Info("Enum applications");

		co_return fg_Move(Return);
	}
}
