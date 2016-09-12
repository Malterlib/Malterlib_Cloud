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
	namespace
	{
		auto g_fFindApplications = []() -> TCSet<CStr>
			{
				CStr FindPath = CFile::fs_GetProgramDirectory() + "/Applications";
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
		;
	}
	
	TCContinuation<TCSet<CStr>> CVersionManagerDaemonActor::CServer::fp_EnumApplications()
	{
		auto QueryFileActor = fp_GetQueryFileActor();
		
		TCContinuation<TCSet<CStr>> Continuation;
		
		fg_Dispatch
			(
				QueryFileActor
				, g_fFindApplications
			)
			> Continuation / [this, Continuation](TCSet<CStr> &&_Result)
			{
				Continuation.f_SetResult(fg_Move(_Result));
			}
		;
		return Continuation;
	}
	
	TCSet<CStr> CVersionManagerDaemonActor::CServer::fp_FilterApplicationsByPermissions(CCallingHostInfo const &_CallingHostInfo, TCSet<CStr> const &_Applications)
	{
		TCSet<CStr> Applications;

		bool bListAllAccess = mp_Permissions.f_HostHasAnyPermission(_CallingHostInfo.f_GetRealHostID(), "Application/ReadAll", "Application/ListAll");
		
		for (auto &Application : _Applications)
		{
			if (!bListAllAccess && !mp_Permissions.f_HostHasPermission(_CallingHostInfo.f_GetRealHostID(), fg_Format("Application/Read/{}", Application)))
				continue;
			Applications[Application];
		}
		
		return Applications;
	}

	
	auto CVersionManagerDaemonActor::CServer::fp_Protocol_ListApplications(CCallingHostInfo const &_CallingHostInfo, CVersionManager::CListApplications &&_Params)
		-> NConcurrency::TCContinuation<CVersionManager::CListApplications::CResult> 
	{
		if (!mp_pCanDestroyTracker)
			return DMibErrorInstance("Shutting down");
		NConcurrency::TCContinuation<CVersionManager::CListApplications::CResult> Continuation;
		auto QueryFileActor = fp_GetQueryFileActor();

		fsp_LogActivityInfo(_CallingHostInfo, "Listing applications");
		
		fg_Dispatch
			(
				QueryFileActor
				, g_fFindApplications
			)
			> [this, Continuation, _CallingHostInfo, _Params](TCAsyncResult<TCSet<CStr>> &&_Result)
			{
				if (!_Result)
				{
					fsp_LogActivityError(_CallingHostInfo, fg_Format("Error listing applications: {}", _Result.f_GetExceptionStr()));
					Continuation.f_SetException(DMibErrorInstance("File error when running query. Consult logs on Version server to diagnose."));
					return;
				}
				CVersionManager::CListApplications::CResult Results;
				Results.m_Applications = fp_FilterApplicationsByPermissions(_CallingHostInfo, *_Result);

				fsp_LogActivityInfo(_CallingHostInfo, fg_Format("Listed applications: {vs}", Results.m_Applications));
				
				Continuation.f_SetResult(fg_Move(Results));
			}
		;
		return Continuation;
	}

	auto CVersionManagerDaemonActor::CServer::fp_Protocol_ListVersions(CCallingHostInfo const &_CallingHostInfo, CVersionManager::CListVersions &&_Params)
		-> NConcurrency::TCContinuation<CVersionManager::CListVersions::CResult> 
	{
		if (!mp_pCanDestroyTracker)
			return DMibErrorInstance("Shutting down");
		NConcurrency::TCContinuation<CVersionManager::CListVersions::CResult> Continuation;
		auto QueryFileActor = fp_GetQueryFileActor();

		fsp_LogActivityInfo(_CallingHostInfo, "Listing versions");
		
		auto fListVersions = [this, Continuation, _CallingHostInfo, _Params](TCSet<CStr> const &_Applications)
			{
				auto QueryFileActor = fp_GetQueryFileActor();
				
				fg_Dispatch
					(
						QueryFileActor
						, [_Applications, _CallingHostInfo]() -> NContainer::TCMap<NStr::CStr, NContainer::TCMap<CVersionManager::CVersionIdentifier, CVersionManager::CVersionInformation>>
						{
							CStr ApplicationDirectory = CFile::fs_GetProgramDirectory() + "/Applications";
							NContainer::TCMap<NStr::CStr, NContainer::TCMap<CVersionManager::CVersionIdentifier, CVersionManager::CVersionInformation>> VersionsPerApplication;
							for (auto &Application : _Applications)
							{
								auto &Versions = VersionsPerApplication[Application];
								CStr ApplicationPath = fg_Format("{}/{}", ApplicationDirectory, Application);
								CFile::CFindFilesOptions FindOptions(ApplicationPath + "/*", false);
								FindOptions.m_AttribMask = EFileAttrib_Directory;
								auto FoundFiles = CFile::fs_FindFiles(FindOptions);
								for (auto &File : FoundFiles)
								{
									CStr Version = CVersionManager::CVersionIdentifier::fs_DecodeFileName(File.m_Path.f_Extract(ApplicationPath.f_GetLen() + 1));
									CVersionManager::CVersionIdentifier VersionID;
									CStr Error;
									if (CVersionManager::fs_IsValidVersionIdentifier(Version, Error, &VersionID))
									{
										auto &OutVersion = Versions[VersionID];
										
										try
										{
											CStr ApplicationInfoPath = fg_Format("{}/{}.json", ApplicationPath, VersionID.f_EncodeFileName());
											if (CFile::fs_FileExists(ApplicationInfoPath))
											{
												CEJSON ApplicationInfo = CEJSON::fs_FromString(CFile::fs_ReadStringFromFile(ApplicationInfoPath), ApplicationInfoPath);
												if (auto pValue = ApplicationInfo.f_GetMember("Time", EEJSONType_Date))
													OutVersion.m_Time = pValue->f_Date();
												if (auto pValue = ApplicationInfo.f_GetMember("Configuration", EJSONType_String))
													OutVersion.m_Configuration = pValue->f_String();
												if (auto pValue = ApplicationInfo.f_GetMember("ExtraInfo", EJSONType_Object))
													OutVersion.m_ExtraInfo = *pValue;
											}
										}
										catch (NException::CException const &_Exception)
										{
											fsp_LogActivityError(_CallingHostInfo, fg_Format("Internal error reading version info: {}", _Exception.f_GetErrorStr()));
										}
									}
								}
							}
							return VersionsPerApplication;
						}
					)
					> [this, Continuation, _CallingHostInfo, _Params]
					(TCAsyncResult<NContainer::TCMap<NStr::CStr, NContainer::TCMap<CVersionManager::CVersionIdentifier, CVersionManager::CVersionInformation>>> &&_Result)
					{
						if (!_Result)
						{
							fsp_LogActivityError(_CallingHostInfo, fg_Format("Error listing versions: {}", _Result.f_GetExceptionStr()));
							Continuation.f_SetException(DMibErrorInstance("File error when running query. Consult logs on version mananger to diagnose."));
							return;
						}
						CVersionManager::CListVersions::CResult Results;
						Results.m_Versions = fg_Move(*_Result);

						TCMap<CStr, CStr> VersionsText;
						for (auto &Application : Results.m_Versions)
							VersionsText[Results.m_Versions.fs_GetKey(Application)] = fg_Format("{} versions", Application.f_GetLen());
						
						fsp_LogActivityInfo(_CallingHostInfo, fg_Format("Listed versions: {vs}", VersionsText));
						
						Continuation.f_SetResult(fg_Move(Results));
					}
				;
			}
		;
		
		if (!_Params.m_ForApplication.f_IsEmpty())
		{
			if (!CVersionManager::fs_IsValidApplicationName(_Params.m_ForApplication))
			{
				CStr Error = "Invalid application format";
				fsp_LogActivityError(_CallingHostInfo, Error);
				return DMibErrorInstance(Error);
			}
			TCSet<CStr> Applications;
			Applications[_Params.m_ForApplication];
			if (fp_FilterApplicationsByPermissions(_CallingHostInfo, Applications).f_IsEmpty())
				return fp_AccessDenied(_CallingHostInfo, "List Versions");
			fListVersions(Applications);
		}
		else
		{
			fg_Dispatch
				(
					QueryFileActor
					, g_fFindApplications
				)
				> [this, Continuation, _CallingHostInfo, fListVersions](TCAsyncResult<TCSet<CStr>> &&_Result)
				{
					if (!_Result)
					{
						fsp_LogActivityError(_CallingHostInfo, fg_Format("Error listing applications when listing Versions: {}", _Result.f_GetExceptionStr()));
						Continuation.f_SetException(DMibErrorInstance("File error when running query. Consult logs on Version server to diagnose."));
						return;
					}
					fListVersions(fp_FilterApplicationsByPermissions(_CallingHostInfo, *_Result));
				}
			;
		}
		return Continuation;
	}
}
