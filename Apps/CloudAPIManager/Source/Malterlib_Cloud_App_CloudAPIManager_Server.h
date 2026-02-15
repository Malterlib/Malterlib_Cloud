// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedApp>
#include <Mib/Concurrency/ActorCallOnce>
#include <Mib/Concurrency/DistributedTrustDDPBridge>
#include <Mib/Cloud/CloudAPIManager>
#include <Mib/Daemon/Daemon>
#include <Mib/Web/HttpClient>

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
			TCFuture<CGetSwiftBaseURL::CResult> f_GetSwiftBaseURL(CGetSwiftBaseURL _Params) override;
			TCFuture<CEnsureContainer::CResult> f_EnsureContainer(CEnsureContainer _Params) override;
			TCFuture<CSignTempURL::CResult> f_SignTempURL(CSignTempURL _Params) override;
			TCFuture<CDeleteObject::CResult> f_DeleteObject(CDeleteObject _Params) override;

			DMibDelegatedActorImplementation(CServer);
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
		TCFuture<void> fp_Destroy() override;
		TCFuture<void> fp_Init();
		void fp_Publish();

		TCFuture<void> fp_SetupCloudContexs();
		TCFuture<void> fp_SetupPermissions();
		TCFuture<void> fp_SetupDDPBridge();
		TCVector<CDistributedTrustDDPBridge::CMethod> fp_GetDDPMethods();

		TCFuture<COpenStackServiceInfo> fp_GetOpenStackServiceInfo(CCloudContext &_CloudContext);

		static TCVector<CStr> fsp_AuditMessages(CStr const &_Error, CExceptionPointer _pException);

		TCActor<CHttpClientActor> const &fp_GetHttpQueryActor();

		TCMap<CStr, CCloudContext> mp_CloudContexts;

		TCSharedPointer<CCanDestroyTracker> mp_pCanDestroyTracker;

		TCDistributedActorInstance<CCloudAPIManagerImplementation> mp_ProtocolInterface;

		CDistributedAppState &mp_AppState;

		CTrustedPermissionSubscription mp_Permissions;

		TCActor<CHttpClientActor> mp_HttpQueryActor;

		TCActor<CDistributedTrustDDPBridge> mp_DDPBridge;
		CActorSubscription mp_DDPBridgeSubscription;
	};
}
