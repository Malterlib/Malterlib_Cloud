// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Cloud/VersionManager>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Encoding/JSONShortcuts>
#include <Mib/CommandLine/TableRenderer>

#include "Malterlib_Cloud_App_CloudClient.h"

#ifdef DPlatformFamily_Windows
#include <Mib/Core/PlatformSpecific/WindowsFilePath>
#endif

namespace NMib::NCloud::NCloudClient
{
	void CCloudClientAppActor::fp_VersionManager_RegisterCommands(CDistributedAppCommandLineSpecification::CSection _Section)
	{
		auto VersionManagerHost = "VersionManagerHost?"_=
			{
				"Names"_= {"--host"}
				, "Default"_= ""
				, "Description"_= "Limit query to only specified host ID."
			}
		;
		auto IncludeHost = "IncludeHost?"_=
			{
				"Names"_= {"--include-host"}
				, "Default"_= false
				, "Description"_= "Include version manager host in output.\n"
			}
		;

		_Section.f_RegisterCommand
			(
				{
					"Names"_= {"--version-manager-list-applications"}
					, "Description"_= "List applications available on remote version managers."
					, "Options"_=
					{
						VersionManagerHost
						, IncludeHost
						, CTableRenderHelper::fs_OutputTypeOption()
					}
				}
				, [this](CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return g_Future <<= self(&CCloudClientAppActor::fp_CommandLine_VersionManager_ListApplications, _Params, _pCommandLine);
				}
				, EDistributedAppCommandFlag_WaitForRemotes
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_= {"--version-manager-list-versions"}
					, "Description"_= "List application versions available on remote version managers."
					, "Options"_=
					{
						VersionManagerHost
						, IncludeHost
						, "Verbose?"_=
						{
							"Names"_= {"--verbose", "-v"}
							, "Default"_= false
							, "Description"_= "Verbose output. Include custom JSON information.\n"
						}
						, CTableRenderHelper::fs_OutputTypeOption()
					}
					, "Parameters"_=
					{
						"Application?"_=
						{
							"Default"_= ""
							, "Description"_= "The application to list versions for.\n"
								"If left empty versions will be listed for all applications you have access to.\n"
						}
					}
				}
				, [this](CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return g_Future <<= self(&CCloudClientAppActor::fp_CommandLine_VersionManager_ListVersions, _Params, _pCommandLine);
				}
				, EDistributedAppCommandFlag_WaitForRemotes
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_= {"--version-manager-upload-version"}
					, "Description"_= "Upload a version to remote version manager.\n"
					, "Options"_=
					{
						"VersionManagerHost?"_=
						{
							"Names"_= {"--host"}
							, "Default"_= ""
							, "Description"_= "The host ID of the host to upload the version to."
						}
						, "SettingsFileFromPackage?"_=
						{
							"Names"_= {"--settings-file-from-package"}
							, "Default"_= true
							, "Description"_= "If the uploaded file is a tar.gz file, look inside it for VersionInfo.json and use as settings file.\n"
							"If --settings-file is specified that will override the settings file inside the uploaded package.\n"
							"Settings in the settings file is overridden by settings specified on the command line.\n"
						}
						, "SettingsFile?"_=
						{
							"Names"_= {"--settings-file"}
							, "Default"_= ""
							, "Description"_= "The JSON file to read settings from.\n"
							"Settings in the settings file is overridden by settings specified on the command line.\n"
							"The format template for the settings file is:\n" +
							CEJSON
							(
								{
									"Application"_= "AppName"
									, "Version"_= "Branch/1.0.1"
									, "Platform"_= DMalterlibCloudPlatform
									, "Configuration"_= DMibStringize(DConfig)
									, "ExtraInfo"_=
									{
										"Executable"_= "ExecutableName"
										, "ExecutableParams"_= {"--daemon-run-standalone", "--debug"}
										, "RunAsUser"_= "ApplicationSpecificUser"
										, "RunAsGroup"_= "ApplicationSpecificGroup"
										, "RunAsUserHasShell"_= false
										, "DistributedApp"_= true
									}
								}
							)
							.f_ToStringColored(CCommandLineDefaults::fs_ColorAnsiFlagsDefault(), "    ").f_Replace("\r\n", "\r").f_Replace("\n", "\r")
						}
						, "Application?"_=
						{
							"Names"_= {"--application"}
							, "Type"_= ""
							, "Description"_= "The application to upload a version to."
						}
						, "Version?"_=
						{
							"Names"_= {"--version"}
							, "Type"_= ""
							, "Description"_= "The version to upload.\n"
								"This is in the format 'Branch/Major.Minor.Patch' as displayed in the output from --version-manager-list-versions.\n"
						}
						, "Platform?"_=
						{
							"Names"_= {"--platform"}
							, "Type"_= ""
							, "Description"_= "The platform of the version to upload.\n"
						}
						, "Configuration?"_=
						{
							"Names"_= {"--configuration"}
							, "Type"_= ""
							, "Description"_= "The configuration for this build. Could be for example Debug or Release.\n"
						}
						, "ExtraInfo?"_=
						{
							"Names"_= {"--info"}
							, "Type"_= EJSONType_Object
							, "Description"_= "EJSON formatted extra information.\n"
						}
						, "Tags?"_=
						{
							"Names"_= {"--tags"}
							, "Default"_= _[_]
							, "Type"_= {""}
							, "Description"_= "The tags to apply to the version.\n"
						}
						, "Time?"_=
						{
							"Names"_= {"--time"}
							, "Default"_= CTime()
							, "Description"_= "The time for this version.\n"
								"By default the time will be deduced from the modification time of the directory or file uploaded.\n"
						}
						, "Force?"_=
						{
							"Names"_= {"--force"}
							, "Default"_= false
							, "Description"_= "Force upload even if version already exists.\n"
						}
						, "TransferQueueSize?"_=
						{
							"Names"_= {"--queue-size"}
							, "Default"_= int64(8*1024*1024)
							, "Description"_= "The amount of data to keep in flight while uploading."
						}
						, "CurrentDirectory?"_=
						{
							"Names"_= _[_]
							, "Default"_= CFile::fs_GetCurrentDirectory()
							, "Hidden"_= true
							, "Description"_= "Internal hidden option to forward current directory."
						}
					}
					, "Parameters"_=
					{
						"Source"_=
						{
							"Default"_= ""
							, "Description"_= "The file or directory to upload.\n"
						}
					}
				}
				, [this](CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return g_Future <<= self(&CCloudClientAppActor::fp_CommandLine_VersionManager_UploadVersion, _Params, _pCommandLine);
				}
				, EDistributedAppCommandFlag_WaitForRemotes
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_= {"--version-manager-change-tags"}
					, "Description"_= "Upload a version to remote version manager\n"
					, "Options"_=
					{
						"VersionManagerHost?"_=
						{
							"Names"_= {"--host"}
							, "Default"_= ""
							, "Description"_= "The host ID of the host to change tags for."
						}
						, "Application"_=
						{
							"Names"_= {"--application"}
							, "Type"_= ""
							, "Description"_= "Change tags for this application."
						}
						, "Version"_=
						{
							"Names"_= {"--version"}
							, "Type"_= ""
							, "Description"_= "Change tags for this version.\n"
								"This is in the format 'Branch/Major.Minor.Patch' as displayed in the output from --version-manager-list-versions.\n"
						}
						, "Platform?"_=
						{
							"Names"_= {"--platform"}
							, "Default"_= ""
							, "Description"_= "The platform to change tags for. Leave empty to change all platforms.\n"
						}
						, "AddTags?"_=
						{
							"Names"_= {"--add"}
							, "Default"_= _[_]
							, "Type"_= {""}
							, "Description"_= "Add these tags.\n"
						}
						, "RemoveTags?"_=
						{
							"Names"_= {"--remove"}
							, "Default"_= _[_]
							, "Type"_= {""}
							, "Description"_= "Remove these tags.\n"
						}
						, "RetryUpgrade?"_=
						{
							"Names"_= {"--retry-upgrade"}
							, "Default"_= false
							, "Description"_= "Increase the retry sequence for this version. AppManagers will retry upgrade if it previously failed.\n"
						}
					}
				}
				, [this](CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return g_Future <<= self(&CCloudClientAppActor::fp_CommandLine_VersionManager_ChangeTags, _Params, _pCommandLine);
				}
				, EDistributedAppCommandFlag_WaitForRemotes
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_= {"--version-manager-download-version"}
					, "Description"_= "Download a version from remote version manager\n"
					, "Options"_=
					{
						"VersionManagerHost?"_=
						{
							"Names"_= {"--host"}
							, "Default"_= ""
							, "Description"_= "The host ID of the host to download the version from."
						}
						, "Application"_=
						{
							"Names"_= {"--application"}
							, "Type"_= ""
							, "Description"_= "The application to download a version from."
						}
						, "Version"_=
						{
							"Names"_= {"--version"}
							, "Type"_= ""
							, "Description"_= "The version to download.\n"
								"This is in the format 'Branch/Major.Minor.Patch' as displayed in the output from --version-manager-list-versions.\n"
						}
						, "Platform?"_=
						{
							"Names"_= {"--platform"}
							, "Default"_= DMalterlibCloudPlatform
							, "Description"_= "The platform of the version to download.\n"
						}
						, "TransferQueueSize?"_=
						{
							"Names"_= {"--queue-size"}
							, "Default"_= int64(8*1024*1024)
							, "Description"_= "The amount of data to keep in flight while downloading."
						}
					}
					, "Parameters"_=
					{
						"DestinationDirectory?"_=
						{
							"Default"_= mp_State.m_RootDirectory
							, "Description"_= "The directory to download the version to."
						}
					}
				}
				, [this](CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return g_Future <<= self(&CCloudClientAppActor::fp_CommandLine_VersionManager_DownloadVersion, _Params, _pCommandLine);
				}
				, EDistributedAppCommandFlag_WaitForRemotes
			)
		;
	}

	TCFuture<void> CCloudClientAppActor::fp_VersionManager_SubscribeToServers()
	{
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

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_VersionManager_ListApplications(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		CStr Host = _Params["VersionManagerHost"].f_String();
		bool bIncludeHost = _Params["IncludeHost"].f_Boolean();

		co_await self(&CCloudClientAppActor::fp_VersionManager_SubscribeToServers).f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for version managers");

		TCActorResultMap<CHostInfo, CVersionManager::CListApplications::CResult> Applications;

		for (auto &TrustedActor : mp_VersionManagers.m_Actors)
		{
			if (!Host.f_IsEmpty() && TrustedActor.m_TrustInfo.m_HostInfo.m_HostID != Host)
				continue;
			CVersionManager::CListApplications Command;
			TrustedActor.m_Actor.f_CallActor(&CVersionManager::f_ListApplications)(fg_Move(Command))
				.f_Timeout(mp_Timeout, "Timed out waiting for version manager to reply")
				> Applications.f_AddResult(TrustedActor.m_TrustInfo.m_HostInfo)
			;
		}

		auto AnsiEncoding = _pCommandLine->f_AnsiEncoding();
		CTableRenderHelper TableRenderer = _pCommandLine->f_TableRenderer();
		TableRenderer.f_AddHeadings("Host", "Application");

		auto Results = co_await Applications.f_GetResults();
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

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_VersionManager_ListVersions(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		CStr Host = _Params["VersionManagerHost"].f_String();
		CStr Application = _Params["Application"].f_String();
		bool bVerbose = _Params["Verbose"].f_Boolean();
		bool bIncludeHost = _Params["IncludeHost"].f_Boolean();

		co_await self(&CCloudClientAppActor::fp_VersionManager_SubscribeToServers).f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for version managers");
		TCActorResultMap<CHostInfo, CVersionManager::CListVersions::CResult> Versions;

		for (auto &TrustedVersionManager : mp_VersionManagers.m_Actors)
		{
			if (!Host.f_IsEmpty() && TrustedVersionManager.m_TrustInfo.m_HostInfo.m_HostID != Host)
				continue;
			CVersionManager::CListVersions Options;
			Options.m_ForApplication = Application;
			TrustedVersionManager.m_Actor.f_CallActor(&CVersionManager::f_ListVersions)(fg_Move(Options))
				.f_Timeout(mp_Timeout, "Timed out waiting for version manager to reply")
				> Versions.f_AddResult(TrustedVersionManager.m_TrustInfo.m_HostInfo)
			;
		}

		auto Results = co_await Versions.f_GetResults();

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
					ExtraInfo = Version.m_ExtraInfo.f_ToStringColored(_pCommandLine->m_AnsiFlags, "  ", EJSONDialectFlag_AllowUndefined);

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

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_VersionManager_UploadVersion(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		CStr Source = CFile::fs_GetFullPath(_Params["Source"].f_String(), _Params["CurrentDirectory"].f_String());
		if (Source.f_IsEmpty())
			co_return DMibErrorInstance("Source must be specified");

		CStr SettingsFile = _Params["SettingsFile"].f_String();
		if (!SettingsFile.f_IsEmpty())
			SettingsFile = CFile::fs_GetFullPath(SettingsFile, _Params["CurrentDirectory"].f_String());

		TCVariant<CStr, CEJSON> SettingsFileOrSettings;
		if (!SettingsFile.f_IsEmpty())
			SettingsFileOrSettings = SettingsFile;
		else if (_Params["SettingsFileFromPackage"].f_Boolean() && (Source.f_EndsWith(".tar.gz") || Source.f_EndsWith(".tar")))
		{
			// tar -xOf AppManager.tar.gz VersionInfo.json

			auto &LaunchActor = mp_LaunchActors.f_Insert() = fg_Construct();

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

			CEJSON VersionInfo = EJSONType_Object;
			if (LaunchResult)
			{
				try
				{
					VersionInfo = CEJSON::fs_FromString(LaunchResult->f_GetStdOut());
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

		CEJSON Settings;
		if (SettingsFileOrSettings.f_IsOfType<CEJSON>())
			Settings = SettingsFileOrSettings.f_Get<1>();
		else
		{
			CStr SettingsFile = SettingsFileOrSettings.f_Get<0>();
			if (!SettingsFile.f_IsEmpty())
			{
				auto SettingsResult = co_await
					(
						g_Dispatch(mp_VersionManagerHelper.f_GetFileActor()) / [SettingsFile]
						{
							return CEJSON::fs_FromString(CFile::fs_ReadStringFromFile(SettingsFile), SettingsFile);
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
				Settings = EJSONType_Object;
		}

		CStr Application;
		CStr Version;
		CStr Configuration;
		CStr Platform;
		CEJSON ExtraInfo;

		auto fApplySettings = [&](CEJSON const &_Settings)
			{
				if (auto *pValue = _Settings.f_GetMember("Application", EJSONType_String))
					Application = pValue->f_String();
				if (auto *pValue = _Settings.f_GetMember("Version", EJSONType_String))
					Version = pValue->f_String();
				if (auto *pValue = _Settings.f_GetMember("Configuration", EJSONType_String))
					Configuration = pValue->f_String();
				if (auto *pValue = _Settings.f_GetMember("Platform", EJSONType_String))
					Platform = pValue->f_String();
				if (auto *pValue = _Settings.f_GetMember("ExtraInfo", EJSONType_Object))
					ExtraInfo = pValue->f_Object();
			}
		;

		fApplySettings(Settings);
		fApplySettings(_Params);

		CStr Host = _Params["VersionManagerHost"].f_String();
		CTime Time = _Params["Time"].f_Date();
		uint64 QueueSize = _Params["TransferQueueSize"].f_Integer();
		bool bForce = _Params["Force"].f_Boolean();
		if (QueueSize < 128*1024)
			QueueSize = 128*1024;

		TCSet<CStr> Tags;
		for (auto &TagJSON : _Params["Tags"].f_Array())
		{
			CStr const &Tag = TagJSON.f_String();
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
			Time = co_await
				(
					g_Dispatch(mp_VersionManagerHelper.f_GetFileActor()) / [Source]() -> CTime
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

		co_await self(&CCloudClientAppActor::fp_VersionManager_SubscribeToServers).f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for version managers");

		CStr Error;
		auto *pVersionManager = mp_VersionManagers.f_GetOneActor(Host, Error);
		if (!pVersionManager)
		{
			co_return DMibErrorInstance
				(
					fg_Format
					(
						"Error selecting version manager for host '{}': {}. Connection might have failed. Use --log-to-stderr to see more info."
						, Host
						, Error
					)
				)
			;
		}

		CVersionManager::CVersionInformation VersionInfo;
		VersionInfo.m_Time = Time;
		VersionInfo.m_Configuration = Configuration;
		VersionInfo.m_ExtraInfo = ExtraInfo;
		VersionInfo.m_Tags = Tags;

		CVersionManagerHelper::CUploadResult UploadResult = co_await mp_VersionManagerHelper.f_Upload
			(
				pVersionManager->m_Actor
				, Application
				, VersionID
				, VersionInfo
				, Source
				, bForce ? CVersionManager::CStartUploadVersion::EFlag_ForceOverwrite : CVersionManager::CStartUploadVersion::EFlag_None
				, QueueSize
			)
		;

		if (!UploadResult.m_DeniedTags.f_IsEmpty())
			*_pCommandLine %= "The following tags were denied: {vs,vb}\n"_f << UploadResult.m_DeniedTags;

		*_pCommandLine += "Upload finished transferring: {ns } bytes at {fe2} MB/s\n"_f
			<< UploadResult.m_TransferResult.m_nBytes
			<< UploadResult.m_TransferResult.f_BytesPerSecond()/1'000'000.0
		;

		co_return 0;
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_VersionManager_DownloadVersion(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		CStr Host = _Params["VersionManagerHost"].f_String();
		CStr Application = _Params["Application"].f_String();
		CStr Version = _Params["Version"].f_String();
		CStr DestinationDirectory = _Params["DestinationDirectory"].f_String();
		CStr Platform = _Params["Platform"].f_String();
		uint64 QueueSize = _Params["TransferQueueSize"].f_Integer();
		if (QueueSize < 128*1024)
			QueueSize = 128*1024;

		if (Application.f_IsEmpty())
			co_return DMibErrorInstance("Application must be specified");
		if (Version.f_IsEmpty())
			co_return DMibErrorInstance("Version must be specified");
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

		co_await self(&CCloudClientAppActor::fp_VersionManager_SubscribeToServers).f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for version managers");

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
				, CFileTransferReceive::EReceiveFlag_IgnoreExisting
				, QueueSize
			)
		;

		*_pCommandLine += "Download finished transferring: {ns } bytes at {fe2} MB/s\n"_f
			<< Results.m_nBytes
			<< Results.f_BytesPerSecond()/1'000'000.0
		;

		co_return 0;
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_VersionManager_ChangeTags(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
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

		auto fParseTags = [](CEJSON const &_Tags)
			{
				TCSet<CStr> OutTags;
				for (auto &TagJSON : _Tags.f_Array())
				{
					CStr const &Tag = TagJSON.f_String();
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

		co_await self(&CCloudClientAppActor::fp_VersionManager_SubscribeToServers).f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for version managers");

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
}
