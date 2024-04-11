// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_App_AppManager.h"
#include <Mib/Concurrency/LogError>

namespace NMib::NCloud::NAppManager
{
	void CAppManagerActor::CApplication::f_Clear()
	{
		m_ProcessLaunch.f_Set<0>();
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

	bool CAppManagerActor::CApplication::f_EncryptionOpened() const
	{
		if (m_pParentApplication && !m_pParentApplication->f_EncryptionOpened())
			return false;

		if (m_Settings.m_EncryptionStorage.f_IsEmpty())
			return true;

		return m_bEncryptionOpened;
	}

	bool CAppManagerActor::CApplication::f_IsChildApp() const
	{
		return !m_Settings.m_ParentApplication.f_IsEmpty();
	}

	bool CAppManagerActor::CApplication::f_IsLaunched() const
	{
		return m_bLaunching || !m_ProcessLaunch.f_IsOfType<void>() || m_bLaunched;
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

	CActorSubscription CAppManagerActor::CApplication::f_SetInProgress()
	{
		DRequire(!f_IsInProgress());
		m_bOperationInProgress = true;
		return g_ActorSubscription / [pThis = m_pThis, pApplication = TCSharedPointer<CApplication>(fg_Explicit(this))]
			{
				DCheck(pApplication->m_bOperationInProgress);
				pApplication->m_bOperationInProgress = false;
				pThis->fp_UpdateApplicationDependencies();
			}
		;
	}

	TCFuture<uint32> CAppManagerActor::CApplication::f_Stop(EStopFlag _Flags)
	{
		return fg_CallSafe
			(
				TCFunctionMovable<TCFuture<uint32> ()>
				(
					[this, _Flags, pApplication = TCSharedPointer<CApplication>(fg_Explicit(this))]() mutable -> TCFuture<uint32>
					{
						if (pApplication->m_bDeleted)
							co_return 0;

						if (_Flags & EStopFlag_PreventLaunchUser)
							pApplication->m_bPreventLaunch_User = true;
						if (_Flags & EStopFlag_PreventLaunchUpdate)
							pApplication->m_bPreventLaunch_Update = true;

						TCPromise<void> SaveStatePromise;

						if (_Flags & (EStopFlag_PreventLaunchUser | EStopFlag_PreventLaunchUpdate))
							m_pThis->fp_UpdateApplicationJSON(pApplication) > SaveStatePromise;
						else
							SaveStatePromise.f_SetResult();

						bool bWasStopped = pApplication->m_bStopped;
						pApplication->m_bLaunched = false;
						pApplication->m_bStopped = true;

						while (pApplication->m_bPendingStop)
							co_await m_OnStops.f_Insert().f_Future();

						pApplication->m_bPendingStop = true;

						bool bSuccessfulStop = false;
						auto Cleanup = g_OnScopeExit / [&, pApplication]
							{
								pApplication->m_bPendingStop = false;
								auto OnStops = fg_Move(pApplication->m_OnStops);
								for (auto &OnStop : OnStops)
								{
									if (bSuccessfulStop)
										OnStop.f_SetResult();
									else
										OnStop.f_SetException(DMibErrorInstance("Pending stop failed"));
								}
							}
						;

						TCActorResultVector<uint32> ChildrenCloses;

						// Stop all children and dependents first
						for (auto &pDependent : f_GetDependents())
							fg_CallSafe(*pDependent, &CApplication::f_Stop, EStopFlag_AutoStart) > ChildrenCloses.f_AddResult();

						for (auto &ChildApp : m_Children)
							fg_CallSafe(ChildApp, &CApplication::f_Stop, EStopFlag_AutoStart) > ChildrenCloses.f_AddResult();

						m_pThis->fp_AppLaunchStateChanged(pApplication, "Stopping", CAppManagerInterface::EStatusSeverity_Warning);

						auto [ChildrenCloseResults, SaveStateResult] = co_await ((ChildrenCloses.f_GetResults() + SaveStatePromise.f_MoveFuture()) % "Failed to stop child application");

						CStr ChildCloseErrors;
						for (auto &ChildCloseResult : ChildrenCloseResults)
						{
							if (!ChildCloseResult)
								fg_AddStrSep(ChildCloseErrors, ChildCloseResult.f_GetExceptionStr(), "\n");
						}

						if (!ChildCloseErrors.f_IsEmpty())
							co_return DMibErrorInstance(fg_Format("Errors stopping child applications: {}", ChildCloseErrors));

						TCAsyncResult<uint32> StopResult;
						if (pApplication->m_ProcessLaunch.f_IsOfType<void>() || bWasStopped || pApplication->m_bDeleted)
							StopResult.f_SetResult(0);
						else
						{
							bool bRanPreStop = false;

							TCAsyncResult<void> PreStopResult;
							if (pApplication->m_AppInterface && pApplication->m_AppInterface->f_InterfaceVersion() >= CDistributedAppInterfaceClient::EProtocolVersion_SupportPreStop)
							{
								DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Pre-stopping application '{}'", pApplication->m_Name);
								bRanPreStop = true;

								PreStopResult = co_await pApplication->m_AppInterface.f_CallActor(&CDistributedAppInterfaceClient::f_PreStop)()
									.f_Timeout(60.0 * 60.0, "Timed out waiting for application pre stop (1 hour)")
									.f_Wrap()
								;
								if (!PreStopResult)
									DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Error pre-stopping application: {}", PreStopResult.f_GetExceptionStr());
							}
							else
								PreStopResult.f_SetResult();

							CLogError LogError("Malterlib/Cloud/AppManager");

							if (bRanPreStop && pApplication->m_BackupClient && PreStopResult)
							{
								auto DestroyBackupResult = co_await fg_Move(pApplication->m_BackupClient).f_Destroy().f_Wrap();

								if (!DestroyBackupResult)
									LogError.f_Log("Error stopping application backup", DestroyBackupResult);
							}

							DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Stopping process '{}'", pApplication->m_Name);

							if (pApplication->m_ProcessLaunch.f_IsOfType<TCActor<CDistributedAppInterfaceLaunchActor>>())
								StopResult = co_await pApplication->m_ProcessLaunch.f_GetAsType<TCActor<CDistributedAppInterfaceLaunchActor>>()(&CProcessLaunchActor::f_StopProcess).f_Wrap();
							else if (pApplication->m_ProcessLaunch.f_IsOfType<TCActor<CDistributedAppInProcessActor>>())
							{
								auto Result = co_await pApplication->m_ProcessLaunch.f_GetAsType<TCActor<CDistributedAppInProcessActor>>().f_Destroy().f_Wrap();
								if (!Result)
									StopResult.f_SetException(fg_Move(Result));
								else
									StopResult.f_SetResult(0);
							}
							else
								StopResult.f_SetException(DMibErrorInstance("Application no longer running, cannot stop"));
								
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

						if (pApplication->m_LaunchStatusSeverity == CAppManagerInterface::EStatusSeverity_None || pApplication->m_LaunchStatus == "Stopping")
							m_pThis->fp_AppLaunchStateChanged(pApplication, "Stopped", CAppManagerInterface::EStatusSeverity_Warning);

						if (pApplication->m_BackupClient)
						{
							auto BackupDestroyResult = co_await fg_Move(pApplication->m_BackupClient).f_Destroy().f_Wrap();
							if (!BackupDestroyResult)
								DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Error stopping application backup: {}", BackupDestroyResult.f_GetExceptionStr());
						}

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

							bSuccessfulStop = true;

							co_return fg_Move(CloseEncryptionResult);
						}

						bSuccessfulStop = true;

						co_return fg_Move(StopResult);
					}
				)
			)
		;
	}

	CStr CAppManagerActor::CApplication::f_GetDirectory()
	{
		if (f_IsChildApp())
			return fg_Format("{}/App/{}/{}", m_pThis->mp_State.m_RootDirectory, m_Settings.m_ParentApplication, m_Name);
		else
			return fg_Format("{}/App/{}", m_pThis->mp_State.m_RootDirectory, m_Name);
	}

	TCFuture<uint32> CAppManagerActor::CApplication::f_CloseEncryption(uint32 _Status)
	{
		return fg_CallSafe
			(
				TCFunctionMovable<TCFuture<uint32> ()>
				(
					[this, _Status, pApplication = TCSharedPointer<CApplication>(fg_Explicit(this))]() -> TCFuture<uint32>
					{
						if (m_Settings.m_EncryptionStorage.f_IsEmpty() || !m_bEncryptionOpened)
							co_return _Status;

						co_await m_pThis->self(&CAppManagerActor::fp_ChangeEncryption, pApplication, EEncryptOperation_Close, false);

						co_return _Status;
					}
				)
			)
		;
	}

	void CAppManagerActor::fp_AppLaunchStateChanged(TCSharedPointer<CApplication> const &_pApplication, CStr const &_State, CAppManagerInterface::EStatusSeverity _Severity)
	{
		fp_SetAppLaunchStatus(_pApplication, _State, _Severity);
		fp_UpdateApplicationDependencies();
	}
}
