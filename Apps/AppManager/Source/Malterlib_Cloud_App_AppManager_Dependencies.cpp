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
	
	bool CAppManagerActor::CApplication::f_DependenciesSatisfied(CStr &o_State, CAppManagerInterface::EStatusSeverity &o_Severity) const
	{
		if (m_bPreventLaunch_User)
		{
			o_State = "Prevented launch because application was previously manually stopped";
			o_Severity = CAppManagerInterface::EStatusSeverity_Warning;
			return false;
		}

		if (m_bPreventLaunch_Update)
		{
			o_State = "Prevented launch because update previously failed";
			o_Severity = CAppManagerInterface::EStatusSeverity_Error;
			return false;
		}
		
		if (m_bPreventLaunch_DelayAfterFailure)
		{
			o_State = "Prevented launch because of previous failure";
			o_Severity = CAppManagerInterface::EStatusSeverity_Error;
			return false;
		}

		if (f_IsChildApp())
		{
			if (!m_pParentApplication)
			{
				o_State = "Parent application does not exist";
				o_Severity = CAppManagerInterface::EStatusSeverity_Error;
				return false;
			}
			
			if (f_NeedsEncryption() && !m_pParentApplication->m_bEncryptionOpened)
			{
				o_State = "Parent application encryption not yet opened";
				o_Severity = CAppManagerInterface::EStatusSeverity_Warning;
				return false;
			}
		}
		else if (f_NeedsEncryption())
		{
			if (m_pThis->mp_KeyManagerSubscription.m_Actors.f_IsEmpty())
			{
				o_State = "Waiting for key manager to become available";
				o_Severity = CAppManagerInterface::EStatusSeverity_Warning;
				return false;
			}
		}
		
		for (auto &Dependency : m_Settings.m_Dependencies)
		{
			auto pDependencyApplication = m_pThis->mp_Applications.f_FindEqual(Dependency);
			if (!pDependencyApplication)
			{
				o_State = fg_Format("Dependent application '{}' does not exists", Dependency);
				o_Severity = CAppManagerInterface::EStatusSeverity_Error;
				return false;
			}
			if ((*pDependencyApplication)->m_LaunchStatusSeverity != CAppManagerInterface::EStatusSeverity_None)
			{
				o_State = fg_Format("Dependent application '{}' not yet launched: {}", Dependency, (*pDependencyApplication)->m_LaunchStatus);
				o_Severity = (*pDependencyApplication)->m_LaunchStatusSeverity;
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
		
		CStr Status;
		CAppManagerInterface::EStatusSeverity Severity;
		if (!Application.f_DependenciesSatisfied(Status, Severity))
		{
			if (!Application.f_IsLaunched())
			{
				// Don't recursively call update application dependencies
				fp_SetAppLaunchStatus(_pApplication, Status, Severity);
				return false;
			}
			
			if (Application.m_Settings.m_bStopOnDependencyFailure)
			{
				Application.f_Stop(EStopFlag_AutoStart) > [this, _pApplication, Status, Severity](TCAsyncResult<uint32> &&_Result)
					{
						auto &Application = *_pApplication;
						if (_Result)
						{
							fp_SetAppLaunchStatus(_pApplication, "Stopped because dependency failed: {}"_f << Status, Severity);
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

						fp_SetAppLaunchStatus(_pApplication, "Failed to stop (when dependency failed): {}"_f << _Result.f_GetExceptionStr(), CAppManagerInterface::EStatusSeverity_Error);
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
		fp_LaunchApp(_pApplication, true) > [this, InProgressScope, _pApplication, pCleanupAutoLaunches](TCAsyncResult<CAppLaunchResult> &&_Result)
			{
				if (_Result)
					return;

				fp_InitialStartupFailed(_Result.f_GetException());
				
				auto &Application = *_pApplication; 
				
				if (Application.m_LaunchStatus.f_IsEmpty())
					fp_AppLaunchStateChanged(_pApplication, fg_Format("Failed launch: {}", _Result.f_GetExceptionStr()), CAppManagerInterface::EStatusSeverity_Error);
				
				DMibLogWithCategory
					(
						Malterlib/Cloud/AppManager
						, Error
						, "App '{}' failed launch: {}"
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
