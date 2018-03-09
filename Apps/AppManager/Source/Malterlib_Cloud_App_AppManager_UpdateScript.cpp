// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
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

	TCContinuation<void> CAppManagerActor::fp_RunUpdateScript
		(
			TCSharedPointer<CApplication> const &_pApplication
			, EUpdateScript _Script
			, CStr const &_Param
			, CVersionManager::CVersionIDAndPlatform const &_VersionID
			, CVersionManager::CVersionInformation *_pVersionInformation
			, fp64 _TimeSinceUpdateStart
		)
	{
		CStr Script = _pApplication->m_Settings.m_UpdateScripts.f_GetScript(_Script);
		if (Script.f_IsEmpty())
			return fg_Explicit();
		
		struct CState
		{
			TCContinuation<void> m_Continuation;
			TCActor<CProcessLaunchActor> m_LaunchActor;
			CActorSubscription m_LaunchSubscription;
			CStr m_ErrorOutput;
			CStr m_StdOutput;
			CStr m_AllOutput;
			
			bool m_bReplied = false;
			void f_Replied()
			{
				m_bReplied = true;
				m_LaunchActor->f_DestroyNoResult(DMibPFile, DMibPLine);
			}
		};
		
		CStr Description = fg_Format("{}/{}", _pApplication->m_Name, _pApplication->m_Settings.m_UpdateScripts.f_GetName(_Script));
		
		TCSharedPointer<CState> pState = fg_Construct();
		pState->m_LaunchActor = fg_ConstructActor<CProcessLaunchActor>();
		
		CStr FileName = CFile::fs_GetExpandedPath(Script, _pApplication->f_GetDirectory());
	
		auto fReportError = [pState, Description](CStr const &_Error)
			{
				if (pState->m_bReplied)
					return;
				pState->m_Continuation.f_SetException(DMibErrorInstance(_Error));
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
				, fg_CreateVector<CStr>(FileName, _Param)
				, _pApplication->f_GetDirectory()
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
								auto ErrorOutput = pState->m_AllOutput.f_Trim();
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
									pState->m_Continuation.f_SetResult();
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
		
		LaunchParams.m_RunAsUser = _pApplication->m_Settings.m_RunAsUser;
#ifdef DPlatformFamily_Windows
		LaunchParams.m_RunAsUserPassword = _pApplication->m_Settings.m_RunAsUserPassword;
#endif
		LaunchParams.m_RunAsGroup = _pApplication->m_Settings.f_GetRunAsGroup();

		LaunchParams.m_Environment["HOME"] = _pApplication->f_GetDirectory() + "/.home";
		LaunchParams.m_Environment["TMPDIR"] = _pApplication->f_GetDirectory() + "/.tmp";
#ifdef DPlatformFamily_Windows
		LaunchParams.m_Environment["TMP"] = _pApplication->f_GetDirectory() + "/.tmp";
		LaunchParams.m_Environment["TEMP"] = _pApplication->f_GetDirectory() + "/.tmp";
#endif
		
		LaunchParams.m_Environment["MalterlibCloud_TimeSinceStart"] = fg_Format("{fe1}", _TimeSinceUpdateStart);
		LaunchParams.m_Environment["MalterlibCloud_Application"] = _pApplication->m_Name;
		LaunchParams.m_Environment["MalterlibCloud_VersionApplication"] = _pApplication->m_Settings.m_VersionManagerApplication;
		if (_VersionID.f_IsValid())
		{
			LaunchParams.m_Environment["MalterlibCloud_Version"] = CStr::fs_ToStr(_VersionID);
			LaunchParams.m_Environment["MalterlibCloud_VersionID"] = CStr::fs_ToStr(_VersionID.m_VersionID);
			LaunchParams.m_Environment["MalterlibCloud_VersionBranch"] = _VersionID.m_VersionID.m_Branch;
			LaunchParams.m_Environment["MalterlibCloud_VersionMajor"] = CStr::fs_ToStr(_VersionID.m_VersionID.m_Major);
			LaunchParams.m_Environment["MalterlibCloud_VersionMinor"] = CStr::fs_ToStr(_VersionID.m_VersionID.m_Minor);
			LaunchParams.m_Environment["MalterlibCloud_VersionRevision"] = CStr::fs_ToStr(_VersionID.m_VersionID.m_Revision);
			LaunchParams.m_Environment["MalterlibCloud_VersionPlatform"] = _VersionID.m_Platform;
		}
		else
		{
			LaunchParams.m_Environment["MalterlibCloud_Version"] = "Unknown";
			LaunchParams.m_Environment["MalterlibCloud_VersionID"] = "Unknown";
		}
		
		if (_pVersionInformation)
		{
			LaunchParams.m_Environment["MalterlibCloud_Time"] = fg_Format("{}", _pVersionInformation->m_Time.f_ToLocal());
			LaunchParams.m_Environment["MalterlibCloud_Configuration"] = fg_Format("{}", _pVersionInformation->m_Configuration);
			LaunchParams.m_Environment["MalterlibCloud_Tags"] = fg_Format("{vs,vb}", _pVersionInformation->m_Tags);
			LaunchParams.m_Environment["MalterlibCloud_RetrySequence"] = fg_Format("{vs,vb}", _pVersionInformation->m_RetrySequence);
			LaunchParams.m_Environment["MalterlibCloud_ExtraInfo"] = _pVersionInformation->m_ExtraInfo.f_ToString("");
			LaunchParams.m_Environment["MalterlibCloud_NumFiles"] = fg_Format("{}", _pVersionInformation->m_nFiles);
			LaunchParams.m_Environment["MalterlibCloud_NumBytes"] = fg_Format("{}", _pVersionInformation->m_nBytes);
		}
		
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
		
		return pState->m_Continuation;
	}
}
