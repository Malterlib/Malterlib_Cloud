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

	NConcurrency::TCFuture<void> CKeyManagerServer::f_Init()
	{
		auto &Internal = *mp_pInternal;
		Internal.m_DistributionManager = co_await Internal.m_Config.m_TrustManager(&CDistributedActorTrustManager::f_GetDistributionManager);

		co_await Internal.f_ReadDatabase();

		co_await Internal.m_KeyManagerInstance.f_Publish<CKeyManager>(Internal.m_DistributionManager, this, CKeyManager::mc_pDefaultNamespace);

		co_return {};
	}

	TCFuture<void> CKeyManagerServer::fp_Destroy()
	{
		auto &Internal = *mp_pInternal;

		CLogError LogError("Mib/Cloud/KeyManagerServer");

		co_await Internal.m_KeyManagerInstance.f_Destroy().f_Wrap() > LogError.f_Warning("Failed to destroy key manager instance");

		co_return {};
	}
}
