// Copyright © 2025 Unbroken AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

#include "Malterlib_Cloud_App_DebugManager.h"

namespace NMib::NCloud::NDebugManager
{
	TCFuture<void> CDebugManagerApp::fp_SetupPermissions()
	{
		TCSet<CStr> Permissions;
		Permissions["DebugManager/ReadAll"];
		Permissions["DebugManager/ReadCrashDump"];
		Permissions["DebugManager/ReadAsset"];
		Permissions["DebugManager/ListAll"];
		Permissions["DebugManager/ListCrashDump"];
		Permissions["DebugManager/ListAsset"];
		Permissions["DebugManager/WriteAll"];
		Permissions["DebugManager/WriteCrashDump"];
		Permissions["DebugManager/WriteAsset"];
		Permissions["DebugManager/DeleteAll"];
		Permissions["DebugManager/DeleteCrashDump"];
		Permissions["DebugManager/DeleteAsset"];

		co_await mp_State.m_TrustManager(&CDistributedActorTrustManager::f_RegisterPermissions, Permissions);;

		TCVector<CStr> SubscribePermissions;
		SubscribePermissions.f_Insert("DebugManager/*");

		mp_Permissions = co_await mp_State.m_TrustManager(&CDistributedActorTrustManager::f_SubscribeToPermissions, SubscribePermissions, fg_ThisActor(this));

		co_return {};
	}
}
