// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud::NAppManager
{
	void CAppManagerActor::CApplication::f_Clear()
	{
		m_ProcessLaunch.f_Clear();
		m_ProcessLaunchSubscription.f_Clear();
	}
	
	void CAppManagerActor::CApplication::f_AbortPendingLaunches()
	{
		for (auto &fOnFinished : m_OnLaunchFinished)
			fOnFinished(true);
		m_OnLaunchFinished.f_Clear();
	}

	void CAppManagerActor::CApplication::f_Delete()
	{
		f_AbortPendingLaunches();
		f_Clear();
		m_bDeleted = true;
	}

	COnScopeExitShared CAppManagerActor::CApplication::f_SetInProgress()
	{
		DRequire(!m_bOperationInProgress);
		m_bOperationInProgress = true;
		return fg_OnScopeExitActor
			(
				fg_ThisActor(m_pThis)
				, [pThis = m_pThis, pApplication = TCSharedPointer<CApplication>(fg_Explicit(this))]
				{
					DCheck(pApplication->m_bOperationInProgress);
					pApplication->m_bOperationInProgress = false;
					
					if (pThis->mp_bPendingAutoUpdate)
						pThis->fp_AutoUpdate_Update();
				}
			)
		;
	}

	TCDispatchedActorCall<uint32> CAppManagerActor::CApplication::f_Stop(bool _bCloseEncryption)
	{
		return fg_Dispatch
			(
				[this, pApplication = TCSharedPointer<CApplication>(fg_Explicit(this)), _bCloseEncryption]() -> TCContinuation<uint32>
				{
					if (!pApplication->m_ProcessLaunch || pApplication->m_bStopped || pApplication->m_bDeleted)
					{
						if (_bCloseEncryption)
							return f_CloseEncryption(0);
						else
							return fg_Explicit(0);
					}
					pApplication->m_bStopped = true;
					TCContinuation<uint32> Continuation;
					pApplication->m_ProcessLaunch(&CProcessLaunchActor::f_StopProcess) > [this, Continuation, pApplication, _bCloseEncryption](TCAsyncResult<uint32> &&_Result)
						{
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

							if (_bCloseEncryption)
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
					
					return Continuation;
				}
			)
		;
		
	}
	
	CStr CAppManagerActor::CApplication::f_GetDirectory()
	{
		return fg_Format("{}/App/{}", CFile::fs_GetProgramDirectory(), m_Name);
	}
	
	TCDispatchedActorCall<uint32> CAppManagerActor::CApplication::f_CloseEncryption(uint32 _Status)
	{
		return fg_Dispatch
			(
				[this, _Status, pApplication = TCSharedPointer<CApplication>(fg_Explicit(this))]() -> TCContinuation<uint32>
				{
					if (m_EncryptionStorage.f_IsEmpty() || !m_bEncryptionOpened)
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
			)
		;
	}
}
