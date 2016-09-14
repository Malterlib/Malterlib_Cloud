// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Cryptography/RandomID>
#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud::NAppManager
{
	TCContinuation<CDistributedAppCommandLineResults> CAppManagerActor::fp_CommandLine_ChangeApplicationSettings(CEJSON const &_Params)
	{
		enum EToUpdateFlag
		{
			EToUpdateFlag_None = 0
			, EToUpdateFlag_Executable = DBit(0)
			, EToUpdateFlag_ExecutableParameters = DBit(1)
			, EToUpdateFlag_RunAsUser = DBit(2)
			, EToUpdateFlag_RunAsGroup = DBit(3)
			, EToUpdateFlag_VersionManagerApplication = DBit(4)
		};

		CStr Name = _Params["Name"].f_String();
		
		auto *pApplication = mp_Applications.f_FindEqual(Name);
		if (!pApplication)
			return DMibErrorInstance(fg_Format("No such application '{}'", Name));
		auto &Application = **pApplication;
		
		CStr Executable;
		TCVector<CStr> ExecutableParameters;
		CStr RunAsUser;
		CStr RunAsGroup;
		CStr VersionManagerApplication;
		
		EToUpdateFlag UpdateFlags = EToUpdateFlag_None; 
		
		bool bUpdateFromVersionInfo = _Params["UpdateFromVersionInfo"].f_Boolean();
		bool bForce = _Params["Force"].f_Boolean();
		
		if (bUpdateFromVersionInfo)
		{
			if (!Application.m_LastInstalledVersion.f_IsValid())
				return DMibErrorInstance("Found no install from last version to get settings from");
			auto &ExtraInfo = Application.m_LastInstalledVersionInfo.m_ExtraInfo;
			if (auto *pValue = ExtraInfo.f_GetMember("Executable", EJSONType_String))
			{
				UpdateFlags |= EToUpdateFlag_Executable;
				Executable = pValue->f_String();
			}

			if (auto *pValue = ExtraInfo.f_GetMember("RunAsUser", EJSONType_String))
			{
				UpdateFlags |= EToUpdateFlag_RunAsUser;
				RunAsUser = pValue->f_String();
			}

			if (auto *pValue = ExtraInfo.f_GetMember("RunAsGroup", EJSONType_String))
			{
				UpdateFlags |= EToUpdateFlag_RunAsGroup;
				RunAsGroup = pValue->f_String();
			}

			if (auto *pValue = ExtraInfo.f_GetMember("ExecutableParams", EJSONType_Array))
			{
				UpdateFlags |= EToUpdateFlag_ExecutableParameters;
				ExecutableParameters.f_Clear();
				for (auto &Param : pValue->f_Array())
				{
					if (!Param.f_IsString())
						continue;
					ExecutableParameters.f_Insert(Param.f_String());
				}
			}
		}
		
		if (auto *pValue = _Params.f_GetMember("Executable"))
		{
			UpdateFlags |= EToUpdateFlag_Executable;
			Executable = pValue->f_String();
		}
		if (auto *pValue = _Params.f_GetMember("ExecutableParameters"))
		{
			UpdateFlags |= EToUpdateFlag_ExecutableParameters;
			ExecutableParameters.f_Clear();
			for (auto &Parameter : pValue->f_Array())
				ExecutableParameters.f_Insert(Parameter.f_String());
		}
		if (auto *pValue = _Params.f_GetMember("RunAsUser"))
		{
			UpdateFlags |= EToUpdateFlag_RunAsUser;
			RunAsUser = pValue->f_String();
		}
		if (auto *pValue = _Params.f_GetMember("RunAsGroup"))
		{
			UpdateFlags |= EToUpdateFlag_RunAsGroup;
			RunAsGroup = pValue->f_String();
		}
		if (auto *pValue = _Params.f_GetMember("VersionManagerApplication"))
		{
			UpdateFlags |= EToUpdateFlag_VersionManagerApplication;
			VersionManagerApplication = pValue->f_String();
		}

		if (Executable.f_IsEmpty() && (UpdateFlags & EToUpdateFlag_Executable))
			return DMibErrorInstance("Trying to set executable to empty");
	
		if (Application.m_bOperationInProgress)
			return DMibErrorInstance("Operation already in progress for application");
		auto InProgressScope = Application.f_SetInProgress();
	
		TCContinuation<CDistributedAppCommandLineResults> Continuation;
		TCSharedPointer<CDistributedAppCommandLineResults> pResult = fg_Construct();
		
		auto fLogInfo = [pResult](CStr const &_Info)
			{
				pResult->f_AddStdOut(_Info + DMibNewLine);
				DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "{}", _Info);
			}
		;
		auto fLogError = [pResult, Continuation](CStr const &_Error)
			{
				pResult->f_AddStdErr(_Error + DMibNewLine);
				pResult->m_Status = 1;
				Continuation.f_SetResult(fg_Move(*pResult));
				DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Command line command failed (change application settings): {}", _Error);
			}
		;
		if (UpdateFlags & EToUpdateFlag_Executable && Application.m_Executable == Executable)
			UpdateFlags &= ~EToUpdateFlag_Executable;
		if (UpdateFlags & EToUpdateFlag_ExecutableParameters && Application.m_ExecutableParameters == ExecutableParameters)
			UpdateFlags &= ~EToUpdateFlag_ExecutableParameters;
		if (UpdateFlags & EToUpdateFlag_RunAsUser && Application.m_RunAsUser == RunAsUser)
			UpdateFlags &= ~EToUpdateFlag_RunAsUser;
		if (UpdateFlags & EToUpdateFlag_RunAsGroup && Application.m_RunAsGroup == RunAsGroup)
			UpdateFlags &= ~EToUpdateFlag_RunAsGroup;
		if (UpdateFlags & EToUpdateFlag_VersionManagerApplication && Application.m_VersionManagerApplication == VersionManagerApplication)
			UpdateFlags &= ~EToUpdateFlag_VersionManagerApplication;
			
		if (UpdateFlags == EToUpdateFlag_None && !bForce)
		{
			fLogInfo("No settings were changed. To updating of file permissions run with --force");
			Continuation.f_SetResult(fg_Move(*pResult));
			return Continuation;
		}
		
		
		fg_ThisActor(this)(&CAppManagerActor::fp_ChangeEncryption, *pApplication, EEncryptOperation_Open, false)
			> 
			[
				this
				, pApplication = *pApplication
				, UpdateFlags
				, fLogInfo
				, fLogError
				, Continuation
				, pResult
				, InProgressScope
				, Executable
				, ExecutableParameters
				, RunAsUser
				, RunAsGroup
				, VersionManagerApplication
			]
			(TCAsyncResult<void> &&_Result)
			{
				if (!_Result)
				{
					fLogError(fg_Format("Failed to open encryption: {}", _Result.f_GetExceptionStr()));
					return;
				}
				if (pApplication->m_bDeleted)
				{
					fLogError("Application has been deleted, aborting");
					return;
				}
				
				fLogInfo("Stopping old application");
				pApplication->f_Stop(false) > 
					[
						this
						, UpdateFlags
						, pApplication
						, pResult
						, fLogInfo
						, fLogError
						, Continuation
						, InProgressScope
						, Executable
						, ExecutableParameters
						, RunAsUser
						, RunAsGroup
						, VersionManagerApplication
					]
					(TCAsyncResult<uint32> &&_ExitStatus)
					{
						fp_OutputApplicationStop(_ExitStatus, *pResult, pApplication->m_Name);
						
						if (!_ExitStatus)
						{
							fLogError("Failed to exit old application, aborting update");
							return;
						}
						
						if (pApplication->m_bDeleted)
						{
							fLogError("Application has been deleted, aborting");
							return;
						}
						
						if (UpdateFlags & EToUpdateFlag_Executable)
							pApplication->m_Executable = Executable;
						if (UpdateFlags & EToUpdateFlag_ExecutableParameters)
							pApplication->m_ExecutableParameters = ExecutableParameters;
						if (UpdateFlags & EToUpdateFlag_RunAsUser)
							pApplication->m_RunAsUser = RunAsUser;
						if (UpdateFlags & EToUpdateFlag_RunAsGroup)
							pApplication->m_RunAsGroup = RunAsGroup;
						if (UpdateFlags & EToUpdateFlag_VersionManagerApplication)
							pApplication->m_VersionManagerApplication = VersionManagerApplication;
						
						fLogInfo("Saving application state and update application files");
						fg_Dispatch
							(
								mp_FileActor
								, [pApplication, Directory = pApplication->f_GetDirectory(), InProgressScope]()
								{
									fsp_UpdateApplicationFiles(Directory, pApplication, pApplication->m_Files);
								}
							)
							+ self(&CAppManagerActor::fp_UpdateApplicationJSON, pApplication)
							> [this, pResult, pApplication, Continuation, fLogInfo, fLogError, InProgressScope](TCAsyncResult<void> &&_Result, TCAsyncResult<void> &&_UpdateJSONResults)
							{
								bool bError = false;
								if (!_Result)
								{
									bError = true;
									fLogError(fg_Format("Failed to update application files: {}", _Result.f_GetExceptionStr()));
								}
								if (!_UpdateJSONResults)
								{
									bError = true;
									fLogError(fg_Format("Failed to save application state: {}", _UpdateJSONResults.f_GetExceptionStr()));
								}
								else
									fLogInfo("Application state successfully stored, so any changes will persist");
								
								if (bError)
									return;
								
								if (pApplication->m_bDeleted)
								{
									fLogError("Application has been deleted, aborting");
									return;
								}
									
								fLogInfo("Launching applicaion with changed settings");
								fg_ThisActor(this)(&CAppManagerActor::fp_LaunchApp, pApplication, false)
									> [fLogError, fLogInfo, Continuation, pResult, InProgressScope](TCAsyncResult<void> &&_Result)
									{
										if (!_Result)
										{
											fLogError(fg_Format("Failed to launch app: {}. Will retry periodically.", _Result.f_GetExceptionStr()));
											return;
										}
										fLogInfo("Application settings were successfully changed");
										Continuation.f_SetResult(fg_Move(*pResult));
									}
								;
							}
						;
					}
				;
			}
		;
		return Continuation;
	}
}
