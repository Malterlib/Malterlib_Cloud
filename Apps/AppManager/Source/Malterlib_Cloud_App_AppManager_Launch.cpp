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
		_pApplication->m_bPreventLaunch_DelayAfterFailure = true;
		fg_OneshotTimer
			(
				10.0
				, [this, _pApplication]
				{
					_pApplication->m_bPreventLaunch_DelayAfterFailure = false;
					fp_UpdateApplicationDependencies();
				}
			)
		;
	}

	auto CAppManagerActor::fp_LaunchAppInternal(TCSharedPointer<CApplication> const &_pApplication, bool _bOpenEncryption)
		-> TCFuture<CAppLaunchResult>
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
					_pApplication->m_AppInterface.f_Clear();
					_pApplication->m_bLaunching = false;
					if (!_pApplication->m_OnLaunchFinished.f_IsEmpty())
					{
						auto fOnFinished = _pApplication->m_OnLaunchFinished.f_Pop();
						fOnFinished(false);
					}
				}
			)
		;
		
		auto fLaunch = [this, _pApplication, pState]() -> TCFuture<CAppLaunchResult>
			{
				TCPromise<CAppLaunchResult> Promise;
				auto &Application = *_pApplication;
				
				if (Application.m_Settings.m_bSelfUpdateSource)
				{
					fp_AppLaunchStateChanged(_pApplication, "Self update source - waiting for update");
					if (_pApplication->m_bJustUpdated)
					{
						_pApplication->m_bJustUpdated = false;
						fp_SelfUpdate(_pApplication) > Promise / [Promise](bool _bQuitManager)
							{
								CAppLaunchResult Result;
								Result.m_bQuitManager = _bQuitManager;
								Promise.f_SetResult(fg_Move(Result));
							}
						;
					}
					else
						Promise.f_SetResult(CAppLaunchResult{});
					_pApplication->m_bLaunched = true;
					return Promise.f_MoveFuture();
				}
				
				if (Application.m_Settings.m_Executable.f_IsEmpty())
				{
					fp_AppLaunchStateChanged(_pApplication, "No executable");
					_pApplication->m_bLaunched = true;
					return fg_Explicit(CAppLaunchResult{});
				}
				
				fp_AppLaunchStateChanged(_pApplication, "Launching");
				
				if (Application.m_Settings.m_bBackupEnabled && !Application.m_BackupClient)
					fp_ApplicationStartBackup(_pApplication);
				
				CStr ApplicationDirectory = Application.f_GetDirectory();
				
				CProcessLaunchActor::CLaunch Launch = CProcessLaunchParams::fs_LaunchExecutable
					(
						fg_Format("{}/{}", ApplicationDirectory, Application.m_Settings.m_Executable)
						, Application.m_Settings.m_ExecutableParameters
						, ApplicationDirectory
						, [this, _pApplication, pState, Promise](CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)
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
										_pApplication->m_OnRegisterDistributedApp.f_Insert().f_Dispatch().f_Timeout(60.0 * 60.0, "Timed out waiting for application to register (1 hour)")
											> [this, pState, Promise, _pApplication](TCAsyncResult<void> &&_Result)
											{
												if (_pApplication->m_bDeleted)
													return;

												if (!_Result)
												{
													DMibLogWithCategory
														(
															Malterlib/Cloud/AppManager
															, Error
															, "Launched app '{}' failed to register: {}"
															, _pApplication->m_Name
															, _Result.f_GetExceptionStr()
														)
													;
													fp_AppLaunchStateChanged(_pApplication, "Launched (app register failed: '{}')"_f << _Result.f_GetExceptionStr());

													if (!Promise.f_IsSet())
														Promise.f_SetResult(CAppLaunchResult{_Result.f_GetExceptionStr()});
													return;
												}

												fp_AppLaunchStateChanged(_pApplication, "Launched (waiting for app to fully start)");
												DCallActor(_pApplication->m_AppInterface, CDistributedAppInterfaceClient::f_GetAppStartResult)
													.f_Timeout(60.0 * 60.0, "Timed out waiting for application start result (1 hour)")
													> [this, pState, Promise, _pApplication](TCAsyncResult<void> &&_Result)
													{
														pState->m_pCleanup.f_Clear();
														if (_Result)
														{
															for (auto &fOnStartedDistributedApp : _pApplication->m_OnStartedDistributedApp)
																fOnStartedDistributedApp.f_SetResult();
															_pApplication->m_OnStartedDistributedApp.f_Clear();

															fp_AppLaunchStateChanged(_pApplication, "Launched");
															if (!Promise.f_IsSet())
																Promise.f_SetResult(CAppLaunchResult{});
														}
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
															fp_AppLaunchStateChanged(_pApplication, "Launched (app startup failed: '{}')"_f << _Result.f_GetExceptionStr());

															if (!Promise.f_IsSet())
																Promise.f_SetResult(CAppLaunchResult{_Result.f_GetExceptionStr()});
														}
													}
												;
											}
										;
										return;
									}
									pState->m_pCleanup.f_Clear();
									fp_AppLaunchStateChanged(_pApplication, "Launched");
									if (!Promise.f_IsSet())
										Promise.f_SetResult(CAppLaunchResult{});
								}
								break;
							case NProcess::EProcessLaunchState_LaunchFailed:
								{
									auto &LaunchError = _State.f_Get<NProcess::EProcessLaunchState_LaunchFailed>();
									if (!_pApplication->m_bStopped)
										fp_ScheduleRelaunchApp(_pApplication);
									
									fp_AppLaunchStateChanged(_pApplication, fg_Format("Failed launch: {}", LaunchError));
									pState->m_pCleanup.f_Clear();
									if (!Promise.f_IsSet())
										Promise.f_SetException(DMibErrorInstance(LaunchError));
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
									
									if (!Promise.f_IsSet())
										Promise.f_SetException(DMibErrorInstance(fg_Format("Launch exited with '{}' before fully launched", ExitStatus)));

									pState->m_pCleanup.f_Clear();
									for (auto &Promise : _pApplication->m_OnRegisterDistributedApp)
										Promise.f_SetException(DMibErrorInstance("Application exited"));
									for (auto &Promise : _pApplication->m_OnStartedDistributedApp)
										Promise.f_SetException(DMibErrorInstance("Application exited"));
									_pApplication->m_OnRegisterDistributedApp.f_Clear();
									_pApplication->m_OnStartedDistributedApp.f_Clear();
									
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
				
				LaunchParams.m_fOnOutput = [_pApplication](EProcessLaunchOutputType _OutputType, NMib::NStr::CStr const &_Output)
					{
						if (_Output.f_IsEmpty())
							return;
						if (_OutputType == EProcessLaunchOutputType_StdErr)
							_pApplication->m_LastStdErr = _Output.f_TrimRight();
						else if (_OutputType != EProcessLaunchOutputType_StdOut)
							_pApplication->m_LastError = _Output.f_TrimRight();
					}
				;
				
				LaunchParams.m_RunAsUser = fp_GetRunAsUser(Application.m_Settings);
#ifdef DPlatformFamily_Windows
				LaunchParams.m_RunAsUserPassword = Application.m_Settings.m_RunAsUserPassword;
#endif
				LaunchParams.m_RunAsGroup = fp_GetRunAsGroup(Application.m_Settings);
				LaunchParams.m_Environment["HOME"] = ApplicationDirectory + "/.home";
				LaunchParams.m_Environment["TMPDIR"] = ApplicationDirectory + "/.tmp";
#ifdef DPlatformFamily_Windows
				LaunchParams.m_Environment["TMP"] = ApplicationDirectory + "/.tmp";
				LaunchParams.m_Environment["TEMP"] = ApplicationDirectory + "/.tmp";
#endif
				LaunchParams.m_bMergeEnvironment = true;
				LaunchParams.m_bCreateNewProcessGroup = true;
				LaunchParams.m_bShowLaunched = false;
				
				Application.m_ProcessLaunch = fg_ConstructActor<CDistributedAppInterfaceLaunchActor>
					(
						mp_State.m_LocalAddress
						, mp_State.m_TrustManager
						, g_ActorFunctor / [_pApplication, this]
						(CStr const &_HostID, CCallingHostInfo const &_HostInfo, CByteVector const &_Certificate) -> TCFuture<void>
						{
							if (_pApplication->m_bDeleted)
								return DMibErrorInstance("Application deleted");
							
							_pApplication->m_AssociatedHostID = _HostID;
							
							TCPromise<void> Promise;
							fp_UpdateApplicationJSON(_pApplication) > [_pApplication, Promise](TCAsyncResult<void> &&_Result)
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
										Promise.f_SetException(DMibErrorInstance("Failed to update application JSON, see AppManager log for details"));
										return;
									}
									Promise.f_SetResult();
								}
							;
							return Promise.f_MoveFuture();
						}
					 	, g_ActorFunctor / [this, _pApplication, pState, Promise](NStr::CStr const &_Error) -> TCFuture<void>
					 	{
							if (!_pApplication->m_Settings.m_bDistributedApp || _pApplication->m_AppInterface)
								return fg_Explicit();

							pState->m_pCleanup.f_Clear();
							DMibLogWithCategory
								(
									Malterlib/Cloud/AppManager
									, Error
									, "Launched app '{}' failed to start up before registering: {}"
									, _pApplication->m_Name
									, _Error
								)
							;

							fp_AppLaunchStateChanged(_pApplication, "Launched (app startup failed: '{}')"_f << _Error);

							if (!Promise.f_IsSet())
								Promise.f_SetResult(CAppLaunchResult{_Error});

							return fg_Explicit();
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
					> [this, _pApplication, pState, Promise](TCAsyncResult<CActorSubscription> &&_Subscription)
					{
						if (_pApplication->m_bDeleted)
							return;
						if (!_Subscription)
						{
							pState->m_pCleanup.f_Clear();
							Promise.f_SetException(_Subscription);
							DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Failed to launch app '{}': {}", _pApplication->m_Name, _Subscription.f_GetExceptionStr());
							fp_ScheduleRelaunchApp(_pApplication);
							return;
						}
						_pApplication->m_ProcessLaunchSubscription = fg_Move(*_Subscription);
					}
				;
				
				return Promise.f_MoveFuture();
			}
		;
		if (_bOpenEncryption)
		{
			TCPromise<CAppLaunchResult> Promise;
			fg_ThisActor(this)(&CAppManagerActor::fp_ChangeEncryption, _pApplication, EEncryptOperation_Open, false)
				> [Promise, this, fLaunch, _pApplication](TCAsyncResult<void> &&_Result)
				{
					if (!_Result)
					{
						CStr Error = fg_Format("Failed to open encryption: {}", _Result.f_GetExceptionStr());
						Promise.f_SetException(DMibErrorInstance(Error)); 
						fp_AppLaunchStateChanged(_pApplication, Error);
						if (!_pApplication->m_bStopped && !_pApplication->m_bDeleted)
							fp_ScheduleRelaunchApp(_pApplication);
						return;
					}
					
					fLaunch() > Promise;
				}
			;
			return Promise.f_MoveFuture();
		}
		
		return fLaunch();
	}
	
	auto CAppManagerActor::fp_LaunchApp(TCSharedPointer<CApplication> const &_pApplication, bool _bOpenEncryption)
		-> TCFuture<CAppLaunchResult>
	{
		if (_pApplication->m_bLaunching)
		{
			TCPromise<CAppLaunchResult> Promise;
			_pApplication->m_OnLaunchFinished.f_Insert
				(
					[this, Promise, _pApplication, _bOpenEncryption](bool _bAborted)
					{
						if (_bAborted)
						{
							Promise.f_SetException(DMibErrorInstance("Application was deleted or shut down before app could attempt launching"));
							return;
						}
						
						DCheck(!_pApplication->m_bLaunching);
						CAppManagerActor::fp_LaunchAppInternal(_pApplication, _bOpenEncryption) > Promise;
					}
				)
			;
			return Promise.f_MoveFuture();
		}
		return fp_LaunchAppInternal(_pApplication, _bOpenEncryption);
	}
}
