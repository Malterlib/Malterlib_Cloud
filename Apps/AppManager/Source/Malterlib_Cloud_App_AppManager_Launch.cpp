// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Concurrency/Actor/Timer>
#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud::NAppManager
{
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
					fp_LaunchApp(_pApplication, true) > fg_DiscardResult();
				}
			)
		;
	}
	
	TCContinuation<bool> CAppManagerActor::fp_LaunchAppInternal(TCSharedPointer<CApplication> const &_pApplication, bool _bOpenEncryption)
	{
		DCheck(!_pApplication->m_bLaunching);

		if (!_pApplication->m_ProcessLaunch.f_IsEmpty() || _pApplication->m_bLaunched)
			return DMibErrorInstance("Application is already launched");

		CStr State;
		if (!_pApplication->f_DependenciesSatisfied(State))
		{
			_pApplication->m_LaunchState = State;
			return DMibErrorInstance(fg_Format("Dependencies not satisfied: {}", State));
		}
		
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
		
		auto fLaunch = [this, _pApplication, pState]() -> TCContinuation<bool>
			{
				TCContinuation<bool> Continuation;
				auto &Application = *_pApplication;
				
				if (Application.m_Settings.m_bSelfUpdateSource)
				{
					fp_AppLaunchStateChanged(_pApplication, "Self update source - waiting for update");
					if (_pApplication->m_bJustUpdated)
					{
						_pApplication->m_bJustUpdated = false;
						Continuation = fp_SelfUpdate(_pApplication);
					}
					else
						Continuation.f_SetResult(false);
					_pApplication->m_bLaunched = true;
					return Continuation;
				}
				
				if (Application.m_Settings.m_Executable.f_IsEmpty())
				{
					fp_AppLaunchStateChanged(_pApplication, "No executable");
					_pApplication->m_bLaunched = true;
					return fg_Explicit(false);
				}
				
				fp_AppLaunchStateChanged(_pApplication, "Launching");
				
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
										fp_AppLaunchStateChanged(_pApplication, "Launched (waiting for distributed app register)");
										_pApplication->m_fOnRegisterDistributedApp = [this, pState, Continuation, _pApplication]
											{
												if (_pApplication->m_bDeleted)
													return;
												fp_AppLaunchStateChanged(_pApplication, "Launched (waiting for app to fully start)");
												DCallActor(_pApplication->m_AppInterface, CDistributedAppInterfaceClient::f_GetAppStartResult) 
													> [this, pState, Continuation, _pApplication](TCAsyncResult<void> &&_Result)
													{
														pState->m_pCleanup.f_Clear();
														if (_Result)
															fp_AppLaunchStateChanged(_pApplication, "Launched");
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
															fp_AppLaunchStateChanged(_pApplication, fg_Format("Launched (app startup failed: '{}')", _Result.f_GetExceptionStr()));
														}
														
														Continuation.f_SetResult(false);
													}
												;
											}
										;
										return;
									}
									pState->m_pCleanup.f_Clear();
									fp_AppLaunchStateChanged(_pApplication, "Launched");
									Continuation.f_SetResult(false);
								}
								break;
							case NProcess::EProcessLaunchState_LaunchFailed:
								{
									auto &LaunchError = _State.f_Get<NProcess::EProcessLaunchState_LaunchFailed>();
									if (!_pApplication->m_bStopped)
										fp_ScheduleRelaunchApp(_pApplication);
									
									fp_AppLaunchStateChanged(_pApplication, fg_Format("Failed launch: {}", LaunchError));
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
										fp_AppLaunchStateChanged
											(
												_pApplication
												, fg_Format
												(
													"{}Exited with {}. {}. {}."
													, RelaunchInfo
													, ExitStatus
													, _pApplication->m_LastStdErr
													, _pApplication->m_LastError
												)
											)
										;
									}
									else
										fp_AppLaunchStateChanged(_pApplication, fg_Format("{}Exited with {}", RelaunchInfo, ExitStatus));
									
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
			TCContinuation<bool> Continuation;
			fg_ThisActor(this)(&CAppManagerActor::fp_ChangeEncryption, _pApplication, EEncryptOperation_Open, false)
				> [Continuation, this, fLaunch, _pApplication](TCAsyncResult<void> &&_Result)
				{
					if (!_Result)
					{
						CStr Error = fg_Format("Failed to open encryption: {}", _Result.f_GetExceptionStr());
						Continuation.f_SetException(DMibErrorInstance(Error)); 
						fp_AppLaunchStateChanged(_pApplication, Error);
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
	
	TCContinuation<bool> CAppManagerActor::fp_LaunchApp(TCSharedPointer<CApplication> const &_pApplication, bool _bOpenEncryption)
	{
		if (_pApplication->m_bLaunching)
		{
			TCContinuation<bool> Continuation;
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
						CAppManagerActor::fp_LaunchAppInternal(_pApplication, _bOpenEncryption) > Continuation;
					}
				)
			;
			return Continuation;
		}
		return fp_LaunchAppInternal(_pApplication, _bOpenEncryption);
	}
}
