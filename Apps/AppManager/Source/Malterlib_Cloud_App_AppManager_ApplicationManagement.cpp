// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Cryptography/RandomID>
#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud::NAppManager
{
	void CAppManagerActor::fp_OutputApplicationStop(TCAsyncResult<uint32> const &_Result, CDistributedAppCommandLineResults &o_Results, CStr const &_Name)
	{
		if (!_Result)
			o_Results.f_AddStdErr(fg_Format("Error stopping application '{}'{\n}", _Result.f_GetExceptionStr()));
		else if (*_Result)
			o_Results.f_AddStdErr(fg_Format("Application '{}' exited with non 0 status: {}{\n}", _Name, *_Result));
	}
	
	constexpr static EFileAttrib gc_RootAttributes = 
				EFileAttrib_UnixAttributesValid  
				| EFileAttrib_UserRead 
				| EFileAttrib_UserWrite
				| EFileAttrib_UserExecute 
				| EFileAttrib_GroupRead
				| EFileAttrib_GroupExecute
				| EFileAttrib_EveryoneRead 
				| EFileAttrib_EveryoneExecute
	;
	
	void CAppManagerActor::fsp_UpdateAttributes(CStr const &_File)
	{
		auto CurrentAttributes = CFile::fs_GetAttributes(_File);
		CFile::fs_SetAttributes
			(
				_File
				, EFileAttrib_UnixAttributesValid 
				| (CurrentAttributes & EFileAttrib_UserRead) 
				| (CurrentAttributes & EFileAttrib_UserWrite)
				| (CurrentAttributes & EFileAttrib_UserExecute) 
				| (CurrentAttributes & EFileAttrib_GroupRead)
				| (CurrentAttributes & EFileAttrib_GroupExecute)
				| (CurrentAttributes & EFileAttrib_EveryoneRead)
				| (CurrentAttributes & EFileAttrib_EveryoneExecute)
			)
		;
	}

	CStr CAppManagerActor::fsp_UnpackApplication
		(
			CStr const &_Source
			, CStr const &_Destination
			, TCSharedPointer<CApplication> const &_pApplication
			, TCVector<CStr> &o_Files
			, TCSet<CStr> const &_AllowExist
			, bool _bForceInstall
		)
	{
		CStr Return;
		if (!_bForceInstall && CFile::fs_FileExists(_Destination))
		{
			auto FoundFiles = CFile::fs_FindFiles(_Destination + "/*");
			for (auto &File : FoundFiles)
			{
				if (!_AllowExist.f_FindEqual(File))
					DMibError(fg_Format("Application already exists at: '{}'. You have to manually delete it to resue the name", _Destination));
			}
		
		}
		if (CFile::fs_FileExists(_Source, EFileAttrib_Directory))
		{
			CFile::fs_DiffCopyFileOrDirectory
				(
					_Source
					, _Destination
					, [&](CFile::EDiffCopyChange _Change, NStr::CStr const &_Source, NStr::CStr const &_Destination, NStr::CStr const &_Link) -> CFile::EDiffCopyChangeAction
					{
						if 
							(
								_bForceInstall 
								&&
								(
									_Change == CFile::EDiffCopyChange_DirectoryDeleted 
									|| _Change == CFile::EDiffCopyChange_FileDeleted 
									|| _Change == CFile::EDiffCopyChange_LinkDeleted
								)
							)
						{
							return CFile::EDiffCopyChangeAction_Skip;
						}
						
						for (auto &Allow : _AllowExist)
						{
							if (_Destination.f_StartsWith(Allow))
								return CFile::EDiffCopyChangeAction_Skip;
						}
						return CFile::EDiffCopyChangeAction_Perform;
					}
				)
			;
		}
		else
		{
			CFile::fs_CreateDirectory(_Destination);
			CStr Output = fsp_RunTool
				(
					CStr::CFormat("[{}] Extracting application") << _pApplication->m_Name
					, "tar"
					, _Destination
					, fg_CreateVector<CStr>("--no-same-owner", "-xf", _Source)
					, ""
					, ""
				)
			;
			Return = Output;
		}
		
		auto &Settings = _pApplication->m_Settings;

		CStr ExcutableFile = fg_Format("{}/{}", _Destination, Settings.m_Executable); 
		if (!CFile::fs_FileExists(ExcutableFile, EFileAttrib_Executable))
			DMibError(fg_Format("Executable file '{}' does not exist or does not have the executable flag set", ExcutableFile));
		
		CFile::CFindFilesOptions FindOptions(_Destination + "/*", true);
		FindOptions.m_AttribMask = EFileAttrib_Directory | EFileAttrib_File | EFileAttrib_Link | EFileAttrib_FindDirectoryLast;
		
		auto Files = CFile::fs_FindFiles(FindOptions);
		
		CFile::fs_SetAttributes(_Destination, gc_RootAttributes);
		
		mint PrefixLen = _Destination.f_GetLen() + 1;
		for (auto &File : Files)
		{
			bool bFound = false;
			for (auto &Allow : _AllowExist)
			{
				if (File.m_Path.f_StartsWith(Allow))
				{
					bFound = true;
					break;
				}
			}
			if (bFound)
				continue;
			
			if (!Settings.m_RunAsUser.f_IsEmpty())
				CFile::fs_SetOwner(File.m_Path, Settings.m_RunAsUser);
			if (!Settings.m_RunAsGroup.f_IsEmpty())
				CFile::fs_SetGroup(File.m_Path, Settings.m_RunAsGroup);
			
			fsp_UpdateAttributes(File.m_Path);
			
			o_Files.f_Insert(File.m_Path.f_Extract(PrefixLen));
		}
		
		return Return;
	}

	TCContinuation<void> CAppManagerActor::fp_UpdateApplicationJSON(TCSharedPointer<CApplication> const &_pApplication)
	{
		if (_pApplication->m_bDeleted)
			return DMibErrorInstance("Application has been deleted");
		auto &Settings = _pApplication->m_Settings;
		
		auto &ApplicationJSON = mp_State.m_StateDatabase.m_Data["Applications"][_pApplication->m_Name];
		ApplicationJSON["Executable"] = Settings.m_Executable; 
		ApplicationJSON["RunAsUser"] = Settings.m_RunAsUser; 
		ApplicationJSON["RunAsGroup"] = Settings.m_RunAsGroup;
		{
			auto &Parameters = ApplicationJSON["Parameters"].f_Array();
			Parameters.f_Clear();
			for (auto &Parameter : Settings.m_ExecutableParameters)
				Parameters.f_Insert(Parameter);
		}
		ApplicationJSON["EncryptionStorage"] = Settings.m_EncryptionStorage;
		ApplicationJSON["ParentApplication"] = Settings.m_ParentApplication;
		ApplicationJSON["VersionManagerApplication"] = Settings.m_VersionManagerApplication;
		ApplicationJSON["LastInstalledVersion"] = _pApplication->m_LastInstalledVersion.f_ToJSON();
		ApplicationJSON["LastInstalledVersionInfo"] = _pApplication->m_LastInstalledVersionInfo.f_ToJSON();
		ApplicationJSON["AutoUpdate"] = Settings.m_bAutoUpdate;
		{
			auto &Array = ApplicationJSON["AutoUpdateTags"].f_Array();
			Array.f_Clear();
			for (auto &Tag : Settings.m_AutoUpdateTags)
				Array.f_Insert(Tag);
		}
		{
			auto &Array = ApplicationJSON["AutoUpdateBranches"].f_Array();
			Array.f_Clear();
			for (auto &Branch : Settings.m_AutoUpdateBranches)
				Array.f_Insert(Branch);
		}
		auto &UpdateScripts = ApplicationJSON["UpdateScripts"];
		UpdateScripts.f_Object();
		UpdateScripts["PreUpdate"] = Settings.m_UpdateScripts.m_PreUpdate; 
		UpdateScripts["PostUpdate"] = Settings.m_UpdateScripts.m_PostUpdate; 
		UpdateScripts["PostLaunch"] = Settings.m_UpdateScripts.m_PostLaunch; 
		UpdateScripts["OnError"] = Settings.m_UpdateScripts.m_OnError;
		
		ApplicationJSON["SelfUpdateSource"] = Settings.m_bSelfUpdateSource; 
		ApplicationJSON["Files"] = _pApplication->m_Files;
	
		TCContinuation<void> Continuation;
		mp_State.m_StateDatabase.f_Save() > Continuation;  
		return Continuation;
	}
	
	void CAppManagerActor::fsp_CreateApplicationUserGroup(CApplicationSettings const &_Settings, TCFunction<void (CStr const &_Info)> const &_fLogInfo, CStr const &_HomeDir)
	{
		if (!_Settings.m_RunAsGroup.f_IsEmpty())
		{
			CStr GroupID;
			if (!NSys::fg_UserManagement_GroupExists(_Settings.m_RunAsGroup, GroupID))
			{
				NSys::fg_UserManagement_CreateGroup(_Settings.m_RunAsGroup, GroupID);
				if (_fLogInfo)
					_fLogInfo(fg_Format("Created group '{}' with resulting group ID: {}", _Settings.m_RunAsGroup, GroupID));
			}
		}
		if (!_Settings.m_RunAsUser.f_IsEmpty())
		{
			CStr UserID;
			if (!NSys::fg_UserManagement_UserExists(_Settings.m_RunAsUser, UserID))
			{
				NSys::fg_UserManagement_CreateUser
					(
						_Settings.m_RunAsGroup
						, _Settings.m_RunAsUser
						, ""
						, _Settings.m_RunAsUser
						, _HomeDir
						, UserID
					)
				;
				if (_fLogInfo)
					_fLogInfo(fg_Format("Created user '{}' with resulting user ID: {}", _Settings.m_RunAsUser, UserID));
			}
		}
	}
	
	void CAppManagerActor::fsp_UpdateApplicationFiles(CStr const &_ApplicationDir, TCSharedPointer<CApplication> const &_pApplication, TCVector<CStr> const &_Files)
	{
		auto &Settings = _pApplication->m_Settings;
		auto fSetOwners = [&](CStr _Directory)
			{
				while (_Directory.f_GetLen() >= _ApplicationDir.f_GetLen())
				{
					if (!Settings.m_RunAsGroup.f_IsEmpty())
						CFile::fs_SetOwner(_Directory, Settings.m_RunAsUser);
					if (!Settings.m_RunAsGroup.f_IsEmpty())
						CFile::fs_SetGroup(_Directory, Settings.m_RunAsGroup);
					_Directory = CFile::fs_GetPath(_Directory);
				}
			}
		;
		for (auto &File : _Files)
		{
			CStr SourcePath = fg_Format("{}/{}", _ApplicationDir, File);
			if (CFile::fs_FileExists(SourcePath, EFileAttrib_File | EFileAttrib_Link))
			{
				CStr Directory = CFile::fs_GetPath(SourcePath);
				fSetOwners(Directory);
			}
		}
		CFile::fs_CreateDirectory(_ApplicationDir + "/.home");
		CFile::fs_CreateDirectory(_ApplicationDir + "/.tmp");
		fSetOwners(_ApplicationDir + "/.home");
		fSetOwners(_ApplicationDir + "/.tmp");
		
		CFile::fs_SetAttributes(_ApplicationDir, gc_RootAttributes);
	}

	TCContinuation<CDistributedAppCommandLineResults> CAppManagerActor::fp_CommandLine_StopApplication(CEJSON const &_Params)
	{
		CStr Name = _Params["Name"].f_String();
		auto pOldApplication = mp_Applications.f_FindEqual(Name);
		if (!pOldApplication)
			return DMibErrorInstance(fg_Format("No such application '{}'", Name));
		
		TCSharedPointer<CApplication> pApplication = *pOldApplication;
		
		if (pApplication->f_IsInProgress())
			return DMibErrorInstance("Operation already in progress for application");
		if (!pApplication->m_ProcessLaunch || pApplication->m_bStopped)
			return DMibErrorInstance("Application already stopped");

		auto InProgressScope = pApplication->f_SetInProgress();
		TCContinuation<CDistributedAppCommandLineResults> Continuation;
		TCSharedPointer<CDistributedAppCommandLineResults> pResult = fg_Construct();
		auto fLogError = [pResult, Continuation](CStr const &_Error)
			{
				pResult->f_AddStdErr(_Error + DMibNewLine);
				pResult->m_Status = 1;
				Continuation.f_SetResult(fg_Move(*pResult));
				DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Command line command failed (stop application): {}", _Error);
			}
		;
		
		pApplication->f_Stop(false) > [this, pApplication, pResult, fLogError, Continuation, InProgressScope]
			(TCAsyncResult<uint32> &&_ExitStatus)
			{
				fp_OutputApplicationStop(_ExitStatus, *pResult, pApplication->m_Name);
				
				if (!_ExitStatus)
				{
					fLogError("Failed to exit old application");
					return;
				}
				
				if (pApplication->m_bDeleted)
				{
					fLogError("Application has been deleted, aborting");
					return;
				}
				Continuation.f_SetResult(fg_Move(*pResult));
			}
		;
		return Continuation;
	}
	
	TCContinuation<CDistributedAppCommandLineResults> CAppManagerActor::fp_CommandLine_StartApplication(CEJSON const &_Params)
	{
		CStr Name = _Params["Name"].f_String();
		auto pOldApplication = mp_Applications.f_FindEqual(Name);
		if (!pOldApplication)
			return DMibErrorInstance(fg_Format("No such application '{}'", Name));
		
		TCSharedPointer<CApplication> pApplication = *pOldApplication;
		
		if (pApplication->f_IsInProgress())
			return DMibErrorInstance("Operation already in progress for application");
		if (pApplication->m_ProcessLaunch)
			return DMibErrorInstance("Application already started");

		auto InProgressScope = pApplication->f_SetInProgress();
		TCContinuation<CDistributedAppCommandLineResults> Continuation;
		TCSharedPointer<CDistributedAppCommandLineResults> pResult = fg_Construct();
		auto fLogError = [pResult, Continuation](CStr const &_Error)
			{
				pResult->f_AddStdErr(_Error + DMibNewLine);
				pResult->m_Status = 1;
				Continuation.f_SetResult(fg_Move(*pResult));
				DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Command line command failed (start application): {}", _Error);
			}
		;
		
		fg_ThisActor(this)(&CAppManagerActor::fp_LaunchApp, pApplication, true)
			> [fLogError, Continuation, pResult, InProgressScope](TCAsyncResult<void> &&_Result)
			{
				if (!_Result)
				{
					fLogError(fg_Format("Failed to launch app: {}. Will retry periodically.", _Result.f_GetExceptionStr()));
					return;
				}
				Continuation.f_SetResult(fg_Move(*pResult));
			}
		;
		return Continuation;
	}
	
	TCContinuation<CDistributedAppCommandLineResults> CAppManagerActor::fp_CommandLine_RestartApplication(CEJSON const &_Params)
	{
		CStr Name = _Params["Name"].f_String();
		auto pOldApplication = mp_Applications.f_FindEqual(Name);
		if (!pOldApplication)
			return DMibErrorInstance(fg_Format("No such application '{}'", Name));
		
		TCSharedPointer<CApplication> pApplication = *pOldApplication;
		
		if (pApplication->f_IsInProgress())
			return DMibErrorInstance("Operation already in progress for application");

		auto InProgressScope = pApplication->f_SetInProgress();
		TCContinuation<CDistributedAppCommandLineResults> Continuation;
		TCSharedPointer<CDistributedAppCommandLineResults> pResult = fg_Construct();
		auto fLogError = [pResult, Continuation](CStr const &_Error)
			{
				pResult->f_AddStdErr(_Error + DMibNewLine);
				pResult->m_Status = 1;
				Continuation.f_SetResult(fg_Move(*pResult));
				DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Command line command failed (restart application): {}", _Error);
			}
		;
		
		pApplication->f_Stop(false) > [this, pApplication, pResult, fLogError, Continuation, InProgressScope]
			(TCAsyncResult<uint32> &&_ExitStatus)
			{
				fp_OutputApplicationStop(_ExitStatus, *pResult, pApplication->m_Name);
				
				if (!_ExitStatus)
				{
					fLogError("Failed to exit old application");
					return;
				}
				
				if (pApplication->m_bDeleted)
				{
					fLogError("Application has been deleted, aborting");
					return;
				}

				fg_ThisActor(this)(&CAppManagerActor::fp_LaunchApp, pApplication, true)
					> [fLogError, Continuation, pResult, InProgressScope](TCAsyncResult<void> &&_Result)
					{
						if (!_Result)
						{
							fLogError(fg_Format("Failed to launch app: {}. Will retry periodically.", _Result.f_GetExceptionStr()));
							return;
						}
						Continuation.f_SetResult(fg_Move(*pResult));
					}
				;
			}
		;
		return Continuation;
	}
}
