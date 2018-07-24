
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
	
	template <typename tf_CResult>
	bool CBackupManagerServer::fp_CheckBackupKey
		(
			CBackupManager::CBackupKey const &_BackupKey
			, CBackupManagerServer::CBackupKey &o_BackupKey
			, CDistributedAppAuditor const &_Auditor
			, NConcurrency::TCContinuation<tf_CResult> &_Continuation
		)
	{
		if (!CBackupManager::fs_IsValidHostname(_BackupKey.m_FriendlyName))
		{
			_Continuation.f_SetException(_Auditor.f_Exception({"Backup key friendly name does not adhere to RFC 1123", fg_Format("'{}'", _BackupKey.m_FriendlyName)}));
			return false;
		}
		if (!CBackupManager::fs_IsValidHostname(_BackupKey.m_ID))
		{
			_Continuation.f_SetException(_Auditor.f_Exception({"Backup key ID name does not adhere to RFC 1123", fg_Format("'{}'", _BackupKey.m_ID)}));
			return false;
		}
		if (_BackupKey.m_Time < g_MinBackupTime || _BackupKey.m_Time > CTime::fs_NowUTC() + g_BackupTimeMaxInFuture)
		{
			_Continuation.f_SetException(_Auditor.f_Exception({"Backup key time is out of range", fg_Format("'{}'", _BackupKey.m_Time)}));
			return false;
		}
		
		o_BackupKey.m_BackupID = _BackupKey.m_ID;
		o_BackupKey.m_BackupTime = _BackupKey.m_Time;
		o_BackupKey.m_BackupName = fg_Format
			(
				"{}_{}"
				, _BackupKey.m_FriendlyName // This makes sure that backups can never be mixed between different hosts
				, _Auditor.f_HostInfo().f_GetRealHostID()
			)
		;
		
		if (o_BackupKey.m_BackupName.f_GetLen() > 128)
		{
			_Continuation.f_SetException(_Auditor.f_Exception({"Backup key friendly name is too long", fg_Format("'{}'", o_BackupKey.m_BackupName)}));
			return false;
		}
		
		return true;
	}		
	
	TCContinuation<void> CBackupManagerServer::fp_DestroyBackupInstance(CBackupKey const &_Key, CDistributedAppAuditor const &_Auditor, bool _bError, CStr const &_Reason)
	{
		auto *pBackupInstance = mp_BackupInstances.f_FindEqual(_Key);
		if (!pBackupInstance || pBackupInstance->m_OwningHost.f_HostInfo() != _Auditor.f_HostInfo())
			return fg_Explicit();
		
		if (!pBackupInstance->m_BackupInstance)
		{
			if (!pBackupInstance->m_bPendingDestroy)
			{
				auto OnDestroyed = fg_Move(pBackupInstance->m_OnDestroyed);
				for (auto &OnDestroy : OnDestroyed)
					OnDestroy();
			}
			return fg_Explicit();
		}

		CStr Message = fg_Format("Backup '{}' stopped: {}", _Key.f_GetDesc(), _Reason);
		if (_bError)
			_Auditor.f_Error(Message);
		else
			_Auditor.f_Info(Message);
	
		TCContinuation<void> Continuation;
		auto pCanDestroyTracker = mp_pCanDestroyTracker;
		pBackupInstance->m_bPendingDestroy = true;
		pBackupInstance->m_BackupInstance->f_Destroy() > [this, pCanDestroyTracker, _Key, _Auditor, Continuation](TCAsyncResult<void> &&_Result)
			{
				if (!_Result)
				{
					DMibLogWithCategory
						(
							Mib/Cloud/BackupManager
							, Error
							, "Failed to destroy backup instance '{}': {}"
							, _Key.f_GetDesc()
						 	, _Result.f_GetExceptionStr()
						)
					;
				}
				auto *pBackupInstance = mp_BackupInstances.f_FindEqual(_Key);
				DMibCheck(pBackupInstance);
				if (!pBackupInstance)
				{
					Continuation.f_SetResult();
					return;
				}
				pBackupInstance->m_bPendingDestroy = false;
				pBackupInstance->m_BackupRunningSubscription.f_Clear();
				auto OnDestroyed = fg_Move(pBackupInstance->m_OnDestroyed);
				if (pBackupInstance->m_OwningHost.f_HostInfo() == _Auditor.f_HostInfo())
					mp_BackupInstances.f_Remove(pBackupInstance->f_GetKey());
				for (auto &OnDestroy : OnDestroyed)
					OnDestroy();
				Continuation.f_SetResult();
			}
		;
		pBackupInstance->m_BackupInstance.f_Clear();
		
		return Continuation;
	}

	auto CBackupManagerServer::CBackupManagerImplementation::f_InitBackup(CBackupManager::CInitBackup &&_Params)
		-> NConcurrency::TCContinuation<TCDistributedActorInterfaceWithID<CBackupManagerBackup>>
	{
		auto pThis = m_pThis;
		
		if (pThis->f_IsDestroyed())
			return DMibErrorInstance("Shutting down");
			
		auto Auditor = pThis->mp_AppState.f_Auditor();
		auto CallingHostID = fg_GetCallingHostID();
			
		NConcurrency::TCContinuation<TCDistributedActorInterfaceWithID<CBackupManagerBackup>> Continuation;
		
		CBackupManagerServer::CBackupKey BackupKey;
		if (!pThis->fp_CheckBackupKey(_Params.m_BackupKey, BackupKey, Auditor, Continuation))
			return Continuation;
		
		pThis->mp_Permissions.f_HasPermission("Start backup", {"Backup/WriteSelf"})
			> Continuation % "Permission denied starting backup" % Auditor /
			[
			 	=
				, Subscription = fg_Move(_Params.m_Subscription)
				, SourceBackupKey = _Params.m_BackupKey
				, Flags = _Params.m_Flags
			]
			(bool _bHasPermission) mutable
			{
				if (!_bHasPermission)
					return Continuation.f_SetException(Auditor.f_AccessDenied("(Start backup)"));

				CStr BackupPermission = fg_Format("Backup/Read/{}", BackupKey.m_BackupName);
				pThis->mp_AppState.m_TrustManager(&CDistributedActorTrustManager::f_RegisterPermissions, fg_CreateSet(BackupPermission)) > fg_DiscardResult();

				Auditor.f_Info(fg_Format("Starting backup '{}'", BackupKey.f_GetDesc()));

				auto NewInstance = pThis->mp_BackupInstances(BackupKey);
				auto &Instance = *NewInstance;

				Auditor.f_HostInfo().f_OnDisconnect
					(
						fg_ThisActor(this)
						, [pThis, BackupKey, Auditor]
						{
							pThis->fp_DestroyBackupInstance(BackupKey, Auditor, true, "Actor host disconnected (restarted)");
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

				auto fInitBackupInstance =
					[
						pThis
						, BackupKey
						, Continuation
						, Auditor
						, Subscription = fg_Move(Subscription)
						, SourceBackupKey = SourceBackupKey
						, Flags = Flags
					]
					() mutable
					{
						if (pThis->f_IsDestroyed())
							return Continuation.f_SetException(Auditor.f_Exception("Shutting down"));

						auto *pBackupInstance = pThis->mp_BackupInstances.f_FindEqual(BackupKey);
						if (!pBackupInstance || pBackupInstance->m_OwningHost.f_HostInfo() != Auditor.f_HostInfo())
						{
							Continuation.f_SetException(Auditor.f_Exception("Another backup was already started taking precedence"));
							return;
						}

						pBackupInstance->m_BackupRunningSubscription = fg_Move(Subscription);

						DCheck(pBackupInstance->m_BackupInstance.f_IsEmpty());
						pBackupInstance->m_BackupInstance = pThis->mp_AppState.m_DistributionManager->f_ConstructActor<CBackupInstance>
							(
								fg_Construct(fg_Format("Backup instance for '{}'", BackupKey.m_BackupName))
								, BackupKey.m_BackupName
								, BackupKey.m_BackupTime
								, BackupKey.m_BackupID
								, pThis->mp_AppState.m_RootDirectory
								, (Flags & CBackupManager::EInitBackupFlag_ForceNew) != 0
								, pThis->fp_CreateBackupSource(BackupKey.m_BackupName)
							)
						;

						TCDistributedActorInterfaceWithID<CBackupManagerBackup> BackupInterface
							{
								pBackupInstance->m_BackupInstance->f_ShareInterface<CBackupManagerBackup>()
								, g_ActorSubscription > [pThis, BackupKey, Auditor]() -> TCContinuation<void>
								{
									auto *pBackupInstance = pThis->mp_BackupInstances.f_FindEqual(BackupKey);
									if (!pBackupInstance || pBackupInstance->m_OwningHost.f_HostInfo() != Auditor.f_HostInfo())
										return fg_Explicit();

									auto &Instance = *pBackupInstance;

									return pThis->fp_DestroyBackupInstance(BackupKey, Instance.m_OwningHost, false, "Backup stopped remotely");
								}
							}
						;

						Auditor.f_Info(fg_Format("Backup initialized for '{}'", BackupKey.f_GetDesc()));
						Continuation.f_SetResult(fg_Move(BackupInterface));
					}
				;

				if (NewInstance.f_WasCreated())
				{
					Instance.m_OwningHost = Auditor;
					fInitBackupInstance();
				}
				else
				{
					Instance.m_OnDestroyed.f_Insert(fg_Move(fInitBackupInstance));
					pThis->fp_DestroyBackupInstance(BackupKey, Instance.m_OwningHost, false, "Old host removed"); // Remove old Host
					Instance.m_OwningHost = Auditor; // Take ownership
				}
			}
		;

		return Continuation;
	}
}
