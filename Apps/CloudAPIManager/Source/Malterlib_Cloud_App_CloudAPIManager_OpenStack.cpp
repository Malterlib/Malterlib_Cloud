// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/DistributedActorTrustManager>
#include <Mib/Concurrency/DistributedActorTrustManagerDatabases/JsonDirectory>
#include <Mib/Encoding/JsonShortcuts>


#include "Malterlib_Cloud_App_CloudAPIManager.h"
#include "Malterlib_Cloud_App_CloudAPIManager_Server.h"

namespace NMib::NCloud::NCloudAPIManager
{
	TCFuture<CCloudAPIManagerDaemonActor::CServer::COpenStackServiceInfo> CCloudAPIManagerDaemonActor::CServer::fp_GetOpenStackServiceInfo(CCloudContext &_CloudContext)
	{
		if
			(
				!_CloudContext.m_pGetToken
				||
				(
					_CloudContext.m_bLastWasError
					&& _CloudContext.m_LastErrorStopwatch.f_GetTime() > 5.0
				)
				||
				(
					_CloudContext.m_TokenExpiresAt.f_IsValid()
					&& (_CloudContext.m_TokenExpiresAt - CTime::fs_NowUTC()) < CTimeSpanConvert::fs_CreateHourSpan(1)
				)
			)
		{
			_CloudContext.m_bLastWasError = false;
			_CloudContext.m_pGetToken = fg_Construct
				(
					g_ActorFunctor / [this, KeystoneInfo = _CloudContext.m_KeystoneInfo, Name = _CloudContext.f_GetName()]() -> TCFuture<COpenStackServiceInfo>
					{
						CStr URL(KeystoneInfo.m_IdentityURL + "auth/tokens");

						CEJsonSorted AuthRequest =
							{
								"auth"_=
								{
									"identity"_=
									{
										"methods"_= _["password"],
										"password"_=
										{
											"user"_=
											{
												"name"_= KeystoneInfo.m_Username
												, "password"_= KeystoneInfo.m_Password
												, "domain"_=
												{
													"id"_= KeystoneInfo.m_DomainId
												}
											}
										}
									}
									, "scope"_=
									{
										"project"_=
										{
											"id"_= KeystoneInfo.m_TenantId
										}
									}
									, "tenantId"_= KeystoneInfo.m_TenantId
								}
							}
						;

						TCMap<CStr, CStr> Headers;

						COpenStackServiceInfo ServiceInfo;

						auto Result = co_await fp_GetHttpQueryActor()(&CHttpClientActor::f_Post, URL, Headers, fg_Move(AuthRequest)).f_Wrap();

						auto *pCloudContext = mp_CloudContexts.f_FindEqual(Name);
						if (!Result)
						{
							if (pCloudContext)
							{
								pCloudContext->m_LastErrorStopwatch.f_Start();
								pCloudContext->m_bLastWasError = true;
								DLogWithCategory(Malterlib/Cloud/CloudAPIManager, Error, "Failed to generate OpenStack service info: {}", Result.f_GetExceptionStr());
							}
							co_return Result.f_GetException();
						}

						if (!Result->m_Headers.f_Exists("x-subject-token"))
							co_return DMibErrorInstance("No token in response");

						ServiceInfo.m_Token = Result->m_Headers["x-subject-token"];

						auto const Json = CJsonSorted::fs_FromString(Result->m_Body);
						auto Token = Json["token"];

						CStr ExpiresAt = Token["expires_at"].f_String();	// Example: "2016-09-30T08:36:31.932109Z"
						int64 Year;
						aint Month, Day, Hour, Minute, Second, nParsed;
						(void)
							(
								NMib::NStr::CStr::CParse("{}-{}-{}T{}:{}:{}.{}Z")
								>> Year
								>> Month
								>> Day
								>> Hour
								>> Minute
								>> Second
							)
							.f_Parse(ExpiresAt, nParsed)
						;
						ServiceInfo.m_TokenExpiresAt = NTime::CTimeConvert::fs_CreateTime(Year, Month, Day, Hour, Minute, Second);

						auto Catalog = Token["catalog"].f_Array();

						for (auto &Service : Catalog)
						{
							auto Endpoints = Service["endpoints"].f_Array();
							for (auto &Endpoint : Endpoints)
							{
								if (Endpoint["region"].f_String() == KeystoneInfo.m_RegionName)
									ServiceInfo.m_URLs[Service["name"].f_String()] = Endpoint["url"].f_String();
							}
						}

						if (pCloudContext)
						{
							pCloudContext->m_bLastWasError = false;
							pCloudContext->m_TokenExpiresAt = ServiceInfo.m_TokenExpiresAt;
							DLogWithCategory(Malterlib/Cloud/CloudAPIManager, Info, "Generate OpenStack service info. Auth token expires at {}", pCloudContext->m_TokenExpiresAt);
						}

						co_return fg_Move(ServiceInfo);
					}
					, true
				)
			;
		}
		return (*_CloudContext.m_pGetToken)();
	}
}
