// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_App_AppManager.h"
#include <Mib/Concurrency/LogError>

namespace NMib::NCloud::NAppManager
{
	void CAppManagerActor::CApplication::f_Clear()
	{
		m_ProcessLaunch.f_Clear();
		m_ProcessLaunchSubscription.f_Clear();
		m_bLaunched = false;
	}
	
	void CAppManagerActor::CApplication::f_AbortPendingLaunches()
	{
		for (auto &fOnFinished : m_OnLaunchFinished)
			fOnFinished(true);
		m_OnLaunchFinished.f_Clear();
		for (auto &Promise : m_OnRegisterDistributedApp)
			Promise.f_SetException(DMibErrorInstance("Aborted launch"));
		for (auto &Promise : m_OnStartedDistributedApp)
			Promise.f_SetException(DMibErrorInstance("Aborted launch"));

		m_OnRegisterDistributedApp.f_Clear();
		m_OnStartedDistributedApp.f_Clear();
	}

	void CAppManagerActor::CApplication::f_Delete()
	{
		f_AbortPendingLaunches();
		f_Clear();
		m_bDeleted = true;
		m_pParentApplication = nullptr;
		m_ChildrenLink.f_Unlink();
		for (auto &Child : m_Children)
			Child.m_pParentApplication = nullptr;
		m_Children.f_Clear();
	}
	
	bool CAppManagerActor::CApplication::f_NeedsEncryption() const
	{
		if (!m_Settings.m_EncryptionStorage.f_IsEmpty())
			return true;
		if (m_pParentApplication && m_pParentApplication->f_NeedsEncryption())
			return true;
		return false;
	}
	
	bool CAppManagerActor::CApplication::f_IsChildApp() const
	{
		return !m_Settings.m_ParentApplication.f_IsEmpty();
	}
	
	bool CAppManagerActor::CApplication::f_IsLaunched() const
	{
		return m_bLaunching || !m_ProcessLaunch.f_IsEmpty() || m_bLaunched;
	}
	
	bool CAppManagerActor::CApplication::f_IsInProgress() const
	{
		if (m_bOperationInProgress)
			return true;
		
		if (m_pParentApplication && m_pParentApplication->m_bOperationInProgress)
			return true;
		
		for (auto &Child : m_Children)
		{
			if (Child.m_bOperationInProgress)
				return true;
		}
		
		return false;
	}

	COnScopeExitShared CAppManagerActor::CApplication::f_SetInProgress()
	{
		DRequire(!f_IsInProgress());
		m_bOperationInProgress = true;
		return g_OnScopeExitActor(fg_ThisActor(m_pThis)) > [pThis = m_pThis, pApplication = TCSharedPointer<CApplication>(fg_Explicit(this))]
			{
				DCheck(pApplication->m_bOperationInProgress);
				pApplication->m_bOperationInProgress = false;
				pThis->fp_UpdateApplicationDependencies();
			}
		;
	}

