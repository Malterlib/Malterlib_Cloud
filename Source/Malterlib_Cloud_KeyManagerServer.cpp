// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Concurrency/LogError>

#include "Malterlib_Cloud_KeyManager.h"
#include "Malterlib_Cloud_KeyManagerServer.h"
#include "Malterlib_Cloud_KeyManagerServer_Internal.h"

namespace NMib::NCloud
{
	using namespace NConcurrency;
	using namespace NConcurrency::NPrivate;
	using namespace NStr;

	NStr::CStr const &ICKeyManagerServerDatabase::CDatabase::CClientStore::f_GetID() const
	{
		return NContainer::TCMap<NStr::CStr, CClientStore>::fs_GetKey(*this);
	}

	NStr::CStr const &ICKeyManagerServerDatabase::CDatabase::CClientKey::f_GetID() const
	{
		return NContainer::TCMap<NStr::CStr, CClientKey>::fs_GetKey(*this);
	}

	bool ICKeyManagerServerDatabase::CDatabase::f_HasAvailableKey(CSymmetricKey _Key)
	{
		auto *pKeys = m_AvailableKeys.f_FindEqual(_Key.f_GetLen());
		if (!pKeys)
			return false;

		return !!pKeys->f_FindEqual(_Key);
	}

	CKeyManagerServer::CInternal::CInternal(CKeyManagerServer *_pThis, CKeyManagerServerConfig &&_Config)
		: m_pThis(_pThis)
		, m_Config(fg_Move(_Config))
	{
		
	}
		
	CKeyManagerServer::CKeyManagerServer(CKeyManagerServerConfig &&_Config)
		: mp_pInternal(fg_Construct(this, fg_Move(_Config)))
	{
	}
	
	CKeyManagerServer::~CKeyManagerServer()
	{
	}

	NConcurrency::TCFuture<void> CKeyManagerServer::f_Init(fp32 _WaitForPublicationsTimeout)
	{
		auto &Internal = *mp_pInternal;
		Internal.m_DistributionManager = co_await Internal.m_Config.m_TrustManager(&CDistributedActorTrustManager::f_GetDistributionManager);
		Internal.m_ThisHostID = co_await Internal.m_Config.m_TrustManager(&CDistributedActorTrustManager::f_GetHostID);

		co_await Internal.f_ReadDatabase();
		co_await Internal.f_SetupServerSync();
		co_await Internal.f_SetupPermissions();

		Internal.m_KeyManagerServerSyncInstance.f_Construct(Internal.m_DistributionManager, this);
		co_await Internal.m_KeyManagerInstance.f_Publish<CKeyManager>(Internal.m_DistributionManager, this, _WaitForPublicationsTimeout);

		DMibFastCheck(Internal.m_KeyManagerInstance.m_Publication.f_IsValid());

		co_return {};
	}

	TCFuture<void> CKeyManagerServer::fp_Destroy()
	{
		auto &Internal = *mp_pInternal;

		CLogError LogError("Mib/Cloud/KeyManagerServer");

		co_await Internal.m_OtherKeyManagersSubscription.f_Destroy().f_Wrap() > LogError.f_Warning("Failed to destroy other key managers subscription");
		co_await Internal.m_KeyManagerServerSyncInstance.f_Destroy().f_Wrap() > LogError.f_Warning("Failed to destroy server sync instance");
		co_await Internal.m_KeyManagerInstance.f_Destroy().f_Wrap() > LogError.f_Warning("Failed to destroy key manager instance");

		co_return {};
	}
}
