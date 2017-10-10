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
		, mp_pCanDestroyTracker(fg_Construct())
	{
#ifdef DPlatformFamily_OSX
		CStr Path = fg_GetSys()->f_GetEnvironmentVariable("PATH");
		if (Path.f_Find("/opt/local/bin") < 0)
			fg_GetSys()->f_SetEnvironmentVariable("PATH", "/opt/local/bin:" + Path);
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


	TCContinuation<TCSet<CStr>> CVersionManagerDaemonActor::CServer::fp_EnumApplications()
	{
		auto QueryFileActor = fp_GetQueryFileActor();
		
		TCContinuation<TCSet<CStr>> Continuation;
		
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
			> Continuation / [Continuation](TCSet<CStr> &&_Result)
			{
				Continuation.f_SetResult(fg_Move(_Result));
			}
		;
		return Continuation;
	}
	
	TCContinuation<void> CVersionManagerDaemonActor::CServer::fp_FindVersions()
	{
		TCContinuation<void> Continuation;

		self(&CVersionManagerDaemonActor::CServer::fp_EnumApplications) > Continuation / [this, Continuation](TCSet<CStr> &&_Applications)
			{
				for (auto &Application : _Applications)
				{
					mp_Applications[Application];
				}
				auto QueryFileActor = fp_GetQueryFileActor();
				
				fg_Dispatch
					(
						QueryFileActor
						, [_Applications, RootDirectory = mp_AppState.m_RootDirectory]() -> NContainer::TCMap<NStr::CStr, NContainer::TCMap<CVersionManager::CVersionIDAndPlatform, CVersionManager::CVersionInformation>>
						{
							CStr ApplicationDirectory = RootDirectory + "/Applications";
							NContainer::TCMap<NStr::CStr, NContainer::TCMap<CVersionManager::CVersionIDAndPlatform, CVersionManager::CVersionInformation>> VersionsPerApplication;
							for (auto &Application : _Applications)
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
											DLogWithCategory(Malterlib/Cloud/VersionManager, Error, "Internal error reading version info: {}", _Exception.f_GetErrorStr());
										}
									}
								}
							}
							return VersionsPerApplication;
						}
					)
					> [this, Continuation]
					(TCAsyncResult<NContainer::TCMap<NStr::CStr, NContainer::TCMap<CVersionManager::CVersionIDAndPlatform, CVersionManager::CVersionInformation>>> &&_Result)
					{
						if (!_Result)
						{
							Continuation.f_SetException(_Result);
							return;
						}
						auto &Result = *_Result;
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
						Continuation.f_SetResult();
					}
				;
			}
		;
		
		return Continuation;
	}
	
	TCContinuation<void> CVersionManagerDaemonActor::CServer::fp_SetupPermissions()
	{
		TCContinuation<void> Continuation;
		
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
		
		mp_AppState.m_TrustManager(&CDistributedActorTrustManager::f_RegisterPermissions, Permissions) > fg_DiscardResult();
		
		TCVector<CStr> SubscribePermissions;
		SubscribePermissions.f_Insert("Application/*");
	
		mp_AppState.m_TrustManager(&CDistributedActorTrustManager::f_SubscribeToPermissions, SubscribePermissions, fg_ThisActor(this)) 
			> Continuation / [this, Continuation](CTrustedPermissionSubscription &&_Subscription)
			{
				mp_Permissions = fg_Move(_Subscription);
				
				
				mp_Permissions.f_OnPermissionsAdded
					(
						[this](CStr const &_HostID, TCSet<CStr> const &_AddedPermissions)
						{
							fp_UpdateSubscriptionsForChangedPermissions(_HostID);
						}
					)
				;
				
				mp_Permissions.f_OnPermissionsRemoved
					(
						[this](CStr const &_HostID, TCSet<CStr> const &_AddedRemoved)
						{
							fp_UpdateSubscriptionsForChangedPermissions(_HostID);
						}
					)
				;
				
				Continuation.f_SetResult();
			}
		;
		
		return Continuation;
	}
	
	TCContinuation<void> CVersionManagerDaemonActor::CServer::fp_Destroy()
	{
		auto pCanDestroy = fg_Move(mp_pCanDestroyTracker);
		if (mp_QueryFileActor)
			mp_QueryFileActor->f_Destroy() > pCanDestroy->f_Track();
		mp_ProtocolInterface.f_Destroy() > pCanDestroy->f_Track();
		return pCanDestroy->m_Continuation;
	}
	
	TCActor<CSeparateThreadActor> const &CVersionManagerDaemonActor::CServer::fp_GetQueryFileActor()
	{
		if (mp_QueryFileActor)
			return mp_QueryFileActor;
		
		mp_QueryFileActor = fg_ConstructActor<CSeparateThreadActor>(fg_Construct("Version manager query file actor"));
		return mp_QueryFileActor;
	}
}
