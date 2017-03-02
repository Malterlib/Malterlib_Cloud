// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud::NAppManager
{
	auto CAppManagerActor::CApplication::f_GetDependents() const -> TCVector<TCSharedPointer<CApplication>>
	{
		TCVector<TCSharedPointer<CApplication>> Return;

		for (auto &pApplication : m_pThis->mp_Applications)
		{
			auto &Application = *pApplication;
			if (Application.m_Settings.m_Dependencies.f_FindEqual(m_Name))
				Return.f_Insert(pApplication);
		}

		return Return;
	}
	
	bool CAppManagerActor::CApplication::f_DependenciesSatisfied(CStr &o_State) const
	{
		if (m_bPreventLaunch_User)
		{
			o_State = "Prevented launch because application was previously manually stopped";
			return false;
		}

		if (m_bPreventLaunch_Update)
		{
			o_State = "Prevented launch because update previously failed";
			return false;
		}

		if (f_IsChildApp())
		{
			if (!m_pParentApplication)
			{
				o_State = "Parent application does not exist";
				return false;
			}
			
			if (f_NeedsEncryption() && !m_pParentApplication->m_bEncryptionOpened)
			{
				o_State = "Parent application encryption not yet opened";
				return false;
			}
		}
		else if (f_NeedsEncryption())
		{
			if (m_pThis->mp_KeyManagerSubscription.m_Actors.f_IsEmpty())
			{
				o_State = "Waiting for key manager to become available";
				return false;
			}
		}
		
		for (auto &Dependency : m_Settings.m_Dependencies)
		{
			auto pDependencyApplication = m_pThis->mp_Applications.f_FindEqual(Dependency);
			if (!pDependencyApplication)
			{
				o_State = fg_Format("Dependent application '{}' does not exists", Dependency);
				return false;
			}
			if ((*pDependencyApplication)->m_LaunchState != "Launched")
			{
				o_State = fg_Format("Dependent application '{}' not yet launched: {}", Dependency, (*pDependencyApplication)->m_LaunchState);
				return false;
			}
		}
		
		return true;
	}
	
	bool CAppManagerActor::fp_AutoStartApp(TCSharedPointer<CApplication> const &_pApplication)
	{
		auto &Application = *_pApplication;
		
		if (Application.f_IsInProgress())
			return false;
		
		CStr State;
		if (!Application.f_DependenciesSatisfied(State))
		{
			if (!Application.f_IsLaunched())
			{
				Application.m_LaunchState = State; // Don't recursively call update application dependencies
				return false;
			}
			
			if (Application.m_Settings.m_bStopOnDependencyFailure)
			{
				Application.f_Stop(EStopFlag_AutoStart) > [_pApplication, State](TCAsyncResult<uint32> &&_Result)
					{
						auto &Application = *_pApplication;
						if (_Result)
						{
							Application.m_LaunchState = fg_Format("Stopped because dependency failed: {}", State);
							return;
						}
						
						DMibLogWithCategory
							(
								Malterlib/Cloud/AppManager
								, Error
								, "App '{}' failed to stop (when dependency failed): {}"
								, Application.m_Name
								, _Result.f_GetExceptionStr()
							)
						;
						
						Application.m_LaunchState = fg_Format("Failed to stop (when dependency failed): {}", _Result.f_GetExceptionStr());
					}
				;
			}
			return false;
		}

		if (Application.f_IsLaunched())
			return true;
		
		++mp_PendingAutoLaunches;
		auto pCleanupAutoLaunches = g_OnScopeExitActor > [this]
			{
				if (--mp_PendingAutoLaunches == 0)
					fp_UpdateApplicationDependencies();
			}
		;
		
		auto InProgressScope = Application.f_SetInProgress();
		fp_LaunchApp(_pApplication, true) > [this, InProgressScope, _pApplication, pCleanupAutoLaunches](TCAsyncResult<bool> &&_Result)
			{
				if (_Result)
					return;

				fp_InitialStartupFailed(_Result.f_GetException());
				
				auto &Application = *_pApplication; 
				
				if (Application.m_LaunchState.f_IsEmpty())
					fp_AppLaunchStateChanged(_pApplication, fg_Format("Failed launch: {}", _Result.f_GetExceptionStr()));
				
				DMibLogWithCategory
					(
						Malterlib/Cloud/AppManager
						, Error
						, "App '{}' failed initial launch: {}"
						, Application.m_Name
						, _Result.f_GetExceptionStr()
					)
				;
			}
		;
		
		return true;
	}
	
	void CAppManagerActor::fp_UpdateApplicationDependencies()
	{
		bool bAllStarted = true;

		for (auto &pApplication : mp_Applications)
		{
			auto &Application = *pApplication;
			if (Application.m_bStopped && !Application.m_bAutoStart)
				continue;
			
			if (!fp_AutoStartApp(pApplication))
				bAllStarted = false;
		}

		if (bAllStarted && mp_PendingAutoLaunches == 0)
		{
			if (!mp_InitialStartupResult.f_IsSet())
				mp_InitialStartupResult.f_SetResult();
		}

		if (mp_bPendingAutoUpdate)
			fp_AutoUpdate_Update();
	}
}
