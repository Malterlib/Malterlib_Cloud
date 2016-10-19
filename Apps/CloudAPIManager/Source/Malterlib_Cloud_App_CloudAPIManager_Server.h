// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedApp>
#include <Mib/Concurrency/ActorCallbackManager>
#include <Mib/Concurrency/ActorCallOnce>
#include <Mib/Concurrency/DistributedTrustDDPBridge>
#include <Mib/Cloud/CloudAPIManager>
#include <Mib/Daemon/Daemon>

namespace NMib::NCloud::NCloudAPIManager
{
	struct CCloudAPIManagerDaemonActor::CServer : public CActor
	{
	public:
		CServer(CDistributedAppState &_AppState);
		~CServer();
		void f_Construct() override;
		TCContinuation<void> f_Destroy() override;

		struct CCloudAPIManagerImplementation : public CCloudAPIManager
		{
			CCloudAPIManagerImplementation(TCActor<CCloudAPIManagerDaemonActor::CServer> &&_Server);
			
			TCContinuation<CEnsureContainer::CResult> f_EnsureContainer(CEnsureContainer &&_Params) override;
			TCContinuation<CSignTempURL::CResult> f_SignTempURL(CSignTempURL &&_Params) override;
			TCContinuation<CDeleteObject::CResult> f_DeleteObject(CDeleteObject &&_Params) override;
		private:
			TCWeakActor<CCloudAPIManagerDaemonActor::CServer> mp_Server;
		};
		
		struct COpenStackKeystoneInfo
		{
			CStr m_Username;
			CStr m_Password;
			CStr m_IdentityURL;
			CStr m_DomainId;
			CStr m_RegionName;
			CStr m_TenantId;
		};
		
		struct COpenStackServiceInfo
		{
			CStr m_Token;
			CTime m_TokenExpiresAt;
			TCMap<CStr, CStr> m_URLs;
		};
		
		struct CCloudContext
		{
			CStr const &f_GetName() const
			{
				return TCMap<CStr, CCloudContext>::fs_GetKey(*this); 
			}
			
			COpenStackKeystoneInfo m_KeystoneInfo;
			
			TCUniquePointer<TCActorCallOnce<COpenStackServiceInfo>> m_pGetToken;
			
			CClock m_LastErrorClock;
			bool m_bLastWasError = false;
			CTime m_TokenExpiresAt;
		};
		
	private:
		void fp_Init();
		void fp_Publish();
		
		TCContinuation<void> fp_SetupCloudContexs();
		TCContinuation<void> fp_SetupPermissions();
		TCContinuation<void> fp_SetupDDPBridge();
		TCVector<CDistributedTrustDDPBridge::CMethod> fp_GetDDPMethods();
		
		TCContinuation<COpenStackServiceInfo> fp_GetOpenStackServiceInfo(CCloudContext &_CloudContext);

		TCContinuation<CCloudAPIManager::CEnsureContainer::CResult> fp_Protocol_EnsureContainer(CCallingHostInfo const &_CallingHostInfo, CCloudAPIManager::CEnsureContainer &&_Params);
		TCContinuation<CCloudAPIManager::CSignTempURL::CResult> fp_Protocol_SignTempURL(CCallingHostInfo const &_CallingHostInfo, CCloudAPIManager::CSignTempURL &&_Params);
		TCContinuation<CCloudAPIManager::CDeleteObject::CResult> fp_Protocol_DeleteObject(CCallingHostInfo const &_CallingHostInfo, CCloudAPIManager::CDeleteObject &&_Params);
		
		CException fp_AccessDenied(CCallingHostInfo const &_CallingHostInfo, CStr const &_Description, CStr const &_UserDescription = CStr());
		
		static void fsp_LogActivityInfo(CCallingHostInfo const &_CallingHostInfo, CStr const &_Info);
		static CException fsp_LogActivityError(CCallingHostInfo const &_CallingHostInfo, CStr const &_Error, CExceptionPointer _pException);
		static void fsp_LogActivityWarning(CCallingHostInfo const &_CallingHostInfo, CStr const &_Error);
		
		TCActor<CSeparateThreadActor> const &fp_GetCURLQueryActor();

		TCMap<CStr, CCloudContext> mp_CloudContexts;
		
		TCSharedPointer<CCanDestroyTracker> mp_pCanDestroyTracker;
		
		TCDistributedActor<CCloudAPIManagerImplementation> mp_ProtocolImplementation;
		CDistributedActorPublication mp_ProtocolPublication;
		CDistributedAppState mp_AppState;
		
		CTrustedPermissionSubscription mp_Permissions;
		
		TCActor<CSeparateThreadActor> mp_CURLQueryActor;

		TCActor<CDistributedTrustDDPBridge> mp_DDPBridge;
		CActorSubscription mp_DDPBridgeSubscription;
	};
}