	TCDispatchedActorCall<uint32> CAppManagerActor::CApplication::f_Stop(EStopFlag _Flags)
	{
		return g_Dispatch / [this, _Flags, pApplication = TCSharedPointer<CApplication>(fg_Explicit(this))]() mutable -> TCFuture<uint32>
			{
				if (pApplication->m_bDeleted)
					co_return 0;

				if (_Flags & EStopFlag_PreventLaunchUser)
					pApplication->m_bPreventLaunch_User = true;
				if (_Flags & EStopFlag_PreventLaunchUpdate)
					pApplication->m_bPreventLaunch_Update = true;
				
				TCFuture<void> SaveStateFuture;

				if (_Flags & (EStopFlag_PreventLaunchUser | EStopFlag_PreventLaunchUpdate))
					SaveStateFuture = m_pThis->fp_UpdateApplicationJSON(pApplication);
				else
					SaveStateFuture = fg_Explicit();
				
				TCPromise<uint32> Promise;
				
				TCActorResultVector<uint32> ChildrenCloses;
				
				// Stop all children and dependents first
				for (auto &pDependent : f_GetDependents())
					pDependent->f_Stop(EStopFlag_AutoStart) > ChildrenCloses.f_AddResult();
				
				for (auto &ChildApp : m_Children)
					ChildApp.f_Stop(EStopFlag_AutoStart) > ChildrenCloses.f_AddResult();
				
				auto [ChildrenCloseResults, SaveStateResult] = co_await ((ChildrenCloses.f_GetResults() + SaveStateFuture) % "Failed to stop child application");

				CStr ChildCloseErrors;
				for (auto &ChildCloseResult : ChildrenCloseResults)
				{
					if (!ChildCloseResult)
						fg_AddStrSep(ChildCloseErrors, ChildCloseResult.f_GetExceptionStr(), "\n");
				}

				if (!ChildCloseErrors.f_IsEmpty())
					co_return DMibErrorInstance(fg_Format("Errors stopping child applications: {}", ChildCloseErrors));

				bool bWasStopped = pApplication->m_bStopped;
				pApplication->m_bLaunched = false;
				pApplication->m_bStopped = true;

				TCAsyncResult<uint32> StopResult;
				if (!pApplication->m_ProcessLaunch || bWasStopped || pApplication->m_bDeleted)
					StopResult.f_SetResult(0);
				else
				{
					TCFuture<void> PreStopFuture;

					bool bRanPreStop = false;

					if (pApplication->m_AppInterface && pApplication->m_AppInterface->f_InterfaceVersion() >= 0x103)
					{
						DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Pre-stopping application '{}'", pApplication->m_Name);
						bRanPreStop = true;
						PreStopFuture = DMibCallActor(pApplication->m_AppInterface, CDistributedAppInterfaceClient::f_PreStop);
					}
					else
						PreStopFuture = fg_Explicit();

					auto PreStopResult = co_await PreStopFuture.f_Timeout(60.0 * 60.0, "Timed out waiting for application pre stop (1 hour)").f_Wrap();
					if (!PreStopResult)
						DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Error pre-stopping application: {}", PreStopResult.f_GetExceptionStr());

					CLogError LogError("Malterlib/Cloud/AppManager");

					if (bRanPreStop && pApplication->m_BackupClient && PreStopResult)
					{
						auto BackupClientDestroyFuture = pApplication->m_BackupClient->f_Destroy();
						pApplication->m_BackupClient.f_Clear();

						auto DestroyBackupResult = co_await BackupClientDestroyFuture.f_Wrap();

						if (!DestroyBackupResult)
							LogError.f_Log("Error stopping application backup", DestroyBackupResult);
					}

					DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Stopping process '{}'", pApplication->m_Name);

					StopResult = co_await pApplication->m_ProcessLaunch(&CProcessLaunchActor::f_StopProcess).f_Wrap();
					if (!StopResult)
						DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Error stopping application: {}", StopResult.f_GetExceptionStr());
					else
					{
						if (*StopResult)
						{
							DMibLogWithCategory
								(
									Malterlib/Cloud/AppManager
									, Error
									, "Application '{}' exited with non 0 status: {}"
									, pApplication->m_Name
									, *StopResult
								)
							;
						}
						else
							DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Application '{}' exited cleanly", pApplication->m_Name);
						pApplication->f_Clear();
					}
				}

				TCFuture<void> BackupDestroyFuture;
				if (pApplication->m_BackupClient)
				{
					BackupDestroyFuture = pApplication->m_BackupClient->f_Destroy();
					pApplication->m_BackupClient.f_Clear();
				}
				else
					BackupDestroyFuture = fg_Explicit();

				auto BackupDestroyResult = co_await BackupDestroyFuture.f_Wrap();
				if (!BackupDestroyResult)
					DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Error stopping application backup: {}", BackupDestroyResult.f_GetExceptionStr());

				pApplication->m_bAutoStart = (_Flags & EStopFlag_AutoStart) != 0;

				if (_Flags & EStopFlag_CloseEncryption)
				{
					if (!StopResult)
						co_return DMibErrorInstance(fg_Format("Error stopping process, cannot close encryption: {}", StopResult.f_GetExceptionStr()));

					if (pApplication->m_bDeleted)
						co_return 0;

					auto CloseEncryptionResult = co_await f_CloseEncryption(*StopResult).f_Wrap();
					if (!CloseEncryptionResult)
						DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Error closing encryption: {}", CloseEncryptionResult.f_GetExceptionStr());

					co_return fg_Move(CloseEncryptionResult);
				}

				co_return fg_Move(StopResult);
			}
		;
	}
	
	CStr CAppManagerActor::CApplication::f_GetDirectory()
	{
		if (f_IsChildApp())
			return fg_Format("{}/App/{}/{}", m_pThis->mp_State.m_RootDirectory, m_Settings.m_ParentApplication, m_Name);
		else
			return fg_Format("{}/App/{}", m_pThis->mp_State.m_RootDirectory, m_Name);
	}
	
	TCDispatchedActorCall<uint32> CAppManagerActor::CApplication::f_CloseEncryption(uint32 _Status)
	{
		return g_Dispatch / [this, _Status, pApplication = TCSharedPointer<CApplication>(fg_Explicit(this))]() -> TCFuture<uint32>
			{
				if (m_Settings.m_EncryptionStorage.f_IsEmpty() || !m_bEncryptionOpened)
					co_return _Status;

				co_await m_pThis->self(&CAppManagerActor::fp_ChangeEncryption, pApplication, EEncryptOperation_Close, false);

				co_return _Status;
			}
		;
	}
	
	void CAppManagerActor::fp_AppLaunchStateChanged(TCSharedPointer<CApplication> const &_pApplication, CStr const &_State)
	{
		_pApplication->m_LaunchState = _State;
		fp_UpdateApplicationDependencies();
	}
}
