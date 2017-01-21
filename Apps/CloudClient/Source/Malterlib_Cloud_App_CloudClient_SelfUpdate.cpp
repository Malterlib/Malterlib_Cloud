// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Cloud/VersionManager>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/Actor/Timer>
#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Process/ProcessLaunch>
#include <CloudVersionInfo.h>

#include "Malterlib_Cloud_App_CloudClient.h"

namespace NMib::NCloud::NCloudClient
{
	auto CCloudClientAppActor::fp_GetSelfUpdateVersion() -> TCContinuation<CSelfUpdateVersion> 
	{
		TCContinuation<CSelfUpdateVersion> Continuation;
		fg_ThisActor(this)(&CCloudClientAppActor::fp_VersionManager_SubscribeToServers).f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for version managers") 
			> [this, Continuation](TCAsyncResult<void> &&_Result)
			{
				if (!_Result)
				{
					Continuation.f_SetException(_Result);
					return;
				}
				
				TCSharedPointer<NContainer::TCMap<TCDistributedActor<CVersionManager>, TCVector<CVersionManager::CNewVersionNotification>>> pVersions = fg_Construct(); 
				TCActorResultVector<CVersionManager::CSubscribeToUpdates::CResult> SubscribeResults;
				
				for (auto &Actor : mp_VersionManagers.m_Actors)
				{
					CVersionManager::CSubscribeToUpdates SubscriptionParams;
					SubscriptionParams.m_Application = "MalterlibCloud"; // All applications we have access to 
					SubscriptionParams.m_nInitial = 1;
					SubscriptionParams.m_DispatchActor = self;
					SubscriptionParams.m_Platforms[DMalterlibCloudPlatform];
					SubscriptionParams.m_Tags["ClientSelfUpdate"];
					SubscriptionParams.m_fOnNewVersions = [this, pVersions, Actor = Actor.m_Actor](CVersionManager::CNewVersionNotifications &&_NewVersions) 
						-> NConcurrency::TCContinuation<CVersionManager::CNewVersionNotifications::CResult>
						{
							(*pVersions)[Actor].f_Insert(fg_Move(_NewVersions.m_NewVersions)); 
							return fg_Explicit(CVersionManager::CNewVersionNotifications::CResult());
						}
					;
					DCallActor(Actor.m_Actor, CVersionManager::f_SubscribeToUpdates, fg_Move(SubscriptionParams)) > SubscribeResults.f_AddResult();
				}
				
				SubscribeResults.f_GetResults() > [Continuation, pVersions](TCAsyncResult<TCVector<TCAsyncResult<CVersionManager::CSubscribeToUpdates::CResult>>> &&_Results)
					{
						if (!_Results)
						{
							Continuation.f_SetException(_Results);
							return;
						}
						auto CurrentVersionInfo = fg_GetCloudVersionInfo();
						NTime::CTime BestVersionTime;
						CVersionManager::CNewVersionNotification BestVersion;
						TCDistributedActor<CVersionManager> BestActor;
						for (auto &ActorVersions : *pVersions)
						{
							for (auto &Version : ActorVersions)
							{
								if (!Version.m_VersionInfo.m_Time.f_IsValid())
									continue;
								if (!BestVersionTime.f_IsValid() || Version.m_VersionInfo.m_Time > BestVersionTime)
								{
									BestVersionTime = Version.m_VersionInfo.m_Time;
									BestVersion = Version;
									BestActor = pVersions->fs_GetKey(ActorVersions); 
								}
							}
						}
						
						if (!BestVersionTime.f_IsValid())
						{
							Continuation.f_SetException(DMibErrorInstance("No new version was found"));
							return;
						}

						if 
							(
								BestVersion.m_VersionIDAndPlatform.m_VersionID.m_Branch == CurrentVersionInfo.m_Version.m_Branch
								&& BestVersion.m_VersionIDAndPlatform.m_VersionID <= CurrentVersionInfo.m_Version
							)
						{
							Continuation.f_SetException(DMibErrorInstance("No new version was found"));
							return;
						}
						CSelfUpdateVersion SelfUpdateVersion;
						SelfUpdateVersion.m_VersionID = BestVersion.m_VersionIDAndPlatform.m_VersionID;
						SelfUpdateVersion.m_Actor = BestActor;
						Continuation.f_SetResult(fg_Move(SelfUpdateVersion));
					}
				;
			}
		;
		return Continuation;
	}

