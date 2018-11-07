// Copyright © 2018 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Concurrency/Actor/Timer>
#include "Malterlib_Cloud_App_AppDistributionManager.h"

namespace NMib::NCloud::NAppDistributionManager
{
	CAppDistributionManagerActor::CAppDistributionManagerActor()
		: CDistributedAppActor(CDistributedAppActor_Settings("AppDistributionManager", false).f_AuditCategory("Malterlib/Cloud/AppDistributionManager"))
	{
	}

	CAppDistributionManagerActor::~CAppDistributionManagerActor() = default;

	TCContinuation<void> CAppDistributionManagerActor::fp_StartApp(NEncoding::CEJSON const &_Params)
	{
		mp_FileActor = fg_ConstructActor<CSeparateThreadActor>(fg_Construct("App distribution manager file operations"));
		
		TCContinuation<void> Continuation;
		fp_ReadState() > Continuation / [this, Continuation]
			{
				if (mp_State.m_bStoppingApp)
					return Continuation.f_SetException(DMibErrorInstance("Startup aborted"));
				
				mp_State.m_TrustManager
					(
						&CDistributedActorTrustManager::f_SubscribeTrustedActors<CVersionManager>
						, CVersionManager::mc_pDefaultNamespace
						, fg_ThisActor(this)
					)
					> Continuation / [this, Continuation](TCTrustedActorSubscription<CVersionManager> &&_VersionSubscrption)
					{
						if (mp_State.m_bStoppingApp)
							return Continuation.f_SetException(DMibErrorInstance("Startup aborted"));

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
								[this](TCWeakDistributedActor<CActor> const &_VersionManagero)
								{
									fp_VersionManagerRemoved(_VersionManagero);
								}
							)
						;

						Continuation.f_SetResult();
					}
				;
			}
		;

		return Continuation;
	}
	
	TCContinuation<void> CAppDistributionManagerActor::fp_StopApp()
	{	
		TCActorResultVector<void> DistributionDestroys;

		for (auto &Distribution : mp_Distributions)
		{
			for (auto &RunningDeploys : Distribution.m_RunningDeploys)
			{
				for (auto &RunningDeploy : RunningDeploys)
					RunningDeploy->f_Destroy() > DistributionDestroys.f_AddResult();
			}
		}

		TCContinuation<void> Continuation;
		
		DistributionDestroys.f_GetResults() > [=](auto &&)
			{
				mp_FileActor->f_Destroy() > Continuation;
			}
		;

		return Continuation;
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
