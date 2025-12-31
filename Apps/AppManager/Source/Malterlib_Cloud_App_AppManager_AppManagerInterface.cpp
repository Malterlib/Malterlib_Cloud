// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_App_AppManager.h"

#include <Mib/Concurrency/LogError>

namespace NMib::NCloud::NAppManager
{
	TCFuture<void> CAppManagerActor::fp_PublishAppManagerInterface()
	{
		return mp_AppManagerInterface.f_Publish<CAppManagerInterface>(mp_State.m_DistributionManager, this);
	}

	TCFuture<void> CAppManagerActor::fp_RegisterPermissions()
	{
		TCSet<CStr> Permissions;
		Permissions["AppManager/VersionAppAll"];

		Permissions["AppManager/CommandAll"];
		Permissions["AppManager/Command/VersionEnum"];
		Permissions["AppManager/Command/ApplicationEnum"];
		Permissions["AppManager/Command/ApplicationAdd"];
		Permissions["AppManager/Command/ApplicationRemove"];
		Permissions["AppManager/Command/ApplicationUpdate"];
		Permissions["AppManager/Command/ApplicationChangeSettings"];
		Permissions["AppManager/Command/ApplicationStart"];
		Permissions["AppManager/Command/ApplicationStop"];
		Permissions["AppManager/Command/ApplicationRestart"];
		Permissions["AppManager/Command/ApplicationSubscribeUpdates"];
		Permissions["AppManager/Command/ApplicationSubscribeChanges"];

		Permissions["AppManager/AppAll"];

		for (auto &pApplication : mp_Applications)
		{
			auto &Application = *pApplication;
			Permissions[fg_Format("AppManager/App/{}", Application.m_Name)];
		}

		co_return co_await mp_State.m_TrustManager(&CDistributedActorTrustManager::f_RegisterPermissions, fg_Move(Permissions));
	}

	TCFuture<void> CAppManagerActor::fp_RegisterApplicationPermissions(TCSharedPointer<CApplication> _pApplication)
	{
		auto Permissions = fg_CreateSet<CStr>(fg_Format("AppManager/App/{}", _pApplication->m_Name));
		co_return co_await mp_State.m_TrustManager(&CDistributedActorTrustManager::f_RegisterPermissions, fg_Move(Permissions));
	}

	TCFuture<void> CAppManagerActor::fp_UnregisterApplicationPermissions(TCSharedPointer<CApplication> _pApplication)
	{
		auto Permissions = fg_CreateSet<CStr>(fg_Format("AppManager/App/{}", _pApplication->m_Name));
		co_return co_await mp_State.m_TrustManager(&CDistributedActorTrustManager::f_UnregisterPermissions, fg_Move(Permissions));
	}

	TCFuture<void> CAppManagerActor::fp_SubscribePermissions()
	{
		TCVector<CStr> SubscribePermissions;
		SubscribePermissions.f_Insert("AppManager/*");

		mp_Permissions = co_await mp_State.m_TrustManager(&CDistributedActorTrustManager::f_SubscribeToPermissions, SubscribePermissions, fg_ThisActor(this));
		mp_Permissions.f_OnPermissionsAdded
			(
				g_ActorFunctorWeak / [=, this](CPermissionIdentifiers _Identifiers, NContainer::TCMap<NStr::CStr, CPermissionRequirements> _Permissions) -> TCFuture<void>
				{
					for (auto &Permission : _Permissions.f_Keys())
					{
						if (Permission == "AppManager/Command/ApplicationSubscribeChanges" || Permission == "AppManager/CommandAll" || Permission.f_StartsWith("AppManager/App"))
						{
							co_await fp_ChangeNotifications_PermissionsChanged().f_Wrap() > fg_LogError("CloudManager", "Failed to update change notification due to permission change");
							break;
						}
					}

					co_return {};
				}
			)
		;
		mp_Permissions.f_OnPermissionsRemoved
			(
				g_ActorFunctorWeak / [=, this](CPermissionIdentifiers _Identifiers, NContainer::TCSet<NStr::CStr> _Permissions) -> TCFuture<void>
				{
					for (auto &Permission : _Permissions)
					{
						if (Permission == "AppManager/Command/ApplicationSubscribeChanges" || Permission == "AppManager/CommandAll" || Permission.f_StartsWith("AppManager/App"))
						{
							co_await fp_ChangeNotifications_PermissionsChanged().f_Wrap() > fg_LogError("CloudManager", "Failed to update change notification due to permission change");
							break;
						}
					}

					co_return {};
				}
			)
		;

		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_SetupAppManagerInterfacePermissions()
	{
		NContainer::TCMap<NStr::CStr, CPermissionRequirements> CommandLinePermissions = {{"AppManager/VersionAppAll", {}}, {"AppManager/AppAll", {}}, {"AppManager/CommandAll", {}}};

		co_await
			(
				mp_State.m_TrustManager
				(
					&CDistributedActorTrustManager::f_AddPermissions
					, CPermissionIdentifiers{mp_State.m_CommandLineHostID, ""}
					, CommandLinePermissions
					, EDistributedActorTrustManagerOrderingFlag_None
				)
				+ fp_RegisterPermissions()
				+ fp_SubscribePermissions()
			)
		;

		co_return {};
	}
}
