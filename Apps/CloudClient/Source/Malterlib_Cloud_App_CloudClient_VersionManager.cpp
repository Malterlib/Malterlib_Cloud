// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Cloud/VersionManager>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/Actor/Timer>
#include <Mib/Encoding/JSONShortcuts>

#include "Malterlib_Cloud_App_CloudClient.h"

namespace NMib::NCloud::NCloudClient
{
	void CCloudClientAppActor::fp_VersionManager_RegisterCommands(CDistributedAppCommandLineSpecification::CSection _Section)
	{
		auto VersionManagerHost = "VersionManagerHost?"_=
			{
				"Names"_= {"--host"}
				, "Default"_= ""
				, "Description"_= "Limit query to only specified host ID"
			}
		;

		
		_Section.f_RegisterCommand
			(
				{
					"Names"_= {"--version-manager-list-applications"}
					, "Description"_= "List applications available on remote version managers"
					, "Options"_=
					{
						VersionManagerHost
					}
				}
				, [this](CEJSON const &_Params)
				{
					return fp_CommandLine_VersionManager_ListApplications(_Params);
				}
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_= {"--version-manager-list-versions"}
					, "Description"_= "List application versions available on remote version managers"
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
				, [this](CEJSON const &_Params)
				{
					return fp_CommandLine_VersionManager_ListVersions(_Params);
				}
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_= {"--version-manager-upload-version"}
					, "Description"_= "Upload a version to remote version manager\n"
					, "Options"_=
					{
						"VersionManagerHost"_=
						{
							"Names"_= {"--host"}
							, "Type"_= ""
							, "Description"_= "The host ID of the host to upload the version to."
						}					
						, "Application"_=
						{
							"Names"_= {"--application"}
							, "Type"_= ""
							, "Description"_= "The application to upload a version to."
						}
						, "Version"_=
						{
							"Names"_= {"--version"}
							, "Type"_= ""
							, "Description"_= "The version to upload.\n"
								"This is in the format 'Branch/Major.Minor.Patch' as displayed in the output from --version-manager-list-versions.\n"
						}
						, "Time?"_=
						{
							"Names"_= {"--time"}
							, "Default"_= CTime()
							, "Description"_= "The time for this version.\n"
								"By default the time will be deduced from the modification time of the directory or file uploaded.\n"
						}
						, "Configuration?"_=
						{
							"Names"_= {"--configuration"}
							, "Default"_= ""
							, "Description"_= "The configuration for this build. Could be for example Debug or Release.\n"
						}
						, "ExtraInfo?"_=
						{
							"Names"_= {"--info"}
							, "Default"_= EJSONType_Object
							, "Description"_= "EJSON formatted extra information.\n"
						}
						, "TransferQueueSize?"_=
						{
							"Names"_= {"--queue-size"}
							, "Default"_= int64(8*1024*1024)
							, "Description"_= "The amount of data to keep in flight while uploading."
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
				, [this](CEJSON const &_Params)
				{
					return fp_CommandLine_VersionManager_UploadVersion(_Params);
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
						"VersionManagerHost"_=
						{
							"Names"_= {"--host"}
							, "Type"_= ""
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
							, "Default"_= ""
							, "Description"_= "The version to download.\n"
								"This is in the format 'Branch/Major.Minor.Patch' as displayed in the output from --version-manager-list-versions.\n"
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
							"Default"_= CFile::fs_GetProgramDirectory()
							, "Description"_= "The directory to download the version to."
						}
					}
				}
				, [this](CEJSON const &_Params)
				{
					return fp_CommandLine_VersionManager_DownloadVersion(_Params);
				}
			)
		;
	}
	
	TCContinuation<void> CCloudClientAppActor::fp_VersionManager_SubscribeToServers()
	{
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
	
	TCContinuation<CDistributedAppCommandLineResults> CCloudClientAppActor::fp_CommandLine_VersionManager_ListApplications(CEJSON const &_Params)
	{
		TCContinuation<CDistributedAppCommandLineResults> Continuation;
		CStr Host = _Params["VersionManagerHost"].f_String();
		
		fg_ThisActor(this)(&CCloudClientAppActor::fp_VersionManager_SubscribeToServers).f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for version managers") 
			> Continuation / [this, Continuation, Host]
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
				
				Applications.f_GetResults() > Continuation / [this, Continuation](TCMap<CHostInfo, TCAsyncResult<CVersionManager::CListApplications::CResult>> &&_Results)
					{
						CDistributedAppCommandLineResults CommandLineResults;
						for (auto &Result : _Results)
						{
							auto &HostInfo = _Results.fs_GetKey(Result);
							CommandLineResults.f_AddStdOut(fg_Format("{}\n", HostInfo.f_GetDesc()));
							if (!Result)
							{
								CommandLineResults.f_AddStdErr(fg_Format("    Failed getting applicatinos for this host: {}\n", Result.f_GetExceptionStr()));
								continue;
							}
							for (auto &Application : Result->m_Applications)
								CommandLineResults.f_AddStdOut(fg_Format("    {}\n", Application));
						}
						Continuation.f_SetResult(fg_Move(CommandLineResults));
					}
				;
			}
		;
		return Continuation;
	}
	
	TCContinuation<CDistributedAppCommandLineResults> CCloudClientAppActor::fp_CommandLine_VersionManager_ListVersions(CEJSON const &_Params)
	{
		TCContinuation<CDistributedAppCommandLineResults> Continuation;
		
		CStr Host = _Params["VersionManagerHost"].f_String();
		CStr Application = _Params["Application"].f_String();
		bool bVerbose = _Params["Verbose"].f_Boolean();
		
		fg_ThisActor(this)(&CCloudClientAppActor::fp_VersionManager_SubscribeToServers).f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for version managers") 
			> Continuation / [this, Continuation, Application, Host, bVerbose]
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
				
				Versions.f_GetResults() > Continuation / [this, Continuation, bVerbose](TCMap<CHostInfo, TCAsyncResult<CVersionManager::CListVersions::CResult>> &&_Results)
					{
						CDistributedAppCommandLineResults CommandLineResults;
						for (auto &Result : _Results)
						{
							auto &HostInfo = _Results.fs_GetKey(Result);
							CommandLineResults.f_AddStdOut(fg_Format("{}\n", HostInfo.f_GetDesc()));
							if (!Result)
							{
								CommandLineResults.f_AddStdErr(fg_Format("    Failed getting versions for this host: {}\n", Result.f_GetExceptionStr()));
								continue;
							}
							for (auto &Versions : Result->m_Versions)
							{
								auto &Application = Result->m_Versions.fs_GetKey(Versions);
								CommandLineResults.f_AddStdOut(fg_Format("    {}\n", Application));
								for (auto &Version : Versions)
								{
									auto &VersionID = Versions.fs_GetKey(Version);
									CommandLineResults.f_AddStdOut(fg_Format("        {}   {}   {}\n", VersionID, Version.m_Configuration, Version.m_Time));
									if (bVerbose && Version.m_ExtraInfo.f_IsObject() && Version.m_ExtraInfo.f_Object().f_OrderedIterator())
									{
										CStr JSONString = Version.m_ExtraInfo.f_ToString("    ");
										while (!JSONString.f_IsEmpty())
										{
											CStr Line = fg_GetStrLineSep(JSONString); 
											CommandLineResults.f_AddStdOut(fg_Format("            {}\n", Line));
										}
									}
								}
							}
						}
						Continuation.f_SetResult(fg_Move(CommandLineResults));
					}
				;
			}
		;
		return Continuation;
	}
	
