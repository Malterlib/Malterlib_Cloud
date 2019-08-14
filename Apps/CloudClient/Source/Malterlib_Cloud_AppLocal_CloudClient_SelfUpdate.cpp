// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Cloud/VersionManager>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Process/ProcessLaunch>

#ifdef DPlatformFamily_Windows
#include <Mib/Core/PlatformSpecific/WindowsFilePath>
#endif

#include "Malterlib_Cloud_AppLocal_CloudClient.h"

namespace NMib::NCloud::NCloudClient
{
	auto CCloudClientAppLocalActor::fp_GetSelfUpdateVersion() -> TCFuture<CSelfUpdateVersion>
	{
		co_await self(&CCloudClientAppLocalActor::fp_VersionManager_SubscribeToServers).f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for version managers");

		TCSharedPointer<NContainer::TCMap<TCDistributedActor<CVersionManager>, TCVector<CVersionManager::CNewVersionNotification>>> pVersions = fg_Construct();
		TCActorResultVector<CVersionManager::CSubscribeToUpdates::CResult> SubscribeResults;

		for (auto &Actor : mp_VersionManagers.m_Actors)
		{
			CVersionManager::CSubscribeToUpdates SubscriptionParams;
			SubscriptionParams.m_Application = "MalterlibCloud"; // All applications we have access to
			SubscriptionParams.m_nInitial = 1;
			SubscriptionParams.m_Platforms[DMalterlibCloudPlatform];
			SubscriptionParams.m_Tags["ClientSelfUpdate"];
			SubscriptionParams.m_fOnNewVersions = g_ActorFunctor / [pVersions, Actor = Actor.m_Actor, AllowDestroy = g_AllowWrongThreadDestroy]
				(CVersionManager::CNewVersionNotifications &&_NewVersions)
				-> NConcurrency::TCFuture<CVersionManager::CNewVersionNotifications::CResult>
				{
					TCPromise<CVersionManager::CNewVersionNotifications::CResult> Promise;
					(*pVersions)[Actor].f_Insert(fg_Move(_NewVersions.m_NewVersions));
					return Promise <<= CVersionManager::CNewVersionNotifications::CResult();
				}
			;
			Actor.m_Actor.f_CallActor(&CVersionManager::f_SubscribeToUpdates)(fg_Move(SubscriptionParams)) > SubscribeResults.f_AddResult();
		}

		auto Results = co_await SubscribeResults.f_GetResults();

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
			co_return DMibErrorInstance("No new version was found");

		if
			(
				BestVersion.m_VersionIDAndPlatform.m_VersionID.m_Branch == (*g_CloudVersion).m_Version.m_Branch
				&& BestVersion.m_VersionIDAndPlatform.m_VersionID <= (*g_CloudVersion).m_Version
			)
		{
			co_return DMibErrorInstance("No new version was found");
		}

		CSelfUpdateVersion SelfUpdateVersion;
		SelfUpdateVersion.m_VersionID = BestVersion.m_VersionIDAndPlatform.m_VersionID;
		SelfUpdateVersion.m_Actor = BestActor;

		co_return fg_Move(SelfUpdateVersion);
	}

	TCFuture<void> CCloudClientAppLocalActor::fp_PreRunCommandLine
		(
			 CStr const &_Command
			 , NEncoding::CEJSON const &_Params
			 , NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine
		)
	{
		if (!_Params["SelfUpdateCheck"].f_Boolean() || _Command == "--self-update")
			co_return {};

		auto Version = co_await fp_GetSelfUpdateVersion().f_Wrap();
		if (!Version)
			co_return {};

		*_pCommandLine %= "\nA new version {} is available for update by running with --self-update\n\n"_f << Version->m_VersionID;

		co_return {};
	}

	TCFuture<uint32> CCloudClientAppLocalActor::fp_CommandLine_SelfUpdate(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		DMibCheck(mp_State.m_RootDirectory == CFile::fs_GetProgramDirectory());

		auto Version = co_await fp_GetSelfUpdateVersion();

		CVersionManager::CVersionIDAndPlatform VersionID;
		VersionID.m_VersionID = Version.m_VersionID;
		VersionID.m_Platform = DMalterlibCloudPlatform;
		CStr DestinationDirectory = CFile::fs_GetProgramDirectory() + "/SelfUpdate";

		auto TransferResult = co_await mp_VersionManagerHelper.f_Download(Version.m_Actor, "MalterlibCloud", VersionID, DestinationDirectory);
		co_await
			(
			 	g_Dispatch(mp_VersionManagerHelper.f_GetFileActor()) / [DestinationDirectory]
				{
					CStr StdOut;
					CStr StdErr;
					uint32 ExitCode = 255;
					CProcessLaunchParams LaunchParams;
					LaunchParams.m_WorkingDirectory = DestinationDirectory;
					CStr PackageFile = DestinationDirectory + "/MalterlibCloud.tar.gz";
					bool bLaunchSuccess = CProcessLaunch::fs_LaunchBlock
						(
							CFile::fs_GetProgramDirectory() / "bin/bsdtar"
							, fg_CreateVector<CStr>
							(
								"--no-same-owner"
								, "-xf"
								, PackageFile
							)
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
						CFile::fs_DiffCopyFileOrDirectory(File, CFile::fs_AppendPath(ProgramDirectory, RelativePath), nullptr, {}, 0.0);
					}

					CFile::fs_DeleteDirectoryRecursive(DestinationDirectory);
				}
			 	% "Failed to update version"
			)
		;

		*_pCommandLine %= "Downloaded {ns } bytes at {fe2} MB/s\n"_f << TransferResult.m_nBytes << TransferResult.f_BytesPerSecond()/1'000'000.0;
		*_pCommandLine %= "Updated to version {}\n"_f << Version.m_VersionID;

		co_return 0;
	}
}
