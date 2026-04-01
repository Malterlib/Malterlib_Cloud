// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Encoding/JsonShortcuts>
#include <Mib/Cryptography/RandomID>
#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud::NAppManager
{
	CStr fg_ConcatOutput(CStr const &_StdOut, CStr const &_StdErr)
	{
		if (_StdOut.f_IsEmpty() && _StdErr.f_IsEmpty())
			return CStr();
		CStr Ret;
		CStr StdOut = _StdOut.f_Trim();
		if (!StdOut.f_IsEmpty())
			fg_AddStrSep(Ret, StdOut, DMibNewLine);
		CStr StdErr = _StdErr.f_Trim();
		if (!StdErr.f_IsEmpty())
			fg_AddStrSep(Ret, StdErr, DMibNewLine);
		return DMibNewLine + Ret;
	}

	CStr CAppManagerActor::fsp_LimitErrorLogSize(CStr const &_String, umint _ExtraSize)
	{
#if DMibPPtrBits < 64
		umint MaxSize = 64 * 1024 - _ExtraSize;
#else
		umint MaxSize = 512 * 1024 - _ExtraSize;
#endif

		auto Lines = _String.f_SplitLine().f_Reverse();

		TCVector<CStr> OutLines;
		umint Size = 0;
		for (auto &Line : Lines)
		{
			auto LineSize = Line.f_GetLen() + 1;
			if (Size + LineSize > MaxSize)
				break;

			Size += LineSize;
			OutLines.f_Insert(fg_Move(Line));
		}

		if (OutLines.f_IsEmpty() && !Lines.f_IsEmpty())
			OutLines.f_Insert(Lines.f_GetFirst().f_Left(MaxSize));

		return CStr::fs_Join(fg_Move(OutLines).f_Reverse(), "\n");
	}

	CStr CAppManagerActor::fsp_RunTool
		(
			CStr const &_Description
			, CStr const &_Tool
			, CStr const &_WorkingDir
			, TCVector<CStr> const &_Arguments
		)
	{
		CProcessLaunchParams Params;
		Params.m_bAllowExecutableLocate = true;
		Params.m_WorkingDirectory = _WorkingDir;

		Params.m_bMergeEnvironment = true;

		DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "{cc}", _Description);

		CStr StdOut;
		CStr StdErr;
		uint32 ExitCode;
		CStr Tool = _Tool;
#ifdef DPlatformFamily_Windows
		Tool += ".exe";
#endif
		if (CProcessLaunch::fs_LaunchBlock(Tool, _Arguments, StdOut, StdErr, ExitCode, Params))
		{
			if (ExitCode)
				DMibError(fg_Format("{cc} failed with exit code {} and reported: {}", _Description, ExitCode, fg_ConcatOutput(StdOut, StdErr)));
		}
		else
			DMibError(fg_Format("Failed to launch {} when {}: {}", _Tool, _Description, StdErr));
		DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "{} was successful {}", _Description, fg_ConcatOutput(StdOut, StdErr));
		return StdOut;
	}

	TCFuture<CAppManagerActor::CBashScriptOutput> CAppManagerActor::fp_RunBashScript
		(
			CStr const &_Script
			, CStr const &_Description
			, TCMap<CStr, CStr> const &_Environment
			, TCFunction<void (NMib::NStr::CStr const &_Output, TCActor<CProcessLaunchActor> const &_LaunchActor)> const &_fOnStdOutput
			, uint32 _WarningErrorStatus
		)
	{
		struct CState
		{
			TCPromise<CBashScriptOutput> m_Promise;
			TCActor<CProcessLaunchActor> m_LaunchActor;
			CActorSubscription m_LaunchSubscription;
			CStr m_ErrorOutput;
			CStr m_StdOutput;

			CAppManagerActor::CBashScriptOutput m_Output;

			bool m_bReplied = false;
			void f_Replied()
			{
				m_bReplied = true;
				fg_Move(m_LaunchActor).f_Destroy().f_DiscardResult();
			}
		};

		TCSharedPointer<CState> pState = fg_Construct();
		pState->m_LaunchActor = fg_ConstructActor<CProcessLaunchActor>();

		CStr FileName = fg_Format("{}/TempScripts/Bash_{}.sh", mp_State.m_RootDirectory, CHash_MD5::fs_DigestFromData(_Script.f_GetStr(), _Script.f_GetLen()).f_GetString());

		auto BlockingActorCheckout = fg_BlockingActor();
		auto BlockingActor = BlockingActorCheckout.f_Actor();

		fg_Dispatch
			(
				BlockingActor
				, [FileName, _Script]
				{
					if (!CFile::fs_FileExists(FileName))
					{
						CFile::fs_CreateDirectory(CFile::fs_GetPath(FileName));
						CFile::fs_WriteStringToFile
							(
								FileName
								, _Script
								, false
								, EFileAttrib_UnixAttributesValid | EFileAttrib_UserExecute | EFileAttrib_UserRead | EFileAttrib_UserWrite
							)
						;
					}
				}
			)
			> pState->m_Promise % fg_Format("[{}] Failed to save temporary script", _Description)
			/ [this, FileName, _Description, _Environment, _fOnStdOutput, pState, _WarningErrorStatus, BlockingActorCheckout = fg_Move(BlockingActorCheckout)]
			{
				auto fReportError = [pState, _Description](CStr const &_Error)
					{
						if (pState->m_bReplied)
							return;
						DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "[{}] {}", _Description, _Error);
						pState->m_Promise.f_SetException(DMibErrorInstance(_Error));
						pState->f_Replied();
					}
				;

				CProcessLaunchParams LaunchParams = CProcessLaunchParams::fs_LaunchExecutable
					(
						CProcessLaunch::fs_GetBashPath()
						, fg_CreateVector<CStr>(FileName)
						, mp_State.m_RootDirectory
						, [pState, _Description, fReportError, _WarningErrorStatus](CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)
						{
							if (!pState->m_LaunchActor)
								return;

							switch (_State.f_GetTypeID())
							{
							case NProcess::EProcessLaunchState_Launched:
								{
									DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Launched bash script '{}'", _Description);
								}
								break;
							case NProcess::EProcessLaunchState_LaunchFailed:
								{
									auto &LaunchError = _State.f_Get<NProcess::EProcessLaunchState_LaunchFailed>();
									fReportError(fg_Format("Failed to launch bash script: {}", LaunchError));
								}
								break;
							case NProcess::EProcessLaunchState_Exited:
								{
									auto ExitStatus = _State.f_Get<NProcess::EProcessLaunchState_Exited>();
									pState->m_Output.m_Status = ExitStatus;
									if (ExitStatus == _WarningErrorStatus)
									{
										DMibLogWithCategory
											(
												Malterlib/Cloud/AppManager
												, Warning
												, "[{}] Bash script exited with warning"
												, _Description
											)
										;
									}
									else if (ExitStatus != 0)
									{
										auto ErrorOutput = pState->m_ErrorOutput.f_Trim();
										if (ErrorOutput.f_IsEmpty())
											fReportError(fg_Format("Bash script exited with error status: {}", ExitStatus));
										else
											fReportError(fg_Format("Bash script exited with error status: {}. Error output:{\n}{}", ExitStatus, ErrorOutput));

										break;
									}
									else
									{
										DMibLogWithCategory
											(
												Malterlib/Cloud/AppManager
												, Info
												, "[{}] Bash script exited with success"
												, _Description
											)
										;
									}

									if (!pState->m_bReplied)
									{
										pState->m_Promise.f_SetResult(fg_Move(pState->m_Output));
										pState->f_Replied();
									}
								}
								break;
							}
						}
					)
				;

				LaunchParams.m_fOnOutput = [pState, _Description, _fOnStdOutput](EProcessLaunchOutputType _OutputType, NMib::NStr::CStr const &_Output)
					{
						if (!pState->m_LaunchActor)
							return;
						if (_Output.f_IsEmpty())
							return;
						DMibLogCategory(Malterlib/Cloud/AppManager);
						auto Output = _Output.f_TrimRight("\r\n");
						NMib::NLog::CSysLogCatScope AppScope(NMib::fg_GetSys()->f_GetLogger(), _Description);
						if (_OutputType == EProcessLaunchOutputType_StdOut)
						{
							pState->m_Output.m_StdOut += _Output;
							DMibLog(Info, "{}", Output);
							if (_fOnStdOutput)
							{
								pState->m_StdOutput += _Output;
								while (pState->m_StdOutput.f_FindChars("\r\n") >= 0)
									_fOnStdOutput(fg_GetStrLineSep(pState->m_StdOutput), pState->m_LaunchActor);
							}
						}
						else
						{
							pState->m_Output.m_StdErr += _Output;
							pState->m_ErrorOutput += _Output;
							DMibLog(Error, "{}", Output);
						}
					}
				;

				LaunchParams.m_Environment += _Environment;
				LaunchParams.m_bMergeEnvironment = true;

				LaunchParams.m_bAllowExecutableLocate = true;

				pState->m_LaunchActor
					(
						&CProcessLaunchActor::f_Launch
						, LaunchParams
						, fg_ThisActor(this)
					)
					> [pState, _Description, fReportError](TCAsyncResult<CActorSubscription> &&_Subscription)
					{
						if (!pState->m_LaunchActor)
							return;
						if (!_Subscription)
						{
							fReportError(fg_Format("[{}] Failed to launch bash script: {}", _Description, _Subscription.f_GetExceptionStr()));
							return;
						}
						pState->m_LaunchSubscription = fg_Move(*_Subscription);
					}
				;
			}
		;

		return pState->m_Promise.f_Future();
	}
}