	TCContinuation<CDistributedAppCommandLineResults> CCloudClientAppActor::fp_CommandLine_VersionManager_UploadVersion(CEJSON const &_Params)
	{
		TCContinuation<CDistributedAppCommandLineResults> Continuation;
		
		CStr Host = _Params["VersionManagerHost"].f_String();
		CStr Application = _Params["Application"].f_String();
		CStr Version = _Params["Version"].f_String();
		CStr Source = _Params["Source"].f_String();
		CTime Time = _Params["Time"].f_Date();
		CStr Configuration = _Params["Configuration"].f_String();
		CEJSON ExtraInfo = _Params["ExtraInfo"];
		uint64 QueueSize = _Params["TransferQueueSize"].f_Integer();
		if (QueueSize < 128*1024)
			QueueSize = 128*1024;
		
		if (Host.f_IsEmpty())
			return DMibErrorInstance("Host must be specified");
		if (Application.f_IsEmpty())
			return DMibErrorInstance("Application must be specified");
		if (Version.f_IsEmpty())
			return DMibErrorInstance("Version must be specified");
		if (Source.f_IsEmpty())
			return DMibErrorInstance("Source must be specified");
		
		if (!CVersionManager::fs_IsValidApplicationName(Application))
			return DMibErrorInstance("Application format is invalid");
		
		CVersionManager::CVersionIdentifier VersionID;
		{
			CStr Error; 
			if (!CVersionManager::fs_IsValidVersionIdentifier(Version, Error, &VersionID))
				return DMibErrorInstance(fg_Format("Version identifier format is invalid: {}", Error));
		}
		
		fg_ThisActor(this)(&CCloudClientAppActor::fp_VersionManager_SubscribeToServers).f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for version managers") 
			> Continuation / [this, Continuation, Application, Host, VersionID, QueueSize, Source, Time, Configuration, ExtraInfo]
			{
				TCActorResultMap<CHostInfo, CVersionManager::CListVersions::CResult> Versions;

				TCDistributedActor<CVersionManager> OneVersionManager;
				CTrustedActorInfo ActorInfo;
				for (auto &TrustedVersionManager : mp_VersionManagers.m_Actors)
				{
					if (TrustedVersionManager.m_TrustInfo.m_HostInfo.m_HostID != Host)
						continue;
					ActorInfo = TrustedVersionManager.m_TrustInfo;
					OneVersionManager = TrustedVersionManager.m_Actor;
					break;
				}
			
				if (!OneVersionManager)
				{
					Continuation.f_SetException(DMibErrorInstance("No suitable version manager found on this host, or connection failed. Use --log-to-stderr to see more info."));
					return;
				}

				mp_UploadVersionSend = fg_ConstructActor<CFileTransferSend>(Source);
				
				CVersionManager::CStartUploadVersion StartUpload;
				StartUpload.m_Application = Application;
				StartUpload.m_VersionID = VersionID;
				StartUpload.m_VersionInfo.m_Time = Time;
				StartUpload.m_VersionInfo.m_Configuration = Configuration;
				StartUpload.m_VersionInfo.m_ExtraInfo = ExtraInfo;
				StartUpload.m_DispatchActor = fg_ThisActor(this);
				StartUpload.m_fStartTransfer = [this](CVersionManager::CStartUploadTransfer &&_Params) -> NConcurrency::TCContinuation<CVersionManager::CStartUploadTransfer::CResult>
					{
						NConcurrency::TCContinuation<CVersionManager::CStartUploadTransfer::CResult> StartTransferContinuation;
						mp_UploadVersionSend(&CFileTransferSend::f_SendFiles, fg_Move(_Params.m_TransferContext)) 
							> StartTransferContinuation / [this, StartTransferContinuation](NConcurrency::CActorSubscription &&_Subscription)
							{
								CVersionManager::CStartUploadTransfer::CResult Result;
								Result.m_Subscription = fg_Move(_Subscription);
								StartTransferContinuation.f_SetResult(fg_Move(Result));
							}
						;
						return StartTransferContinuation;
					}
				;

				DMibCallActor
					(
						OneVersionManager
						, CVersionManager::f_UploadVersion
						, fg_Move(StartUpload)
					)
					.f_Timeout(mp_Timeout, "Timed out waiting for version manager to reply")
					> Continuation % "Failed to start upload on remote server" / [this, Continuation](CVersionManager::CStartUploadVersion::CResult &&_Result)
					{
						mp_UploadVersionSubscription = fg_Move(_Result.m_Subscription);
						mp_UploadVersionSend(&CFileTransferSend::f_GetResult) > [this, Continuation](TCAsyncResult<CFileTransferResult> &&_Results)
							{
								mp_UploadVersionSubscription.f_Clear();
								if (!_Results)
									Continuation.f_SetException(fg_Move(_Results));
								else
								{
									auto &Results = *_Results;
									CDistributedAppCommandLineResults CommandLine;

									CommandLine.f_AddStdOut
										(
											fg_Format
											(
												"Download finished transferring: {} bytes at {fe2} MB/s\n"
												, Results.m_nBytes
												, Results.f_BytesPerSecond()/1'000'000.0
											)
										)
									;
									Continuation.f_SetResult(fg_Move(CommandLine));
								}
							}
						;
					}
				;
			}
		;
		return Continuation;
	}
	
