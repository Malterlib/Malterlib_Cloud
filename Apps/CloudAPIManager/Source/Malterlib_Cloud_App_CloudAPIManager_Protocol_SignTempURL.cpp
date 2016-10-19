// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/DistributedActorTrustManager>
#include <Mib/Concurrency/DistributedActorTrustManagerDatabases/JSONDirectory>
#include <Mib/Network/SSL>

#include "Malterlib_Cloud_App_CloudAPIManager.h"
#include "Malterlib_Cloud_App_CloudAPIManager_Server.h"
#include "Malterlib_Cloud_App_CloudAPIManager_CurlWrapper.h"

namespace NMib::NCloud::NCloudAPIManager
{
	TCContinuation<CCloudAPIManager::CSignTempURL::CResult> CCloudAPIManagerDaemonActor::CServer::fp_Protocol_SignTempURL
		(
			CCallingHostInfo const &_CallingHostInfo
			, CCloudAPIManager::CSignTempURL &&_Params
		)
	{
		TCContinuation<CCloudAPIManager::CSignTempURL::CResult> Continuation;
		
		if (!CCloudAPIManager::fs_IsValidCloudContext(_Params.m_CloudContext))
			return fsp_LogActivityError(_CallingHostInfo, "Cloud context format not valid", nullptr);
		
		if (!CCloudAPIManager::fs_IsValidContainerName(_Params.m_ContainerName))
			return fsp_LogActivityError(_CallingHostInfo, "Container name format not valid", nullptr);
		
		if (!CCloudAPIManager::fs_IsValidObjectId(_Params.m_ObjectId))
			return fsp_LogActivityError(_CallingHostInfo, "Object id format not valid", nullptr);
		
		if (!CCloudAPIManager::fs_IsValidTempURLKey(_Params.m_TempURLKey))
			return fsp_LogActivityError(_CallingHostInfo, "Temp URL key format not valid", nullptr);
		
		if (!mp_Permissions.f_HostHasAnyPermission(_CallingHostInfo.f_GetRealHostID(), "ObjectStorage/SignTempURLAll", fg_Format("ObjectStorage/SignTempURL/{}", _Params.m_CloudContext)))
			return fp_AccessDenied(_CallingHostInfo, "Sign Temp URL");
		
		auto *pCloudContext = mp_CloudContexts.f_FindEqual(_Params.m_CloudContext);
		if (!pCloudContext)
			return fsp_LogActivityError(_CallingHostInfo,  fg_Format("No such cloud context: {}", _Params.m_CloudContext), nullptr);
		
		fp_GetOpenStackServiceInfo(*pCloudContext) > Continuation / [this, Continuation, _Params, _CallingHostInfo](COpenStackServiceInfo &&_ServiceInfo)
			{
				fg_Dispatch
					(
						fp_GetCURLQueryActor()
						, [ServiceInfo = fg_Move(_ServiceInfo), _Params]() -> CStr
						{
							if (!ServiceInfo.m_URLs.f_Exists("swift"))
								DErrorCloudAPI("Swift service not available");
							
							if (_Params.m_ContainerName.f_IsEmpty())
								DErrorCloudAPI("Parameter containerName is empty");
							
							if (_Params.m_ObjectId.f_IsEmpty())
								DErrorCloudAPI("Parameter objectId is empty");
							
							if (_Params.m_TempURLKey.f_IsEmpty())
								DErrorCloudAPI("Parameter tempURLKey is empty");
							
							aint ValidTimeInSeconds = 60*60;
							
							NTime::CTime EpochStart = NTime::CTimeConvert::fs_CreateTime(1970, 1, 1);
							NTime::CTimeSpan TimeSinceEpoch = CTime::fs_NowUTC() - EpochStart;
							
							int64 Expires = (TimeSinceEpoch.f_GetSeconds() + ValidTimeInSeconds) * constant_int64(1000);
							
							CStr FullURL = fg_Format("{}/{}/{}", ServiceInfo.m_URLs["swift"], _Params.m_ContainerName, _Params.m_ObjectId);
							NHTTP::CURL URL(FullURL);
							
							CStr HMAC_Body = fg_Format("{}\n{}\n{}", _Params.m_Method, Expires, URL.f_GetFullPath());
							
							TCVector<uint8> Data;
							Data.f_Insert((uint8*)HMAC_Body.f_GetStr(), HMAC_Body.f_GetLen());
							
							TCVector<uint8> Key;
							Key.f_Insert((uint8*)_Params.m_TempURLKey.f_GetStr(), _Params.m_TempURLKey.f_GetLen());
							
							auto Digest = fg_MessageAuthenication_HMAC_SHA1(Data, Key);
		
							CStr SignedURL = fg_Format("{}?temp_url_sig={}&temp_url_expires={}", FullURL, Digest.f_GetString(), Expires);
							return SignedURL;
						}
					)
					> [Continuation, _Params, _CallingHostInfo](TCAsyncResult<CStr> &&_Value)
					{
						if (!_Value)
						{
							CStr Error = fg_Format("Failed to sign temp URL {}/{} on {}", _Params.m_ContainerName, _Params.m_ObjectId, _Params.m_CloudContext);
							Continuation.f_SetException(fsp_LogActivityError(_CallingHostInfo, Error, _Value.f_GetException()));
							return;
						}
						CCloudAPIManager::CSignTempURL::CResult Result;
						Result.m_SignedURL = *_Value;
						Continuation.f_SetResult(Result);
						fsp_LogActivityInfo(_CallingHostInfo, fg_Format("Sign temp URL {}/{} on {}", _Params.m_ContainerName, _Params.m_ObjectId, _Params.m_CloudContext));
					}
				;
			}
		;
		
		return Continuation;
	}
}

