// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Cryptography/RandomID>
#include "Malterlib_Cloud_App_AppManager.h"

#ifdef DPlatformFamily_Windows
#include <Mib/Core/PlatformSpecific/WindowsFilePath>
#endif

namespace NMib::NCloud::NAppManager
{
	CStr CAppManagerActor::fp_GetApplicationStopErrors(TCAsyncResult<uint32> const &_Result, CStr const &_Name)
	{
		if (!_Result)
			return fg_Format("Error stopping application '{}'{\n}", _Result.f_GetExceptionStr());
		else if (*_Result)
			return fg_Format("Application '{}' exited with non 0 status: {}{\n}", _Name, *_Result);
		return {};
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
		 	CStr const &_ApplicationRootDirectory
			, CStr const &_Source
			, CStr const &_Destination
			, CStr const &_ApplicationName
			, CApplicationSettings const &_Settings 
			, TCVector<CStr> &o_Files
			, TCSet<CStr> const &_AllowExist
			, bool _bForceInstall
		 	, TCSharedPointer<CUniqueUserGroup> const &_pUniqueUserGroup
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
				 	, {}
					, 0.0
				)
			;
		}
		else
		{
			CFile::fs_CreateDirectory(_Destination);
			TCVector<CStr> Options;

			CStr Output = fsp_RunTool
				(
					CStr::CFormat("[{}] Extracting application") << _ApplicationName
					, _ApplicationRootDirectory / "bin/bsdtar"
					, _Destination
					, fg_CreateVector<CStr>
					(
						"--no-same-owner"
						, "-xf"
						, _Source
					)
				)
			;
			Return = Output;
		}
		
		auto &Settings = _Settings;

		if (!Settings.m_Executable.f_IsEmpty())
		{
			CStr ExcutableFile = fg_Format("{}/{}", _Destination, Settings.m_Executable); 
			if 
				(
					!CFile::fs_FileExists
					(
						ExcutableFile
	#ifdef DPlatformFamily_Windows
						, EFileAttrib_File
	#else
						, EFileAttrib_Executable
	#endif
					)
				)
			{
				DMibError(fg_Format("Executable file '{}' does not exist or does not have the executable flag set", ExcutableFile));
			}
		}
		
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
				CFile::fs_SetOwner(File.m_Path, _pUniqueUserGroup->f_GetUser(Settings.m_RunAsUser));
			if (!Settings.m_RunAsGroup.f_IsEmpty())
				CFile::fs_SetGroup(File.m_Path, _pUniqueUserGroup->f_GetGroup(Settings.m_RunAsGroup));
			
			fsp_UpdateAttributes(File.m_Path);
			
			o_Files.f_Insert(File.m_Path.f_Extract(PrefixLen));
		}
		
		return Return;
	}

	TCFuture<void> CAppManagerActor::fp_UpdateApplicationJSON(TCSharedPointer<CApplication> const &_pApplication)
	{
		TCPromise<void> Promise;

		auto &Application = *_pApplication;
		if (Application.m_bDeleted)
			return Promise <<= DMibErrorInstance("Application has been deleted");
		auto &Settings = Application.m_Settings;
		
		auto &ApplicationJSON = mp_State.m_StateDatabase.m_Data["Applications"][Application.m_Name];
		ApplicationJSON["Executable"] = Settings.m_Executable; 
		ApplicationJSON["RunAsUser"] = Settings.m_RunAsUser; 
		ApplicationJSON["RunAsGroup"] = Settings.m_RunAsGroup;
		ApplicationJSON["RunAsUserHasShell"] = Settings.m_bRunAsUserHasShell;
		ApplicationJSON["DistributedApp"] = Settings.m_bDistributedApp;
		{
			auto &Parameters = ApplicationJSON["Parameters"].f_Array();
			Parameters.f_Clear();
			for (auto &Parameter : Settings.m_ExecutableParameters)
				Parameters.f_Insert(Parameter);
		}
		ApplicationJSON["EncryptionStorage"] = Settings.m_EncryptionStorage;
		ApplicationJSON["EncryptionFileSystem"] = Settings.m_EncryptionFileSystem;
		ApplicationJSON["ParentApplication"] = Settings.m_ParentApplication;
		ApplicationJSON["VersionManagerApplication"] = Settings.m_VersionManagerApplication;
		ApplicationJSON["LastInstalledVersion"] = Application.m_LastInstalledVersion.f_ToJSON();
		ApplicationJSON["LastInstalledVersionInfo"] = Application.m_LastInstalledVersionInfo.f_ToJSON();
		ApplicationJSON["LastInstalledVersionFinished"] = Application.m_LastInstalledVersionFinished.f_ToJSON();
		ApplicationJSON["LastInstalledVersionInfoFinished"] = Application.m_LastInstalledVersionInfoFinished.f_ToJSON();
		ApplicationJSON["LastTriedInstalledVersion"] = Application.m_LastTriedInstalledVersion.f_ToJSON();
		ApplicationJSON["LastTriedInstalledVersionInfo"] = Application.m_LastTriedInstalledVersionInfo.f_ToJSON();
		ApplicationJSON["LastFailedInstalledVersionFailureStage"] = (uint32)Application.m_LastFailedInstalledVersionFailureStage;
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
		{
			auto &UpdateScripts = ApplicationJSON["UpdateScripts"] = CEJSON();
			UpdateScripts.f_Object();
			UpdateScripts["PreUpdate"] = Settings.m_UpdateScripts.m_PreUpdate; 
			UpdateScripts["PostUpdate"] = Settings.m_UpdateScripts.m_PostUpdate; 
			UpdateScripts["PostLaunch"] = Settings.m_UpdateScripts.m_PostLaunch; 
			UpdateScripts["OnError"] = Settings.m_UpdateScripts.m_OnError;
		}
		
		ApplicationJSON["SelfUpdateSource"] = Settings.m_bSelfUpdateSource;
		ApplicationJSON["Files"] = Application.m_Files;

		ApplicationJSON["AssociatedHostID"] = Application.m_AssociatedHostID;
		ApplicationJSON["UpdateGroup"] = Settings.m_UpdateGroup;
#ifdef DPlatformFamily_Windows
		ApplicationJSON["RunAsUserPassword"] = Settings.m_RunAsUserPassword;
#endif

		{
			auto &BackupJSON = ApplicationJSON["Backup"] = CEJSON();
			BackupJSON.f_Object();
			{
				auto &JSON = BackupJSON["IncludeWildcards"] = CEJSON();
				JSON.f_Object();
				for (auto &Destination : Settings.m_Backup_IncludeWildcards)
				{
					auto &DestinationJSON = JSON[Settings.m_Backup_IncludeWildcards.fs_GetKey(Destination)];
					
					if (Destination)
						DestinationJSON = *Destination;
					else
						DestinationJSON = nullptr;
				}
			}
			{
				auto &JSON = BackupJSON["ExcludeWildcards"];
				JSON.f_Array().f_Clear();
				for (auto &Wildcard : Settings.m_Backup_ExcludeWildcards)
					JSON.f_Insert(Wildcard);
			}
			{
				auto &JSON = BackupJSON["AddSyncFlagsWildcards"] = CEJSON();
				JSON.f_Object();
				for (auto &Flags : Settings.m_Backup_AddSyncFlagsWildcards)
					JSON[Settings.m_Backup_AddSyncFlagsWildcards.fs_GetKey(Flags)] = CDirectoryManifestFile::fs_GenerateSyncFlags(Flags);
			}
			{
				auto &JSON = BackupJSON["RemoveSyncFlagsWildcards"] = CEJSON();
				JSON.f_Object();
				for (auto &Flags : Settings.m_Backup_RemoveSyncFlagsWildcards)
					JSON[Settings.m_Backup_RemoveSyncFlagsWildcards.fs_GetKey(Flags)] = CDirectoryManifestFile::fs_GenerateSyncFlags(Flags);
			}
			BackupJSON["NewBackupIntervalHours"] = CTimeSpanConvert(Settings.m_Backup_NewBackupInterval).f_GetHoursFloat();
			BackupJSON["Enabled"] = Settings.m_bBackupEnabled;
		}

		{
			auto &RegisterInfo = ApplicationJSON["RegisterInfo"] = CEJSON();
			RegisterInfo.f_Object();
			RegisterInfo["UpdateType"] = Application.m_RegisterInfo.m_UpdateType;
			
			if (Application.m_RegisterInfo.m_Resources_Files)
				RegisterInfo["ResourcesFiles"] = *Application.m_RegisterInfo.m_Resources_Files;
			
			if (Application.m_RegisterInfo.m_Resources_FilesPerProcess)
				RegisterInfo["ResourcesFilesPerProcess"] = *Application.m_RegisterInfo.m_Resources_FilesPerProcess;
			
			if (Application.m_RegisterInfo.m_Resources_Threads)
				RegisterInfo["ResourcesThreads"] = *Application.m_RegisterInfo.m_Resources_Threads;
			
			if (Application.m_RegisterInfo.m_Resources_Processes)
				RegisterInfo["ResourcesProcesses"] = *Application.m_RegisterInfo.m_Resources_Processes;
		}
		{
			auto &Array = ApplicationJSON["Dependencies"].f_Array();
			Array.f_Clear();
			for (auto &Dependency : Settings.m_Dependencies)
				Array.f_Insert(Dependency);
		}
		ApplicationJSON["StopOnDependencyFailure"] = Settings.m_bStopOnDependencyFailure;
		
		ApplicationJSON["PreventLaunchUser"] = Application.m_bPreventLaunch_User; 
		ApplicationJSON["PreventLaunchUpdate"] = Application.m_bPreventLaunch_Update;
		ApplicationJSON["AppManagerVersion"] = Settings.m_AppManagerVersion;

		return Promise <<= mp_State.m_StateDatabase.f_Save();
	}
	
	void CAppManagerActor::fsp_CreateApplicationUserGroup
		(
		 	CApplicationSettings const &_Settings
		 	, TCFunction<void (CStr const &_Info)> const &_fLogInfo
		 	, CStr const &_HomeDir
		 	, TCSharedPointer<CUniqueUserGroup> const &_pUniqueUserGroup
		)
	{
		CStr Group = _pUniqueUserGroup->f_GetGroup(_Settings.m_RunAsGroup);
		CStr User = _pUniqueUserGroup->f_GetUser(_Settings.m_RunAsUser);

		if (!_Settings.m_RunAsGroup.f_IsEmpty())
		{
			CStr GroupID;
			if (!NSys::fg_UserManagement_GroupExists(Group, GroupID))
			{
				NSys::fg_UserManagement_CreateGroup(Group, GroupID);
				if (_fLogInfo)
					_fLogInfo(fg_Format("Created group '{}' with resulting group ID: {}", Group, GroupID));
			}
		}
		if (!_Settings.m_RunAsUser.f_IsEmpty())
		{
			CStr UserID;
			if (!NSys::fg_UserManagement_UserExists(User, UserID))
			{
				NSys::fg_UserManagement_CreateUser
					(
						Group
						, User
#ifdef DPlatformFamily_Windows
						, _Settings.m_RunAsUserPassword
#else
						, ""
#endif
						, User
						, _HomeDir
						, UserID
					 	, _Settings.m_bRunAsUserHasShell ? NSys::EUserManagementCreateUserFlag_ShellAccess : NSys::EUserManagementCreateUserFlag_None
					)
				;
				if (_fLogInfo)
					_fLogInfo(fg_Format("Created user '{}' with resulting user ID: {}", User, UserID));
			}
		}
	}
	
	void CAppManagerActor::fsp_UpdateApplicationFilePermissions
		(
		 	CStr const &_ApplicationDir
		 	, TCSharedPointer<CApplication> const &_pApplication
		 	, TCVector<CStr> const &_Files
		 	, TCSharedPointer<CUniqueUserGroup> const &_pUniqueUserGroup
			, TCFunction<void (CStr const &_Info)> const &_fLogInfo
		)
	{
		auto &Settings = _pApplication->m_Settings;
		auto User = _pUniqueUserGroup->f_GetUser(Settings.m_RunAsUser);
		auto Group = _pUniqueUserGroup->f_GetGroup(Settings.m_RunAsGroup);
		auto fSetOwners = [&](CStr _Directory)
			{
				while (_Directory.f_GetLen() >= _ApplicationDir.f_GetLen())
				{
					if (!User.f_IsEmpty())
						CFile::fs_SetOwner(_Directory, User);
					if (!Group.f_IsEmpty())
						CFile::fs_SetGroup(_Directory, Group);
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

#ifdef DPlatformFamily_Linux
		if (auto pLowPortExetutables = _pApplication->m_LastTriedInstalledVersionInfo.m_ExtraInfo.f_GetMember("LowPortExecutables", EJSONType_Array))
		{
			for (auto &Executable : pLowPortExetutables->f_Array())
			{
				if (!Executable.f_IsString())
					continue;

				CStr ExecutablePath = _ApplicationDir / Executable.f_String();
				try
				{
					if (CFile::fs_FileExists(ExecutablePath))
						CProcessLaunch::fs_LaunchTool("setcap", {"cap_net_bind_service=ep", ExecutablePath});
					_fLogInfo("Setup low port executable support: setcap cap_net_bind_service=ep {}"_f << ExecutablePath);
				}
				catch (CException const &_Exception)
				{
					_fLogInfo("Failed to setup low port executable support running 'setcap cap_net_bind_service=ep {}': {}"_f << ExecutablePath << _Exception);
				}
			}
		}
#endif

		CFile::fs_CreateDirectory(_ApplicationDir + "/.home");
		CFile::fs_CreateDirectory(_ApplicationDir + "/.tmp");
		fSetOwners(_ApplicationDir + "/.home");
		fSetOwners(_ApplicationDir + "/.tmp");
		
		CFile::fs_SetAttributes(_ApplicationDir, gc_RootAttributes);
	}

	TCFuture<uint32> CAppManagerActor::fp_CommandLine_StopApplication(CEJSON _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		co_await mp_AppManagerInterface.m_pActor->self(&CAppManagerInterfaceImplementation::f_Stop, _Params["Name"].f_String());
		co_return 0;
	}
	
	TCFuture<uint32> CAppManagerActor::fp_CommandLine_StartApplication(CEJSON _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		co_await mp_AppManagerInterface.m_pActor->self(&CAppManagerInterfaceImplementation::f_Start, _Params["Name"].f_String());
		co_return 0;
	}
	
	TCFuture<uint32> CAppManagerActor::fp_CommandLine_RestartApplication(CEJSON _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		co_await mp_AppManagerInterface.m_pActor->self(&CAppManagerInterfaceImplementation::f_Restart, _Params["Name"].f_String());
		co_return 0;
	}
	
	TCFuture<void> CAppManagerActor::fp_ClearPreventLaunch(TCSharedPointer<CApplication> _pApplication)
	{
		if (!_pApplication->m_bPreventLaunch_User && !_pApplication->m_bPreventLaunch_Update && !_pApplication->m_bPreventLaunch_DelayAfterFailure)
			co_return {};
		
		_pApplication->m_bPreventLaunch_User = false;
		_pApplication->m_bPreventLaunch_Update = false;
		_pApplication->m_bPreventLaunch_DelayAfterFailure = false;
	
		co_await (fp_UpdateApplicationJSON(_pApplication) % "Failed to save application state");
		co_return {};
	}
	
	NConcurrency::TCFuture<void> CAppManagerActor::CAppManagerInterfaceImplementation::f_Start(NStr::CStr const &_Name)
	{
		auto pThis = m_pThis;
		auto Auditor = pThis->f_Auditor();

		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;

		Permissions["Command"] = {{"AppManager/CommandAll", "AppManager/Command/ApplicationStart"}};
		Permissions["App"] = {CPermissionQuery{"AppManager/AppAll", fg_Format("AppManager/App/{}", _Name)}.f_Description("Access application {} in AppManager"_f << _Name)};

		NContainer::TCMap<NStr::CStr, bool> HasPermissions = co_await
			(
			 	pThis->mp_Permissions.f_HasPermissions("Start application", Permissions) % "Permission denied starting application" % Auditor
			)
		;

		if (!HasPermissions["Command"])
			co_return Auditor.f_AccessDenied("(Application start, command)");

		if (!HasPermissions["App"])
			co_return Auditor.f_AccessDenied("(Application start, app name)");

		auto pOldApplication = pThis->mp_Applications.f_FindEqual(_Name);
		if (!pOldApplication)
			co_return Auditor.f_Exception(fg_Format("No such application '{}'", _Name));

		TCSharedPointer<CApplication> pApplication = *pOldApplication;

		if (pApplication->f_IsInProgress())
			co_return Auditor.f_Exception("Operation already in progress for application");
		if (pApplication->m_ProcessLaunch)
			co_return Auditor.f_Exception("Application already started");

		auto InProgressScope = pApplication->f_SetInProgress();

		co_await pThis->fp_ClearPreventLaunch(pApplication);

		auto LaunchAppResult = co_await pThis->fp_LaunchApp(pApplication, true).f_Wrap();

		if (!LaunchAppResult)
			co_return Auditor.f_Exception(fg_Format("Failed to launch app. Might retry periodically. {}", LaunchAppResult.f_GetExceptionStr()));

		if (LaunchAppResult->m_StartupError)
			co_return Auditor.f_Exception(fg_Format("Application startup failed: {}", LaunchAppResult->m_StartupError));

		Auditor.f_Info(fg_Format("Started '{}'", _Name));

		co_return {};
	}
	
	NConcurrency::TCFuture<void> CAppManagerActor::CAppManagerInterfaceImplementation::f_Stop(NStr::CStr const &_Name)
	{
		auto pThis = m_pThis;
		auto Auditor = pThis->f_Auditor();

		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;

		Permissions["Command"] = {{"AppManager/CommandAll", "AppManager/Command/ApplicationStop"}};
		Permissions["App"] = {CPermissionQuery{"AppManager/AppAll", fg_Format("AppManager/App/{}", _Name)}.f_Description("Access application {} in AppManager"_f << _Name)};

		NContainer::TCMap<NStr::CStr, bool> HasPermissions = co_await
			(
			 	pThis->mp_Permissions.f_HasPermissions("Stop application", Permissions) % "Permission denied stopping application" % Auditor
			)
		;

		if (!HasPermissions["Command"])
			co_return Auditor.f_AccessDenied("(Application stop, command)");

		if (!HasPermissions["App"])
			co_return Auditor.f_AccessDenied("(Application stop, app name)");

		auto pOldApplication = pThis->mp_Applications.f_FindEqual(_Name);
		if (!pOldApplication)
			co_return Auditor.f_Exception(fg_Format("No such application '{}'", _Name));

		TCSharedPointer<CApplication> pApplication = *pOldApplication;

		if (pApplication->f_IsInProgress())
			co_return Auditor.f_Exception("Operation already in progress for application");
		if (!pApplication->m_ProcessLaunch || pApplication->m_bStopped)
			co_return Auditor.f_Exception("Application already stopped");

		auto InProgressScope = pApplication->f_SetInProgress();

		TCAsyncResult<uint32> ExitStatus = co_await pApplication->f_Stop(EStopFlag_PreventLaunchUser).f_Wrap();

		NStr::CStr Error = pThis->fp_GetApplicationStopErrors(ExitStatus, pApplication->m_Name);

		if (!Error.f_IsEmpty())
			Auditor.f_Warning(Error);

		if (!ExitStatus)
			co_return Auditor.f_Exception("Failed to exit old application");

		if (pApplication->m_bDeleted)
			co_return Auditor.f_Exception("Application has been deleted, aborting");

		co_return {};
	}
	
	NConcurrency::TCFuture<void> CAppManagerActor::CAppManagerInterfaceImplementation::f_Restart(NStr::CStr const &_Name)
	{
		auto pThis = m_pThis;
		auto Auditor = pThis->f_Auditor();

		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;

		Permissions["Command"] = {{"AppManager/CommandAll", "AppManager/Command/ApplicationRestart"}};
		Permissions["App"] = {CPermissionQuery{"AppManager/AppAll", fg_Format("AppManager/App/{}", _Name)}.f_Description("Access application {} in AppManager"_f << _Name)};

		NContainer::TCMap<NStr::CStr, bool> HasPermissions = co_await
			(
			 	pThis->mp_Permissions.f_HasPermissions("Restart application", Permissions) % "Permission denied restarting application" % Auditor
			)
		;

		if (!HasPermissions["Command"])
			co_return Auditor.f_AccessDenied("(Application restart, command)");

		if (!HasPermissions["App"])
			co_return Auditor.f_AccessDenied("(Application restart, app name)");

		auto pOldApplication = pThis->mp_Applications.f_FindEqual(_Name);
		if (!pOldApplication)
			co_return Auditor.f_Exception(fg_Format("No such application '{}'", _Name));

		TCSharedPointer<CApplication> pApplication = *pOldApplication;

		if (pApplication->f_IsInProgress())
			co_return Auditor.f_Exception("Operation already in progress for application");

		auto InProgressScope = pApplication->f_SetInProgress();

		TCAsyncResult<uint32> ExitStatus = co_await pApplication->f_Stop(EStopFlag_None).f_Wrap();

		NStr::CStr Error = pThis->fp_GetApplicationStopErrors(ExitStatus, pApplication->m_Name);

		if (!Error.f_IsEmpty())
			Auditor.f_Warning(Error);

		if (!ExitStatus)
			co_return Auditor.f_Exception("Failed to exit old application");

		if (pApplication->m_bDeleted)
			co_return Auditor.f_Exception("Application has been deleted, aborting");

		co_await pThis->fp_ClearPreventLaunch(pApplication);

		TCAsyncResult<CAppLaunchResult> Result = co_await pThis->fp_LaunchApp(pApplication, true).f_Wrap();

		if (!Result)
			co_return Auditor.f_Exception(fg_Format("Failed to launch app. Might retry periodically. {}", Result.f_GetExceptionStr()));

		if (Result->m_StartupError)
			co_return Auditor.f_Exception(fg_Format("Application startup failed: {}", Result->m_StartupError));

		Auditor.f_Info(fg_Format("Restarted '{}'", _Name));
		co_return {};
	}
}
