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
	auto CCloudAPIManagerDaemonActor::CServer::CCloudAPIManagerImplementation::f_SignTempURL(CSignTempURL &&_Params) -> TCFuture<CSignTempURL::CResult>
	{
		auto pThis = m_pThis;
		auto Auditor = pThis->mp_AppState.f_Auditor();
		
		TCPromise<CCloudAPIManager::CSignTempURL::CResult> Promise;
		
		if (!CCloudAPIManager::fs_IsValidCloudContext(_Params.m_CloudContext))
			return Auditor.f_Exception("Cloud context format not valid");
		
		if (!CCloudAPIManager::fs_IsValidContainerName(_Params.m_ContainerName))
			return Auditor.f_Exception("Container name format not valid");
		
		if (!CCloudAPIManager::fs_IsValidObjectId(_Params.m_ObjectId))
			return Auditor.f_Exception("Object id format not valid");
		
		if (!CCloudAPIManager::fs_IsValidTempURLKey(_Params.m_TempURLKey))
			return Auditor.f_Exception("Temp URL key format not valid");
		
		pThis->mp_Permissions.f_HasPermission("Sign Temp URL", {"ObjectStorage/SignTempURLAll", fg_Format("ObjectStorage/SignTempURL/{}", _Params.m_CloudContext)})
			> Promise % "Permission denied signing temp URL" % Auditor / [=](bool _bHasPermission)
			{
				if (!_bHasPermission)
					return Promise.f_SetException(Auditor.f_AccessDenied("(Sign Temp URL)"));

				auto *pCloudContext = pThis->mp_CloudContexts.f_FindEqual(_Params.m_CloudContext);
				if (!pCloudContext)
					return Promise.f_SetException(Auditor.f_Exception(fg_Format("No such cloud context: {}", _Params.m_CloudContext)));

				pThis->fp_GetOpenStackServiceInfo(*pCloudContext) > Promise / [pThis, Promise, _Params, Auditor](COpenStackServiceInfo &&_ServiceInfo)
					{
						fg_Dispatch
							(
								pThis->fp_GetCURLQueryActor()
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
									NWeb::NHTTP::CURL URL(FullURL);

									CStr HMAC_Body = fg_Format("{}\n{}\n{}", _Params.m_Method, Expires, URL.f_GetFullPath());

									CByteVector Data;
									Data.f_Insert((uint8*)HMAC_Body.f_GetStr(), HMAC_Body.f_GetLen());

									CSecureByteVector Key;
									Key.f_Insert((uint8*)_Params.m_TempURLKey.f_GetStr(), _Params.m_TempURLKey.f_GetLen());

									auto Digest = fg_MessageAuthenication_HMAC_SHA1(Data, Key);

									CStr SignedURL = fg_Format("{}?temp_url_sig={}&temp_url_expires={}", FullURL, Digest.f_GetString(), Expires);
									return SignedURL;
								}
							)
							> [Promise, _Params, Auditor](TCAsyncResult<CStr> &&_Value)
							{
								if (!_Value)
								{
									CStr Error = fg_Format("Failed to sign temp URL {}/{} on {}", _Params.m_ContainerName, _Params.m_ObjectId, _Params.m_CloudContext);
									Promise.f_SetException(Auditor.f_Exception(fsp_AuditMessages(Error, _Value.f_GetException())));
									return;
								}
								CCloudAPIManager::CSignTempURL::CResult Result;
								Result.m_SignedURL = *_Value;
								Promise.f_SetResult(Result);
								Auditor.f_Info(fg_Format("Sign temp URL {}/{} on {}", _Params.m_ContainerName, _Params.m_ObjectId, _Params.m_CloudContext));
							}
						;
					}
				;
			}
		;
		return Promise.f_MoveFuture();
	}
}

