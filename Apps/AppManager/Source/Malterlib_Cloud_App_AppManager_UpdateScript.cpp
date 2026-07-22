// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Encoding/JsonShortcuts>
#include <Mib/Cryptography/RandomID>
#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud::NAppManager
{
	CStr const &CAppManagerActor::CUpdateScripts::f_GetScript(EUpdateScript _Script) const
	{
		switch (_Script)
		{
		case EUpdateScript_PreUpdate:
			return m_PreUpdate;
		case EUpdateScript_PostUpdate:
			return m_PostUpdate;
		case EUpdateScript_PostLaunch:
			return m_PostLaunch;
		case EUpdateScript_OnError:
			return m_OnError;
		}
		DMibNeverGetHere;
		return m_PreUpdate;
	}

	CStr CAppManagerActor::CUpdateScripts::f_GetName(EUpdateScript _Script) const
	{
		switch (_Script)
		{
		case EUpdateScript_PreUpdate:
			return "PreUpdate";
		case EUpdateScript_PostUpdate:
			return "PostUpdate";
		case EUpdateScript_PostLaunch:
			return "PostLaunch";
		case EUpdateScript_OnError:
			return "OnError";
		}
		DMibNeverGetHere;
		return "Unknown";
	}

	TCFuture<void> CAppManagerActor::fp_RunBashScript(CAppManagerEnvironmentInterface::CEnvironmentScript _Script, NStr::CStrSecure _RunAsUserPassword)
	{
		struct CState
		{
			TCPromise<void> m_Promise;
			TCActor<CProcessLaunchActor> m_LaunchActor;
			CActorSubscription m_LaunchSubscription;
			CStr m_ErrorOutput;
			CStr m_StdOutput;
			CStr m_AllOutput;

			bool m_bReplied = false;
			void f_Replied()
			{
				m_bReplied = true;
				fg_Move(m_LaunchActor).f_Destroy().f_DiscardResult();
			}
		};
		TCSharedPointer<CState> pState = fg_Construct();

		if (_Script.m_Script.f_IsEmpty())
			return pState->m_Promise <<= g_Void;

		CStr Description = _Script.m_Description;

		pState->m_LaunchActor = fg_ConstructActor<CProcessLaunchActor>();

		CStr FileName = CFile::fs_GetExpandedPath(_Script.m_Script, _Script.m_Directory);

		auto fReportError = [pState, Description](CStr const &_Error)
			{
				if (pState->m_bReplied)
					return;
				pState->m_Promise.f_SetException(DMibErrorInstance(_Error));
				pState->f_Replied();
			}
		;

		DMibLogWithCategory
			(
				Malterlib/Cloud/AppManager
				, Info
				, "[{}] Launch update script"
				, Description
			)
		;

		CProcessLaunchParams LaunchParams = CProcessLaunchParams::fs_LaunchExecutable
			(
				CProcessLaunch::fs_GetBashPath()
				, fg_CreateVector<CStr>(FileName, _Script.m_Parameter)
				, _Script.m_Directory
				, [pState, Description, fReportError](CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)
				{
					if (!pState->m_LaunchActor)
						return;

					switch (_State.f_GetTypeID())
					{
					case NProcess::EProcessLaunchState_Launched:
						{
							DMibLogWithCategory
								(
									Malterlib/Cloud/AppManager
									, Info
									, "[{}] Launched update script"
									, Description
								)
							;
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
							if (ExitStatus != 0)
							{
								auto ErrorOutput = fsp_LimitErrorLogSize(pState->m_AllOutput.f_Trim(), 128);
								if (ErrorOutput.f_IsEmpty())
									fReportError(fg_Format("Exit status: {}", ExitStatus));
								else
									fReportError(fg_Format("Exit status: {}{\n}{\n}{}", ExitStatus, ErrorOutput));
							}
							else
							{
								DMibLogWithCategory
									(
										Malterlib/Cloud/AppManager
										, Info
										, "[{}] Bash script exited with success"
										, Description
									)
								;
								if (!pState->m_bReplied)
								{
									pState->m_Promise.f_SetResult();
									pState->f_Replied();
								}
							}
						}
						break;
					}
				}
			)
		;

		LaunchParams.m_fOnOutput = [pState, Description](EProcessLaunchOutputType _OutputType, NMib::NStr::CStr const &_Output)
			{
				if (!pState->m_LaunchActor)
					return;
				if (_Output.f_IsEmpty())
					return;
				DMibLogCategory(Malterlib/Cloud/AppManager);
				auto Output = _Output.f_TrimRight("\r\n");
				NMib::NLog::CSysLogCatScope AppScope(NMib::fg_GetSys()->f_GetLogger(), Description);
				if (_OutputType == EProcessLaunchOutputType_StdOut)
				{
					DMibLog(Info, "{}", Output);
					pState->m_StdOutput += _Output;
					pState->m_AllOutput += _Output;
				}
				else
				{
					DMibLog(Error, "{}", Output);
					pState->m_ErrorOutput += _Output;
					pState->m_AllOutput += _Output;
				}
			}
		;

		LaunchParams.m_RunAsUser = _Script.m_RunAsUser;
#ifdef DPlatformFamily_Windows
		LaunchParams.m_RunAsUserPassword = _RunAsUserPassword;
#endif
		LaunchParams.m_RunAsGroup = _Script.m_RunAsGroup;

		for (auto &Value : _Script.m_Environment)
			LaunchParams.m_Environment[_Script.m_Environment.fs_GetKey(Value)] = Value;

		LaunchParams.m_bMergeEnvironment = true;
		LaunchParams.m_bAllowExecutableLocate = true;

		pState->m_LaunchActor
			(
				&CProcessLaunchActor::f_Launch
				, LaunchParams
				, fg_ThisActor(this)
			)
			> [pState, Description, fReportError](TCAsyncResult<CActorSubscription> &&_Subscription)
			{
				if (!pState->m_LaunchActor)
					return;
				if (!_Subscription)
				{
					fReportError(fg_Format("[{}] Failed to launch bash script: {}", Description, _Subscription.f_GetExceptionStr()));
					return;
				}
				pState->m_LaunchSubscription = fg_Move(*_Subscription);
			}
		;

		return pState->m_Promise.f_Future();
	}

	TCFuture<void> CAppManagerActor::fp_RunUpdateScript
		(
			TCSharedPointer<CApplication> _pApplication
			, EUpdateScript _Script
			, CStr _Param
			, CVersionManager::CVersionIDAndPlatform _VersionID
			, CVersionManager::CVersionInformation const *_pVersionInformation
			, CVersionManager::CVersionIDAndPlatform _PreviousVersionID
			, CVersionManager::CVersionInformation _PreviousVersionInformation
			, fp64 _TimeSinceUpdateStart
		)
	{
		CStr Script = _pApplication->m_Settings.m_UpdateScripts.f_GetScript(_Script);
		if (Script.f_IsEmpty())
			co_return {};

		CAppManagerEnvironmentInterface::CEnvironmentScript EnvironmentScript;
		EnvironmentScript.m_Description = fg_Format("{}/{}", _pApplication->m_Name, _pApplication->m_Settings.m_UpdateScripts.f_GetName(_Script));
		EnvironmentScript.m_Script = Script;
		EnvironmentScript.m_Application = _pApplication->m_Name;
		EnvironmentScript.m_Parameter = _Param;
		EnvironmentScript.m_RunAsUser = fp_GetRunAsUser(_pApplication->m_Settings);
		EnvironmentScript.m_RunAsGroup = fp_GetRunAsGroup(_pApplication->m_Settings);

		auto &Environment = EnvironmentScript.m_Environment;

		// An environment that manages the application storage derives the
		// directory and the home and temporary directories from the name
		if (!fp_ApplicationRemoteStorageEnvironment(*_pApplication))
		{
			EnvironmentScript.m_Directory = _pApplication->f_GetDirectory();

			Environment["HOME"] = _pApplication->f_GetDirectory() + "/.home";
			Environment["TMPDIR"] = _pApplication->f_GetDirectory() + "/.tmp";
#ifdef DPlatformFamily_Windows
			Environment["TMP"] = _pApplication->f_GetDirectory() + "/.tmp";
			Environment["TEMP"] = _pApplication->f_GetDirectory() + "/.tmp";
#endif
		}

		Environment["MalterlibCloud_TimeSinceStart"] = fg_Format("{fe1}", _TimeSinceUpdateStart);
		Environment["MalterlibCloud_Application"] = _pApplication->m_Name;
		Environment["MalterlibCloud_VersionApplication"] = _pApplication->m_Settings.m_VersionManagerApplication;

		auto fAddVersionInformation = [&](CStr const &_Prefix, CVersionManager::CVersionIDAndPlatform const &_VersionID, CVersionManager::CVersionInformation const *_pVersionInformation)
			{
				auto fPrefixName = [&](CStr const &_Name) -> CStr
					{
						return "MalterlibCloud_{}{}"_f << _Prefix << _Name;
					}
				;

				if (_VersionID.f_IsValid())
				{
					Environment[fPrefixName("Version")] = CStr::fs_ToStr(_VersionID);
					Environment[fPrefixName("VersionID")] = CStr::fs_ToStr(_VersionID.m_VersionID);
					Environment[fPrefixName("VersionBranch")] = _VersionID.m_VersionID.m_Branch;
					Environment[fPrefixName("VersionMajor")] = CStr::fs_ToStr(_VersionID.m_VersionID.m_Major);
					Environment[fPrefixName("VersionMinor")] = CStr::fs_ToStr(_VersionID.m_VersionID.m_Minor);
					Environment[fPrefixName("VersionRevision")] = CStr::fs_ToStr(_VersionID.m_VersionID.m_Revision);
					Environment[fPrefixName("VersionPlatform")] = _VersionID.m_Platform;
				}
				else
				{
					Environment[fPrefixName("Version")] = "Unknown";
					Environment[fPrefixName("VersionID")] = "Unknown";
				}

				if (!_pVersionInformation)
					return;

				Environment[fPrefixName("Time")] = "{}"_f << _pVersionInformation->m_Time.f_ToLocal();
				Environment[fPrefixName("Configuration")] = "{}"_f << _pVersionInformation->m_Configuration;
				Environment[fPrefixName("Tags")] = "{vs,vb}"_f << _pVersionInformation->m_Tags;
				Environment[fPrefixName("RetrySequence")] = "{}"_f << _pVersionInformation->m_RetrySequence;
				Environment[fPrefixName("ExtraInfo")] = _pVersionInformation->m_ExtraInfo.f_ToString(nullptr);
				Environment[fPrefixName("NumFiles")] = "{}"_f << _pVersionInformation->m_nFiles;
				Environment[fPrefixName("NumBytes")] = "{}"_f << _pVersionInformation->m_nBytes;
			}
		;

		fAddVersionInformation("", _VersionID, _pVersionInformation);
		fAddVersionInformation("Previous", _PreviousVersionID, &_PreviousVersionInformation);

		if (_pVersionInformation)
		{
			if (auto pUpdateScriptEnv = _pVersionInformation->m_ExtraInfo.f_GetMember("UpdateScriptEnvironment", EJsonType_Object))
			{
				for (auto &Member : pUpdateScriptEnv->f_Object())
				{
					if (!Member.f_Value().f_IsString())
					{
						DMibLogWithCategory
							(
								Malterlib/Cloud/AppManager
								, Info
								, "[{}] Invalid type in UpdateScriptEnvironment.{}"
								, EnvironmentScript.m_Description
								, Member.f_Name()
							)
						;
						continue;
					}

					Environment[Member.f_Name()] = Member.f_Value().f_String();
				}
			}
		}

		if (!_pApplication->m_Settings.m_LaunchEnvironment.f_IsEmpty())
		{
			auto *pFindEnvironment = mp_Environments.f_FindEqual(_pApplication->m_Settings.m_LaunchEnvironment);
			if (!pFindEnvironment)
				co_return DMibErrorInstance("Cannot run update script: no such environment '{}'"_f << _pApplication->m_Settings.m_LaunchEnvironment);

			auto pEnvironment = *pFindEnvironment;

			co_await fp_EnsureEnvironmentStarted(pEnvironment);

			co_return co_await pEnvironment->m_AgentInterface.f_CallActor(&CAppManagerEnvironmentInterface::f_RunScript)(fg_Move(EnvironmentScript))
				.f_Timeout(60.0 * 60.0, "Timed out running update script in environment (1 hour)")
			;
		}

#ifdef DPlatformFamily_Windows
		co_return co_await fp_RunBashScript(fg_Move(EnvironmentScript), _pApplication->m_Settings.m_RunAsUserPassword);
#else
		co_return co_await fp_RunBashScript(fg_Move(EnvironmentScript));
#endif
	}
}