	TCContinuation<CDistributedAppCommandLineResults> CCloudClientAppActor::fp_CommandLine_VersionManager_DownloadVersion(CEJSON const &_Params)
	{
		TCContinuation<CDistributedAppCommandLineResults> Continuation;
		
		CStr Host = _Params["VersionManagerHost"].f_String();
		CStr Application = _Params["Application"].f_String();
		CStr Version = _Params["Version"].f_String();
		CStr DestinationDirectory = _Params["DestinationDirectory"].f_String();
		uint64 QueueSize = _Params["TransferQueueSize"].f_Integer();
		if (QueueSize < 128*1024)
			QueueSize = 128*1024;
		
		if (Host.f_IsEmpty())
			return DMibErrorInstance("Host must be specified");
		if (Application.f_IsEmpty())
			return DMibErrorInstance("Application must be specified");
		if (Version.f_IsEmpty())
			return DMibErrorInstance("Version must be specified");
		if (DestinationDirectory.f_IsEmpty())
			return DMibErrorInstance("Destination directory must be specified");
		
		if (!CVersionManager::fs_IsValidApplicationName(Application))
			return DMibErrorInstance("Application format is invalid");
		
		CVersionManager::CVersionIdentifier VersionID;
		{
			CStr Error; 
			if (!CVersionManager::fs_IsValidVersionIdentifier(Version, Error, &VersionID))
				return DMibErrorInstance(fg_Format("Version identifier format is invalid: {}", Error));
		}
		
		fg_ThisActor(this)(&CCloudClientAppActor::fp_VersionManager_SubscribeToServers).f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for version managers") 
			> Continuation / [this, Continuation, Application, Host, VersionID, QueueSize, DestinationDirectory]
			{
				TCActorResultMap<CHostInfo, CVersionManager::CListVersions::CResult> Versions;

				TCDistributedActor<CVersionManager> OneVersionManager;
				CTrustedActorInfo ActorInfo;
				for (auto &TrustedVersionManager : mp_VersionManagers.m_Actors)
				{
					if (TrustedVersionManager.m_TrustInfo.m_HostInfo.m_HostID != Host)
						continue;
					ActorInfo = TrustedVersionManager.m_TrustInfo;
					OneVersionManager = TrustedVersionManager.m_Actor;
					break;
				}
			
				if (!OneVersionManager)
				{
					Continuation.f_SetException(DMibErrorInstance("No suitable version manager found on this host, or connection failed. Use --log-to-stderr to see more info."));
					return;
				}

				mp_DownloadVersionReceive = fg_ConstructActor<CFileTransferReceive>(DestinationDirectory); 

				mp_DownloadVersionReceive(&CFileTransferReceive::f_ReceiveFiles, QueueSize, true) 
					> Continuation % "Failed to initialize file transfer context" 
					/ [this, Application, VersionID, OneVersionManager, Continuation]
					(CFileTransferContext &&_TransferContext)
					{
						CVersionManager::CStartDownloadVersion StartDownload;
						StartDownload.m_Application = Application;
						StartDownload.m_VersionID = VersionID;
						StartDownload.m_TransferContext = fg_Move(_TransferContext);

						DMibCallActor
							(
								OneVersionManager
								, CVersionManager::f_DownloadVersion
								, fg_Move(StartDownload)
							)
							.f_Timeout(mp_Timeout, "Timed out waiting for version manager to reply")
							> Continuation % "Failed to start download on remote server" / [this, Continuation](CVersionManager::CStartDownloadVersion::CResult &&_Result)
							{
								mp_DownloadVersionSubscription = fg_Move(_Result.m_Subscription);

								mp_DownloadVersionReceive(&CFileTransferReceive::f_GetResult) > [this, Continuation](TCAsyncResult<CFileTransferResult> &&_Results)
									{
										mp_DownloadVersionSubscription.f_Clear();
										if (!_Results)
											Continuation.f_SetException(fg_Move(_Results));
										else
										{
											auto &Results = *_Results;
											CDistributedAppCommandLineResults CommandLine;

											CommandLine.f_AddStdOut
												(
													fg_Format
													(
														"Download finished transferring: {} bytes at {fe2} MB/s\n"
														, Results.m_nBytes
														, Results.f_BytesPerSecond()/1'000'000.0
													)
												)
											;
											Continuation.f_SetResult(fg_Move(CommandLine));
										}
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
