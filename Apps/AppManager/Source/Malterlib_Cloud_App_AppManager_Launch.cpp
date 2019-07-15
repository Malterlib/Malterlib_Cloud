// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
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

	auto CAppManagerActor::fp_LaunchAppInternal(TCSharedPointer<CApplication> const &_pApplication, bool _bOpenEncryption) -> TCFuture<CAppLaunchResult>
	{
		DCheck(!_pApplication->m_bLaunching);

		if (!_pApplication->m_ProcessLaunch.f_IsEmpty() || _pApplication->m_bLaunched)
			co_return DMibErrorInstance("Application is already launched");

		CStr Status;
		CAppManagerInterface::EStatusSeverity Severity;
		if (!_pApplication->f_DependenciesSatisfied(Status, Severity))
		{
			fp_SetAppLaunchStatus(_pApplication, Status, Severity);
			co_return DMibErrorInstance(fg_Format("Dependencies not satisfied: {}", Status));
		}

		_pApplication->m_bLaunching = true;
		_pApplication->m_bStopped = false;
		_pApplication->m_bDistributedStartupFinished = false;
		struct CState
		{
			COnScopeExitShared m_pCleanup;
		};

		TCSharedPointer<CState> pState = fg_Construct();
		pState->m_pCleanup = fg_OnScopeExitShared
			(
				[_pApplication]
				{
					if (!_pApplication->m_bDistributedStartupFinished)
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

		if (_bOpenEncryption)
		{
			auto Result = co_await (self(&CAppManagerActor::fp_ChangeEncryption, _pApplication, EEncryptOperation_Open, false) % "Failed to open encryption").f_Wrap();
			if (!Result)
			{
				fp_AppLaunchStateChanged(_pApplication, Result.f_GetExceptionStr(), CAppManagerInterface::EStatusSeverity_Error);
				if (!_pApplication->m_bStopped && !_pApplication->m_bDeleted)
					fp_ScheduleRelaunchApp(_pApplication);

				co_return Result.f_GetException();
			}
		}

		auto &Application = *_pApplication;

		if (Application.m_Settings.m_bSelfUpdateSource)
		{
			CAppLaunchResult Result;
			if (_pApplication->m_bJustUpdated)
			{
				_pApplication->m_bJustUpdated = false;

				fp_AppLaunchStateChanged(_pApplication, "Self update source - running self update", CAppManagerInterface::EStatusSeverity_Warning);
				Result.m_bQuitManager = co_await self(&CAppManagerActor::fp_SelfUpdate, _pApplication);
				if (Result.m_bQuitManager)
					fp_AppLaunchStateChanged(_pApplication, "Self update source - restarting self", CAppManagerInterface::EStatusSeverity_Warning);
				else
					fp_AppLaunchStateChanged(_pApplication, "Self update source - waiting for update", CAppManagerInterface::EStatusSeverity_None);
			}
			else
				fp_AppLaunchStateChanged(_pApplication, "Self update source - waiting for update", CAppManagerInterface::EStatusSeverity_None);

			_pApplication->m_bLaunched = true;

			co_return fg_Move(Result);
		}

		if (Application.m_Settings.m_Executable.f_IsEmpty())
		{
			fp_AppLaunchStateChanged(_pApplication, "No executable", CAppManagerInterface::EStatusSeverity_None);
			_pApplication->m_bLaunched = true;
			co_return CAppLaunchResult{};
		}

		if (Application.m_Settings.m_AppManagerVersion < mc_CurrentAppMangerVersion)
		{
			fp_AppLaunchStateChanged(_pApplication, "Upgrading app manager version", CAppManagerInterface::EStatusSeverity_Warning);

			co_await self(&CAppManagerActor::fp_UpdateAppManagerApplicationVersion, _pApplication, Application.m_Settings.m_AppManagerVersion);

			Application.m_Settings.m_AppManagerVersion = mc_CurrentAppMangerVersion;
			co_await fp_UpdateApplicationJSON(_pApplication);
		}

		fp_AppLaunchStateChanged(_pApplication, "Launching", CAppManagerInterface::EStatusSeverity_Warning);

		if (Application.m_Settings.m_bBackupEnabled && !Application.m_BackupClient)
			fp_ApplicationStartBackup(_pApplication);

		CStr ApplicationDirectory = Application.f_GetDirectory();

		TCPromise<CAppLaunchResult> LaunchPromise;
		CProcessLaunchActor::CLaunch Launch = CProcessLaunchParams::fs_LaunchExecutable
			(
				fg_Format("{}/{}", ApplicationDirectory, Application.m_Settings.m_Executable)
				, Application.m_Settings.m_ExecutableParameters
				, ApplicationDirectory
				, [this, _pApplication, pState, LaunchPromise](CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)
				{
					if (_pApplication->m_bDeleted)
						return;

					switch (_State.f_GetTypeID())
					{
					case NProcess::EProcessLaunchState_Launched:
						{
							if (_pApplication->m_Settings.m_bDistributedApp)
							{
								fp_AppLaunchStateChanged(_pApplication, "Launched (waiting for distributed app register)", CAppManagerInterface::EStatusSeverity_Warning);

								TCFuture<void> RegisterFuture;

								if (_pApplication->m_AppInterface)
									RegisterFuture = fg_Explicit();
								else
									RegisterFuture = _pApplication->m_OnRegisterDistributedApp.f_Insert().f_Future().f_Timeout(60.0 * 60.0, "Timed out waiting for application to register (1 hour)");

								fg_Move(RegisterFuture) > [this, pState, LaunchPromise, _pApplication](TCAsyncResult<void> &&_Result)
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
											fp_AppLaunchStateChanged
												(
												 	_pApplication
												 	, "Launched (app register failed: '{}')"_f << _Result.f_GetExceptionStr()
												 	, CAppManagerInterface::EStatusSeverity_Error
												)
											;

											if (!LaunchPromise.f_IsSet())
												LaunchPromise.f_SetResult(CAppLaunchResult{_Result.f_GetExceptionStr()});
											return;
										}

										fp_AppLaunchStateChanged(_pApplication, "Launched (waiting for app to fully start)", CAppManagerInterface::EStatusSeverity_Warning);
										_pApplication->m_AppInterface.f_CallActor(&CDistributedAppInterfaceClient::f_GetAppStartResult)()
											.f_Timeout(60.0 * 60.0, "Timed out waiting for application start result (1 hour)")
											> [this, pState, LaunchPromise, _pApplication](TCAsyncResult<void> &&_Result)
											{
												if (_pApplication->m_bLaunching)
													_pApplication->m_bDistributedStartupFinished = true;

												pState->m_pCleanup.f_Clear();
												if (_Result)
												{
													for (auto &fOnStartedDistributedApp : _pApplication->m_OnStartedDistributedApp)
														fOnStartedDistributedApp.f_SetResult();
													_pApplication->m_OnStartedDistributedApp.f_Clear();

													fp_AppLaunchStateChanged(_pApplication, "Launched", CAppManagerInterface::EStatusSeverity_None);
													if (!LaunchPromise.f_IsSet())
														LaunchPromise.f_SetResult(CAppLaunchResult{});
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
													fp_AppLaunchStateChanged
														(
														 	_pApplication
														 	, "Launched (app startup failed: '{}')"_f << _Result.f_GetExceptionStr()
														 	, CAppManagerInterface::EStatusSeverity_Error
														)
													;

													if (!LaunchPromise.f_IsSet())
														LaunchPromise.f_SetResult(CAppLaunchResult{_Result.f_GetExceptionStr()});
												}
											}
										;
									}
								;
								return;
							}
							pState->m_pCleanup.f_Clear();
							fp_AppLaunchStateChanged(_pApplication, "Launched", CAppManagerInterface::EStatusSeverity_None);
							if (!LaunchPromise.f_IsSet())
								LaunchPromise.f_SetResult(CAppLaunchResult{});
						}
						break;
					case NProcess::EProcessLaunchState_LaunchFailed:
						{
							auto &LaunchError = _State.f_Get<NProcess::EProcessLaunchState_LaunchFailed>();
							if (!_pApplication->m_bStopped)
								fp_ScheduleRelaunchApp(_pApplication);

							fp_AppLaunchStateChanged(_pApplication, fg_Format("Failed launch: {}", LaunchError), CAppManagerInterface::EStatusSeverity_Error);
							pState->m_pCleanup.f_Clear();
							if (!LaunchPromise.f_IsSet())
								LaunchPromise.f_SetException(DMibErrorInstance(LaunchError));
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
									 	, _pApplication->m_bStopped ? CAppManagerInterface::EStatusSeverity_None : CAppManagerInterface::EStatusSeverity_Error
									)
								;
							}
							else
							{
								fp_AppLaunchStateChanged
									(
									 	_pApplication
									 	, fg_Format("{}Exited with {}", RelaunchInfo, ExitStatus)
									 	, _pApplication->m_bStopped ? CAppManagerInterface::EStatusSeverity_None : CAppManagerInterface::EStatusSeverity_Error
									)
								;
							}

							if (!LaunchPromise.f_IsSet())
								LaunchPromise.f_SetException(DMibErrorInstance(fg_Format("Launch exited with '{}' before fully launched", ExitStatus)));

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
						co_return DMibErrorInstance("Application deleted");

					_pApplication->m_AssociatedHostID = _HostID;

					auto Result = co_await fp_UpdateApplicationJSON(_pApplication).f_Wrap();

					if (!Result)
					{
						DMibLogWithCategory
							(
								Malterlib/Cloud/AppManager
								, Info
								, "Failed to update application JSON when granting connection ticket for '{}': {}"
								, _pApplication->m_Name
								, Result.f_GetExceptionStr()
							)
						;
						co_return DMibErrorInstance("Failed to update application JSON, see AppManager log for details");
					}

					co_return {};
				}
				, g_ActorFunctor / [this, _pApplication, pState, LaunchPromise](NStr::CStr const &_Error) -> TCFuture<void>
				{
					if (!_pApplication->m_Settings.m_bDistributedApp || _pApplication->m_AppInterface)
						co_return {};

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

					fp_AppLaunchStateChanged(_pApplication, "Launched (app startup failed: '{}')"_f << _Error, CAppManagerInterface::EStatusSeverity_Error);

					if (!LaunchPromise.f_IsSet())
						LaunchPromise.f_SetResult(CAppLaunchResult{_Error});

					co_return {};
				}
				, Application.m_Name
				, false
			)
		;

		auto LaunchSubscription = co_await Application.m_ProcessLaunch
			(
				&CProcessLaunchActor::f_Launch
				, Launch
				, fg_ThisActor(this)
			)
			.f_Wrap()
		;

		if (_pApplication->m_bDeleted)
			co_return DMibErrorInstance("Application deleted");

		if (!LaunchSubscription)
		{
			pState->m_pCleanup.f_Clear();
			DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Failed to launch app '{}': {}", _pApplication->m_Name, LaunchSubscription.f_GetExceptionStr());
			fp_ScheduleRelaunchApp(_pApplication);
			co_return LaunchSubscription.f_GetException();
		}

		_pApplication->m_ProcessLaunchSubscription = fg_Move(*LaunchSubscription);

		co_return co_await LaunchPromise.f_MoveFuture();
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
						self(&CAppManagerActor::fp_LaunchAppInternal, _pApplication, _bOpenEncryption) > Promise;
					}
				)
			;
			return Promise.f_MoveFuture();
		}
		return self(&CAppManagerActor::fp_LaunchAppInternal, _pApplication, _bOpenEncryption);
	}
}
