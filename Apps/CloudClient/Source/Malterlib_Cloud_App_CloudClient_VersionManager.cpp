// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Cloud/VersionManager>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/AsyncDestroy>
#include <Mib/Encoding/JsonShortcuts>
#include <Mib/CommandLine/TableRenderer>

#include "Malterlib_Cloud_App_CloudClient.h"

#ifdef DPlatformFamily_Windows
#include <Mib/Core/PlatformSpecific/WindowsFilePath>
#endif

namespace NMib::NCloud::NCloudClient
{
	void CCloudClientAppActor::fp_VersionManager_RegisterCommands(CDistributedAppCommandLineSpecification::CSection _Section)
	{
		auto VersionManagerHost = "VersionManagerHost?"_o=
			{
				"Names"_o= _o["--host"]
				, "Default"_o= ""
				, "Description"_o= "Limit query to only specified host ID."
			}
		;
		auto IncludeHost = "IncludeHost?"_o=
			{
				"Names"_o= _o["--include-host"]
				, "Default"_o= false
				, "Description"_o= "Include version manager host in output.\n"
			}
		;
		auto HiddenCurrentDirectoryOption = "CurrentDirectory?"_o=
			{
				"Names"_o= _o[]
				, "Default"_o= CFile::fs_GetCurrentDirectory()
				, "Hidden"_o= true
				, "Description"_o= "Internal hidden option to forward current directory."
			}
		;

		_Section.f_RegisterCommand
			(
				{
					"Names"_o= _o["--version-manager-list-applications"]
					, "Description"_o= "List applications available on remote version managers."
					, "Options"_o=
					{
						VersionManagerHost
						, IncludeHost
						, CTableRenderHelper::fs_OutputTypeOption()
					}
				}
				, [this](CEJsonSorted &&_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_VersionManager_ListApplications(fg_Move(_Params), fg_Move(_pCommandLine));
				}
				, EDistributedAppCommandFlag_WaitForRemotes
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_o= _o["--version-manager-list-versions"]
					, "Description"_o= "List application versions available on remote version managers."
					, "Options"_o=
					{
						VersionManagerHost
						, IncludeHost
						, "Verbose?"_o=
						{
							"Names"_o= _o["--verbose", "-v"]
							, "Default"_o= false
							, "Description"_o= "Verbose output. Include custom JSON information.\n"
						}
						, CTableRenderHelper::fs_OutputTypeOption()
					}
					, "Parameters"_o=
					{
						"Application?"_o=
						{
							"Default"_o= ""
							, "Description"_o= "The application to list versions for.\n"
								"If left empty versions will be listed for all applications you have access to.\n"
						}
					}
				}
				, [this](CEJsonSorted &&_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_VersionManager_ListVersions(fg_Move(_Params), fg_Move(_pCommandLine));
				}
				, EDistributedAppCommandFlag_WaitForRemotes
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_o= _o["--version-manager-upload-version"]
					, "Description"_o= "Upload a version to remote version manager.\n"
					, "Options"_o=
					{
						"VersionManagerHost?"_o=
						{
							"Names"_o= _o["--host"]
							, "Type"_o= ""
							, "Description"_o= "The host ID of the host to upload the version to."
						}
						, "VersionManagerHosts?"_o=
						{
							"Names"_o= _o["--hosts"]
							, "Type"_o= _o[""]
							, "Description"_o= "The host IDs of the hosts to upload the version to."
						}
						, "SettingsFileFromPackage?"_o=
						{
							"Names"_o= _o["--settings-file-from-package"]
							, "Default"_o= true
							, "Description"_o= "If the uploaded file is a tar.gz file, look inside it for VersionInfo.json and use as settings file.\n"
							"If --settings-file is specified that will override the settings file inside the uploaded package.\n"
							"Settings in the settings file is overridden by settings specified on the command line.\n"
						}
						, "SettingsFile?"_o=
						{
							"Names"_o= _o["--settings-file"]
							, "Default"_o= ""
							, "Description"_o= "The JSON file to read settings from.\n"
							"Settings in the settings file is overridden by settings specified on the command line.\n"
							"The format template for the settings file is:\n" +
							CEJsonOrdered
							{
								"Application"_o= "AppName"
								, "Version"_o= "Branch/1.0.1"
								, "Platform"_o= DMalterlibCloudPlatform
								, "Configuration"_o= DMibStringize(DConfig)
								, "ExtraInfo"_o=
								{
									"Executable"_o= "ExecutableName"
									, "ExecutableParams"_o= _o["--daemon-run-standalone", "--debug"]
									, "RunAsUser"_o= "ApplicationSpecificUser"
									, "RunAsGroup"_o= "ApplicationSpecificGroup"
									, "RunAsUserHasShell"_o= false
									, "DistributedApp"_o= true
								}
							}
							.f_ToStringColored(CCommandLineDefaults::fs_ColorAnsiFlagsDefault(), "    ").f_Replace("\r\n", "\r").f_Replace("\n", "\r")
						}
						, "Application?"_o=
						{
							"Names"_o= _o["--application"]
							, "Type"_o= ""
							, "Description"_o= "The application to upload a version to."
						}
						, "Version?"_o=
						{
							"Names"_o= _o["--version"]
							, "Type"_o= ""
							, "Description"_o= "The version to upload.\n"
								"This is in the format 'Branch/Major.Minor.Patch' as displayed in the output from --version-manager-list-versions.\n"
						}
						, "Platform?"_o=
						{
							"Names"_o= _o["--platform"]
							, "Type"_o= ""
							, "Description"_o= "The platform of the version to upload.\n"
						}
						, "Configuration?"_o=
						{
							"Names"_o= _o["--configuration"]
							, "Type"_o= ""
							, "Description"_o= "The configuration for this build. Could be for example Debug or Release.\n"
						}
						, "ExtraInfo?"_o=
						{
							"Names"_o= _o["--info"]
							, "Type"_o= _o={}
							, "Description"_o= "EJSON formatted extra information.\n"
						}
						, "Tags?"_o=
						{
							"Names"_o= _o["--tags"]
							, "Default"_o= _o[]
							, "Type"_o= _o[""]
							, "Description"_o= "The tags to apply to the version.\n"
						}
						, "Time?"_o=
						{
							"Names"_o= _o["--time"]
							, "Default"_o= CTime()
							, "Description"_o= "The time for this version.\n"
								"By default the time will be deduced from the modification time of the directory or file uploaded.\n"
						}
						, "Force?"_o=
						{
							"Names"_o= _o["--force"]
							, "Default"_o= false
							, "Description"_o= "Force upload even if version already exists.\n"
						}
						, "TransferQueueSize?"_o=
						{
							"Names"_o= _o["--queue-size"]
							, "Default"_o= int64(NFile::gc_IdealNetworkQueueSize)
							, "Description"_o= "The amount of data to keep in flight while uploading."
						}
						, HiddenCurrentDirectoryOption
					}
					, "Parameters"_o=
					{
						"Source"_o=
						{
							"Default"_o= ""
							, "Description"_o= "The file or directory to upload.\n"
						}
					}
				}
				, [this](CEJsonSorted &&_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_VersionManager_UploadVersion(fg_Move(_Params), fg_Move(_pCommandLine));
				}
				, EDistributedAppCommandFlag_WaitForRemotes
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_o= _o["--version-manager-change-tags"]
					, "Description"_o= "Upload a version to remote version manager.\n"
					, "Options"_o=
					{
						"VersionManagerHost?"_o=
						{
							"Names"_o= _o["--host"]
							, "Default"_o= ""
							, "Description"_o= "The host ID of the host to change tags for."
						}
						, "Application"_o=
						{
							"Names"_o= _o["--application"]
							, "Type"_o= ""
							, "Description"_o= "Change tags for this application."
						}
						, "Version"_o=
						{
							"Names"_o= _o["--version"]
							, "Type"_o= ""
							, "Description"_o= "Change tags for this version.\n"
								"This is in the format 'Branch/Major.Minor.Patch' as displayed in the output from --version-manager-list-versions.\n"
						}
						, "Platform?"_o=
						{
							"Names"_o= _o["--platform"]
							, "Default"_o= ""
							, "Description"_o= "The platform to change tags for. Leave empty to change all platforms.\n"
						}
						, "AddTags?"_o=
						{
							"Names"_o= _o["--add"]
							, "Default"_o= _o[]
							, "Type"_o= _o[""]
							, "Description"_o= "Add these tags.\n"
						}
						, "RemoveTags?"_o=
						{
							"Names"_o= _o["--remove"]
							, "Default"_o= _o[]
							, "Type"_o= _o[""]
							, "Description"_o= "Remove these tags.\n"
						}
						, "RetryUpgrade?"_o=
						{
							"Names"_o= _o["--retry-upgrade"]
							, "Default"_o= false
							, "Description"_o= "Increase the retry sequence for this version. AppManagers will retry upgrade if it previously failed.\n"
						}
					}
				}
				, [this](CEJsonSorted &&_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_VersionManager_ChangeTags(fg_Move(_Params), fg_Move(_pCommandLine));
				}
				, EDistributedAppCommandFlag_WaitForRemotes
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_o= _o["--version-manager-download-version"]
					, "Description"_o= "Download a version from remote version manager.\n"
					, "Options"_o=
					{
						"VersionManagerHost?"_o=
						{
							"Names"_o= _o["--host"]
							, "Default"_o= ""
							, "Description"_o= "The host ID of the host to download the version from."
						}
						, "Application"_o=
						{
							"Names"_o= _o["--application"]
							, "Type"_o= ""
							, "Description"_o= "The application to download a version from."
						}
						, "Version"_o=
						{
							"Names"_o= _o["--version"]
							, "Type"_o= ""
							, "Description"_o= "The version to download.\n"
								"This is in the format 'Branch/Major.Minor.Patch' as displayed in the output from --version-manager-list-versions.\n"
						}
						, "Platform?"_o=
						{
							"Names"_o= _o["--platform"]
							, "Default"_o= DMalterlibCloudPlatform
							, "Description"_o= "The platform of the version to download.\n"
						}
						, "TransferQueueSize?"_o=
						{
							"Names"_o= _o["--queue-size"]
							, "Default"_o= int64(NFile::gc_IdealNetworkQueueSize)
							, "Description"_o= "The amount of data to keep in flight while downloading."
						}
						, HiddenCurrentDirectoryOption
					}
					, "Parameters"_o=
					{
						"DestinationDirectory?"_o=
						{
							"Default"_o= mp_State.m_RootDirectory
							, "Description"_o= "The directory to download the version to."
						}
					}
				}
				, [this](CEJsonSorted &&_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_VersionManager_DownloadVersion(fg_Move(_Params), fg_Move(_pCommandLine));
				}
				, EDistributedAppCommandFlag_WaitForRemotes
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_o= _o["--version-manager-copy-versions"]
					, "Description"_o= "Copy versions between version managers."
					, "Options"_o=
					{
						"SourceHost"_o=
						{
							"Names"_o= _o["--source-host"]
							, "Type"_o= ""
							, "Description"_o= "Host ID of source version manager."
						}
						, "DestHost"_o=
						{
							"Names"_o= _o["--dest-host"]
							, "Type"_o= ""
							, "Description"_o= "Host ID of destination version manager."
						}
						, "LatestOnly?"_o=
						{
							"Names"_o= _o["--latest-only"]
							, "Default"_o= false
							, "Description"_o= "Only copy the latest version of each application (all platforms at that version)."
						}
						, "Platforms?"_o=
						{
							"Names"_o= _o["--platforms"]
							, "Default"_o= _o[]
							, "Type"_o= _o[""]
							, "Description"_o= "Filter to specific platforms (supports wildcards)."
						}
						, "Applications?"_o=
						{
							"Names"_o= _o["--applications"]
							, "Default"_o= _o[]
							, "Type"_o= _o[""]
							, "Description"_o= "Filter to specific applications (supports wildcards)."
						}
						, "Versions?"_o=
						{
							"Names"_o= _o["--versions"]
							, "Default"_o= _o[]
							, "Type"_o= _o[""]
							, "Description"_o= "Filter to specific versions (supports wildcards, format: 'Branch/Major.Minor.Patch')."
						}
						, "Tags?"_o=
						{
							"Names"_o= _o["--tags"]
							, "Default"_o= _o[]
							, "Type"_o= _o[""]
							, "Description"_o= "Filter to versions that have specific tags (supports wildcards)."
						}
						, "CopyTags?"_o=
						{
							"Names"_o= _o["--copy-tags"]
							, "Default"_o= _o[]
							, "Type"_o= _o[""]
							, "Description"_o= "Tags to copy to destination. Format: 'Tag' or 'SourceTag=DestTag' for renaming."
						}
						, "Pretend?"_o=
						{
							"Names"_o= _o["--pretend"]
							, "Default"_o= true
							, "Description"_o= "Preview what would be copied without transferring."
						}
						, "Force?"_o=
						{
							"Names"_o= _o["--force"]
							, "Default"_o= false
							, "Description"_o= "Overwrite versions that already exist on destination."
						}
						, "TransferQueueSize?"_o=
						{
							"Names"_o= _o["--queue-size"]
							, "Default"_o= int64(NFile::gc_IdealNetworkQueueSize)
							, "Description"_o= "Amount of data to keep in flight while transferring."
						}
						, CTableRenderHelper::fs_OutputTypeOption()
					}
				}
				, [this](CEJsonSorted &&_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_VersionManager_CopyVersions(fg_Move(_Params), fg_Move(_pCommandLine));
				}
				, EDistributedAppCommandFlag_WaitForRemotes
			)
		;
	}

	TCFuture<void> CCloudClientAppActor::fp_VersionManager_SubscribeToServers()
	{
		auto CheckDestroy = co_await fp_CheckStoppedOrDestroyedOnResume();

		if (!mp_VersionManagers.f_IsEmpty())
			co_return {};
		DMibLogWithCategory(Malterlib/Cloud/CloudClient, Info, "Subscribing to version managers");

		auto Subscription = co_await mp_State.m_TrustManager->f_SubscribeTrustedActors<NCloud::CVersionManager>().f_Wrap();

		if (!Subscription)
		{
			DMibLogWithCategory(Malterlib/Cloud/CloudClient, Error, "Failed to subscribe to version managers: {}", Subscription.f_GetExceptionStr());
			co_return Subscription.f_GetException();
		}

		mp_VersionManagers = fg_Move(*Subscription);

		if (mp_VersionManagers.m_Actors.f_IsEmpty())
			co_return DMibErrorInstance("Not connected to any version managers, or they are not trusted for 'com.malterlib/Cloud/VersionManager' namespace");

		co_return {};
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_VersionManager_ListApplications(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		auto CheckDestroy = co_await fp_CheckStoppedOrDestroyedOnResume();

		CStr Host = _Params["VersionManagerHost"].f_String();
		bool bIncludeHost = _Params["IncludeHost"].f_Boolean();

		co_await fp_VersionManager_SubscribeToServers().f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for version managers");

		TCFutureMap<CHostInfo, CVersionManager::CListApplications::CResult> Applications;

		for (auto &TrustedActor : mp_VersionManagers.m_Actors)
		{
			if (!Host.f_IsEmpty() && TrustedActor.m_TrustInfo.m_HostInfo.m_HostID != Host)
				continue;
			CVersionManager::CListApplications Command;
			TrustedActor.m_Actor.f_CallActor(&CVersionManager::f_ListApplications)(fg_Move(Command))
				.f_Timeout(mp_Timeout, "Timed out waiting for version manager to reply")
				> Applications[TrustedActor.m_TrustInfo.m_HostInfo]
			;
		}

		auto AnsiEncoding = _pCommandLine->f_AnsiEncoding();
		CTableRenderHelper TableRenderer = _pCommandLine->f_TableRenderer();
		TableRenderer.f_AddHeadings("Host", "Application");

		auto Results = co_await fg_AllDoneWrapped(Applications);
		for (auto &Result : Results)
		{
			auto &HostInfo = Results.fs_GetKey(Result);
			CStr HostDescription = HostInfo.f_GetDescColored(_pCommandLine->m_AnsiFlags);
			if (!Result)
			{
				*_pCommandLine %= "{}Failed getting applications for host{} '{}': {}\n"_f
					<< AnsiEncoding.f_StatusError()
					<< AnsiEncoding.f_Default()
					<< HostInfo.f_GetDescColored(_pCommandLine->m_AnsiFlags)
					<< Result.f_GetExceptionStr()
				;
				continue;
			}
			for (auto &Application : Result->m_Applications)
				TableRenderer.f_AddRow(HostDescription, Application);
		}

		if (!bIncludeHost)
			TableRenderer.f_RemoveColumn(0);

		TableRenderer.f_Output(_Params);

		co_return 0;
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_VersionManager_ListVersions(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		auto CheckDestroy = co_await fp_CheckStoppedOrDestroyedOnResume();

		CStr Host = _Params["VersionManagerHost"].f_String();
		CStr Application = _Params["Application"].f_String();
		bool bVerbose = _Params["Verbose"].f_Boolean();
		bool bIncludeHost = _Params["IncludeHost"].f_Boolean();

		co_await fp_VersionManager_SubscribeToServers().f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for version managers");
		TCFutureMap<CHostInfo, CVersionManager::CListVersions::CResult> Versions;

		for (auto &TrustedVersionManager : mp_VersionManagers.m_Actors)
		{
			if (!Host.f_IsEmpty() && TrustedVersionManager.m_TrustInfo.m_HostInfo.m_HostID != Host)
				continue;
			CVersionManager::CListVersions Options;
			Options.m_ForApplication = Application;
			TrustedVersionManager.m_Actor.f_CallActor(&CVersionManager::f_ListVersions)(fg_Move(Options))
				.f_Timeout(mp_Timeout, "Timed out waiting for version manager to reply")
				> Versions[TrustedVersionManager.m_TrustInfo.m_HostInfo]
			;
		}

		auto Results = co_await fg_AllDoneWrapped(Versions);

		auto AnsiEncoding = _pCommandLine->f_AnsiEncoding();
		CTableRenderHelper TableRenderer = _pCommandLine->f_TableRenderer();
		TableRenderer.f_SetOptions(CTableRenderHelper::EOption_Rounded | CTableRenderHelper::EOption_AvoidRowSeparators);
		TableRenderer.f_AddHeadings("Host", "Application", "Version", "Platforms", "Config", "Time", "Tags", "Retry", "Size", "Files", "Extra");

		struct CRow
		{
			auto operator <=> (CRow const &_Right) const
			{
				return m_VersionInformation.m_Time <=> _Right.m_VersionInformation.m_Time;
			}

			CHostInfo m_HostInfo;
			CStr m_Application;
			CVersionManager::CVersionIDAndPlatform m_VersionID;
			CVersionManager::CVersionInformation m_VersionInformation;
		};

		struct CUniqueVersionKey
		{
			auto operator <=> (CUniqueVersionKey const &_Right) const = default;

			CStr m_Application;
			CVersionManager::CVersionID m_VersionID;
		};

		struct CVersionPlatforms
		{
			auto operator <=> (CVersionPlatforms const &_Right) const
			{
				return m_EndTime <=> _Right.m_EndTime;
			}

			TCVector<CRow> m_Rows;
			CTime m_StartTime = CTime::fs_EndOfTime();
			CTime m_EndTime = CTime::fs_StartOfTime();
		};

		TCMap<CUniqueVersionKey, CVersionPlatforms> VersionMap;

		for (auto &Result : Results)
		{
			auto &HostInfo = Results.fs_GetKey(Result);
			if (!Result)
			{
				*_pCommandLine %= "{}Failed getting versions for host{} '{}': {}\n"_f
					<< AnsiEncoding.f_StatusError()
					<< AnsiEncoding.f_Default()
					<< HostInfo.f_GetDescColored(_pCommandLine->m_AnsiFlags)
					<< Result.f_GetExceptionStr()
				;
				continue;
			}
			for (auto &Versions : Result->m_Versions)
			{
				auto &Application = Result->m_Versions.fs_GetKey(Versions);
				for (auto &Version : Versions)
				{
					auto &VersionID = Versions.fs_GetKey(Version);
					CUniqueVersionKey Key{Application, VersionID.m_VersionID};
					auto &VersionPlatforms = VersionMap[Key];
					VersionPlatforms.m_Rows.f_Insert(CRow{HostInfo, Application, VersionID, Version});
					VersionPlatforms.m_StartTime = fg_Min(VersionPlatforms.m_StartTime, Version.m_Time);
					VersionPlatforms.m_EndTime = fg_Max(VersionPlatforms.m_EndTime, Version.m_Time);
				}
			}
		}

		TCVector<CVersionPlatforms> VersionPlatforms;

		for (auto &Version : VersionMap)
		{
			Version.m_Rows.f_Sort();
			VersionPlatforms.f_Insert(Version);
		}

		VersionPlatforms.f_Sort();

		for (auto &Version : VersionPlatforms)
		{
			for (auto &Row : Version.m_Rows)
			{
				auto &HostInfo = Row.m_HostInfo;
				auto &Application = Row.m_Application;
				auto &VersionID = Row.m_VersionID;
				auto &Version = Row.m_VersionInformation;
				auto HostDescription = HostInfo.f_GetDescColored(_pCommandLine->m_AnsiFlags);

				CStr ExtraInfo;
				if (bVerbose && Version.m_ExtraInfo.f_IsValid())
					ExtraInfo = Version.m_ExtraInfo.f_ToStringColored(_pCommandLine->m_AnsiFlags, "  ", EJsonDialectFlag_AllowUndefined);

				TableRenderer.f_AddRow
					(
						HostDescription
						, Application
						, VersionID.m_VersionID
						, VersionID.m_Platform
						, Version.m_Configuration
						, "{tc6}"_f << Version.m_Time.f_ToLocal()
						, "{vs,vb,a-}"_f << Version.m_Tags
						, Version.m_RetrySequence
						, "{ns }"_f << Version.m_nBytes
						, Version.m_nFiles
						, ExtraInfo
					)
				;
			}
			TableRenderer.f_ForceRowSeparator();
		}

		if (!bVerbose)
			TableRenderer.f_RemoveColumn(10);

		if (!bIncludeHost)
			TableRenderer.f_RemoveColumn(0);

		TableRenderer.f_Output(_Params);

		co_return 0;
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_VersionManager_UploadVersion(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		auto CheckDestroy = co_await fp_CheckStoppedOrDestroyedOnResume();

		auto AnsiFlags = _pCommandLine->m_AnsiFlags;

		CStr Source = CFile::fs_GetFullPath(_Params["Source"].f_String(), _Params["CurrentDirectory"].f_String());
		if (Source.f_IsEmpty())
			co_return DMibErrorInstance("Source must be specified");

		CStr SettingsFile = _Params["SettingsFile"].f_String();
		if (!SettingsFile.f_IsEmpty())
			SettingsFile = CFile::fs_GetFullPath(SettingsFile, _Params["CurrentDirectory"].f_String());

		TCVariant<CStr, CEJsonSorted> SettingsFileOrSettings;
		if (!SettingsFile.f_IsEmpty())
			SettingsFileOrSettings = SettingsFile;
		else if (_Params["SettingsFileFromPackage"].f_Boolean() && (Source.f_EndsWith(".tar.gz") || Source.f_EndsWith(".tar")))
		{
			// tar -xOf AppManager.tar.gz VersionInfo.json

			TCActor<CProcessLaunchActor> LaunchActor = fg_Construct();
			auto DestroyLaunch = co_await fg_AsyncDestroy(LaunchActor);

			CProcessLaunchActor::CSimpleLaunch Launch
				{
					CFile::fs_GetProgramDirectory() / "bin/bsdtar"
					,
					{
						"-xqOf"
						, Source
						, "*VersionInfo.json"
					}
					, CFile::fs_GetPath(Source)
					, CProcessLaunchActor::ESimpleLaunchFlag_GenerateExceptionOnNonZeroExitCode
				}
			;

			auto LaunchResult = co_await LaunchActor(&CProcessLaunchActor::f_LaunchSimple, fg_Move(Launch)).f_Wrap();

			CEJsonSorted VersionInfo = EJsonType_Object;
			if (LaunchResult)
			{
				try
				{
					VersionInfo = CEJsonSorted::fs_FromString(LaunchResult->f_GetStdOut());
				}
				catch (CException const &_Exception)
				{
					*_pCommandLine %= "Failed to parse version info from VersionInfo.json in package (Disable with --no-settings-file-from-package): {}"_f << _Exception;
					co_return 1;
				}
			}
			else
			{
				*_pCommandLine %= "Failed to extract version info from package (Disable with --no-settings-file-from-package): {}"_f << LaunchResult.f_GetExceptionStr();
				co_return 1;
			}

			SettingsFileOrSettings = VersionInfo;
		}
		else
			SettingsFileOrSettings = CStr{};

		CEJsonSorted Settings;
		if (SettingsFileOrSettings.f_IsOfType<CEJsonSorted>())
			Settings = SettingsFileOrSettings.f_Get<1>();
		else
		{
			CStr SettingsFile = SettingsFileOrSettings.f_Get<0>();
			if (!SettingsFile.f_IsEmpty())
			{
				auto BlockingActorCheckout = fg_BlockingActor();
				auto SettingsResult = co_await
					(
						g_Dispatch(BlockingActorCheckout) / [SettingsFile]
						{
							return CEJsonSorted::fs_FromString(CFile::fs_ReadStringFromFile(SettingsFile), SettingsFile);
						}
					)
					.f_Wrap();
				;

				if (!SettingsResult)
					co_return DMibErrorInstance(fg_Format("Error reading settings file: {}", SettingsResult.f_GetExceptionStr()));
				if (!SettingsResult->f_IsObject())
					co_return DMibErrorInstance("Settings file does not contain a valid JSON object");
				Settings = fg_Move(*SettingsResult);
			}
			else
				Settings = EJsonType_Object;
		}

		CStr Application;
		CStr Version;
		CStr Configuration;
		CStr Platform;
		CEJsonSorted ExtraInfo;

		auto fApplySettings = [&](CEJsonSorted const &_Settings)
			{
				if (auto *pValue = _Settings.f_GetMember("Application", EJsonType_String))
					Application = pValue->f_String();
				if (auto *pValue = _Settings.f_GetMember("Version", EJsonType_String))
					Version = pValue->f_String();
				if (auto *pValue = _Settings.f_GetMember("Configuration", EJsonType_String))
					Configuration = pValue->f_String();
				if (auto *pValue = _Settings.f_GetMember("Platform", EJsonType_String))
					Platform = pValue->f_String();
				if (auto *pValue = _Settings.f_GetMember("ExtraInfo", EJsonType_Object))
					ExtraInfo = pValue->f_Object();
			}
		;

		fApplySettings(Settings);
		fApplySettings(_Params);

		TCVector<CStr> UploadHosts;

		if (auto *pValue = _Params.f_GetMember("VersionManagerHosts"))
		{
			if (_Params.f_GetMember("VersionManagerHost"))
				co_return DMibErrorInstance("You cannot specify both --host and --hosts at the same time");

			UploadHosts = pValue->f_StringArray();
		}
		else if (auto *pValue = _Params.f_GetMember("VersionManagerHost"))
			UploadHosts.f_Insert(pValue->f_String());

		CTime Time = _Params["Time"].f_Date();
		uint64 QueueSize = _Params["TransferQueueSize"].f_Integer();
		bool bForce = _Params["Force"].f_Boolean();
		if (QueueSize < 128*1024)
			QueueSize = 128*1024;

		TCSet<CStr> Tags;
		for (auto &TagJson : _Params["Tags"].f_Array())
		{
			CStr const &Tag = TagJson.f_String();
			if (!CVersionManager::fs_IsValidTag(Tag))
				co_return DMibErrorInstance(fg_Format("'{}' is not a valid tag", Tag));
			Tags[Tag];
		}

		if (Application.f_IsEmpty())
			co_return DMibErrorInstance("Application must be specified");
		if (Version.f_IsEmpty())
			co_return DMibErrorInstance("Version must be specified");

		if (!CVersionManager::fs_IsValidApplicationName(Application))
			co_return DMibErrorInstance("Application format is invalid");

		CVersionManager::CVersionIDAndPlatform VersionID;
		{
			CStr Error;
			if (!CVersionManager::fs_IsValidVersionIdentifier(Version, Error, &VersionID.m_VersionID))
				co_return DMibErrorInstance(fg_Format("Version identifier format is invalid: {}", Error));
		}

		if (!CVersionManager::fs_IsValidPlatform(Platform))
			co_return DMibErrorInstance("Invalid version platform format");

		VersionID.m_Platform = Platform;

		if (!Time.f_IsValid())
		{
			auto BlockingActorCheckout = fg_BlockingActor();
			Time = co_await
				(
					g_Dispatch(BlockingActorCheckout) / [Source]() -> CTime
					{
						if (!CFile::fs_FileExists(Source))
							DMibError(fg_Format("Source specified does not exist: {}", Source));
						if (CFile::fs_FileExists(Source, EFileAttrib_File))
							return CFile::fs_GetWriteTime(Source);
						CFile::CFindFilesOptions FindOptions{Source + "/*", true};
						FindOptions.m_AttribMask = EFileAttrib_File;
						CTime ReturnTime;
						for (auto &FoundFile : CFile::fs_FindFiles(FindOptions))
						{
							auto FoundTime = CFile::fs_GetWriteTime(FoundFile.m_Path);
							if (!ReturnTime.f_IsValid() || FoundTime > ReturnTime)
								ReturnTime = FoundTime;
						}
						return ReturnTime;
					}
					% "Failed to deduce backup time"
				)
			;
		}

		co_await fp_VersionManager_SubscribeToServers().f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for version managers");

		auto HostsToFind = TCSet<CStr>::fs_FromContainer(UploadHosts);

		CVersionManager::CVersionInformation VersionInfo;
		VersionInfo.m_Time = Time;
		VersionInfo.m_Configuration = Configuration;
		VersionInfo.m_ExtraInfo = ExtraInfo;
		VersionInfo.m_Tags = Tags;

		TCFutureVector<void> Results;

		for (auto &VersionManager : mp_VersionManagers.m_Actors)
		{
			if (!UploadHosts.f_IsEmpty() && !HostsToFind.f_Remove(VersionManager.m_TrustInfo.m_HostInfo.m_HostID))
				continue;

			fg_CallSafe
				(
					[=, this, Actor = VersionManager.m_Actor, HostInfo = VersionManager.m_TrustInfo.m_HostInfo] -> TCUnsafeFuture<void>
					{
						CVersionManagerHelper::CUploadResult UploadResult = co_await mp_VersionManagerHelper.f_Upload
							(
								Actor
								, Application
								, VersionID
								, VersionInfo
								, Source
								, bForce ? CVersionManager::CStartUploadVersion::EFlag_ForceOverwrite : CVersionManager::CStartUploadVersion::EFlag_None
								, QueueSize
							)
						;

						if (!UploadResult.m_DeniedTags.f_IsEmpty())
							*_pCommandLine %= "{}: The following tags were denied: {vs,vb}\n"_f << HostInfo.f_GetDescColored(AnsiFlags) << UploadResult.m_DeniedTags;

						*_pCommandLine += "{}: Upload finished transferring in {}: {ns } bytes at {fe2} MB/s\n"_f
							<< HostInfo.f_GetDescColored(AnsiFlags)
							<< fg_SecondsDurationToHumanReadable(UploadResult.m_TransferResult.m_nSeconds)
							<< UploadResult.m_TransferResult.m_nBytes
							<< UploadResult.m_TransferResult.f_BytesPerSecond() / 1'000'000.0
						;

						co_return {};
					}
				)
				> Results
			;
		}

		if (Results.f_IsEmpty())
			co_return DMibErrorInstance("Did not find any host to upload to. Connection might have failed. Use --log-to-stderr to see more info.");

		co_await fg_AllDone(fg_Move(Results));

		if (!HostsToFind.f_IsEmpty())
		{
			co_return DMibErrorInstance
				(
					fg_Format
					(
						"Did not find these hosts: {vs}. Connection might have failed. Use --log-to-stderr to see more info."
						, HostsToFind
					)
				)
			;
		}

		co_return 0;
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_VersionManager_DownloadVersion(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		auto CheckDestroy = co_await fp_CheckStoppedOrDestroyedOnResume();

		CStr Host = _Params["VersionManagerHost"].f_String();
		CStr Application = _Params["Application"].f_String();
		CStr Version = _Params["Version"].f_String();
		CStr Platform = _Params["Platform"].f_String();
		uint64 QueueSize = _Params["TransferQueueSize"].f_Integer();
		if (QueueSize < 128*1024)
			QueueSize = 128*1024;

		if (Application.f_IsEmpty())
			co_return DMibErrorInstance("Application must be specified");
		if (Version.f_IsEmpty())
			co_return DMibErrorInstance("Version must be specified");

		CStr DestinationDirectory = CFile::fs_GetExpandedPath(_Params["DestinationDirectory"].f_String(), _Params["CurrentDirectory"].f_String());
		if (DestinationDirectory.f_IsEmpty())
			co_return DMibErrorInstance("Destination directory must be specified");

		if (!CVersionManager::fs_IsValidApplicationName(Application))
			co_return DMibErrorInstance("Application format is invalid");

		CVersionManager::CVersionIDAndPlatform VersionID;
		{
			CStr Error;
			if (!CVersionManager::fs_IsValidVersionIdentifier(Version, Error, &VersionID.m_VersionID))
				co_return DMibErrorInstance(fg_Format("Version identifier format is invalid: {}", Error));
		}
		if (!CVersionManager::fs_IsValidPlatform(Platform))
			co_return DMibErrorInstance("Version platform format is invalid");

		VersionID.m_Platform = Platform;

		co_await fp_VersionManager_SubscribeToServers().f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for version managers");

		CStr Error;
		auto *pVersionManager = mp_VersionManagers.f_GetOneActor(Host, Error);
		if (!pVersionManager)
			co_return DMibErrorInstance(fg_Format("Error selecting version manager: {}. Connection might have failed. Use --log-to-stderr to see more info.", Error));

		CFileTransferResult Results = co_await mp_VersionManagerHelper.f_Download
			(
				pVersionManager->m_Actor
				, Application
				, VersionID
				, DestinationDirectory
				, CFileTransferReceive::EReceiveFlag_None
				, QueueSize
			)
		;

		*_pCommandLine += "Download finished transferring: {ns } bytes at {fe2} MB/s\n"_f
			<< Results.m_nBytes
			<< Results.f_BytesPerSecond()/1'000'000.0
		;

		co_return 0;
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_VersionManager_ChangeTags(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		auto CheckDestroy = co_await fp_CheckStoppedOrDestroyedOnResume();

		CStr Host = _Params["VersionManagerHost"].f_String();
		CStr Application = _Params["Application"].f_String();
		CStr Version = _Params["Version"].f_String();
		CStr Platform = _Params["Platform"].f_String();
		bool bRetryUpgrade = _Params["RetryUpgrade"].f_Boolean();

		if (Application.f_IsEmpty())
			co_return DMibErrorInstance("Application must be specified");
		if (Version.f_IsEmpty())
			co_return DMibErrorInstance("Version must be specified");

		if (!CVersionManager::fs_IsValidApplicationName(Application))
			co_return DMibErrorInstance("Application format is invalid");

		CVersionManager::CVersionID VersionID;
		{
			CStr Error;
			if (!CVersionManager::fs_IsValidVersionIdentifier(Version, Error, &VersionID))
				co_return DMibErrorInstance(fg_Format("Version identifier format is invalid: {}", Error));
		}
		if (!Platform.f_IsEmpty() && !CVersionManager::fs_IsValidPlatform(Platform))
			co_return DMibErrorInstance("Version platform format is invalid");

		auto fParseTags = [](CEJsonSorted const &_Tags)
			{
				TCSet<CStr> OutTags;
				for (auto &TagJson : _Tags.f_Array())
				{
					CStr const &Tag = TagJson.f_String();
					if (!CVersionManager::fs_IsValidTag(Tag))
						DMibError(fg_Format("'{}' is not a valid tag", Tag));
					OutTags[Tag];
				}
				return OutTags;
			}
		;

		TCSet<CStr> AddTags;
		TCSet<CStr> RemoveTags;
		{
			auto CaptureScope = co_await g_CaptureExceptions;

			AddTags = fParseTags(_Params["AddTags"]);
			RemoveTags = fParseTags(_Params["RemoveTags"]);
		}

		if (AddTags.f_IsEmpty() && RemoveTags.f_IsEmpty() && !bRetryUpgrade)
			co_return DMibErrorInstance("No changes specified. Specify tags to add and remove with --add and --remove, or specify --retry-upgrade");

		co_await fp_VersionManager_SubscribeToServers().f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for version managers");

		CStr Error;
		auto *pVersionManager = mp_VersionManagers.f_GetOneActor(Host, Error);
		if (!pVersionManager)
			co_return DMibErrorInstance(fg_Format("Error selecting version manager: {}. Connection might have failed. Use --log-to-stderr to see more info.", Error));

		CVersionManager::CChangeTags ChangeTags;
		ChangeTags.m_Application = Application;
		ChangeTags.m_VersionID = VersionID;
		ChangeTags.m_Platform = Platform;
		ChangeTags.m_AddTags = AddTags;
		ChangeTags.m_RemoveTags = RemoveTags;
		ChangeTags.m_bIncreaseRetrySequence = bRetryUpgrade;

		CVersionManager::CChangeTags::CResult Result = co_await
			(
				pVersionManager->m_Actor.f_CallActor(&CVersionManager::f_ChangeTags)(fg_Move(ChangeTags))
				.f_Timeout(mp_Timeout, "Timed out waiting for version manager to reply")
				% "Failed to change tags"
			)
		;

		if (!Result.m_DeniedTags.f_IsEmpty())
			*_pCommandLine %= "The following tags were denied: {vs,vb}\n"_f << Result.m_DeniedTags;

		co_return 0;
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_VersionManager_CopyVersions(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		using namespace NStr;

		auto CheckDestroy = co_await fp_CheckStoppedOrDestroyedOnResume();

		auto AnsiEncoding = _pCommandLine->f_AnsiEncoding();

		CStr SourceHost = _Params["SourceHost"].f_String();
		CStr DestHost = _Params["DestHost"].f_String();
		bool bLatestOnly = _Params["LatestOnly"].f_Boolean();
		bool bPretend = _Params["Pretend"].f_Boolean();
		bool bForce = _Params["Force"].f_Boolean();
		uint64 QueueSize = _Params["TransferQueueSize"].f_Integer();

		if (QueueSize < 128 * 1024)
			QueueSize = 128 * 1024;

		if (SourceHost.f_IsEmpty())
			co_return DMibErrorInstance("Source host must be specified with --source-host");
		if (DestHost.f_IsEmpty())
			co_return DMibErrorInstance("Destination host must be specified with --dest-host");
		if (SourceHost == DestHost)
			co_return DMibErrorInstance("Source and destination hosts cannot be the same");

		auto ApplicationFilters = _Params["Applications"].f_StringArray();
		auto VersionFilters = _Params["Versions"].f_StringArray();
		auto PlatformFilters = _Params["Platforms"].f_StringArray();
		auto TagFilters = _Params["Tags"].f_StringArray();

		TCMap<CStr, CStr> CopyTagMappings;
		for (auto &CopyTag : _Params["CopyTags"].f_StringArray())
		{
			CStr SourceTag, DestTag;
			aint nVarsParsed = 0;
			aint nCharsParsed = (CStr::CParse("{}={}") >> SourceTag >> DestTag).f_Parse(CopyTag, nVarsParsed);
			if (nVarsParsed == 2 && nCharsParsed == CopyTag.f_GetLen())
				CopyTagMappings[SourceTag] = DestTag;
			else
				CopyTagMappings[CopyTag] = CopyTag;
		}

		auto fContainsWildcard = [](CStr const &_Pattern) -> bool
			{
				return _Pattern.f_FindChars("*?") >= 0;
			}
		;

		auto fMatchesFilter = [](CStr const &_Value, TCVector<CStr> const &_Filters) -> bool
			{
				if (_Filters.f_IsEmpty())
					return true;
				for (auto &Filter : _Filters)
				{
					if (fg_StrMatchWildcard(_Value.f_GetStr(), Filter.f_GetStr()) == EMatchWildcardResult_WholeStringMatchedAndPatternExhausted)
						return true;
				}
				return false;
			}
		;

		auto fMatchesTagFilter = [](TCSet<CStr> const &_Tags, TCVector<CStr> const &_Filters) -> bool
			{
				if (_Filters.f_IsEmpty())
					return true;
				for (auto &Tag : _Tags)
				{
					for (auto &Filter : _Filters)
					{
						if (fg_StrMatchWildcard(Tag.f_GetStr(), Filter.f_GetStr()) == EMatchWildcardResult_WholeStringMatchedAndPatternExhausted)
							return true;
					}
				}
				return false;
			}
		;

		// Only explicitly specified tags are copied, others are dropped
		auto fTransformTags = [&CopyTagMappings](TCSet<CStr> const &_SourceTags) -> TCSet<CStr>
			{
				TCSet<CStr> DestTags;
				for (auto &Tag : _SourceTags)
				{
					if (auto *pMapping = CopyTagMappings.f_FindEqual(Tag))
						DestTags[*pMapping];
				}
				return DestTags;
			}
		;

		bool bApplicationFiltersHaveWildcards = false;
		for (auto &Filter : ApplicationFilters)
		{
			if (fContainsWildcard(Filter))
			{
				bApplicationFiltersHaveWildcards = true;
				break;
			}
		}

		co_await fp_VersionManager_SubscribeToServers().f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for version managers");

		CStr Error;
		auto *pSourceVersionManager = mp_VersionManagers.f_GetOneActor(SourceHost, Error);
		if (!pSourceVersionManager)
			co_return DMibErrorInstance("Error finding source version manager '{}': {}. Connection might have failed. Use --log-to-stderr to see more info."_f << SourceHost << Error);

		auto *pDestVersionManager = mp_VersionManagers.f_GetOneActor(DestHost, Error);
		if (!pDestVersionManager)
			co_return DMibErrorInstance("Error finding destination version manager '{}': {}. Connection might have failed. Use --log-to-stderr to see more info."_f << DestHost << Error);

		CVersionManager::CListVersions SourceQuery;
		CVersionManager::CListVersions DestQuery;
		if (ApplicationFilters.f_GetLen() == 1 && !bApplicationFiltersHaveWildcards)
		{
			SourceQuery.m_ForApplication = ApplicationFilters[0];
			DestQuery.m_ForApplication = ApplicationFilters[0];
		}

		auto SourceVersionsFuture = pSourceVersionManager->m_Actor.f_CallActor(&CVersionManager::f_ListVersions)(fg_Move(SourceQuery))
			.f_Timeout(mp_Timeout, "Timed out waiting for source version manager to reply")
		;
		auto DestVersionsFuture = pDestVersionManager->m_Actor.f_CallActor(&CVersionManager::f_ListVersions)(fg_Move(DestQuery))
			.f_Timeout(mp_Timeout, "Timed out waiting for destination version manager to reply")
		;

		auto SourceVersionsResult = co_await fg_Move(SourceVersionsFuture).f_Wrap();
		if (!SourceVersionsResult)
			co_return DMibErrorInstance("Failed to list versions from source: {}"_f << SourceVersionsResult.f_GetExceptionStr());

		auto DestVersionsResult = co_await fg_Move(DestVersionsFuture).f_Wrap();
		if (!DestVersionsResult)
			co_return DMibErrorInstance("Failed to list versions from destination: {}"_f << DestVersionsResult.f_GetExceptionStr());

		auto &SourceVersions = SourceVersionsResult->m_Versions;
		auto &DestVersions = DestVersionsResult->m_Versions;

		struct CVersionToCopy
		{
			CStr m_Application;
			CVersionManager::CVersionIDAndPlatform m_VersionIDAndPlatform;
			CVersionManager::CVersionInformation m_VersionInfo;
		};
		TCVector<CVersionToCopy> VersionsToCopy;

		auto fFormatVersionID = [](CVersionManager::CVersionID const &_VersionID) -> CStr
			{
				return CStr::fs_ToStr(_VersionID);
			}
		;

		for (auto &AppVersions : SourceVersions)
		{
			CStr const &Application = SourceVersions.fs_GetKey(AppVersions);

			if (!fMatchesFilter(Application, ApplicationFilters))
				continue;

			if (bLatestOnly)
			{
				CVersionManager::CVersionID LatestVersion;
				bool bFirst = true;
				for (auto &VersionEntry : AppVersions)
				{
					auto const &VersionIDAndPlatform = AppVersions.fs_GetKey(VersionEntry);

					if (!fMatchesFilter(fFormatVersionID(VersionIDAndPlatform.m_VersionID), VersionFilters))
						continue;

					if (!fMatchesTagFilter(VersionEntry.m_Tags, TagFilters))
						continue;

					if (bFirst || VersionIDAndPlatform.m_VersionID > LatestVersion)
					{
						LatestVersion = VersionIDAndPlatform.m_VersionID;
						bFirst = false;
					}
				}

				if (bFirst)
					continue;

				for (auto &VersionEntry : AppVersions)
				{
					auto const &VersionIDAndPlatform = AppVersions.fs_GetKey(VersionEntry);
					if (VersionIDAndPlatform.m_VersionID != LatestVersion)
						continue;

					if (!fMatchesFilter(VersionIDAndPlatform.m_Platform, PlatformFilters))
						continue;

					if (!fMatchesTagFilter(VersionEntry.m_Tags, TagFilters))
						continue;

					if (!bForce)
					{
						auto *pDestApp = DestVersions.f_FindEqual(Application);
						if (pDestApp && pDestApp->f_FindEqual(VersionIDAndPlatform))
							continue;
					}

					VersionsToCopy.f_Insert(CVersionToCopy{Application, VersionIDAndPlatform, VersionEntry});
				}
			}
			else
			{
				for (auto &VersionEntry : AppVersions)
				{
					auto const &VersionIDAndPlatform = AppVersions.fs_GetKey(VersionEntry);

					if (!fMatchesFilter(fFormatVersionID(VersionIDAndPlatform.m_VersionID), VersionFilters))
						continue;

					if (!fMatchesFilter(VersionIDAndPlatform.m_Platform, PlatformFilters))
						continue;

					if (!fMatchesTagFilter(VersionEntry.m_Tags, TagFilters))
						continue;

					if (!bForce)
					{
						auto *pDestApp = DestVersions.f_FindEqual(Application);
						if (pDestApp && pDestApp->f_FindEqual(VersionIDAndPlatform))
							continue;
					}

					VersionsToCopy.f_Insert(CVersionToCopy{Application, VersionIDAndPlatform, VersionEntry});
				}
			}
		}

		// Sort versions in descending order (newest first) so that when tags are applied,
		// AppManager upgrades to the newest version directly
		VersionsToCopy.f_Sort([](CVersionToCopy const &_Left, CVersionToCopy const &_Right)
			{
				if (auto Result = _Left.m_Application <=> _Right.m_Application; Result != 0)
					return Result;

				return _Right.m_VersionIDAndPlatform <=> _Left.m_VersionIDAndPlatform;
			}
		);

		if (VersionsToCopy.f_IsEmpty())
		{
			*_pCommandLine += "No versions to copy. All matching versions already exist on destination or no matching versions found on source.\n";
			co_return 0;
		}

		uint64 nTotalBytes = 0;
		for (auto &Version : VersionsToCopy)
			nTotalBytes += Version.m_VersionInfo.m_nBytes;

		if (bPretend)
		{
			CTableRenderHelper TableRenderer = _pCommandLine->f_TableRenderer();
			TableRenderer.f_SetOptions(CTableRenderHelper::EOption_Rounded | CTableRenderHelper::EOption_AvoidRowSeparators);
			TableRenderer.f_AddHeadings("Application", "Version", "Platform", "Config", "Tags", "Size", "Files");

			for (auto &Version : VersionsToCopy)
			{
				TableRenderer.f_AddRow
					(
						Version.m_Application
						, Version.m_VersionIDAndPlatform.m_VersionID
						, Version.m_VersionIDAndPlatform.m_Platform
						, Version.m_VersionInfo.m_Configuration
						, "{vs,vb,a-}"_f << fTransformTags(Version.m_VersionInfo.m_Tags)
						, "{ns }"_f << Version.m_VersionInfo.m_nBytes
						, Version.m_VersionInfo.m_nFiles
					)
				;
			}

			TableRenderer.f_Output(_Params);

			*_pCommandLine += "{}Dry run:{} Would copy {} version(s), total size: {ns }\n\n"_f
				<< AnsiEncoding.f_StatusNormal()
				<< AnsiEncoding.f_Default()
				<< VersionsToCopy.f_GetLen()
				<< nTotalBytes
			;

			co_return 0;
		}

		*_pCommandLine += "Copying {} version(s), total size: {ns }\n"_f << VersionsToCopy.f_GetLen() << nTotalBytes;

		uint32 nSuccessful = 0;
		uint32 nFailed = 0;
		uint64 nBytesDownloaded = 0;
		uint64 nBytesUploaded = 0;

		for (auto &Version : VersionsToCopy)
		{
			co_await g_AsyncDestroy;

			*_pCommandLine += "Copying {} {} ({})...\n"_f
				<< Version.m_Application
				<< Version.m_VersionIDAndPlatform.m_VersionID
				<< Version.m_VersionIDAndPlatform.m_Platform
			;

			CStr TempDir = CFile::fs_GetTemporaryDirectory() / ("CloudClient_CopyVersion_{}"_f << fg_RandomID());
			{
				auto BlockingActorCheckout = fg_BlockingActor();
				co_await
					(
						g_Dispatch(BlockingActorCheckout) / [TempDir]
						{
							CFile::fs_CreateDirectory(TempDir);
						}
					)
				;
			}

			auto Cleanup = co_await fg_AsyncDestroy
				(
					[TempDir]() mutable -> TCUnsafeFuture<void>
					{
						auto LocalTempDir = TempDir;
						auto BlockingActorCheckout = fg_BlockingActor();
						co_await
							(
								g_Dispatch(BlockingActorCheckout) / [LocalTempDir]
								{
									try
									{
										CFile::fs_DeleteDirectoryRecursive(LocalTempDir);
									}
									catch (...)
									{
										// Ignore cleanup errors
									}
								}
							)
						;
						co_return {};
					}
				)
			;

			auto DownloadResult = co_await mp_VersionManagerHelper.f_Download
				(
					pSourceVersionManager->m_Actor
					, Version.m_Application
					, Version.m_VersionIDAndPlatform
					, TempDir
					, CFileTransferReceive::EReceiveFlag_None
					, QueueSize
				)
				.f_Wrap()
			;

			if (!DownloadResult)
			{
				*_pCommandLine %= "{}Failed{} to download {} {} ({}): {}\n"_f
					<< AnsiEncoding.f_StatusError()
					<< AnsiEncoding.f_Default()
					<< Version.m_Application
					<< Version.m_VersionIDAndPlatform.m_VersionID
					<< Version.m_VersionIDAndPlatform.m_Platform
					<< DownloadResult.f_GetExceptionStr()
				;
				++nFailed;
				continue;
			}
			nBytesDownloaded += DownloadResult->m_nBytes;

			*_pCommandLine += "Downloaded {} {} ({}): {ns } bytes in {} at {fe2} MB/s\n"_f
				<< Version.m_Application
				<< Version.m_VersionIDAndPlatform.m_VersionID
				<< Version.m_VersionIDAndPlatform.m_Platform
				<< DownloadResult->m_nBytes
				<< fg_SecondsDurationToHumanReadable(DownloadResult->m_nSeconds)
				<< DownloadResult->f_BytesPerSecond() / 1'000'000.0
			;

			TCVector<CStr> PackageFiles;
			{
				auto BlockingActorCheckout = fg_BlockingActor();
				PackageFiles = co_await
					(
						g_Dispatch(BlockingActorCheckout) / [TempDir]
						{
							return CFile::fs_FindFiles(TempDir / "*", EFileAttrib_File, false);
						}
					)
				;
			}

			if (PackageFiles.f_IsEmpty())
			{
				*_pCommandLine %= "{}Failed{} to find package file for {} {} ({}): No package file found in download\n"_f
					<< AnsiEncoding.f_StatusError()
					<< AnsiEncoding.f_Default()
					<< Version.m_Application
					<< Version.m_VersionIDAndPlatform.m_VersionID
					<< Version.m_VersionIDAndPlatform.m_Platform
				;
				++nFailed;
				continue;
			}

			if (PackageFiles.f_GetLen() > 1)
			{
				*_pCommandLine %= "{}Failed{} to find package file for {} {} ({}): Multiple package files found in download: {}\n"_f
					<< AnsiEncoding.f_StatusError()
					<< AnsiEncoding.f_Default()
					<< Version.m_Application
					<< Version.m_VersionIDAndPlatform.m_VersionID
					<< Version.m_VersionIDAndPlatform.m_Platform
					<< PackageFiles
				;
				++nFailed;
				continue;
			}

			CStr PackageFile = PackageFiles[0];

			auto DestVersionInfo = Version.m_VersionInfo;
			DestVersionInfo.m_Tags = fTransformTags(Version.m_VersionInfo.m_Tags);

			auto UploadResult = co_await mp_VersionManagerHelper.f_Upload
				(
					pDestVersionManager->m_Actor
					, Version.m_Application
					, Version.m_VersionIDAndPlatform
					, DestVersionInfo
					, PackageFile
					, bForce ? CVersionManager::CStartUploadVersion::EFlag_ForceOverwrite : CVersionManager::CStartUploadVersion::EFlag_None
					, QueueSize
				)
				.f_Wrap()
			;

			if (!UploadResult)
			{
				*_pCommandLine %= "{}Failed{} to upload {} {} ({}): {}\n"_f
					<< AnsiEncoding.f_StatusError()
					<< AnsiEncoding.f_Default()
					<< Version.m_Application
					<< Version.m_VersionIDAndPlatform.m_VersionID
					<< Version.m_VersionIDAndPlatform.m_Platform
					<< UploadResult.f_GetExceptionStr()
				;
				++nFailed;
				continue;
			}

			if (!UploadResult->m_DeniedTags.f_IsEmpty())
			{
				*_pCommandLine %= "Tags denied for {} {} ({}): {vs,vb}\n"_f
					<< Version.m_Application
					<< Version.m_VersionIDAndPlatform.m_VersionID
					<< Version.m_VersionIDAndPlatform.m_Platform
					<< UploadResult->m_DeniedTags
				;
			}

			*_pCommandLine += "{}Uploaded{} {} {} ({}): {ns } bytes in {} at {fe2} MB/s\n"_f
				<< AnsiEncoding.f_StatusNormal()
				<< AnsiEncoding.f_Default()
				<< Version.m_Application
				<< Version.m_VersionIDAndPlatform.m_VersionID
				<< Version.m_VersionIDAndPlatform.m_Platform
				<< UploadResult->m_TransferResult.m_nBytes
				<< fg_SecondsDurationToHumanReadable(UploadResult->m_TransferResult.m_nSeconds)
				<< UploadResult->m_TransferResult.f_BytesPerSecond() / 1'000'000.0
			;

			nBytesUploaded += UploadResult->m_TransferResult.m_nBytes;
			++nSuccessful;
		}

		*_pCommandLine += "\nCopy complete: {} succeeded, {} failed, {ns } bytes downloaded, {ns } bytes uploaded\n"_f
			<< nSuccessful
			<< nFailed
			<< nBytesDownloaded
			<< nBytesUploaded
		;

		if (nFailed > 0)
			co_return 1;

		co_return 0;
	}
}