	TCContinuation<CDistributedAppCommandLineResults> CCloudClientAppActor::fp_PreRunCommandLine(CStr const &_Command, NEncoding::CEJSON const &_Params)
	{
		if (!_Params["SelfUpdateCheck"].f_Boolean() || _Command == "--self-update")
			return fg_Explicit(CDistributedAppCommandLineResults());
		TCContinuation<CDistributedAppCommandLineResults> Continuation;
		fp_GetSelfUpdateVersion() > [Continuation](TCAsyncResult<CSelfUpdateVersion> &&_Version)
			{
				if (!_Version)
				{
					Continuation.f_SetResult(CDistributedAppCommandLineResults());
					return;
				}
				CDistributedAppCommandLineResults CommandLineResults;
				auto &Version = *_Version;
				CommandLineResults.f_AddStdErr(fg_Format("\nA new version {} is available for update by running with --self-update\n\n", Version.m_VersionID));
				Continuation.f_SetResult(fg_Move(CommandLineResults));
			}
		;
		return Continuation;
	}
	
	TCContinuation<CDistributedAppCommandLineResults> CCloudClientAppActor::fp_CommandLine_SelfUpdate(CEJSON const &_Params)
	{
		TCContinuation<CDistributedAppCommandLineResults> Continuation;
		fp_GetSelfUpdateVersion() > [this, Continuation](TCAsyncResult<CSelfUpdateVersion> &&_Version)
			{
				if (!_Version)
				{
					Continuation.f_SetException(_Version);
					return;
				}
				
				auto &Version = *_Version;
				CVersionManager::CVersionIDAndPlatform VersionID;
				VersionID.m_VersionID = Version.m_VersionID;
				VersionID.m_Platform = DMalterlibCloudPlatform;
				CStr DestinationDirectory = CFile::fs_GetProgramDirectory() + "/SelfUpdate"; 

				fp_VersionManager_DownloadVersion
					(
						Version.m_Actor
						, DestinationDirectory
						, "MalterlibCloud"
						, VersionID
					)
					> Continuation / [this, Continuation, Version, DestinationDirectory](CFileTransferResult &&_TransferResult)
					{
						if (!mp_VersionManagerFile)
							mp_VersionManagerFile = fg_ConstructActor<CSeparateThreadActor>(fg_Construct("Version file actor"));
						fg_Dispatch
							(
								mp_VersionManagerFile
								, [DestinationDirectory]
								{
									CStr StdOut;
									CStr StdErr;
									uint32 ExitCode = 255;
									CProcessLaunchParams LaunchParams;
									LaunchParams.m_WorkingDirectory = DestinationDirectory;
									CStr PackageFile = DestinationDirectory + "/MalterlibCloud.tar.gz";
									bool bLaunchSuccess = CProcessLaunch::fs_LaunchBlock
										(
											"tar"
											, fg_CreateVector<CStr>("--no-same-owner", "-xf", PackageFile)
											, StdOut
											, StdErr
											, ExitCode
											, LaunchParams
										)
									;
									
									if (!bLaunchSuccess || ExitCode != 0)
									{
										DMibError(fg_Format("tar ({}) failed to extract new version: {}", ExitCode, StdErr));
									}
									
									CFile::fs_DeleteFile(PackageFile);
									
									auto FilesToUpdate = CFile::fs_FindFiles(DestinationDirectory + "/*", EFileAttrib_File, true);
									CStr ProgramDirectory = CFile::fs_GetProgramDirectory();
									for (auto &File : FilesToUpdate)
									{
										CStr RelativePath = File.f_Extract(DestinationDirectory.f_GetLen() + 1);
										CFile::fs_DiffCopyFileOrDirectory(File, CFile::fs_AppendPath(ProgramDirectory, RelativePath), nullptr, 0.0);
									}
									
									CFile::fs_DeleteDirectoryRecursive(DestinationDirectory);									
								}
							)
							> Continuation % "Failed to update version" / [_TransferResult, Continuation, Version]
							{
								CDistributedAppCommandLineResults CommandLine;

								CommandLine.f_AddStdOut
									(
										fg_Format
										(
											"Downloaded {ns } bytes at {fe2} MB/s\n"
											, _TransferResult.m_nBytes
											, _TransferResult.f_BytesPerSecond()/1'000'000.0
										)
									)
								;
						
								CommandLine.f_AddStdErr(fg_Format("Updated to version {}\n", Version.m_VersionID));
								
								Continuation.f_SetResult(fg_Move(CommandLine));
							}
						;			
					}
				;
			}
		;
		return Continuation;
	}
}
