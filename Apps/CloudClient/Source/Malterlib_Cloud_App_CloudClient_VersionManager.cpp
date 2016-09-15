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
						"VersionManagerHost?"_=
						{
							"Names"_= {"--host"}
							, "Default"_= ""
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
						, "Tags?"_=
						{
							"Names"_= {"--tags"}
							, "Default"_= _[_]
							, "Type"_= {""}
							, "Description"_= "The tags to apply to the version.\n"
						}
						, "ExtraInfo?"_=
						{
							"Names"_= {"--info"}
							, "Default"_= EJSONType_Object
							, "Description"_= "EJSON formatted extra information.\n"
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
							, "Description"_= "Internal hidden option to forward current directory"
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
					}
				}
				, [this](CEJSON const &_Params)
				{
					return fp_CommandLine_VersionManager_ChangeTags(_Params);
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
									CommandLineResults.f_AddStdOut
										(
											fg_Format
											(
												"        {sl10,a-} {sl10,a-} {tc6,sl24,a-} {ns ,sl12} bytes ({sl5} files)   {vs,vb,a-}\n"
												, VersionID
												, Version.m_Configuration
												, Version.m_Time.f_ToLocal()
												, Version.m_nBytes
												, Version.m_nFiles
												, Version.m_Tags
											)
										)
									;
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
		CStr Source = CFile::fs_GetFullPath(_Params["Source"].f_String(), _Params["CurrentDirectory"].f_String());
		CTime Time = _Params["Time"].f_Date();
		CStr Configuration = _Params["Configuration"].f_String();
		CEJSON ExtraInfo = _Params["ExtraInfo"];
		uint64 QueueSize = _Params["TransferQueueSize"].f_Integer();
		bool bForce = _Params["Force"].f_Boolean();
		if (QueueSize < 128*1024)
			QueueSize = 128*1024;

		TCSet<CStr> Tags;
		for (auto &TagJSON : _Params["Tags"].f_Array())
		{
			CStr const &Tag = TagJSON.f_String();
			if (!CVersionManager::fs_IsValidTag(Tag))
				return DMibErrorInstance(fg_Format("'{}' is not a valid tag", Tag));
			Tags[Tag];
		}
		
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

		auto fDoCommand = [this, bForce, Continuation, Application, Host, VersionID, QueueSize, Source, Configuration, ExtraInfo, Tags](CTime const &_Time)
			{
				fg_ThisActor(this)(&CCloudClientAppActor::fp_VersionManager_SubscribeToServers).f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for version managers") 
					> Continuation / [this, bForce, Continuation, Application, Host, VersionID, QueueSize, Source, _Time, Configuration, ExtraInfo, Tags]
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

						mp_UploadVersionSend = fg_ConstructActor<CFileTransferSend>(Source);
						
						CVersionManager::CStartUploadVersion StartUpload;
						StartUpload.m_Application = Application;
						StartUpload.m_VersionID = VersionID;
						StartUpload.m_VersionInfo.m_Time = _Time;
						StartUpload.m_VersionInfo.m_Configuration = Configuration;
						StartUpload.m_VersionInfo.m_ExtraInfo = ExtraInfo;
						StartUpload.m_VersionInfo.m_Tags = Tags; 						
						StartUpload.m_QueueSize = QueueSize;
						StartUpload.m_DispatchActor = fg_ThisActor(this);
						if (bForce)
							StartUpload.m_Flags |= CVersionManager::CStartUploadVersion::EFlag_ForceOverwrite; 
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
								pVersionManager->m_Actor
								, CVersionManager::f_UploadVersion
								, fg_Move(StartUpload)
							)
							.f_Timeout(mp_Timeout, "Timed out waiting for version manager to reply")
							> Continuation % "Failed to start upload on remote server" / [this, Continuation](CVersionManager::CStartUploadVersion::CResult &&_Result)
							{
								mp_UploadVersionSubscription = fg_Move(_Result.m_Subscription);
								mp_UploadVersionSend(&CFileTransferSend::f_GetResult) > [this, Continuation, DeniedTags = _Result.m_DeniedTags](TCAsyncResult<CFileTransferResult> &&_Results)
									{
										mp_UploadVersionSubscription.f_Clear();
										if (!_Results)
											Continuation.f_SetException(fg_Move(_Results));
										else
										{
											auto &Results = *_Results;
											CDistributedAppCommandLineResults CommandLine;

											if (!DeniedTags.f_IsEmpty())
												CommandLine.f_AddStdErr(fg_Format("The following tags were denied: {vs,vb}\n", DeniedTags));
											
											CommandLine.f_AddStdOut
												(
													fg_Format
													(
														"Upload finished transferring: {} bytes at {fe2} MB/s\n"
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
		
		if (Time.f_IsValid())
			fDoCommand(Time);
		else
		{
			if (!mp_VersionManagerFile)
				mp_VersionManagerFile = fg_ConstructActor<CSeparateThreadActor>(fg_Construct("Version file actor"));
			
			fg_Dispatch
				(
					mp_VersionManagerFile
					, [Source]() -> CTime
					{
						if (!CFile::fs_FileExists(Source))
							DMibError("Source specified does not exists");
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
				)
				> Continuation % "Failed to deduce backup time" / [fDoCommand](CTime const &_Time)
				{
					fDoCommand(_Time);
				}
			;			
		}
		
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
				
				mp_DownloadVersionReceive = fg_ConstructActor<CFileTransferReceive>(DestinationDirectory); 

				mp_DownloadVersionReceive(&CFileTransferReceive::f_ReceiveFiles, QueueSize, CFileTransferReceive::EReceiveFlag_IgnoreExisting) 
					> Continuation % "Failed to initialize file transfer context" 
					/ [this, Application, VersionID, OneVersionManager = pVersionManager->m_Actor, Continuation]
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
	
	TCContinuation<CDistributedAppCommandLineResults> CCloudClientAppActor::fp_CommandLine_VersionManager_ChangeTags(CEJSON const &_Params)
	{
		TCContinuation<CDistributedAppCommandLineResults> Continuation;
		
		CStr Host = _Params["VersionManagerHost"].f_String();
		CStr Application = _Params["Application"].f_String();
		CStr Version = _Params["Version"].f_String();
		
		if (Application.f_IsEmpty())
			return DMibErrorInstance("Application must be specified");
		if (Version.f_IsEmpty())
			return DMibErrorInstance("Version must be specified");
		
		if (!CVersionManager::fs_IsValidApplicationName(Application))
			return DMibErrorInstance("Application format is invalid");
		
		CVersionManager::CVersionIdentifier VersionID;
		{
			CStr Error; 
			if (!CVersionManager::fs_IsValidVersionIdentifier(Version, Error, &VersionID))
				return DMibErrorInstance(fg_Format("Version identifier format is invalid: {}", Error));
		}

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
		
		if (AddTags.f_IsEmpty() && RemoveTags.f_IsEmpty())
			return DMibErrorInstance("No changes specified. Specify tags to add and remove with --add and --remove");
		
		fg_ThisActor(this)(&CCloudClientAppActor::fp_VersionManager_SubscribeToServers).f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for version managers") 
			> Continuation / [this, Continuation, Application, Host, VersionID, AddTags, RemoveTags]
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
				ChangeTags.m_AddTags = AddTags;
				ChangeTags.m_RemoveTags = RemoveTags;

				DMibCallActor
					(
						pVersionManager->m_Actor
						, CVersionManager::f_ChangeTags
						, fg_Move(ChangeTags)
					)
					.f_Timeout(mp_Timeout, "Timed out waiting for version manager to reply")
					> Continuation % "Failed to change tags" / [this, Continuation](CVersionManager::CChangeTags::CResult &&_Result)
					{
						CDistributedAppCommandLineResults CommandLine;
						if (!_Result.m_DeniedTags.f_IsEmpty())
							CommandLine.f_AddStdErr(fg_Format("The following tags were denied: {vs,vb}\n", _Result.m_DeniedTags));
						Continuation.f_SetResult(fg_Move(CommandLine));
					}
				;
			}
		;
		return Continuation;
	}
}
