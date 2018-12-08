// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Cloud/VersionManager>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/Actor/Timer>
#include <Mib/Encoding/JSONShortcuts>

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
		_Section.f_RegisterCommand
			(
				{
					"Names"_= {"--version-manager-list-applications"}
					, "Description"_= "List applications available on remote version managers."
					, "Options"_=
					{
						VersionManagerHost
					}
				}
				, [this](CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return fp_CommandLine_VersionManager_ListApplications(_Params, _pCommandLine);
				}
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
						, "Verbose?"_=
						{
							"Names"_= {"--verbose", "-v"}
							, "Default"_= false
							, "Description"_= "Verbose output. Include custom JSON information.\n"
						}
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
					return fp_CommandLine_VersionManager_ListVersions(_Params, _pCommandLine);
				}
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
							.f_ToStringColored("    ", CDistributedAppActor::fs_ColorEnabledDefault()).f_Replace("\r\n", "\r").f_Replace("\n", "\r")
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
					return fp_CommandLine_VersionManager_UploadVersion(_Params, _pCommandLine);
				}
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
					return fp_CommandLine_VersionManager_ChangeTags(_Params, _pCommandLine);
				}
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
					return fp_CommandLine_VersionManager_DownloadVersion(_Params, _pCommandLine);
				}
			)
		;
	}
	
	TCContinuation<void> CCloudClientAppActor::fp_VersionManager_SubscribeToServers()
	{
		if (!mp_VersionManagers.f_IsEmpty())
			return fg_Explicit();
		DMibLogWithCategory(Malterlib/Cloud/CloudClient, Info, "Subscribing to version managers");
		
		TCContinuation<void> Continuation;
		
		mp_State.m_TrustManager
			(
				&CDistributedActorTrustManager::f_SubscribeTrustedActors<NCloud::CVersionManager>
				, "com.malterlib/Cloud/VersionManager"
				, fg_ThisActor(this)
			)
			> [this, Continuation](TCAsyncResult<TCTrustedActorSubscription<CVersionManager>> &&_Subscription)
			{
				if (!_Subscription)
				{
					DMibLogWithCategory(Malterlib/Cloud/CloudClient, Error, "Failed to subscribe to version managers: {}", _Subscription.f_GetExceptionStr());
					Continuation.f_SetException(_Subscription);
					return;
				}
				mp_VersionManagers = fg_Move(*_Subscription);
				if (mp_VersionManagers.m_Actors.f_IsEmpty())
				{
					Continuation.f_SetException(DMibErrorInstance("Not connected to any version managers, or they are not trusted for 'com.malterlib/Cloud/VersionManager' namespace"));
					return;
				}
				Continuation.f_SetResult();
			}
		;
		return Continuation;
	}
	
	TCContinuation<uint32> CCloudClientAppActor::fp_CommandLine_VersionManager_ListApplications(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		TCContinuation<uint32> Continuation;
		CStr Host = _Params["VersionManagerHost"].f_String();
		
		fg_ThisActor(this)(&CCloudClientAppActor::fp_VersionManager_SubscribeToServers).f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for version managers") 
			> Continuation / [=]
			{
				TCActorResultMap<CHostInfo, CVersionManager::CListApplications::CResult> Applications;
				
				for (auto &TrustedActor : mp_VersionManagers.m_Actors)
				{
					if (!Host.f_IsEmpty() && TrustedActor.m_TrustInfo.m_HostInfo.m_HostID != Host)
						continue;
					CVersionManager::CListApplications Command;
					DMibCallActor
						(
							TrustedActor.m_Actor
							, CVersionManager::f_ListApplications
							, fg_Move(Command)
						)
						.f_Timeout(mp_Timeout, "Timed out waiting for version manager to reply")
						> Applications.f_AddResult(TrustedActor.m_TrustInfo.m_HostInfo)
					;
				}
				
				Applications.f_GetResults() > Continuation / [=](TCMap<CHostInfo, TCAsyncResult<CVersionManager::CListApplications::CResult>> &&_Results)
					{
						for (auto &Result : _Results)
						{
							auto &HostInfo = _Results.fs_GetKey(Result);
							*_pCommandLine += "{}\n"_f << HostInfo.f_GetDesc();
							if (!Result)
							{
								*_pCommandLine %= "    Failed getting applicatinos for this host: {}\n"_f << Result.f_GetExceptionStr();
								continue;
							}
							for (auto &Application : Result->m_Applications)
								*_pCommandLine += "    {}\n"_f << Application;
						}
						Continuation.f_SetResult(0);
					}
				;
			}
		;
		return Continuation;
	}
	
	TCContinuation<uint32> CCloudClientAppActor::fp_CommandLine_VersionManager_ListVersions(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		TCContinuation<uint32> Continuation;
		
		CStr Host = _Params["VersionManagerHost"].f_String();
		CStr Application = _Params["Application"].f_String();
		bool bVerbose = _Params["Verbose"].f_Boolean();
		
		fg_ThisActor(this)(&CCloudClientAppActor::fp_VersionManager_SubscribeToServers).f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for version managers")
			> Continuation / [=]
			{
				TCActorResultMap<CHostInfo, CVersionManager::CListVersions::CResult> Versions;
				
				for (auto &TrustedVersionManager : mp_VersionManagers.m_Actors)
				{
					if (!Host.f_IsEmpty() && TrustedVersionManager.m_TrustInfo.m_HostInfo.m_HostID != Host)
						continue;
					CVersionManager::CListVersions Options;
					Options.m_ForApplication = Application;
					DMibCallActor
						(
							TrustedVersionManager.m_Actor
							, CVersionManager::f_ListVersions
							, fg_Move(Options)
						)
						.f_Timeout(mp_Timeout, "Timed out waiting for version manager to reply")
						> Versions.f_AddResult(TrustedVersionManager.m_TrustInfo.m_HostInfo)
					;
				}
				
				Versions.f_GetResults() > Continuation / [=](TCMap<CHostInfo, TCAsyncResult<CVersionManager::CListVersions::CResult>> &&_Results)
					{
						for (auto &Result : _Results)
						{
							auto &HostInfo = _Results.fs_GetKey(Result);
							*_pCommandLine += "{}\n"_f << HostInfo.f_GetDesc();
							if (!Result)
							{
								*_pCommandLine %= "    Failed getting versions for this host: {}\n"_f << Result.f_GetExceptionStr();
								continue;
							}
							for (auto &Versions : Result->m_Versions)
							{
								auto &Application = Result->m_Versions.fs_GetKey(Versions);
								*_pCommandLine += "    {}\n"_f << Application;
								for (auto &Version : Versions)
								{
									auto &VersionID = Versions.fs_GetKey(Version);
									*_pCommandLine += "        {sl20,a-} {sl10,a-} {tc6,sl24,a-} {sl15,a-} {ns ,sl12} bytes ({sl5} files)   {vs,vb,a-}   {}\n"_f
										<< VersionID.m_VersionID
										<< Version.m_Configuration
										<< Version.m_Time.f_ToLocal()
										<< VersionID.m_Platform
										<< Version.m_nBytes
										<< Version.m_nFiles
										<< Version.m_Tags
										<< Version.m_RetrySequence
									;
									if (bVerbose && Version.m_ExtraInfo.f_IsObject() && Version.m_ExtraInfo.f_Object().f_OrderedIterator())
									{
										CStr JSONString = Version.m_ExtraInfo.f_ToString("    ");
										while (!JSONString.f_IsEmpty())
										{
											CStr Line = fg_GetStrLineSep(JSONString); 
											*_pCommandLine += "            {}\n"_f << Line;
										}
									}
								}
							}
						}
						Continuation.f_SetResult(0);
					}
				;
			}
		;
		return Continuation;
	}
	
	TCContinuation<uint32> CCloudClientAppActor::fp_CommandLine_VersionManager_UploadVersion(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		CStr Source = CFile::fs_GetFullPath(_Params["Source"].f_String(), _Params["CurrentDirectory"].f_String());
		if (Source.f_IsEmpty())
			return DMibErrorInstance("Source must be specified");

		TCContinuation<uint32> Continuation;
		
		CStr SettingsFile = _Params["SettingsFile"].f_String();
		if (!SettingsFile.f_IsEmpty())
			SettingsFile = CFile::fs_GetFullPath(SettingsFile, _Params["CurrentDirectory"].f_String());

		TCContinuation<TCVariant<CStr, CEJSON>> SettingsFileContinuation;
		if (!SettingsFile.f_IsEmpty())
			SettingsFileContinuation.f_SetResult(SettingsFile);
		else if (_Params["SettingsFileFromPackage"].f_Boolean() && Source.f_EndsWith(".tar.gz"))
		{
			// tar -xOf AppManager.tar.gz VersionInfo.json

			auto &LaunchActor = mp_LaunchActors.f_Insert() = fg_Construct();

			CProcessLaunchActor::CSimpleLaunch Launch
				{
	#ifdef DPlatformFamily_Windows
					"tar.exe"
	#else
					"tar"
	#endif
					,
					{
						"-xOf"
	#ifdef DPlatformFamily_Windows
						, NFile::NPlatform::fg_ConvertToMinGWPath(Source)
	#else
						, Source
	#endif
						, "VersionInfo.json"
					}
					, CFile::fs_GetPath(Source)
					, CProcessLaunchActor::ESimpleLaunchFlag_GenerateExceptionOnNonZeroExitCode
				}
			;

			LaunchActor(&CProcessLaunchActor::f_LaunchSimple, fg_Move(Launch))
				> [SettingsFileContinuation, _pCommandLine](TCAsyncResult<CProcessLaunchActor::CSimpleLaunchResult> &&_LaunchResult)
				{
					CEJSON VersionInfo = EJSONType_Object;
					if (_LaunchResult)
					{
						try
						{
							VersionInfo = CEJSON::fs_FromString(_LaunchResult->f_GetStdOut());
						}
						catch (CException const &_Exception)
						{
							*_pCommandLine %= "Failed to parse version info from VersionInfo.json in package (Disable with --no-settings-file-from-package): {}"_f << _Exception;
						}
					}
					else
						*_pCommandLine %= "Failed to extract version info from package (Disable with --no-settings-file-from-package): {}"_f << _LaunchResult.f_GetExceptionStr();

					SettingsFileContinuation.f_SetResult(fg_Move(VersionInfo));
				}
			;
		}
		else
			SettingsFileContinuation.f_SetResult();

		SettingsFileContinuation > Continuation / [=](TCVariant<CStr, CEJSON> const &_SettingsFileOrSettings)
			{
				TCContinuation<CEJSON> SettingsContinuation;
				if (_SettingsFileOrSettings.f_IsOfType<CEJSON>())
					SettingsContinuation.f_SetResult(_SettingsFileOrSettings.f_Get<1>());
				else
				{
					CStr SettingsFile = _SettingsFileOrSettings.f_Get<0>();
					if (!SettingsFile.f_IsEmpty())
					{
						g_Dispatch(mp_VersionManagerHelper.f_GetFileActor()) > [SettingsFile]
							{
								return CEJSON::fs_FromString(CFile::fs_ReadStringFromFile(SettingsFile), SettingsFile);
							}
							> [SettingsContinuation](TCAsyncResult<CEJSON> &&_Settings)
							{
								if (!_Settings)
								{
									SettingsContinuation.f_SetException(DMibErrorInstance(fg_Format("Error reading settings file: {}", _Settings.f_GetExceptionStr())));
									return;
								}
								if (!_Settings->f_IsObject())
								{
									SettingsContinuation.f_SetException(DMibErrorInstance("Settings file does not contain a valid JSON object"));
									return;
								}
								SettingsContinuation.f_SetResult(*_Settings);
							}
						;
					}
					else
						SettingsContinuation.f_SetResult(EJSONType_Object);
				}

				SettingsContinuation > Continuation / [=](CEJSON &&_Settings)
					{
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

						fApplySettings(_Settings);
						fApplySettings(_Params);

						CStr Host = _Params["VersionManagerHost"].f_String();
						CTime Time = _Params["Time"].f_Date();
						uint64 QueueSize = _Params["TransferQueueSize"].f_Integer();
						bool bForce = _Params["Force"].f_Boolean();
						if (QueueSize < 128*1024)
							QueueSize = 128*1024;

						auto fReportError = [Continuation](CStr const &_Error)
							{
								Continuation.f_SetException(DMibErrorInstance(_Error));
							}
						;

						TCSet<CStr> Tags;
						for (auto &TagJSON : _Params["Tags"].f_Array())
						{
							CStr const &Tag = TagJSON.f_String();
							if (!CVersionManager::fs_IsValidTag(Tag))
								return fReportError(fg_Format("'{}' is not a valid tag", Tag));
							Tags[Tag];
						}

						if (Application.f_IsEmpty())
							return fReportError("Application must be specified");
						if (Version.f_IsEmpty())
							return fReportError("Version must be specified");

						if (!CVersionManager::fs_IsValidApplicationName(Application))
							return fReportError("Application format is invalid");

						CVersionManager::CVersionIDAndPlatform VersionID;
						{
							CStr Error;
							if (!CVersionManager::fs_IsValidVersionIdentifier(Version, Error, &VersionID.m_VersionID))
								return fReportError(fg_Format("Version identifier format is invalid: {}", Error));
						}

						if (!CVersionManager::fs_IsValidPlatform(Platform))
							return fReportError("Invalid version platform format");

						VersionID.m_Platform = Platform;

						TCContinuation<CTime> TimeContinuation;

						if (Time.f_IsValid())
							TimeContinuation.f_SetResult(Time);
						else
						{
							g_Dispatch(mp_VersionManagerHelper.f_GetFileActor()) > [Source]() -> CTime
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
								> TimeContinuation % "Failed to deduce backup time";
							;
						}

						TimeContinuation > Continuation / [=](CTime &&_Time)
							{
								fp_VersionManager_SubscribeToServers().f_Dispatch().f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for version managers") > Continuation / [=]
									{
										CStr Error;
										auto *pVersionManager = mp_VersionManagers.f_GetOneActor(Host, Error);
										if (!pVersionManager)
										{
											Continuation.f_SetException
												(
													DMibErrorInstance
													(
														fg_Format
														(
															"Error selecting version manager for host '{}': {}. Connection might have failed. Use --log-to-stderr to see more info."
															, Host
															, Error
														)
													)
												)
											;
											return;
										}

										CVersionManager::CVersionInformation VersionInfo;
										VersionInfo.m_Time = _Time;
										VersionInfo.m_Configuration = Configuration;
										VersionInfo.m_ExtraInfo = ExtraInfo;
										VersionInfo.m_Tags = Tags;

										mp_VersionManagerHelper.f_Upload
											(
												pVersionManager->m_Actor
												, Application
												, VersionID
												, VersionInfo
												, Source
												, bForce ? CVersionManager::CStartUploadVersion::EFlag_ForceOverwrite : CVersionManager::CStartUploadVersion::EFlag_None
												, QueueSize
											)
											> Continuation / [=](CVersionManagerHelper::CUploadResult &&_UploadResult)
											{
												if (!_UploadResult.m_DeniedTags.f_IsEmpty())
													*_pCommandLine %= "The following tags were denied: {vs,vb}\n"_f << _UploadResult.m_DeniedTags;

												*_pCommandLine += "Upload finished transferring: {ns } bytes at {fe2} MB/s\n"_f
													<< _UploadResult.m_TransferResult.m_nBytes
													<< _UploadResult.m_TransferResult.f_BytesPerSecond()/1'000'000.0
												;
												Continuation.f_SetResult(0);
											}
										;
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
	
	TCContinuation<uint32> CCloudClientAppActor::fp_CommandLine_VersionManager_DownloadVersion(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		TCContinuation<uint32> Continuation;
		
		CStr Host = _Params["VersionManagerHost"].f_String();
		CStr Application = _Params["Application"].f_String();
		CStr Version = _Params["Version"].f_String();
		CStr DestinationDirectory = _Params["DestinationDirectory"].f_String();
		CStr Platform = _Params["Platform"].f_String();
		uint64 QueueSize = _Params["TransferQueueSize"].f_Integer();
		if (QueueSize < 128*1024)
			QueueSize = 128*1024;
		
		if (Application.f_IsEmpty())
			return DMibErrorInstance("Application must be specified");
		if (Version.f_IsEmpty())
			return DMibErrorInstance("Version must be specified");
		if (DestinationDirectory.f_IsEmpty())
			return DMibErrorInstance("Destination directory must be specified");
		
		if (!CVersionManager::fs_IsValidApplicationName(Application))
			return DMibErrorInstance("Application format is invalid");
		
		CVersionManager::CVersionIDAndPlatform VersionID;
		{
			CStr Error; 
			if (!CVersionManager::fs_IsValidVersionIdentifier(Version, Error, &VersionID.m_VersionID))
				return DMibErrorInstance(fg_Format("Version identifier format is invalid: {}", Error));
		}
		if (!CVersionManager::fs_IsValidPlatform(Platform))
			return DMibErrorInstance("Version platform format is invalid");
		
		VersionID.m_Platform = Platform;
		
		fg_ThisActor(this)(&CCloudClientAppActor::fp_VersionManager_SubscribeToServers).f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for version managers") 
			> Continuation / [=]
			{
				CStr Error;
				auto *pVersionManager = mp_VersionManagers.f_GetOneActor(Host, Error);
				if (!pVersionManager)
				{
					Continuation.f_SetException
						(
							DMibErrorInstance(fg_Format("Error selecting version manager: {}. Connection might have failed. Use --log-to-stderr to see more info.", Error))
						)
					;
					return;
				}
				
				mp_VersionManagerHelper.f_Download(pVersionManager->m_Actor, Application, VersionID, DestinationDirectory, CFileTransferReceive::EReceiveFlag_IgnoreExisting, QueueSize)
					> Continuation / [=](CFileTransferResult &&_Results)
					{
						*_pCommandLine += "Download finished transferring: {ns } bytes at {fe2} MB/s\n"_f
							<< _Results.m_nBytes
							<< _Results.f_BytesPerSecond()/1'000'000.0
						;
						Continuation.f_SetResult(0);
					}
				;
			}
		;
		return Continuation;
	}
	
	TCContinuation<uint32> CCloudClientAppActor::fp_CommandLine_VersionManager_ChangeTags(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		TCContinuation<uint32> Continuation;
		
		CStr Host = _Params["VersionManagerHost"].f_String();
		CStr Application = _Params["Application"].f_String();
		CStr Version = _Params["Version"].f_String();
		CStr Platform = _Params["Platform"].f_String();
		bool bRetryUpgrade = _Params["RetryUpgrade"].f_Boolean();
		
		if (Application.f_IsEmpty())
			return DMibErrorInstance("Application must be specified");
		if (Version.f_IsEmpty())
			return DMibErrorInstance("Version must be specified");
		
		if (!CVersionManager::fs_IsValidApplicationName(Application))
			return DMibErrorInstance("Application format is invalid");
		
		CVersionManager::CVersionID VersionID;
		{
			CStr Error; 
			if (!CVersionManager::fs_IsValidVersionIdentifier(Version, Error, &VersionID))
				return DMibErrorInstance(fg_Format("Version identifier format is invalid: {}", Error));
		}
		if (!Platform.f_IsEmpty() && !CVersionManager::fs_IsValidPlatform(Platform))
			return DMibErrorInstance("Version platform format is invalid");

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
		try
		{
			AddTags = fParseTags(_Params["AddTags"]);
			RemoveTags = fParseTags(_Params["RemoveTags"]);
		}
		catch (CException const &_Error)
		{
			return _Error;
		}
		
		if (AddTags.f_IsEmpty() && RemoveTags.f_IsEmpty() && !bRetryUpgrade)
			return DMibErrorInstance("No changes specified. Specify tags to add and remove with --add and --remove, or specify --retry-upgrade");
		
		fg_ThisActor(this)(&CCloudClientAppActor::fp_VersionManager_SubscribeToServers).f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for version managers") 
			> Continuation / [=]
			{
				CStr Error;
				auto *pVersionManager = mp_VersionManagers.f_GetOneActor(Host, Error);
				if (!pVersionManager)
				{
					Continuation.f_SetException
						(
							DMibErrorInstance(fg_Format("Error selecting version manager: {}. Connection might have failed. Use --log-to-stderr to see more info.", Error))
						)
					;
					return;
				}

				CVersionManager::CChangeTags ChangeTags;
				ChangeTags.m_Application = Application;
				ChangeTags.m_VersionID = VersionID;
				ChangeTags.m_Platform = Platform;
				ChangeTags.m_AddTags = AddTags;
				ChangeTags.m_RemoveTags = RemoveTags;
				ChangeTags.m_bIncreaseRetrySequence = bRetryUpgrade;

				DMibCallActor
					(
						pVersionManager->m_Actor
						, CVersionManager::f_ChangeTags
						, fg_Move(ChangeTags)
					)
					.f_Timeout(mp_Timeout, "Timed out waiting for version manager to reply")
					> Continuation % "Failed to change tags" / [=](CVersionManager::CChangeTags::CResult &&_Result)
					{
						if (!_Result.m_DeniedTags.f_IsEmpty())
							*_pCommandLine %= "The following tags were denied: {vs,vb}\n"_f << _Result.m_DeniedTags;
						Continuation.f_SetResult(0);
					}
				;
			}
		;
		return Continuation;
	}
}
