// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Concurrency/Actor/Timer>
#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud::NAppManager
{
	void CAppManagerActor::fp_DoInitialApplicationLaunch(TCSharedPointer<CApplication> const &_pApplication)
	{
		if (_pApplication->f_IsChildApp())
		{
			if (_pApplication->m_LaunchState.f_IsEmpty())
				_pApplication->m_LaunchState = "Waiting for parent application to launch";
			return;
		}
		if (_pApplication->f_IsInProgress())
		{
			if (_pApplication->m_LaunchState.f_IsEmpty())
				_pApplication->m_LaunchState = "In progress operation prevented inital launch";
			return;
		}
		auto InProgressScope = _pApplication->f_SetInProgress();
		fg_ThisActor(this)(&CAppManagerActor::fp_LaunchApp, _pApplication, true) > [InProgressScope, _pApplication](TCAsyncResult<void> &&_Result)
			{
				if (_Result)
					return;
				if (_pApplication->m_LaunchState.f_IsEmpty())
					_pApplication->m_LaunchState = fg_Format("Failed launch: {}", _Result.f_GetExceptionStr());
				DMibLogWithCategory
					(
						Malterlib/Cloud/AppManager
						, Error
						, "App '{}' failed initial launch: {}"
						, _pApplication->m_Name
						, _Result.f_GetExceptionStr()
					)
				;
			}
		;
	}
	
	void CAppManagerActor::fp_LaunchEncryptedApps()
	{
		for (auto &pApplication : mp_Applications)
		{
			if (!pApplication->f_NeedsEncryption())
				continue;
			fp_DoInitialApplicationLaunch(pApplication);
		}
	}

	void CAppManagerActor::fp_LaunchNormalApps()
	{
		for (auto &pApplication : mp_Applications)
		{
			if (pApplication->f_NeedsEncryption())
			{
				if (pApplication->m_LaunchState.f_IsEmpty())
					pApplication->m_LaunchState = "Waiting for key manager to become available";
				continue;
			}
			fp_DoInitialApplicationLaunch(pApplication);
		}
	}

	void CAppManagerActor::fp_ScheduleRelaunchApp(TCSharedPointer<CApplication> const &_pApplication)
	{
		_pApplication->f_Clear();
		fg_OneshotTimer
			(
				10.0
				, [this, _pApplication]
				{
					if (_pApplication->m_bDeleted || _pApplication->m_ProcessLaunch || _pApplication->m_bStopped || _pApplication->m_bOperationInProgress)
						return;
					fg_ThisActor(this)(&CAppManagerActor::fp_LaunchApp, _pApplication, true) > fg_DiscardResult();
				}
			)
		;
	}
	
	TCContinuation<void> CAppManagerActor::fp_LaunchAppInternal(TCSharedPointer<CApplication> const &_pApplication, bool _bOpenEncryption)
	{
		DCheck(!_pApplication->m_bLaunching);

		if (!_pApplication->m_ProcessLaunch.f_IsEmpty())
			return DMibErrorInstance("Application is already launched");
		if (_pApplication->f_IsChildApp() && !_pApplication->m_pParentApplication)
			return DMibErrorInstance("Missing parent application");
		
		_pApplication->m_bLaunching = true;
		_pApplication->m_bStopped = false;
		struct CState
		{
			COnScopeExitShared m_pCleanup;
		};
		
		TCSharedPointer<CState> pState = fg_Construct();
		pState->m_pCleanup = fg_OnScopeExitShared
			(
				[_pApplication]
				{
					_pApplication->m_bLaunching = false;
					if (!_pApplication->m_OnLaunchFinished.f_IsEmpty())
					{
						auto fOnFinished = _pApplication->m_OnLaunchFinished.f_Pop();
						fOnFinished(false);
					}
				}
			)
		;
		
		auto fLaunch = [this, _pApplication, pState]() -> TCContinuation<void>
			{
				TCContinuation<void> Continuation;
				auto &Application = *_pApplication;
				
				if (Application.m_Settings.m_bSelfUpdateSource)
				{
					_pApplication->m_LaunchState = "Self update source - waiting for update";
					if (_pApplication->m_bJustUpdated)
					{
						_pApplication->m_bJustUpdated = false;
						fp_SelfUpdate(_pApplication);
					}
					return fg_Explicit();
				}
				if (Application.m_Settings.m_Executable.f_IsEmpty())
				{
					_pApplication->m_LaunchState = "No executable";
					return fg_Explicit();
				}
				for (auto &ChildApp : Application.m_Children)
				{
					// Launch children
					TCSharedPointer<CApplication> pChildApp = fg_Explicit(&ChildApp);
					self(&CAppManagerActor::fp_LaunchApp, pChildApp, false) > fg_DiscardResult();							
				}
				
				_pApplication->m_LaunchState = "Launching";
				
				CStr ApplicationDirectory = Application.f_GetDirectory();
				
				CProcessLaunchActor::CLaunch Launch = CProcessLaunchParams::fs_LaunchExecutable
					(
						fg_Format("{}/{}", ApplicationDirectory, Application.m_Settings.m_Executable)
						, Application.m_Settings.m_ExecutableParameters
						, ApplicationDirectory
						, [this, _pApplication, pState, Continuation](CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)
						{
							if (_pApplication->m_bDeleted)
								return;
							
							switch (_State.f_GetTypeID())
							{
							case NProcess::EProcessLaunchState_Launched:
								{
									if (_pApplication->m_Settings.m_bDistributedApp)
									{
										_pApplication->m_LaunchState = "Launched (waiting for distributed app register)";
										_pApplication->m_fOnRegisterDistributedApp = [pState, Continuation, _pApplication]
											{
												if (_pApplication->m_bDeleted)
													return;
												_pApplication->m_LaunchState = "Launched (waiting for app to fully start)";
												DCallActor(_pApplication->m_AppInterface, CDistributedAppInterfaceClient::f_GetAppStartResult) 
													> [pState, Continuation, _pApplication](TCAsyncResult<void> &&_Result)
													{
														pState->m_pCleanup.f_Clear();
														if (_Result)
															_pApplication->m_LaunchState = "Launched";
														else
														{
															DMibLogWithCategory
																(
																	Malterlib/Cloud/AppManager
																	, Error
																	, "Launched app '{}' failed to start up: {}"
																	, _pApplication->m_Name
																	, _Result.f_GetExceptionStr()
																)
															;
															_pApplication->m_LaunchState = fg_Format("Launched (app startup failed: '{}')", _Result.f_GetExceptionStr());
														}
														
														Continuation.f_SetResult();
													}
												;
											}
										;
										return;
									}
									pState->m_pCleanup.f_Clear();
									_pApplication->m_LaunchState = "Launched";
									Continuation.f_SetResult();
								}
								break;
							case NProcess::EProcessLaunchState_LaunchFailed:
								{
									auto &LaunchError = _State.f_Get<NProcess::EProcessLaunchState_LaunchFailed>();
									fp_ScheduleRelaunchApp(_pApplication);
									
									_pApplication->m_LaunchState = fg_Format("Failed launch: {}", LaunchError);
									pState->m_pCleanup.f_Clear();
									Continuation.f_SetException(DMibErrorInstance(LaunchError));
								}
								break;
							case NProcess::EProcessLaunchState_Exited:
								{
									auto ExitStatus = _State.f_Get<NProcess::EProcessLaunchState_Exited>();

									CStr RelaunchInfo;
									if (!_pApplication->m_bStopped)
										RelaunchInfo = "Waiting to retry launching.";
									
									if (ExitStatus)
									{
										_pApplication->m_LaunchState = fg_Format
											(
												"{}Exited with {}. {}. {}."
												, RelaunchInfo
												, ExitStatus
												, _pApplication->m_LastStdErr
												, _pApplication->m_LastError
											)
										;
									}
									else
										_pApplication->m_LaunchState = fg_Format("{}Exited with {}", RelaunchInfo, ExitStatus);
									
									if (!_pApplication->m_bStopped)
										fp_ScheduleRelaunchApp(_pApplication);
								}
								break;
							}
						}
					)
				;
				
				Launch.m_ToLog = CProcessLaunchActor::ELogFlag_All;
				if (mp_bLogLaunchesToStdErr)
					Launch.m_ToLog |= CProcessLaunchActor::ELogFlag_All | CProcessLaunchActor::ELogFlag_AdditionallyOutputToStdErr;
				Launch.m_LogName = _pApplication->m_Name;
				
				auto &LaunchParams = Launch.m_Params;
				
				LaunchParams.m_fOnOutput = [this, _pApplication](EProcessLaunchOutputType _OutputType, NMib::NStr::CStr const &_Output)
					{
						if (_Output.f_IsEmpty())
							return;
						if (_OutputType == EProcessLaunchOutputType_StdErr)
							_pApplication->m_LastStdErr = _Output.f_TrimRight();
						else if (_OutputType != EProcessLaunchOutputType_StdOut)
							_pApplication->m_LastError = _Output.f_TrimRight();
					}
				;
				
				LaunchParams.m_RunAsUser = Application.m_Settings.m_RunAsUser;
				LaunchParams.m_RunAsGroup = Application.m_Settings.m_RunAsGroup;
				LaunchParams.m_Environment["HOME"] = ApplicationDirectory + "/.home";
				LaunchParams.m_Environment["TMPDIR"] = ApplicationDirectory + "/.tmp";
				LaunchParams.m_bMergeEnvironment = true;
				LaunchParams.m_bCreateNewProcessGroup = true;
				
				Application.m_ProcessLaunch = fg_ConstructActor<CDistributedAppInterfaceLaunchActor>
					(
						mp_State.m_LocalAddress
						, mp_State.m_TrustManager
						, g_ActorFunctor > [_pApplication, this]
						(CStr const &_HostID, CCallingHostInfo const &_HostInfo, TCVector<uint8> const &_Certificate) -> TCContinuation<void>
						{
							if (_pApplication->m_bDeleted)
								return DMibErrorInstance("Application deleted");
							
							_pApplication->m_AssociatedHostID = _HostID;
							
							TCContinuation<void> Continuation;
							fp_UpdateApplicationJSON(_pApplication) > [_pApplication, Continuation](TCAsyncResult<void> &&_Result)
								{
									if (!_Result)
									{
										DMibLogWithCategory
											(
												Malterlib/Cloud/AppManager
												, Info
												, "Failed to update application JSON when granting connection ticket for '{}': {}"
												, _pApplication->m_Name
												, _Result.f_GetExceptionStr()
											)
										;
										Continuation.f_SetException(DMibErrorInstance("Failed to update application JSON, see AppManager log for details"));
										return;
									}
									Continuation.f_SetResult();
								}
							;
							return Continuation;
						}
						, Application.m_Name
						, false
					)
				;
				
				Application.m_ProcessLaunch
					(
						&CProcessLaunchActor::f_Launch
						, Launch
						, fg_ThisActor(this)
					)
					> [this, _pApplication, pState, Continuation](TCAsyncResult<CActorSubscription> &&_Subscription)
					{
						if (_pApplication->m_bDeleted)
							return;
						if (!_Subscription)
						{
							pState->m_pCleanup.f_Clear();
							Continuation.f_SetException(_Subscription);
							DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Failed to launch app '{}': {}", _pApplication->m_Name, _Subscription.f_GetExceptionStr());
							fp_ScheduleRelaunchApp(_pApplication);
							return;
						}
						_pApplication->m_ProcessLaunchSubscription = fg_Move(*_Subscription);
					}
				;
				
				return Continuation;
			}
		;
		if (_bOpenEncryption)
		{
			TCContinuation<void> Continuation;
			fg_ThisActor(this)(&CAppManagerActor::fp_ChangeEncryption, _pApplication, EEncryptOperation_Open, false)
				> [Continuation, this, fLaunch, _pApplication](TCAsyncResult<void> &&_Result)
				{
					if (!_Result)
					{
						CStr Error = fg_Format("Failed to open encryption: {}", _Result.f_GetExceptionStr());
						Continuation.f_SetException(DMibErrorInstance(Error)); 
						_pApplication->m_LaunchState = Error;
						if (!_pApplication->m_bStopped && !_pApplication->m_bDeleted)
							fp_ScheduleRelaunchApp(_pApplication);
						return;
					}
					
					fLaunch() > Continuation;
				}
			;
			return Continuation;
		}
		
		return fLaunch();
	}
	
	TCContinuation<void> CAppManagerActor::fp_LaunchApp(TCSharedPointer<CApplication> const &_pApplication, bool _bOpenEncryption)
	{
		if (_pApplication->m_bLaunching)
		{
			TCContinuation<void> Continuation;
			_pApplication->m_OnLaunchFinished.f_Insert
				(
					[this, Continuation, _pApplication, _bOpenEncryption](bool _bAborted)
					{
						if (_bAborted)
						{
							Continuation.f_SetException(DMibErrorInstance("Application was deleted or shut down before app could attempt launching"));
							return;
						}
						
						DCheck(!_pApplication->m_bLaunching);
						fg_ThisActor(this)(&CAppManagerActor::fp_LaunchAppInternal, _pApplication, _bOpenEncryption) > Continuation;
					}
				)
			;
			return Continuation;
		}
		return fp_LaunchAppInternal(_pApplication, _bOpenEncryption);
	}
}
