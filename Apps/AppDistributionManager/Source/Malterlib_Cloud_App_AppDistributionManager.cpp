// Copyright © 2018 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Concurrency/LogError>
#include <Mib/Encoding/JSONShortcuts>

#include "Malterlib_Cloud_App_AppDistributionManager.h"

namespace NMib::NCloud::NAppDistributionManager
{
	CAppDistributionManagerActor::CAppDistributionManagerActor()
		: CDistributedAppActor(CDistributedAppActor_Settings("AppDistributionManager").f_AuditCategory("Malterlib/Cloud/AppDistributionManager"))
	{
	}

	CAppDistributionManagerActor::~CAppDistributionManagerActor() = default;

	TCFuture<void> CAppDistributionManagerActor::fp_StartApp(NEncoding::CEJSONSorted const &_Params)
	{
		mp_FileActor = fg_ConstructActor<CSeparateThreadActor>(fg_Construct("App distribution manager file operations"));
		
		co_await fp_ReadState();

		auto OnResume = co_await fg_OnResume
			(
				[&]() -> NException::CExceptionPointer
				{
					if (mp_State.m_bStoppingApp)
						return DMibErrorInstance("Shutting down");
					return {};
				}
			)
		;

		mp_VersionManagerSubscription = co_await mp_State.m_TrustManager->f_SubscribeTrustedActors<CVersionManager>();

		co_await mp_VersionManagerSubscription.f_OnActor
			(
				g_ActorFunctor / [this](TCDistributedActor<CVersionManager> const &_VersionManager, CTrustedActorInfo const &_ActorInfo) -> TCFuture<void>
				{
					co_await self(&CAppDistributionManagerActor::fp_VersionManagerAdded, _VersionManager, _ActorInfo);

					co_return {};
				}
				, g_ActorFunctor / [this](TCWeakDistributedActor<CActor> const &_VersionManager, CTrustedActorInfo &&_ActorInfo) -> TCFuture<void>
				{
					co_await self(&CAppDistributionManagerActor::fp_VersionManagerRemoved, _VersionManager);

					co_return {};
				}
			)
		;

		co_return {};
	}
	
	TCFuture<void> CAppDistributionManagerActor::fp_StopApp()
	{	
		CLogError LogError("Mib/Cloud/AppDistributionManager");

		TCActorResultVector<void> DistributionDestroys;

		for (auto &Distribution : mp_Distributions)
		{
			for (auto &RunningDeploys : Distribution.m_RunningDeploys)
			{
				for (auto &RunningDeploy : RunningDeploys)
					RunningDeploy.f_Destroy() > DistributionDestroys.f_AddResult();
			}
		}

		co_await DistributionDestroys.f_GetUnwrappedResults().f_Wrap() > LogError.f_Warning("Failed to stop distribution deploys");

		if (mp_FileActor)
			co_await fg_Move(mp_FileActor).f_Destroy().f_Wrap() > LogError.f_Warning("Failed to stop file actor");

		co_return {};
	}

	TCFuture<void> CAppDistributionManagerActor::fp_Destroy()
	{
		CLogError LogError("Mib/Cloud/AppDistributionManager");
		{
			auto pCanDestroy = fg_Move(mp_pCanDestroy);
			auto DestroyFuture = fg_Exchange(pCanDestroy, nullptr)->f_Future();
			co_await fg_Move(DestroyFuture).f_Wrap() > LogError.f_Warning("Failed to destroy can destroy");
		}

		{
			TCActorResultVector<void> DestroyResults;

			for (auto &VersionManager : mp_VersionManagers)
			{
				if (VersionManager.m_Subscription)
					fg_Exchange(VersionManager.m_Subscription, nullptr)->f_Destroy() > DestroyResults.f_AddResult();
			}
			mp_VersionManagers.f_Clear();

			for (auto &Download : mp_Downloads)
			{
				if (Download.m_DownloadVersionReceive)
					fg_Move(Download.m_DownloadVersionReceive).f_Destroy() > DestroyResults.f_AddResult();

				if (Download.m_Subscription)
					fg_Exchange(Download.m_Subscription, nullptr)->f_Destroy() > DestroyResults.f_AddResult();
			}
			mp_Downloads.f_Clear();

			co_await DestroyResults.f_GetUnwrappedResults().f_Wrap() > LogError.f_Warning("Failed to destroy app distribution manager");
		}

		co_await mp_VersionManagerSubscription.f_Destroy().f_Wrap() > LogError.f_Warning("Failed to destroy vesion manager subscription");

		if (mp_FileActor)
			co_await fg_Move(mp_FileActor).f_Destroy().f_Wrap() > LogError.f_Warning("Failed to destroy file actor");

		co_await CDistributedAppActor::fp_Destroy();

		co_await fg_Move(mp_DistributeSequencer).f_Destroy().f_Wrap() > LogError.f_Warning("Failed to destroy sequencer");

		co_return {};
	}

	TCActor<CDeployDestination> CAppDistributionManagerActor::fp_CreateDeploy(EDeployDestination _Type)
	{
		switch (_Type)
		{
			case EDeployDestination_FileSystem: return fg_Construct<CDeployDestination_FileSystem>();
		}
		DNeverGetHere;
		return nullptr;
	}
}

namespace NMib::NCloud
{
	TCActor<CDistributedAppActor> fg_ConstructApp_AppDistributionManager()
	{
		return fg_Construct<NAppDistributionManager::CAppDistributionManagerActor>();
	}
}
