// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Encoding/JsonShortcuts>
#include <Mib/Cryptography/RandomID>
#include <Mib/Concurrency/ActorSubscription>
#include <Mib/Concurrency/LogError>
#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud::NAppManager
{
	auto CAppManagerActor::fp_ApplicationFromHostID(CStr const &_HostID) -> TCSharedPointer<CApplication>
	{
		for (auto &pApplication : mp_Applications)
		{
			auto &Application = *pApplication;
			if (Application.m_AssociatedHostID == _HostID)
				return pApplication;
		}

		return nullptr;
	}

	auto CAppManagerActor::fp_ApplicationFromLaunchID(CStr const &_LaunchID) -> TCSharedPointer<CApplication>
	{
		for (auto &pApplication : mp_Applications)
		{
			auto &Application = *pApplication;
			if (Application.m_LaunchID == _LaunchID)
				return pApplication;
		}

		return nullptr;
	}

	NConcurrency::TCFuture<NConcurrency::TCActorSubscriptionWithID<>> CAppManagerActor::CDistributedAppInterfaceServerImplementation::f_RegisterDistributedApp
		(
			NConcurrency::TCDistributedActorInterfaceWithID<CDistributedAppInterfaceClient> _ClientInterface
			, NConcurrency::TCDistributedActorInterfaceWithID<CDistributedActorTrustManagerInterface> _TrustInterface
			, CRegisterInfo _RegisterInfo
		)
	{
		if (!_ClientInterface)
			co_return DMibErrorInstance("Invalid client interface");

		auto pThis = m_pThis;

		CCallingHostInfo CallingHostInfo = NConcurrency::fg_GetCallingHostInfo();

		auto HostID = CallingHostInfo.f_GetRealHostID();

		TCSharedPointer<CApplication> pApplication;

		if (_RegisterInfo.m_LaunchID)
			pApplication = pThis->fp_ApplicationFromLaunchID(*_RegisterInfo.m_LaunchID);

		if (!pApplication)
			pApplication = pThis->fp_ApplicationFromHostID(HostID);

		if (!pApplication)
		{
			DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Unassociated application registered: {}", CallingHostInfo.f_GetHostInfo().f_GetDesc());
			co_return DErrorInstance("Application not associated with your host");
		}

		auto &Application = *pApplication;

		Application.m_AppInterface = fg_Move(_ClientInterface);

		bool bApplicationJsonChanged = false;
		bool bHostIDChanged = false;

		if (Application.m_AssociatedHostID != HostID)
		{
			Application.m_AssociatedHostID = HostID;
			bApplicationJsonChanged = true;
			bHostIDChanged = true;
		}

		if (!Application.m_RegisterInfo.f_IsSameIgnoringLaunchID(_RegisterInfo))
		{
			bool bUpdateTypeChanged = _RegisterInfo.m_UpdateType != Application.m_RegisterInfo.m_UpdateType;
			Application.m_RegisterInfo = _RegisterInfo;
			if (bUpdateTypeChanged)
				pThis->fp_OnAppUpdateInfoChange(pApplication);
			bApplicationJsonChanged = true;
			pThis->fp_UpdateLimits();
		}

		if (bHostIDChanged)
			pThis->fp_SendAppChange_AddedOrChanged(*pApplication);

		if (bApplicationJsonChanged)
			pThis->fp_UpdateApplicationJson(pApplication) > fg_LogError("Malterlib/Cloud/AppManager", "Failed to update application JSON");

		DMibLogWithCategory
			(
				Malterlib/Cloud/AppManager
				, Info
				, "Application '{}' registered from host '{}' and uses update type '{}'"
				, Application.m_Name
				, CallingHostInfo.f_GetHostInfo().f_GetDesc()
				, fsp_UpdateTypeToStr(_RegisterInfo.m_UpdateType)
			)
		;

		for (auto &fOnRegisterDistributedApp : Application.m_OnRegisterDistributedApp)
			fOnRegisterDistributedApp.f_SetResult();
		Application.m_OnRegisterDistributedApp.f_Clear();

		co_return g_ActorSubscription / [pApplication, AssignSequence = ++Application.m_AppInterfaceAssignSequence, HostInfo = CallingHostInfo.f_GetHostInfo()]() -> TCFuture<void>
			{
				if (pApplication->m_bDeleted || AssignSequence != pApplication->m_AppInterfaceAssignSequence)
					co_return {};

				auto &Application = *pApplication;

				TCFuture<void> DestroyFuture = Application.m_AppInterface.f_Destroy();
				Application.m_AppInterface.f_Clear();

				DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Application registration lost: {}", HostInfo.f_GetDesc());

				co_await fg_Move(DestroyFuture);
				co_return {};
			}
		;
	}

	TCFuture<TCActorSubscriptionWithID<>> CAppManagerActor::CDistributedAppInterfaceServerImplementation::f_RegisterConfigFiles(CConfigFiles _ConfigFiles)
	{
		auto pThis = m_pThis;

		if (!pThis->mp_HostMonitor)
			co_return {};

		CCallingHostInfo CallingHostInfo = NConcurrency::fg_GetCallingHostInfo();

		DMibLogWithCategory
			(
				Malterlib/Cloud/AppManager
				, Info
				, "Host '{}' registered config files: {}"
				, CallingHostInfo.f_GetHostInfo().f_GetDesc()
				, CStr::fs_ToStr(_ConfigFiles.m_Files).f_Trim()
			)
		;

		co_return co_await pThis->mp_HostMonitor(&CHostMonitor::f_MonitorConfigs, fg_Move(_ConfigFiles));
	}

	TCFuture<TCDistributedActorInterfaceWithID<CDistributedAppSensorReporter>> CAppManagerActor::CDistributedAppInterfaceServerImplementation::f_GetSensorReporter()
	{
		auto pThis = m_pThis;

		CCallingHostInfo CallingHostInfo = NConcurrency::fg_GetCallingHostInfo();

		auto pApplication = pThis->fp_ApplicationFromHostID(CallingHostInfo.f_GetRealHostID());
		if (!pApplication)
		{
			DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Unassociated application requested sensor reporter: {}", CallingHostInfo.f_GetHostInfo().f_GetDesc());
			co_return DErrorInstance("Application not associated with your host");
		}

		co_return TCDistributedActorInterfaceWithID<CDistributedAppSensorReporter>
			(
				pThis->mp_SensorReporterInterface.m_Actor->f_ShareInterface<CDistributedAppSensorReporter>()
				, g_ActorSubscription / []
				{
				}
			)
		;
	}

	TCFuture<TCDistributedActorInterfaceWithID<CDistributedAppLogReporter>> CAppManagerActor::CDistributedAppInterfaceServerImplementation::f_GetLogReporter()
	{
		auto pThis = m_pThis;

		CCallingHostInfo CallingHostInfo = NConcurrency::fg_GetCallingHostInfo();

		auto pApplication = pThis->fp_ApplicationFromHostID(CallingHostInfo.f_GetRealHostID());
		if (!pApplication)
		{
			DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Unassociated application requested log reporter: {}", CallingHostInfo.f_GetHostInfo().f_GetDesc());
			co_return DErrorInstance("Application not associated with your host");
		}

		co_return TCDistributedActorInterfaceWithID<CDistributedAppLogReporter>
			(
				pThis->mp_LogReporterInterface.m_Actor->f_ShareInterface<CDistributedAppLogReporter>()
				, g_ActorSubscription / []
				{
				}
			)
		;
	}

	TCFuture<void> CAppManagerActor::fp_PublishAppInterface()
	{
		return mp_AppInterfaceServer.f_Publish<CDistributedAppInterfaceServer>(mp_State.m_DistributionManager, this);
	}
}
