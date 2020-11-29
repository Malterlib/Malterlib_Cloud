// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/DistributedActorTrustManager>
#include <Mib/Concurrency/DistributedActorTrustManagerDatabases/JSONDirectory>

#include "Malterlib_Cloud_App_VersionManager.h"
#include "Malterlib_Cloud_App_VersionManager_Server.h"

namespace NMib::NCloud::NVersionManager
{
	CVersionManagerDaemonActor::CServer::CServer(CDistributedAppState &_AppState)
		: mp_AppState(_AppState)
	{
#ifdef DPlatformFamily_OSX
		CStr Path = fg_GetSys()->f_GetEnvironmentVariable("PATH");

		CStr OriginalPath = Path;

		if (Path.f_Find("/usr/local/bin") < 0)
			Path = "/usr/local/bin:" + Path;
		if (Path.f_Find("/opt/homebrew/bin") < 0)
			Path = "/opt/homebrew/bin:" + Path;

		if (Path != OriginalPath)
			fg_GetSys()->f_SetEnvironmentVariable("PATH", Path);
#endif
		fp_Init();
	}

	CVersionManagerDaemonActor::CServer::~CServer()
	{
	}

	void CVersionManagerDaemonActor::CServer::fp_Init()
	{
		fg_ThisActor(this)(&CVersionManagerDaemonActor::CServer::fp_FindVersions)
			> [this](TCAsyncResult<void> &&_ResultVersions)
			{
				if (!_ResultVersions)
				{
					DLogWithCategory(Malterlib/Cloud/VersionManager, Error, "Failed to find versions, aborting startup: {}", _ResultVersions.f_GetExceptionStr());
					return;
				}
				self(&CVersionManagerDaemonActor::CServer::fp_SetupPermissions)
					> [this](TCAsyncResult<void> &&_ResultPermissions)
					{
						if (!_ResultPermissions)
						{
							DLogWithCategory(Malterlib/Cloud/VersionManager, Error, "Failed to setup permissions, aborting startup: {}", _ResultPermissions.f_GetExceptionStr());
							return;
						}
						fp_Publish();
					}
				;
			}
		;
	}

	TCSet<CStr> CVersionManagerDaemonActor::CServer::fp_ApplicationSet()
	{
		TCSet<CStr> Return;
		for (auto &Application : mp_Applications)
			Return[Application.f_GetName()];
		return Return;
	}

	TCFuture<TCSet<CStr>> CVersionManagerDaemonActor::CServer::fp_EnumApplications()
	{
		TCPromise<TCSet<CStr>> Promise;

		auto QueryFileActor = fp_GetQueryFileActor();

		fg_Dispatch
			(
				QueryFileActor
				, [RootDirectory = mp_AppState.m_RootDirectory]() -> TCSet<CStr>
				{
					CStr FindPath = RootDirectory + "/Applications";
					CFile::CFindFilesOptions FindOptions(FindPath + "/*", false);
					FindOptions.m_AttribMask = EFileAttrib_Directory;
					auto FoundFiles = CFile::fs_FindFiles(FindOptions);
					TCSet<CStr> Applications;
					for (auto &File : FoundFiles)
					{
						CStr Application = File.m_Path.f_Extract(FindPath.f_GetLen() + 1);
						if (CVersionManager::fs_IsValidApplicationName(Application))
							Applications[Application];
					}
					return Applications;
				}
			)
			> Promise / [Promise](TCSet<CStr> &&_Result)
			{
				Promise.f_SetResult(fg_Move(_Result));
			}
		;
		return Promise.f_MoveFuture();
	}

