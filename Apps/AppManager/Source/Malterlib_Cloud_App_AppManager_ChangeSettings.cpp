// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Cryptography/RandomID>
#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud::NAppManager
{
	namespace
	{
		enum EToUpdateFlag
		{
			EToUpdateFlag_None = 0
			, EToUpdateFlag_Executable = DBit(0)
			, EToUpdateFlag_ExecutableParameters = DBit(1)
			, EToUpdateFlag_RunAsUser = DBit(2)
			, EToUpdateFlag_RunAsGroup = DBit(3)
			, EToUpdateFlag_VersionManagerApplication = DBit(4)
			, EToUpdateFlag_AutoUpdateTags = DBit(5)
			, EToUpdateFlag_AutoUpdateBranches = DBit(6)
		};
	}
	TCContinuation<CDistributedAppCommandLineResults> CAppManagerActor::fp_CommandLine_ChangeApplicationSettings(CEJSON const &_Params)
	{
		static constexpr auto c_NeedRestartUpdateFlags = EToUpdateFlag_Executable | EToUpdateFlag_ExecutableParameters | EToUpdateFlag_RunAsUser | EToUpdateFlag_RunAsGroup;

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
		bool bAutoUpdate = false;
		TCSet<CStr> AutoUpdateTags;
		TCSet<CStr> AutoUpdateBranches;
		if (auto *pValue = _Params.f_GetMember("AutoUpdateTags"))
		{
			UpdateFlags |= EToUpdateFlag_AutoUpdateTags;
			if (pValue->f_IsArray())
			{
				bAutoUpdate = true;
				for (auto &TagJSON : pValue->f_Array())
				{
					auto &Tag = TagJSON.f_String();
					if (!CVersionManager::fs_IsValidTag(Tag))
						return DMibErrorInstance(fg_Format("'{}' is not a valid tag", Tag));
					AutoUpdateTags[Tag];
				}
			}
		}
		if (auto *pValue = _Params.f_GetMember("AutoUpdateBranches"))
		{
			UpdateFlags |= EToUpdateFlag_AutoUpdateBranches;
			for (auto &BranchJSON : pValue->f_Array())
			{
				auto &Branch = BranchJSON.f_String();
				if (!CVersionManager::fs_IsValidBranch(Branch))
					return DMibErrorInstance(fg_Format("'{}' is not a valid branch", Branch));
				AutoUpdateBranches[Branch];
			}
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
		if (UpdateFlags & EToUpdateFlag_AutoUpdateTags && Application.m_bAutoUpdate == bAutoUpdate && Application.m_AutoUpdateTags == AutoUpdateTags)
			UpdateFlags &= ~EToUpdateFlag_AutoUpdateTags;
		if (UpdateFlags & EToUpdateFlag_AutoUpdateBranches && Application.m_AutoUpdateBranches == AutoUpdateBranches)
			UpdateFlags &= ~EToUpdateFlag_AutoUpdateBranches;
			
		if (UpdateFlags == EToUpdateFlag_None && !bForce)
		{
			fLogInfo("No settings were changed. To updating of file permissions run with --force");
			Continuation.f_SetResult(fg_Move(*pResult));
			return Continuation;
		}

		auto fUpdateSettings = [=, pApplication = *pApplication]
			{
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
				if (UpdateFlags & EToUpdateFlag_AutoUpdateTags)
				{
					pApplication->m_bAutoUpdate = bAutoUpdate;
					pApplication->m_AutoUpdateTags = AutoUpdateTags;
				}
				if (UpdateFlags & EToUpdateFlag_AutoUpdateBranches)
					pApplication->m_AutoUpdateBranches = AutoUpdateBranches;
			}
		;
		
		if (!(UpdateFlags & c_NeedRestartUpdateFlags) && !bForce)
		{
			fUpdateSettings();
			fLogInfo("Saving application state");
			self(&CAppManagerActor::fp_UpdateApplicationJSON, *pApplication)
				> [=, InProgressScope = InProgressScope](TCAsyncResult<void> &&_UpdateJSONResults)
				{
					if (!_UpdateJSONResults)
					{
						fLogError(fg_Format("Failed to save application state: {}", _UpdateJSONResults.f_GetExceptionStr()));
						return;
					}
					fLogInfo("Application settings were successfully changed");
					Continuation.f_SetResult(fg_Move(*pResult));
				}
			;
			return Continuation;
		}
		
		fg_ThisActor(this)(&CAppManagerActor::fp_ChangeEncryption, *pApplication, EEncryptOperation_Open, false)
			>[=, pApplication = *pApplication](TCAsyncResult<void> &&_Result)
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
				pApplication->f_Stop(false) > [=](TCAsyncResult<uint32> &&_ExitStatus)
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

						fUpdateSettings();						
						
						fLogInfo("Saving application state and update application files");
						fg_Dispatch
							(
								mp_FileActor
								, [=, Directory = pApplication->f_GetDirectory(), InProgressScope = InProgressScope]()
								{
									fsp_UpdateApplicationFiles(Directory, pApplication, pApplication->m_Files);
								}
							)
							+ self(&CAppManagerActor::fp_UpdateApplicationJSON, pApplication)
							> [=](TCAsyncResult<void> &&_Result, TCAsyncResult<void> &&_UpdateJSONResults)
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
									> [=, InProgressScope = InProgressScope](TCAsyncResult<void> &&_Result)
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
