
#include "Malterlib_Cloud_App_BackupManager.h"
#include "Malterlib_Cloud_App_BackupManager_Internal.h"
#include <Mib/Cryptography/Hashes/SHA>
#include <Mib/Concurrency/ActorSubscription>

namespace NMib::NCloud::NBackupManager
{
	namespace
	{
		CTime g_MinBackupTime = CTimeConvert::fs_CreateTime(1970);
		CTimeSpan g_BackupTimeMaxInFuture = CTimeSpanConvert::fs_CreateHourSpan(2);
	}
	
	CExceptionPointer CBackupManagerServer::fp_CheckBackupKey
		(
			CBackupManager::CBackupKey const &_BackupKey
			, CBackupManagerServer::CBackupKey &o_BackupKey
			, CDistributedAppAuditor const &_Auditor
		)
	{
		if (!CBackupManager::fs_IsValidHostname(_BackupKey.m_FriendlyName))
			return _Auditor.f_Exception({"Backup key friendly name does not adhere to RFC 1123", fg_Format("'{}'", _BackupKey.m_FriendlyName)}).f_ExceptionPointer();

		if (!CBackupManager::fs_IsValidHostname(_BackupKey.m_ID))
			return _Auditor.f_Exception({"Backup key ID name does not adhere to RFC 1123", fg_Format("'{}'", _BackupKey.m_ID)}).f_ExceptionPointer();

		if (_BackupKey.m_Time < g_MinBackupTime || _BackupKey.m_Time > CTime::fs_NowUTC() + g_BackupTimeMaxInFuture)
			return _Auditor.f_Exception({"Backup key time is out of range", fg_Format("'{}'", _BackupKey.m_Time)}).f_ExceptionPointer();

		o_BackupKey.m_BackupID = _BackupKey.m_ID;
		o_BackupKey.m_BackupTime = _BackupKey.m_Time;
		o_BackupKey.m_BackupName = "{}_{}"_f
			<< _BackupKey.m_FriendlyName // This makes sure that backups can never be mixed between different hosts
			<< _Auditor.f_HostInfo().f_GetRealHostID()
		;
		
		if (o_BackupKey.m_BackupName.f_GetLen() > 128)
			return _Auditor.f_Exception({"Backup key friendly name is too long", fg_Format("'{}'", o_BackupKey.m_BackupName)}).f_ExceptionPointer();

		return nullptr;
	}		
	
	TCFuture<void> CBackupManagerServer::fp_DestroyBackupInstance(CBackupKey _Key, CDistributedAppAuditor _Auditor, bool _bError, CStr _Reason)
	{
		auto *pBackupInstance = mp_BackupInstances.f_FindEqual(_Key);
		if (!pBackupInstance || pBackupInstance->m_OwningHost.f_HostInfo() != _Auditor.f_HostInfo())
			co_return {};
		
		if (!pBackupInstance->m_BackupInstance)
		{
			if (!pBackupInstance->m_bPendingDestroy)
			{
				auto OnDestroyed = fg_Move(pBackupInstance->m_OnDestroyed);
				for (auto &OnDestroy : OnDestroyed)
					OnDestroy.f_SetResult();
			}
			co_return {};
		}

		CStr Message = fg_Format("Backup '{}' stopped: {}", _Key.f_GetDesc(), _Reason);
		if (_bError)
			_Auditor.f_Error(Message);
		else
			_Auditor.f_Info(Message);
	
		auto pCanDestroyTracker = mp_pCanDestroyTracker;
		pBackupInstance->m_bPendingDestroy = true;
		auto DestroyResult = co_await fg_Move(pBackupInstance->m_BackupInstance).f_Destroy().f_Wrap();

		if (!DestroyResult)
		{
			DMibLogWithCategory
				(
					Mib/Cloud/BackupManager
					, Error
					, "Failed to destroy backup instance '{}': {}"
					, _Key.f_GetDesc()
					, DestroyResult.f_GetExceptionStr()
				)
			;
		}

		pBackupInstance = mp_BackupInstances.f_FindEqual(_Key);
		DMibCheck(pBackupInstance);
		if (!pBackupInstance)
			co_return {};

		pBackupInstance->m_bPendingDestroy = false;
		pBackupInstance->m_BackupRunningSubscription.f_Clear();
		auto OnDestroyed = fg_Move(pBackupInstance->m_OnDestroyed);
		if (pBackupInstance->m_OwningHost.f_HostInfo() == _Auditor.f_HostInfo())
			mp_BackupInstances.f_Remove(pBackupInstance->f_GetKey());
		for (auto &OnDestroy : OnDestroyed)
			OnDestroy.f_SetResult();

		co_return {};
	}

