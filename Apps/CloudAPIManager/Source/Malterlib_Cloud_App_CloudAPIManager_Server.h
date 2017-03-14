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
		using CActorHolder = CDelegatedActorHolder;
		
		CServer(CDistributedAppState &_AppState);
		~CServer();

		struct CCloudAPIManagerImplementation : public CCloudAPIManager
		{
			TCContinuation<CGetSwiftBaseURL::CResult> f_GetSwiftBaseURL(CGetSwiftBaseURL &&_Params) override;
			TCContinuation<CEnsureContainer::CResult> f_EnsureContainer(CEnsureContainer &&_Params) override;
			TCContinuation<CSignTempURL::CResult> f_SignTempURL(CSignTempURL &&_Params) override;
			TCContinuation<CDeleteObject::CResult> f_DeleteObject(CDeleteObject &&_Params) override;
			
			CServer *m_pThis;
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
			CStr m_SwiftStoragePolicy;
			
			TCUniquePointer<TCActorCallOnce<COpenStackServiceInfo>> m_pGetToken;
			
			CClock m_LastErrorClock;
			bool m_bLastWasError = false;
			CTime m_TokenExpiresAt;
		};
		
	private:
		TCContinuation<void> fp_Destroy() override;
		void fp_Init();
		void fp_Publish();
		
		TCContinuation<void> fp_SetupCloudContexs();
		TCContinuation<void> fp_SetupPermissions();
		TCContinuation<void> fp_SetupDDPBridge();
		TCVector<CDistributedTrustDDPBridge::CMethod> fp_GetDDPMethods();
		
		TCContinuation<COpenStackServiceInfo> fp_GetOpenStackServiceInfo(CCloudContext &_CloudContext);

		static TCVector<CStr> fsp_AuditMessages(CStr const &_Error, CExceptionPointer _pException);
		
		TCActor<CSeparateThreadActor> const &fp_GetCURLQueryActor();

		TCMap<CStr, CCloudContext> mp_CloudContexts;
		
		TCSharedPointer<CCanDestroyTracker> mp_pCanDestroyTracker;

		TCDelegatedActorInterface<CCloudAPIManagerImplementation> mp_ProtocolInterface;
		
		CDistributedAppState &mp_AppState;
		
		CTrustedPermissionSubscription mp_Permissions;
		
		TCActor<CSeparateThreadActor> mp_CURLQueryActor;

		TCActor<CDistributedTrustDDPBridge> mp_DDPBridge;
		CActorSubscription mp_DDPBridgeSubscription;
	};
}
