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
			void CAppManagerActor::fp_LaunchApps()
			{
				for (auto &pApplication : mp_Applications)
					fp_LaunchApp(pApplication);
			}

			void CAppManagerActor::fp_ScheduleRelaunchApp(TCSharedPointer<CApplication> const &_pApplication)
			{
				_pApplication->f_Clear();
				fg_OneshotTimer
					(
						10.0
						, [this, _pApplication]
						{
							if (_pApplication->m_bDeleted || _pApplication->m_ProcessLaunch)
								return;
							fp_LaunchApp(_pApplication);
						}
					)
				;
			}
			
			void CAppManagerActor::fp_LaunchApp(TCSharedPointer<CApplication> const &_pApplication)
			{
				auto &Application = *_pApplication;

				_pApplication->m_LaunchState = "Launching";
				
				CStr ApplicationDirectory = Application.f_GetDirectory();
				
				CProcessLaunchParams LaunchParams = CProcessLaunchParams::fs_LaunchExecutable
					(
						fg_Format("{}/{}", ApplicationDirectory, Application.m_Executable)
						, Application.m_ExecutableParameters
						, ApplicationDirectory
						, [this, _pApplication](CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)
						{
							if (_pApplication->m_bDeleted)
								return;
							
							switch (_State.f_GetTypeID())
							{
							case NProcess::EProcessLaunchState_Launched:
								{
									DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Launched app '{}'", _pApplication->m_Name);
									_pApplication->m_LaunchState = "Launched";
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
				
				Application.m_ProcessLaunch = fg_ConstructActor<CProcessLaunchActor>();
				
				Application.m_ProcessLaunch
					(
						&CProcessLaunchActor::f_Launch
						, LaunchParams
						, EProcessLaunchCloseFlag_StopProcess | EProcessLaunchCloseFlag_BlockOnExit
						, fg_ThisActor(this)
					)
					> [this, _pApplication](TCAsyncResult<CActorCallback> &&_Subscription)
					{
						if (_pApplication->m_bDeleted)
							return;
						if (!_Subscription)
						{
							DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Failed to launch app '{}': {}", _pApplication->m_Name, _Subscription.f_GetExceptionStr());
							fp_ScheduleRelaunchApp(_pApplication);
							return;
						}
						_pApplication->m_ProcessLaunchSubscription = fg_Move(*_Subscription);
					}
				;
			}
		}		
	}
}