	TCFuture<void> CVersionManagerDaemonActor::CServer::fp_FindVersions()
	{
		auto Applications = co_await self(&CVersionManagerDaemonActor::CServer::fp_EnumApplications);
		for (auto &Application : Applications)
			mp_Applications[Application];
		auto QueryFileActor = fp_GetQueryFileActor();

		auto Result = co_await
			(
			 	g_Dispatch(QueryFileActor) /
			 	[Applications, RootDirectory = mp_AppState.m_RootDirectory]()
			 	-> NContainer::TCMap<NStr::CStr, NContainer::TCMap<CVersionManager::CVersionIDAndPlatform, CVersionManager::CVersionInformation>>
				{
					CStr ApplicationDirectory = RootDirectory + "/Applications";
					NContainer::TCMap<NStr::CStr, NContainer::TCMap<CVersionManager::CVersionIDAndPlatform, CVersionManager::CVersionInformation>> VersionsPerApplication;
					for (auto &Application : Applications)
					{
						auto &Versions = VersionsPerApplication[Application];
						CStr ApplicationPath = fg_Format("{}/{}", ApplicationDirectory, Application);
						CFile::CFindFilesOptions FindOptions(ApplicationPath + "/*", false);
						FindOptions.m_AttribMask = EFileAttrib_Directory;
						auto FoundFiles = CFile::fs_FindFiles(FindOptions);
						for (auto &File : FoundFiles)
						{
							CStr Version = CVersionManager::CVersionID::fs_DecodeFileName(File.m_Path.f_Extract(ApplicationPath.f_GetLen() + 1));
							CVersionManager::CVersionIDAndPlatform VersionID;
							CStr Error;
							if (!CVersionManager::fs_IsValidVersionIdentifier(Version, Error, &VersionID.m_VersionID))
								continue;
							CStr VersionIDPath = fg_Format("{}/{}", ApplicationPath, VersionID.m_VersionID.f_EncodeFileName());
							CFile::CFindFilesOptions FindOptions(VersionIDPath + "/*", false);
							FindOptions.m_AttribMask = EFileAttrib_Directory;
							auto FoundFiles = CFile::fs_FindFiles(FindOptions);
							for (auto &File : FoundFiles)
							{
								CStr Platform = File.m_Path.f_Extract(VersionIDPath.f_GetLen() + 1);
								if (!CVersionManager::fs_IsValidPlatform(Platform))
									continue;
								VersionID.m_Platform = Platform;
								try
								{
									CStr VersionPath = fg_Format("{}/{}", ApplicationPath, VersionID.f_EncodeFileName());
									CStr VersionInfoPath = fg_Format("{}.json", VersionPath);
									CVersionManager::CVersionInformation OutVersion;
									if (CFile::fs_FileExists(VersionInfoPath))
									{
										CEJSON ApplicationInfo = CEJSON::fs_FromString(CFile::fs_ReadStringFromFile(VersionInfoPath), VersionInfoPath);
										if (auto pValue = ApplicationInfo.f_GetMember("Time", EEJSONType_Date))
											OutVersion.m_Time = pValue->f_Date();
										if (auto pValue = ApplicationInfo.f_GetMember("Configuration", EJSONType_String))
											OutVersion.m_Configuration = pValue->f_String();
										if (auto pValue = ApplicationInfo.f_GetMember("ExtraInfo", EJSONType_Object))
											OutVersion.m_ExtraInfo = *pValue;
										if (auto pValue = ApplicationInfo.f_GetMember("Tags", EJSONType_Array))
										{
											for (auto &Value : pValue->f_Array())
											{
												if (Value.f_IsString())
													OutVersion.m_Tags[Value.f_String()];
											}
										}
										if (auto pValue = ApplicationInfo.f_GetMember("RetrySequence", EJSONType_Integer))
											OutVersion.m_RetrySequence = pValue->f_Integer();
									}
									{
										auto Files = CFile::fs_FindFiles(VersionPath + "/*", EFileAttrib_File, true);
										OutVersion.m_nFiles = Files.f_GetLen();
										for (auto &File : Files)
											OutVersion.m_nBytes += CFile::fs_GetFileSize(File);
									}

									// Only use versions that has the .json file
									Versions[VersionID] = fg_Move(OutVersion);
								}
								catch (NException::CException const &_Exception)
								{
									[[maybe_unused]] auto &Exception = _Exception;
									DLogWithCategory(Malterlib/Cloud/VersionManager, Error, "Internal error reading version info: {}", Exception.f_GetErrorStr());
								}
							}
						}
					}
					return VersionsPerApplication;
				}
			)
		;

		for (auto &ApplicationVersions : Result)
		{
			auto &Application = mp_Applications[Result.fs_GetKey(ApplicationVersions)];
			for (auto &Version : ApplicationVersions)
			{
				auto &OutVersion = Application.m_Versions[ApplicationVersions.fs_GetKey(Version)];
				OutVersion.m_VersionInfo = Version;
				Application.m_VersionsByTime.f_Insert(OutVersion);
				mp_KnownTags += Version.m_Tags;
			}
		}

		co_return {};
	}

	TCFuture<void> CVersionManagerDaemonActor::CServer::fp_SetupPermissions()
	{
		TCSet<CStr> Permissions;
		Permissions["Application/ReadAll"];
		Permissions["Application/ListAll"];
		Permissions["Application/WriteAll"];
		Permissions["Application/TagAll"];

		for (auto &Application : mp_Applications)
		{
			Permissions[fg_Format("Application/Read/{}", Application.f_GetName())];
			Permissions[fg_Format("Application/Write/{}", Application.f_GetName())];
		}

		for (auto &Tag : mp_KnownTags)
		{
			Permissions[fg_Format("Application/Tag/{}", Tag)];
		}

		co_await mp_AppState.m_TrustManager(&CDistributedActorTrustManager::f_RegisterPermissions, Permissions);;

		TCVector<CStr> SubscribePermissions;
		SubscribePermissions.f_Insert("Application/*");

		mp_Permissions = co_await mp_AppState.m_TrustManager(&CDistributedActorTrustManager::f_SubscribeToPermissions, SubscribePermissions, fg_ThisActor(this));

		mp_Permissions.f_OnPermissionsAdded
			(
				[this](CPermissionIdentifiers const &_Identity, TCMap<CStr, CPermissionRequirements> const &_AddedPermissions)
				{
					fp_UpdateSubscriptionsForChangedPermissions(_Identity);
				}
			)
		;

		mp_Permissions.f_OnPermissionsRemoved
			(
				[this](CPermissionIdentifiers const &_Identity, TCSet<CStr> const &_RemovedPermissions)
				{
					fp_UpdateSubscriptionsForChangedPermissions(_Identity);
				}
			)
		;

		co_return {};
	}

	TCFuture<void> CVersionManagerDaemonActor::CServer::fp_Destroy()
	{
		co_await mp_ProtocolInterface.f_Destroy();

		if (mp_QueryFileActor)
			co_await mp_QueryFileActor.f_Destroy();

		co_return {};
	}

	TCActor<CSeparateThreadActor> const &CVersionManagerDaemonActor::CServer::fp_GetQueryFileActor()
	{
		if (mp_QueryFileActor)
			return mp_QueryFileActor;

		mp_QueryFileActor = fg_ConstructActor<CSeparateThreadActor>(fg_Construct("Version manager query file actor"));
		return mp_QueryFileActor;
	}
}
