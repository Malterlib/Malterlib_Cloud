// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Cryptography/RandomID>
#include <Mib/CommandLine/TableRenderer>
#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud::NAppManager
{
	CAppManagerInterface::CApplicationInfo CAppManagerActor::fp_GetApplicationInfo(CApplication const &_Application)
	{
		auto &Settings = _Application.m_Settings;

		CAppManagerInterface::CApplicationInfo OutApplication;

		OutApplication.m_Status = _Application.m_LaunchStatus;
		OutApplication.m_StatusSeverity = _Application.m_LaunchStatusSeverity;

		OutApplication.m_EncryptionStorage = Settings.m_EncryptionStorage;
		OutApplication.m_EncryptionFileSystem = Settings.m_EncryptionFileSystem;

		OutApplication.m_ParentApplication = Settings.m_ParentApplication;

		OutApplication.m_Version = _Application.m_LastInstalledVersion;
		OutApplication.m_VersionInfo = _Application.m_LastInstalledVersionInfo;

		if
			(
				fp_VersionIsNewer
				(
					_Application.m_LastTriedInstalledVersion
					, _Application.m_LastTriedInstalledVersionInfo
					, OutApplication.m_Version
					, OutApplication.m_VersionInfo
					, _Application.m_Settings.m_UpdateTags
					, _Application.m_Settings.m_UpdateBranches
				)
			)
		{
			OutApplication.m_FailedVersion = _Application.m_LastTriedInstalledVersion;
			OutApplication.m_FailedVersionInfo = _Application.m_LastTriedInstalledVersionInfo;
			OutApplication.m_FailedVersionError = _Application.m_LastTriedInstalledVersionError;
		}

		OutApplication.m_WantVersion = _Application.m_WantVersion;
		OutApplication.m_WantVersionInfo = _Application.m_WantVersionInfo;

		OutApplication.m_NewestUnconditionalVersion = _Application.m_NewestUnconditionalVersion;
		OutApplication.m_NewestUnconditionalVersionInfo = _Application.m_NewestUnconditionalVersionInfo;

		OutApplication.m_VersionManagerApplication = Settings.m_VersionManagerApplication;

		OutApplication.m_Executable = Settings.m_Executable;
		OutApplication.m_Parameters = Settings.m_ExecutableParameters;
		OutApplication.m_RunAsUser = Settings.m_RunAsUser;
		OutApplication.m_RunAsGroup = Settings.m_RunAsGroup;
		OutApplication.m_bRunAsUserHasShell = Settings.m_bRunAsUserHasShell;

		OutApplication.m_Backup_IncludeWildcards = Settings.m_Backup_IncludeWildcards;
		OutApplication.m_Backup_ExcludeWildcards = Settings.m_Backup_ExcludeWildcards;
		OutApplication.m_Backup_AddSyncFlagsWildcards = Settings.m_Backup_AddSyncFlagsWildcards;
		OutApplication.m_Backup_RemoveSyncFlagsWildcards = Settings.m_Backup_RemoveSyncFlagsWildcards;
		OutApplication.m_Backup_NewBackupInterval = Settings.m_Backup_NewBackupInterval;

		OutApplication.m_bAutoUpdate = Settings.m_bAutoUpdate;
		OutApplication.m_UpdateTags = Settings.m_UpdateTags;
		OutApplication.m_UpdateBranches = Settings.m_UpdateBranches;

		OutApplication.m_UpdateScriptPreUpdate = Settings.m_UpdateScripts.m_PreUpdate;
		OutApplication.m_UpdateScriptPostUpdate = Settings.m_UpdateScripts.m_PostUpdate;
		OutApplication.m_UpdateScriptPostLaunch = Settings.m_UpdateScripts.m_PostLaunch;
		OutApplication.m_UpdateScriptOnError = Settings.m_UpdateScripts.m_OnError;

		OutApplication.m_bSelfUpdateSource = Settings.m_bSelfUpdateSource;

		OutApplication.m_UpdateGroup = Settings.m_UpdateGroup;

		OutApplication.m_bDistributedApp = Settings.m_bDistributedApp;

		OutApplication.m_Dependencies = Settings.m_Dependencies;

		OutApplication.m_bStopOnDependencyFailure = Settings.m_bStopOnDependencyFailure;

		OutApplication.m_bBackupEnabled = Settings.m_bBackupEnabled;
		OutApplication.m_bLaunchInProcess = Settings.m_bLaunchInProcess;

		return OutApplication;
	}

	auto CAppManagerActor::CAppManagerInterfaceImplementation::f_GetInstalled() -> TCFuture<TCMap<CStr, CApplicationInfo>>
	{
		auto pThis = m_pThis;
		auto Auditor = pThis->f_Auditor();

		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;

		Permissions["//Command//"] = {{"AppManager/CommandAll", "AppManager/Command/ApplicationEnum"}};

		for (auto &pApplication : m_pThis->mp_Applications)
		{
			auto &Application = *pApplication;

			Permissions[Application.m_Name]
				= {CPermissionQuery{"AppManager/AppAll", fg_Format("AppManager/App/{}", Application.m_Name)}.f_Description("Access application {} in AppManager"_f << Application.m_Name)}
			;
		}

		auto HasPermissions = co_await
			(
			 	pThis->mp_Permissions.f_HasPermissions("Enumerate Apps in AppManager", Permissions) % "Permission denied enumerating installed applications" % Auditor
			)
		;

		if (!HasPermissions["//Command//"])
			co_return Auditor.f_AccessDenied("(Application enum)");

		TCMap<CStr, CApplicationInfo> OutputApplications;
		for (auto &pApplication : pThis->mp_Applications)
		{
			auto &Application = *pApplication;

			auto pHasPermission = HasPermissions.f_FindEqual(Application.m_Name);
			if (!pHasPermission || !*pHasPermission)
				continue;

			OutputApplications[Application.m_Name] = pThis->fp_GetApplicationInfo(Application);
		}

		Auditor.f_Info("Enum applications");
		co_return fg_Move(OutputApplications);
	}

	auto CAppManagerActor::CAppManagerInterfaceImplementation::f_GetAvailableVersions(CStr const &_Application) -> TCFuture<CVersionsAvailableForUpdate>
	{
		auto pThis = m_pThis;
		auto Auditor = pThis->f_Auditor();

		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;

		Permissions["//Command//"] = {{"AppManager/CommandAll", "AppManager/Command/VersionEnum"}};

		for (auto &Application : m_pThis->mp_VersionManagerApplications)
		{
			auto const &Name = Application.f_GetApplicationName();
			if (!_Application.f_IsEmpty() && Name != _Application)
				continue;

			Permissions[Name] = {CPermissionQuery{"AppManager/VersionAppAll", "AppManager/VersionApp/{}"_f << Name}.f_Description("Get versions of application {} in AppManager"_f << Name)};
		}

		NContainer::TCMap<NStr::CStr, bool> HasPermissions = co_await
			(
			 	pThis->mp_Permissions.f_HasPermissions("Enumerate versions in AppManager", Permissions) % "Permission denied enumerating versions" % Auditor
			)
		;

		if (!HasPermissions["//Command//"])
			co_return Auditor.f_AccessDenied("(Versions available for update)");

		TCMap<CStr, TCVector<CApplicationVersion>> Versions;

		for (auto &Application : pThis->mp_VersionManagerApplications)
		{
			auto const &Name = Application.f_GetApplicationName();
			if (!_Application.f_IsEmpty() && Name != _Application)
				continue;

			auto pHasPermission = HasPermissions.f_FindEqual(Name);
			if (!pHasPermission || !*pHasPermission)
				continue;

			auto &OutVersions = Versions[Name];
			for (auto &Version : Application.m_VersionsByTime)
			{
				auto &OutVersion = OutVersions.f_Insert();
				OutVersion.m_VersionID = Version.f_GetVersionID();
				OutVersion.m_VersionInfo = Version.m_VersionInfo;
			}
		}

		Auditor.f_Info("Enum versions available for update");
		co_return fg_Move(Versions);
	}

	TCFuture<uint32> CAppManagerActor::fp_CommandLine_EnumApplications(CEJSON _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		bool bVerbose = _Params["Verbose"].f_Boolean();
		bool bExtraVerbose = _Params["ExtraVerbose"].f_Boolean();
		CStr Name = _Params["Name"].f_String();

		auto AnsiEncoding = _pCommandLine->f_AnsiEncoding();
		CTableRenderHelper TableRenderer = _pCommandLine->f_TableRenderer();
		TableRenderer.f_AddHeadings("Application", "Update", "Version", "Status", "Other", "Backup", "Script", "Extra Info");

		TCMap<CStr, CAppManagerInterface::CApplicationInfo> ApplicationInfo = co_await mp_AppManagerInterface.m_Actor(&CAppManagerInterfaceImplementation::f_GetInstalled);
		for (auto &Application : ApplicationInfo)
		{
			auto &ApplicationName = ApplicationInfo.fs_GetKey(Application);
			if (!Name.f_IsEmpty() && ApplicationName != Name)
				continue;

			auto fSyntaxHighlight = [&](auto const &_Value)
				{
					return CEJSON(_Value).f_ToStringColored(_pCommandLine->m_AnsiFlags, nullptr, EJSONDialectFlag_AllowUndefined);
				}
			;

			auto fAddProperty = [&](CStr &o_String, CStr const &_Property, auto const &_Value)
				{
					o_String += "{}{}:{} "_f << AnsiEncoding.f_Prompt() << _Property << AnsiEncoding.f_Default();
					o_String += CStr::fs_ToStr(_Value);
					o_String += "\n";
				}
			;

			auto fAddHeading = [&](CStr &o_String, CStr const &_Heading)
				{
					if (!o_String.f_IsEmpty())
						o_String += "\n";
					o_String += "{}{}{}\n"_f << AnsiEncoding.f_Bold() << _Heading << AnsiEncoding.f_Default();
				}
			;

			CStr Other;

			{
				CStr Type = "App";
				if (Application.m_bLaunchInProcess)
					Type = "In Process Distributed App";
				else if (Application.m_bSelfUpdateSource)
					Type = "Self Update Source";
				else if (Application.m_Executable.f_IsEmpty())
					Type = "Container";
				else if (Application.m_bDistributedApp)
					Type = "Distributed App";

				fAddProperty(Other, "Type", Type);
			}

			fAddHeading(Other, "Encryption");
			fAddProperty(Other, "Storage", Application.m_EncryptionStorage);
			fAddProperty(Other, "File system", Application.m_EncryptionFileSystem);

			fAddHeading(Other, "Depend");
			fAddProperty(Other, "Parent application", Application.m_ParentApplication);
			fAddProperty(Other, "Dependencies", "{vs}"_f << Application.m_Dependencies);
			fAddProperty(Other, "Stop on dependency failure", fSyntaxHighlight(Application.m_bStopOnDependencyFailure));

			fAddHeading(Other, "Launch");
			fAddProperty(Other, "Executable", Application.m_Executable);
			fAddProperty(Other, "Parameters", "{vs}"_f << Application.m_Parameters);
			fAddProperty(Other, "User", Application.m_RunAsUser);
			fAddProperty(Other, "Group", Application.m_RunAsGroup);
			fAddProperty(Other, "User has shell", fSyntaxHighlight(Application.m_bRunAsUserHasShell));

			CStr Backup;
			fAddProperty(Backup, "Enabled", fSyntaxHighlight(Application.m_bBackupEnabled));
			fAddProperty(Backup, "Interval", "{} h"_f << CTimeSpanConvert(Application.m_Backup_NewBackupInterval).f_GetHoursFloat());

			fAddHeading(Backup, "Wildcards");
			fAddProperty(Backup, "Include", "{}"_f << Application.m_Backup_IncludeWildcards);
			fAddProperty(Backup, "Exclude", "{}"_f << Application.m_Backup_ExcludeWildcards);

			fAddHeading(Backup, "Sync Flags");
			fAddProperty(Backup, "Add", "{}"_f << Application.m_Backup_AddSyncFlagsWildcards);
			fAddProperty(Backup, "Remove", "{}"_f << Application.m_Backup_RemoveSyncFlagsWildcards);

			CStr Update = Application.m_bAutoUpdate ? "Auto Update" : "Manual Update";

			if (bExtraVerbose)
			{
				Update += "\n\n";
				fAddProperty(Update, "Tags", Application.m_UpdateTags);
				fAddProperty(Update, "Branches", Application.m_UpdateBranches);
				fAddProperty(Update, "Update Group", Application.m_UpdateGroup);
			}
			else if (bVerbose)
			{
				Update += "\n\n";
				fAddProperty(Update, "Tags", "{vs}"_f << Application.m_UpdateTags);
				fAddProperty(Update, "Branches", "{vs}"_f << Application.m_UpdateBranches);
				fAddProperty(Update, "Update Group", Application.m_UpdateGroup);
			}

			CStr Script;
			fAddProperty(Script, "Pre Update", Application.m_UpdateScriptPreUpdate);
			fAddProperty(Script, "Post Update", Application.m_UpdateScriptPostUpdate);
			fAddProperty(Script, "Post Launch Update", Application.m_UpdateScriptPostLaunch);
			fAddProperty(Script, "On Error Update", Application.m_UpdateScriptOnError);

			CStr Version;
			Version = "{}"_f << Application.m_Version;
			if (bExtraVerbose || bVerbose)
			{
				Version += "\n\n";
				fAddProperty(Version, "Application", Application.m_VersionManagerApplication);
				fAddProperty(Version, "Time", Application.m_VersionInfo.m_Time.f_ToLocal());
				fAddProperty(Version, "Config", Application.m_VersionInfo.m_Configuration);
				fAddProperty(Version, "Size", "{ns }"_f << Application.m_VersionInfo.m_nBytes);
				fAddProperty(Version, "Files", Application.m_VersionInfo.m_nFiles);
			}

			CStr Status;
			if (Application.m_StatusSeverity == CAppManagerInterface::EStatusSeverity_Error)
				Status = AnsiEncoding.f_StatusError(Application.m_Status);
			else if (Application.m_StatusSeverity == CAppManagerInterface::EStatusSeverity_Warning)
				Status = AnsiEncoding.f_StatusWarning(Application.m_Status);
			else
				Status = AnsiEncoding.f_StatusNormal(Application.m_Status);

			TableRenderer.f_AddRow
				(
				 	ApplicationName
				 	, Update
				 	, Version
				 	, Status
				 	, Other
				 	, Backup
				 	, Script
					, Application.m_VersionInfo.m_ExtraInfo.f_IsValid()
					? Application.m_VersionInfo.m_ExtraInfo.f_ToStringColored(_pCommandLine->m_AnsiFlags, "  ", EJSONDialectFlag_AllowUndefined)
					: CStr()
				)
			;
		}

		if (!bExtraVerbose)
		{
			TableRenderer.f_RemoveColumn(7);
			TableRenderer.f_RemoveColumn(6);
			TableRenderer.f_RemoveColumn(5);
			TableRenderer.f_RemoveColumn(4);
		}

		TableRenderer.f_Output(_Params);

		co_return 0;
	}

	TCFuture<uint32> CAppManagerActor::fp_CommandLine_ListAvailableVersions(CEJSON _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		bool bVerbose = _Params["Verbose"].f_Boolean();
		CAppManagerInterface::CVersionsAvailableForUpdate Results = co_await
			mp_AppManagerInterface.m_Actor(&CAppManagerInterfaceImplementation::f_GetAvailableVersions, _Params["Application"].f_String())
		;

		{
			bool bPlatformsChanged = false;
			for (auto &Platform : _Params["ExtraPlatforms"].f_Array())
			{
				if (mp_KnownPlatforms(Platform.f_String()).f_WasCreated())
					bPlatformsChanged = true;
			}

			if (bPlatformsChanged)
				co_await fp_VersionManagerResubscribeAll();
		}

		CTableRenderHelper TableRenderer = _pCommandLine->f_TableRenderer();
		TableRenderer.f_AddHeadings("Application", "Version", "Platform", "Config", "Time", "Size", "Files", "Tags", "Retry", "Extra Info");
		TableRenderer.f_SetOptions(CTableRenderHelper::EOption_Rounded | CTableRenderHelper::EOption_AvoidRowSeparators);
		TableRenderer.f_SetAlignRight(5);
		TableRenderer.f_SetAlignRight(6);
		TableRenderer.f_SetAlignRight(8);

		for (auto &Versions : Results)
		{
			auto &ApplicationName = Results.fs_GetKey(Versions);
			for (auto &Version : Versions)
			{
				TableRenderer.f_AddRow
					(
						ApplicationName
						, Version.m_VersionID.m_VersionID
						, Version.m_VersionID.m_Platform
						, Version.m_VersionInfo.m_Configuration
						, "{tc6}"_f << Version.m_VersionInfo.m_Time.f_ToLocal()
						, "{ns }"_f << Version.m_VersionInfo.m_nBytes
						, "{}"_f << Version.m_VersionInfo.m_nFiles
						, "{vs,vb}"_f << Version.m_VersionInfo.m_Tags
						, "{}"_f << Version.m_VersionInfo.m_RetrySequence
						, Version.m_VersionInfo.m_ExtraInfo.f_IsValid()
						? Version.m_VersionInfo.m_ExtraInfo.f_ToStringColored(_pCommandLine->m_AnsiFlags, "  ", EJSONDialectFlag_AllowUndefined)
						: CStr()
					)
				;
			}
		}

 		if (!bVerbose)
			TableRenderer.f_RemoveColumn(9);

		TableRenderer.f_Output(_Params);

		co_return 0;
	}

	TCFuture<uint32> CAppManagerActor::fp_CommandLine_VersionManagerList(CEJSON _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		CTableRenderHelper TableRenderer = _pCommandLine->f_TableRenderer();
		auto AnsiEncoding = _pCommandLine->f_AnsiEncoding();
		TableRenderer.f_AddHeadings("Host");
		TableRenderer.f_SetOptions(CTableRenderHelper::EOption_Rounded | CTableRenderHelper::EOption_AvoidRowSeparators);

		for (auto &Manager : mp_VersionManagers)
			TableRenderer.f_AddRow(Manager.m_HostInfo.m_HostInfo.f_GetDescColored(AnsiEncoding.f_Flags()));

		TableRenderer.f_Output(_Params);

		co_return 0;
	}

	TCFuture<uint32> CAppManagerActor::fp_CommandLine_CloudManagerList(CEJSON _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		CTableRenderHelper TableRenderer = _pCommandLine->f_TableRenderer();
		auto AnsiEncoding= _pCommandLine->f_AnsiEncoding();
		TableRenderer.f_AddHeadings("Host");
		TableRenderer.f_SetOptions(CTableRenderHelper::EOption_Rounded | CTableRenderHelper::EOption_AvoidRowSeparators);

		for (auto &Manager : mp_CloudManagers)
			TableRenderer.f_AddRow(Manager.m_HostInfo.m_HostInfo.f_GetDescColored(AnsiEncoding.f_Flags()));

		TableRenderer.f_Output(_Params);

		co_return 0;
	}
}
