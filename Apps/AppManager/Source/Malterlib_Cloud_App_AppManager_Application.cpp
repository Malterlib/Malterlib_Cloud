// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_App_AppManager.h"

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
		m_fOnRegisterDistributedApp.f_Clear();
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
		return fg_OnScopeExitActor
			(
				fg_ThisActor(m_pThis)
				, [pThis = m_pThis, pApplication = TCSharedPointer<CApplication>(fg_Explicit(this))]
				{
					DCheck(pApplication->m_bOperationInProgress);
					pApplication->m_bOperationInProgress = false;
					pThis->fp_UpdateApplicationDependencies();
				}
			)
		;
	}

	TCDispatchedActorCall<uint32> CAppManagerActor::CApplication::f_Stop(EStopFlag _Flags)
	{
		return g_Dispatch > [this, _Flags, pApplication = TCSharedPointer<CApplication>(fg_Explicit(this))]() mutable -> TCContinuation<uint32>
			{
				if (pApplication->m_bDeleted)
					return fg_Explicit(0);

				TCContinuation<void> SaveStateContinuation;
				
				if (_Flags & EStopFlag_PreventLaunchUser)
					pApplication->m_bPreventLaunch_User = true;
				if (_Flags & EStopFlag_PreventLaunchUpdate)
					pApplication->m_bPreventLaunch_Update = true;
				
				if (_Flags & (EStopFlag_PreventLaunchUser | EStopFlag_PreventLaunchUpdate))
					SaveStateContinuation = m_pThis->fp_UpdateApplicationJSON(pApplication);
				else
					SaveStateContinuation.f_SetResult();
				
				TCContinuation<uint32> Continuation;
				
				TCActorResultVector<uint32> ChildrenCloses;
				
				// Stop all children and dependents first
				for (auto &pDependent : f_GetDependents())
					pDependent->f_Stop(EStopFlag_AutoStart) > ChildrenCloses.f_AddResult();
				
				for (auto &ChildApp : m_Children)
					ChildApp.f_Stop(EStopFlag_AutoStart) > ChildrenCloses.f_AddResult();
				
				ChildrenCloses.f_GetResults()
					+ SaveStateContinuation.f_Dispatch()
					> Continuation % "Failed to stop child application" 
					/ [=]
					(TCVector<TCAsyncResult<uint32>> &&_ChildrenCloseResults, CVoidTag) mutable
					{
						CStr ChildCloseErrors;
						for (auto &ChildCloseResult : _ChildrenCloseResults)
						{
							if (!ChildCloseResult)
								fg_AddStrSep(ChildCloseErrors, ChildCloseResult.f_GetExceptionStr(), "\n");
						}
						if (!ChildCloseErrors.f_IsEmpty())
						{
							Continuation.f_SetException(DMibErrorInstance(fg_Format("Errors stopping child applications: {}", ChildCloseErrors)));
							return;
						}
				
						bool bWasStopped = pApplication->m_bStopped;
						pApplication->m_bLaunched = false;
						pApplication->m_bStopped = true;
						
						if (!pApplication->m_ProcessLaunch || bWasStopped || pApplication->m_bDeleted)
						{
							pApplication->m_bAutoStart = (_Flags & EStopFlag_AutoStart) != 0;

							if (_Flags & EStopFlag_CloseEncryption)
							{
								g_Dispatch > [this, pApplication]() -> TCContinuation<uint32>
									{
										if (pApplication->m_bDeleted)
											return fg_Explicit(0);
										return f_CloseEncryption(0);
									}
									> Continuation
								;
							}
							else
							{
								Continuation.f_SetResult(0);
							}
							return;
						}

						pApplication->m_ProcessLaunch(&CProcessLaunchActor::f_StopProcess) > [=](TCAsyncResult<uint32> &&_Result)
							{
								pApplication->m_bAutoStart = (_Flags & EStopFlag_AutoStart) != 0;

								if (!_Result)
									DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Error stopping application: {}", _Result.f_GetExceptionStr());
								else
								{
									if (*_Result)
										DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Application '{}' exited with non 0 status: {}", pApplication->m_Name, *_Result);
									else
										DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Application '{}' exited cleanly", pApplication->m_Name);
									pApplication->f_Clear();
								}

								if (_Flags & EStopFlag_CloseEncryption)
								{
									if (!_Result)
									{
										Continuation.f_SetException(DMibErrorInstance(fg_Format("Error stopping process, cannot close encryption: {}", _Result.f_GetExceptionStr())));
										return;
									}
									f_CloseEncryption(*_Result) > [Continuation](TCAsyncResult<uint32> &&_Result)
										{
											if (!_Result)
												DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Error closing encryption: {}", _Result.f_GetExceptionStr());
											
											Continuation.f_SetResult(fg_Move(_Result));
										}
									;
									return;
								}
								
								Continuation.f_SetResult(fg_Move(_Result));
							}
						;
					}
				;
				
				return Continuation;
			}
		;
	}
	
	CStr CAppManagerActor::CApplication::f_GetDirectory()
	{
		if (f_IsChildApp())
			return fg_Format("{}/App/{}/{}", CFile::fs_GetProgramDirectory(), m_Settings.m_ParentApplication, m_Name);
		else
			return fg_Format("{}/App/{}", CFile::fs_GetProgramDirectory(), m_Name);
	}
	
	TCDispatchedActorCall<uint32> CAppManagerActor::CApplication::f_CloseEncryption(uint32 _Status)
	{
		return g_Dispatch > [this, _Status, pApplication = TCSharedPointer<CApplication>(fg_Explicit(this))]() -> TCContinuation<uint32>
			{
				if (m_Settings.m_EncryptionStorage.f_IsEmpty() || !m_bEncryptionOpened)
					return fg_Explicit(_Status);
				
				TCContinuation<uint32> Continuation;
				fg_ThisActor(m_pThis)(&CAppManagerActor::fp_ChangeEncryption, pApplication, EEncryptOperation_Close, false) 
					> Continuation / [_Status, Continuation]
					{
						Continuation.f_SetResult(_Status);
					}
				;
				return Continuation;
			}
		;
	}
	
	void CAppManagerActor::fp_AppLaunchStateChanged(TCSharedPointer<CApplication> const &_pApplication, CStr const &_State)
	{
		_pApplication->m_LaunchState = _State;
		fp_UpdateApplicationDependencies();
	}
}