	auto CBackupManagerServer::CBackupManagerImplementation::f_InitBackup(CBackupManager::CInitBackup _Params)
		-> NConcurrency::TCFuture<TCDistributedActorInterfaceWithID<CBackupManagerBackup>>
	{
		auto pThis = m_pThis;
		
		auto OnResume = co_await pThis->f_CheckDestroyedOnResume();
			
		auto Auditor = pThis->mp_AppState.f_Auditor();
		auto CallingHostID = fg_GetCallingHostID();
			
		CBackupManagerServer::CBackupKey BackupKey;
		if (auto pException = pThis->fp_CheckBackupKey(_Params.m_BackupKey, BackupKey, Auditor))
			co_return fg_Move(pException);

		TCVector<CStr> Permissions = {"Backup/WriteSelf"};
		
		bool bHasPermission = co_await (pThis->mp_Permissions.f_HasPermission("Start backup", Permissions) % "Permission denied starting backup" % Auditor);
		if (!bHasPermission)
			co_return Auditor.f_AccessDenied("(Start backup)", Permissions);

		CStr BackupPermission = fg_Format("Backup/Read/{}", BackupKey.m_BackupName);
		pThis->mp_AppState.m_TrustManager(&CDistributedActorTrustManager::f_RegisterPermissions, fg_CreateSet(BackupPermission)).f_DiscardResult();

		Auditor.f_Info(fg_Format("Starting backup '{}'", BackupKey.f_GetDesc()));

		auto NewInstance = pThis->mp_BackupInstances(BackupKey);
		auto &Instance = *NewInstance;

		Auditor.f_HostInfo().f_OnDisconnect
			(
				g_ActorFunctorWeak(fg_ThisActor(pThis)) / [pThis, BackupKey, Auditor]() -> TCFuture<void>
				{
					co_await pThis->fp_DestroyBackupInstance(BackupKey, Auditor, true, "Actor host disconnected (restarted)");
					co_return {};
				}
			)
			> [pThis, BackupKey, Auditor](TCAsyncResult<CActorSubscription> &&_Subscription)
			{
				if (!_Subscription || !*_Subscription)
					return;
				auto *pBackupInstance = pThis->mp_BackupInstances.f_FindEqual(BackupKey);
				if (!pBackupInstance || pBackupInstance->m_OwningHost.f_HostInfo() != Auditor.f_HostInfo())
					return;
				pBackupInstance->m_OnDisconnectSubscrption = fg_Move(*_Subscription);
			}
		;


		if (NewInstance.f_WasCreated())
			Instance.m_OwningHost = Auditor;
		else
		{
			auto OnDestroyedFuture = Instance.m_OnDestroyed.f_Insert().f_Future();
			pThis->fp_DestroyBackupInstance(BackupKey, Instance.m_OwningHost, false, "Old host removed").f_DiscardResult(); // Remove old Host
			Instance.m_OwningHost = Auditor; // Take ownership

			co_await fg_Move(OnDestroyedFuture);
		}

		auto *pBackupInstance = pThis->mp_BackupInstances.f_FindEqual(BackupKey);
		if (!pBackupInstance || pBackupInstance->m_OwningHost.f_HostInfo() != Auditor.f_HostInfo())
			co_return Auditor.f_Exception("Another backup was already started taking precedence");

		pBackupInstance->m_BackupRunningSubscription = fg_Move(_Params.m_Subscription);

		DCheck(pBackupInstance->m_BackupInstance.f_IsEmpty());
		pBackupInstance->m_BackupInstance = pThis->mp_AppState.m_DistributionManager->f_ConstructActor<CBackupInstance>
			(
				fg_Construct(fg_Format("Backup instance for '{}'", BackupKey.m_BackupName))
				, BackupKey.m_BackupName
				, BackupKey.m_BackupTime
				, BackupKey.m_BackupID
				, pThis->mp_AppState.m_RootDirectory
				, (_Params.m_Flags & CBackupManager::EInitBackupFlag_ForceNew) != 0
				, pThis->fp_CreateBackupSource(BackupKey.m_BackupName)
			)
		;

		TCDistributedActorInterfaceWithID<CBackupManagerBackup> BackupInterface
			{
				pBackupInstance->m_BackupInstance->f_ShareInterface<CBackupManagerBackup>()
				, g_ActorSubscription / [pThis, BackupKey, Auditor]() -> TCFuture<void>
				{
					auto *pBackupInstance = pThis->mp_BackupInstances.f_FindEqual(BackupKey);
					if (!pBackupInstance || pBackupInstance->m_OwningHost.f_HostInfo() != Auditor.f_HostInfo())
						co_return {};

					auto &Instance = *pBackupInstance;

					co_return co_await pThis->fp_DestroyBackupInstance(BackupKey, Instance.m_OwningHost, false, "Backup stopped remotely");
				}
			}
		;

		Auditor.f_Info(fg_Format("Backup initialized for '{}'", BackupKey.f_GetDesc()));

		co_return fg_Move(BackupInterface);
	}
}
