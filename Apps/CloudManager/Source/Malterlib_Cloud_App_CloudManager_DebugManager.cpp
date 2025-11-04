// Copyright © 2023 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_App_CloudManager.h"
#include "Malterlib_Cloud_App_CloudManager_Internal.h"
#include "Malterlib_Cloud_App_CloudManager_Database.h"

#include <Mib/Concurrency/LogError>
#include <Mib/Process/Platform>

namespace NMib::NCloud::NCloudManager
{
	auto CCloudManagerServer::CDebugManagerImplementation::f_Asset_List(CAssetList _Params) -> TCFuture<CAssetList::CResult>
	{
		co_return DMibErrorInstance("Not supported in CloudManager bridge for DebugManager");
	}

	auto CCloudManagerServer::CDebugManagerImplementation::f_Asset_Upload(CAssetUpload _Params) -> TCFuture<CAssetUpload::CResult>
	{
		co_return DMibErrorInstance("Not supported in CloudManager bridge for DebugManager");
	}

	auto CCloudManagerServer::CDebugManagerImplementation::f_Asset_Download(CAssetDownload _Params) -> TCFuture<CAssetDownload::CResult>
	{
		co_return DMibErrorInstance("Not supported in CloudManager bridge for DebugManager");
	}

	auto CCloudManagerServer::CDebugManagerImplementation::f_Asset_Delete(CAssetDelete _Params) -> TCFuture<CAssetDelete::CResult>
	{
		co_return DMibErrorInstance("Not supported in CloudManager bridge for DebugManager");
	}

	auto CCloudManagerServer::CDebugManagerImplementation::f_CrashDump_List(CCrashDumpList _Params) -> TCFuture<CCrashDumpList::CResult>
	{
		co_return DMibErrorInstance("Not supported in CloudManager bridge for DebugManager");
	}

	auto CCloudManagerServer::CDebugManagerImplementation::f_CrashDump_Download(CCrashDumpDownload _Params) -> TCFuture<CCrashDumpDownload::CResult>
	{
		co_return DMibErrorInstance("Not supported in CloudManager bridge for DebugManager");
	}

	auto CCloudManagerServer::CDebugManagerImplementation::f_CrashDump_Delete(CCrashDumpDelete _Params) -> TCFuture<CCrashDumpDelete::CResult>
	{
		co_return DMibErrorInstance("Not supported in CloudManager bridge for DebugManager");
	}


	TCFuture<void> CCloudManagerServer::fp_InitDebugMananger()
	{
		mp_DebugManagers = co_await mp_AppState.m_TrustManager->f_SubscribeTrustedActors<NCloud::CDebugManager>();

		mp_DebugManagers.f_OnActor
			(
				g_ActorFunctor / [this](TCDistributedActor<CDebugManager> _Actor, CTrustedActorInfo _TrustInfo) -> TCFuture<void>
				{
					auto &LocalManager = mp_LocalDebugManagers[_Actor];
					LocalManager.m_DebugManagerInterface.f_Construct(mp_AppState.m_DistributionManager, this);
					LocalManager.m_DebugManagerInterface.m_pActor->m_UpstreamActor = _Actor;
					LocalManager.m_ID = fg_RandomID();

					TCFutureVector<void> AddResults;

					for (auto &Subscription : this->mp_DebugManagerSubscriptions)
						Subscription.m_fOnAdd(LocalManager.m_DebugManagerInterface.m_Actor->f_ShareInterface<CDebugManager>(), LocalManager.m_ID) > AddResults;

					co_await fg_AllDone(AddResults);

					co_return {};
				}
				, g_ActorFunctor / [this](TCWeakDistributedActor<CActor> _Actor, CTrustedActorInfo _TrustInfo) -> TCFuture<void>
				{
					auto *pLocalManager = mp_LocalDebugManagers.f_FindEqual(_Actor);

					TCFutureVector<void> DestroyResults;

					if (pLocalManager)
					{
						for (auto &Subscription : this->mp_DebugManagerSubscriptions)
							Subscription.m_fOnRemove(pLocalManager->m_ID) > DestroyResults;

						pLocalManager->m_DebugManagerInterface.f_Destroy() > DestroyResults;
						mp_LocalDebugManagers.f_Remove(pLocalManager);
					}

					co_await fg_AllDone(DestroyResults);

					co_return {};
				}
			)
		;

		co_return {};
	}

	auto CCloudManagerServer::CDebugManagerImplementation::f_CrashDump_Upload(CCrashDumpUpload _Params) -> TCFuture<CCrashDumpUpload::CResult>
	{
		auto pThis = m_pThis;
		auto OnResume = co_await pThis->f_CheckDestroyedOnResume();

		auto UpstreamActor = m_UpstreamActor.f_Lock();

		if (!UpstreamActor)
			co_return DMibErrorInstance("Upstream DebugManager no longer exists");

		co_return co_await UpstreamActor.f_CallActor(&CDebugManager::f_CrashDump_Upload)(fg_Move(_Params));
	}
}
