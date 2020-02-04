// Copyright © 2018 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
#include "Malterlib_Cloud_App_AppDistributionManager.h"

namespace NMib::NCloud::NAppDistributionManager
{
	CAppDistributionManagerActor::CAppDistributionManagerActor()
		: CDistributedAppActor(CDistributedAppActor_Settings("AppDistributionManager").f_AuditCategory("Malterlib/Cloud/AppDistributionManager"))
	{
	}

	CAppDistributionManagerActor::~CAppDistributionManagerActor() = default;

	TCFuture<void> CAppDistributionManagerActor::fp_StartApp(NEncoding::CEJSON const &_Params)
	{
		mp_FileActor = fg_ConstructActor<CSeparateThreadActor>(fg_Construct("App distribution manager file operations"));
		
		TCPromise<void> Promise;
		fp_ReadState() > Promise / [this, Promise]
			{
				if (mp_State.m_bStoppingApp)
					return Promise.f_SetException(DMibErrorInstance("Startup aborted"));
				
				mp_State.m_TrustManager
					(
						&CDistributedActorTrustManager::f_SubscribeTrustedActors<CVersionManager>
						, CVersionManager::mc_pDefaultNamespace
						, fg_ThisActor(this)
					)
					> Promise / [this, Promise](TCTrustedActorSubscription<CVersionManager> &&_VersionSubscrption)
					{
						if (mp_State.m_bStoppingApp)
							return Promise.f_SetException(DMibErrorInstance("Startup aborted"));

						mp_VersionManagerSubscription = fg_Move(_VersionSubscrption);
						mp_VersionManagerSubscription.f_OnActor
							(
								[this](TCDistributedActor<CVersionManager> const &_VersionManager, CTrustedActorInfo const &_ActorInfo)
								{
									fp_VersionManagerAdded(_VersionManager, _ActorInfo);
								}
							)
						;
						mp_VersionManagerSubscription.f_OnRemoveActor
							(
								[this](TCWeakDistributedActor<CActor> const &_VersionManager, CTrustedActorInfo &&_ActorInfo)
								{
									fp_VersionManagerRemoved(_VersionManager);
								}
							)
						;

						Promise.f_SetResult();
					}
				;
			}
		;

		return Promise.f_MoveFuture();
	}
	
	TCFuture<void> CAppDistributionManagerActor::fp_StopApp()
	{	
		TCActorResultVector<void> DistributionDestroys;

		for (auto &Distribution : mp_Distributions)
		{
			for (auto &RunningDeploys : Distribution.m_RunningDeploys)
			{
				for (auto &RunningDeploy : RunningDeploys)
					RunningDeploy.f_Destroy() > DistributionDestroys.f_AddResult();
			}
		}

		co_await DistributionDestroys.f_GetResults();

		co_await mp_FileActor.f_Destroy();

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
