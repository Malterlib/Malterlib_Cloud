// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Concurrency/ActorSubscription>
#include <Mib/Concurrency/LogError>

#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud::NAppManager
{
	TCFuture<void> CAppManagerActor::fp_CloudManagerAdded(TCDistributedActor<CCloudManager> const &_CloudManager, CTrustedActorInfo const &_Info)
	{
		CCloudManager::CAppManagerInfo Info;
		Info.m_Environment = mp_State.m_ConfigDatabase.m_Data.f_GetMemberValue("Environment", "").f_String();
		Info.m_HostName = NProcess::NPlatform::fg_Process_GetFullyQualiedHostName();
		Info.m_Platform = DMibStringize(DPlatform);
		Info.m_PlatformFamily = DMibStringize(DPlatformFamily);
  		Info.m_Version = (*g_CloudVersion).m_Version;
		Info.m_ProgramDirectory = mp_Settings.m_RootDirectory;

		auto Subscription = co_await _CloudManager.f_CallActor(&CCloudManager::f_RegisterAppManager)(mp_AppManagerInterface.m_Actor->f_ShareInterface<CAppManagerInterface>(), fg_Move(Info));

		auto &NewManager = mp_CloudManagers[_CloudManager];
		NewManager.m_HostInfo = _Info;
		NewManager.m_RegisterSubscription = fg_Move(Subscription);

		co_return {};
	}

	void CAppManagerActor::fp_CloudManagerRemoved(TCWeakDistributedActor<CActor> const &_CloudManager)
	{
		auto pCloudManagerState = mp_CloudManagers.f_FindEqual(_CloudManager);
		if (!pCloudManagerState)
			return;

		if (pCloudManagerState->m_RegisterSubscription)
			pCloudManagerState->m_RegisterSubscription->f_Destroy() > fg_DiscardResult();

		mp_CloudManagers.f_Remove(_CloudManager);
	}
}
