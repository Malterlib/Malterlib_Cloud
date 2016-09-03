// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Concurrency/Actor/Timer>
#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib
{
	namespace NCloud
	{
		namespace NAppManager
		{
			void CAppManagerActor::fp_LaunchEncryptedApps()
			{
				for (auto &pApplication : mp_Applications)
				{
					if (!pApplication->m_EncryptionStorage.f_IsEmpty() && !pApplication->m_bOperationInProgress)
					{
						auto InProgressScope = pApplication->f_SetInProgress();
						fg_ThisActor(this)(&CAppManagerActor::fp_LaunchApp, pApplication, true) > [InProgressScope](TCAsyncResult<void> &&_Result)
							{
							}
						;
					}
				}
			}

			void CAppManagerActor::fp_LaunchNormalApps()
			{
				for (auto &pApplication : mp_Applications)
				{
					if (pApplication->m_EncryptionStorage.f_IsEmpty())
					{
						auto InProgressScope = pApplication->f_SetInProgress();
						fg_ThisActor(this)(&CAppManagerActor::fp_LaunchApp, pApplication, true) > [InProgressScope](TCAsyncResult<void> &&_Result)
							{
							}
						;
					}
					else
						pApplication->m_LaunchState = "Waiting for key manager to become available";
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
				{
					return DMibErrorInstance("Application is already launched");
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
				
				auto fLaunch = [this, _pApplication, pState]() -> TCContinuation<void>
					{
						TCContinuation<void> Continuation;
						auto &Application = *_pApplication;
						
						_pApplication->m_LaunchState = "Launching";
						
						CStr ApplicationDirectory = Application.f_GetDirectory();
						
						CProcessLaunchParams LaunchParams = CProcessLaunchParams::fs_LaunchExecutable
							(
								fg_Format("{}/{}", ApplicationDirectory, Application.m_Executable)
								, Application.m_ExecutableParameters
								, ApplicationDirectory
								, [this, _pApplication, pState, Continuation](CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)
								{
									if (_pApplication->m_bDeleted)
										return;
									
									switch (_State.f_GetTypeID())
									{
									case NProcess::EProcessLaunchState_Launched:
										{
											DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Launched app '{}'", _pApplication->m_Name);
											_pApplication->m_LaunchState = "Launched";
											pState->m_pCleanup.f_Clear();
											Continuation.f_SetResult();
										}
										break;
									case NProcess::EProcessLaunchState_LaunchFailed:
										{
											auto &LaunchError = _State.f_Get<NProcess::EProcessLaunchState_LaunchFailed>();
											DMibLogWithCategory
												(
													Malterlib/Cloud/AppManager
													, Error
													, "Failed to launch app '{}': {}"
													, _pApplication->m_Name
													, LaunchError 
												)
											;
											fp_ScheduleRelaunchApp(_pApplication);
											
											_pApplication->m_LaunchState = fg_Format("Failed launch: {}", LaunchError);
											pState->m_pCleanup.f_Clear();
											Continuation.f_SetException(DMibErrorInstance(LaunchError));
										}
										break;
									case NProcess::EProcessLaunchState_Exited:
										{
											auto ExitStatus = _State.f_Get<NProcess::EProcessLaunchState_Exited>();
											if (ExitStatus != 0)
											{
												DMibLogWithCategory
													(
														Malterlib/Cloud/AppManager
														, Error
														, "App '{}' exited with status {}"
														, _pApplication->m_Name
														, ExitStatus
													)
												;
											}
											else
											{
												DMibLogWithCategory
													(
														Malterlib/Cloud/AppManager
														, Info
														, "App '{}' exited with status {}"
														, _pApplication->m_Name
														, ExitStatus
													)
												;
											}

											CStr RelaunchInfo;
											if (!_pApplication->m_bStopped)
												RelaunchInfo = "Waiting to retry launching. ";
											
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
						
						LaunchParams.m_fOnOutput = [this, _pApplication](EProcessLaunchOutputType _OutputType, NMib::NStr::CStr const &_Output)
							{
								if (_Output.f_IsEmpty())
									return;
								DMibLogCategory(Malterlib/Cloud/AppManager);
								auto Output = _Output.f_TrimRight("\r\n");
								NMib::NLog::CSysLogCatScope AppScope(NMib::fg_GetSys()->f_GetLogger(), _pApplication->m_Name);
								if (_OutputType == EProcessLaunchOutputType_StdOut)
									DMibLog(Info, "{}", Output);
								else
								{
									if (_OutputType == EProcessLaunchOutputType_StdErr)
										_pApplication->m_LastStdErr = Output;
									else
										_pApplication->m_LastError = Output;
									DMibLog(Error, "{}", _Output.f_TrimRight("\r\n"));
								}
							}
						;
						
						LaunchParams.m_RunAsUser = Application.m_RunAsUser;
						LaunchParams.m_RunAsGroup = Application.m_RunAsGroup;
						LaunchParams.m_Environment["HOME"] = ApplicationDirectory + "/.home";
						LaunchParams.m_Environment["TMPDIR"] = ApplicationDirectory + "/.tmp";
						LaunchParams.m_bMergeEnvironment = true;
						
						Application.m_ProcessLaunch = fg_ConstructActor<CProcessLaunchActor>();
						
						Application.m_ProcessLaunch
							(
								&CProcessLaunchActor::f_Launch
								, LaunchParams
								, EProcessLaunchCloseFlag_StopProcess | EProcessLaunchCloseFlag_BlockOnExit
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
							
							fg_Dispatch(fLaunch) > Continuation;
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
	}
}
