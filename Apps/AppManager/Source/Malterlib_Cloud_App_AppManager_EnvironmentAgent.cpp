// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Cryptography/RandomID>

#include <Mib/Concurrency/LogError>

#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud::NAppManager
{
	CAppManagerEnvironmentInterface::CAppManagerEnvironmentInterface()
	{
		DPublishActorFunction(CAppManagerEnvironmentInterface::f_GetAgentInfo);
		DPublishActorFunction(CAppManagerEnvironmentInterface::f_LaunchApplication);
		DPublishActorFunction(CAppManagerEnvironmentInterface::f_StopApplication);
		DPublishActorFunction(CAppManagerEnvironmentInterface::f_RunScript);
	}

	CAppManagerEnvironmentInterface::~CAppManagerEnvironmentInterface()
	{
	}

	CAppManagerEnvironmentHostInterface::CAppManagerEnvironmentHostInterface()
	{
		DPublishActorFunction(CAppManagerEnvironmentHostInterface::f_RegisterEnvironmentAgent);
		DPublishActorFunction(CAppManagerEnvironmentHostInterface::f_ReportApplicationState);
		DPublishActorFunction(CAppManagerEnvironmentHostInterface::f_RequestApplicationConnectionTicket);
	}

	CAppManagerEnvironmentHostInterface::~CAppManagerEnvironmentHostInterface()
	{
	}

	template <typename tf_CStream>
	void CAppManagerEnvironmentInterface::CAgentInfo::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Platform;
		_Stream % m_PlatformFamily;
	}
	DMibDistributedStreamImplement(CAppManagerEnvironmentInterface::CAgentInfo);

	template <typename tf_CStream>
	void CAppManagerEnvironmentInterface::CEnvironmentLaunch::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Name;
		_Stream % m_Directory;
		_Stream % m_Executable;
		_Stream % m_Parameters;
		_Stream % m_RunAsUser;
		_Stream % m_RunAsGroup;
		_Stream % m_bRunAsUserHasShell;
		_Stream % m_bDistributedApp;
		_Stream % m_InterfaceAddress;
		_Stream % m_LaunchID;
	}
	DMibDistributedStreamImplement(CAppManagerEnvironmentInterface::CEnvironmentLaunch);

	template <typename tf_CStream>
	void CAppManagerEnvironmentInterface::CEnvironmentScript::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Description;
		_Stream % m_Script;
		_Stream % m_Directory;
		_Stream % m_Parameter;
		_Stream % m_Environment;
		_Stream % m_RunAsUser;
		_Stream % m_RunAsGroup;
	}
	DMibDistributedStreamImplement(CAppManagerEnvironmentInterface::CEnvironmentScript);

	template <typename tf_CStream>
	void CAppManagerEnvironmentInterface::CApplicationStateChange::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Application;
		_Stream % m_State;
		_Stream % m_Status;
		_Stream % m_StatusSeverity;
		_Stream % m_ExitStatus;
	}
	DMibDistributedStreamImplement(CAppManagerEnvironmentInterface::CApplicationStateChange);

	auto CAppManagerActor::CAppManagerEnvironmentInterfaceImplementation::f_GetAgentInfo() -> TCFuture<CAgentInfo>
	{
		CAgentInfo Info;
		Info.m_Platform = DMalterlibCloudPlatform;
		Info.m_PlatformFamily = DMibStringize(DPlatformFamily);

		co_return fg_Move(Info);
	}

	TCFuture<CStr> CAppManagerActor::CAppManagerEnvironmentInterfaceImplementation::f_LaunchApplication(CEnvironmentLaunch _Launch)
	{
		auto pThis = m_pThis;

		if (!pThis->mp_bEnvironmentAgent)
			co_return DMibErrorInstance("Not running as an environment agent");

		if (_Launch.m_Name.f_IsEmpty() || _Launch.m_Directory.f_IsEmpty() || _Launch.m_Executable.f_IsEmpty())
			co_return DMibErrorInstance("Invalid launch of application in environment: name, directory and executable are required");

		auto *pFindApplication = pThis->mp_Applications.f_FindEqual(_Launch.m_Name);
		if (pFindApplication)
		{
			auto pExisting = *pFindApplication;
			if (pExisting->f_IsLaunched() || pExisting->m_bLaunching)
			{
				auto &ExistingSettings = pExisting->m_Settings;

				bool bSameLaunch
					= pExisting->m_DirectoryOverride == _Launch.m_Directory
					&& ExistingSettings.m_Executable == _Launch.m_Executable
					&& ExistingSettings.m_ExecutableParameters == _Launch.m_Parameters
					&& ExistingSettings.m_RunAsUser == _Launch.m_RunAsUser
					&& ExistingSettings.m_RunAsGroup == _Launch.m_RunAsGroup
					&& ExistingSettings.m_bDistributedApp == _Launch.m_bDistributedApp
				;

				// The application keeps running in the environment while the host
				// AppManager restarts, so a matching launch adopts it; the host takes
				// over with the launch id the application already runs with
				if (bSameLaunch)
					co_return fg_TempCopy(pExisting->m_EnvironmentHostLaunchID);

				co_return DMibErrorInstance("Application '{}' is already launched in the environment with different settings"_f << _Launch.m_Name);
			}

			pExisting->f_Delete();
			pThis->mp_Applications.f_Remove(_Launch.m_Name);
		}

		auto pApplication = pThis->mp_Applications[_Launch.m_Name] = fg_Construct(_Launch.m_Name, pThis);

		auto &Application = *pApplication;
		Application.m_bEphemeral = true;
		Application.m_DirectoryOverride = _Launch.m_Directory;
		Application.m_EnvironmentHostAddress = _Launch.m_InterfaceAddress;
		Application.m_EnvironmentHostLaunchID = _Launch.m_LaunchID;

		auto &Settings = Application.m_Settings;
		Settings.m_Executable = _Launch.m_Executable;
		Settings.m_ExecutableParameters = _Launch.m_Parameters;
		Settings.m_RunAsUser = _Launch.m_RunAsUser;
		Settings.m_RunAsGroup = _Launch.m_RunAsGroup;
		Settings.m_bRunAsUserHasShell = _Launch.m_bRunAsUserHasShell;
		Settings.m_bDistributedApp = _Launch.m_bDistributedApp;
		Settings.m_bBackupEnabled = false;
		Settings.m_bAutoUpdate = false;

		{
			auto BlockingActorCheckout = fg_BlockingActor();

			co_await
				(
					(
						g_Dispatch(BlockingActorCheckout)
						/ [Settings, Directory = _Launch.m_Directory, pUniqueUserGroup = pThis->mp_pUniqueUserGroup]()
						{
							fsp_CreateApplicationUserGroup
								(
									Settings
									, [](CStr const &_Info)
									{
										DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Environment agent launch: {}", _Info);
									}
									, Directory / ".home"
									, pUniqueUserGroup
								)
							;
						}
					)
					% "Failed to create user and group for application in environment"
				)
			;
		}

		if (pApplication->m_bDeleted)
			co_return DMibErrorInstance("Application was deleted while launching");

		CAppLaunchResult Result = co_await pThis->fp_LaunchApp(pApplication, false);

		if (Result.m_StartupError)
		{
			// The host retries the launch periodically; remove the failed ephemeral
			// application so the retry does not hit the already-launched check
			if (!pApplication->m_bDeleted)
			{
				co_await pApplication->f_Stop(EStopFlag_PreventLaunchUser).f_Wrap()
					> fg_LogError("Malterlib/Cloud/AppManager", "Failed to stop application after failed environment launch")
				;

				pApplication->f_AbortPendingLaunches();
				pApplication->f_Delete();

				auto *pFindCurrent = pThis->mp_Applications.f_FindEqual(_Launch.m_Name);
				if (pFindCurrent && *pFindCurrent == pApplication)
					pThis->mp_Applications.f_Remove(_Launch.m_Name);
			}

			co_return DMibErrorInstance(fg_Move(Result.m_StartupError));
		}

		co_return fg_Move(_Launch.m_LaunchID);
	}

	TCFuture<uint32> CAppManagerActor::CAppManagerEnvironmentInterfaceImplementation::f_StopApplication(CStr _Name)
	{
		auto pThis = m_pThis;

		if (!pThis->mp_bEnvironmentAgent)
			co_return DMibErrorInstance("Not running as an environment agent");

		auto *pFindApplication = pThis->mp_Applications.f_FindEqual(_Name);
		if (!pFindApplication)
			co_return DMibErrorInstance("No such application '{}' in the environment"_f << _Name);

		auto pApplication = *pFindApplication;

		uint32 ExitStatus = co_await pApplication->f_Stop(EStopFlag_PreventLaunchUser);

		pApplication->f_AbortPendingLaunches();
		pApplication->f_Delete();
		pThis->mp_Applications.f_Remove(_Name);

		co_return ExitStatus;
	}

	TCFuture<void> CAppManagerActor::CAppManagerEnvironmentInterfaceImplementation::f_RunScript(CEnvironmentScript _Script)
	{
		auto pThis = m_pThis;

		if (!pThis->mp_bEnvironmentAgent)
			co_return DMibErrorInstance("Not running as an environment agent");

		co_return co_await pThis->fp_RunBashScript(fg_Move(_Script));
	}

	void CAppManagerActor::fp_ForwardApplicationStateChange(CAppManagerEnvironmentInterface::CApplicationStateChange _Change)
	{
		if (!mp_EnvironmentHostActor)
			return;

		mp_EnvironmentHostActor.f_CallActor(&CAppManagerEnvironmentHostInterface::f_ReportApplicationState)(fg_Move(_Change))
			> fg_LogError("Malterlib/Cloud/AppManager", "Failed to report application state to environment host")
		;
	}

	TCFuture<void> CAppManagerActor::fp_RegisterWithEnvironmentHost()
	{
		CStr EnvironmentHostID = fg_GetSys()->f_GetEnvironmentVariable("MalterlibAppManagerEnvironmentHostID");
		if (EnvironmentHostID.f_IsEmpty())
			co_return DMibErrorInstance("No environment host id provided");

		CStr LaunchID = mp_Settings.m_InterfaceSettings.m_LaunchID;

		co_await mp_State.m_TrustManager
			(
				&CDistributedActorTrustManager::f_AllowHostsForNamespace
				, CStr(CAppManagerEnvironmentHostInterface::mc_pDefaultNamespace)
				, fg_CreateSet(EnvironmentHostID)
				, EDistributedActorTrustManagerOrderingFlag_None
			)
		;

		mp_EnvironmentHostActors = co_await mp_State.m_TrustManager->f_SubscribeTrustedActors<CAppManagerEnvironmentHostInterface>();

		co_await mp_EnvironmentHostActors.f_OnActor
			(
				g_ActorFunctor / [this, LaunchID](TCDistributedActor<CAppManagerEnvironmentHostInterface> _Host, CTrustedActorInfo _ActorInfo) -> TCFuture<void>
				{
					mp_EnvironmentHostActor = _Host;

					NConcurrency::TCDistributedActorInterfaceWithID<CAppManagerEnvironmentInterface> InterfaceWithID
						(
							mp_EnvironmentInterface.m_Actor->f_ShareInterface<CAppManagerEnvironmentInterface>()
							, g_ActorSubscription / []
							{
							}
						)
					;

					auto Registration = co_await _Host.f_CallActor(&CAppManagerEnvironmentHostInterface::f_RegisterEnvironmentAgent)(LaunchID, fg_Move(InterfaceWithID))
						.f_Timeout(60.0, "Timed out registering environment agent with host")
					;

					mp_EnvironmentHostRegistration = fg_Move(Registration);

					co_return {};
				}
				, g_ActorFunctor / [](TCWeakDistributedActor<CActor> _RemovedActor, CTrustedActorInfo _ActorInfo) -> TCFuture<void>
				{
					co_return {};
				}
				, "Malterlib/Cloud/AppManager"
				, "Failed to handle '{}' for environment host"
			)
		;

		co_return {};
	}

	auto CAppManagerActor::CAppManagerEnvironmentHostInterfaceImplementation::f_RegisterEnvironmentAgent
		(
			CStr _LaunchID
			, NConcurrency::TCDistributedActorInterfaceWithID<CAppManagerEnvironmentInterface> _Interface
		)
		-> TCFuture<NConcurrency::TCActorSubscriptionWithID<>>
	{
		auto pThis = m_pThis;

		CCallingHostInfo CallingHostInfo = fg_GetCallingHostInfo();
		auto HostID = CallingHostInfo.f_GetRealHostID();

		for (auto &pEnvironment : pThis->mp_Environments)
		{
			bool bMatch = !pEnvironment->m_LaunchID.f_IsEmpty() && pEnvironment->m_LaunchID == _LaunchID;
			if (!bMatch)
				bMatch = !pEnvironment->m_AgentHostID.f_IsEmpty() && pEnvironment->m_AgentHostID == HostID;

			if (!bMatch)
				continue;

			pEnvironment->m_AgentHostID = HostID;

			DMibLogWithCategory
				(
					Malterlib/Cloud/AppManager
					, Info
					, "Environment agent interface for '{}' registered from host '{}'"
					, pEnvironment->m_Name
					, CallingHostInfo.f_GetHostInfo().f_GetDesc()
				)
			;

			co_await pThis->fp_OnEnvironmentAgentConnected(pEnvironment, fg_Move(_Interface));

			co_return g_ActorSubscription / [pThis, pEnvironment]() -> TCFuture<void>
				{
					if (pEnvironment->m_bDeleted || pEnvironment->m_bStopping)
						co_return {};

					pThis->fp_OnEnvironmentAgentDisconnected(pEnvironment);

					co_return {};
				}
			;
		}

		DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Unassociated environment agent tried to register: {}", CallingHostInfo.f_GetHostInfo().f_GetDesc());
		co_return DErrorInstance("Environment agent not associated with your host");
	}

	TCFuture<void> CAppManagerActor::CAppManagerEnvironmentHostInterfaceImplementation::f_ReportApplicationState(CAppManagerEnvironmentInterface::CApplicationStateChange _Change)
	{
		auto pThis = m_pThis;

		CCallingHostInfo CallingHostInfo = fg_GetCallingHostInfo();

		if (!pThis->fp_EnvironmentFromHostID(CallingHostInfo.f_GetRealHostID()))
			co_return DErrorInstance("Environment agent not associated with your host");

		pThis->fp_OnEnvironmentApplicationStateChange(_Change);

		co_return {};
	}

	TCFuture<CStr> CAppManagerActor::CAppManagerEnvironmentHostInterfaceImplementation::f_RequestApplicationConnectionTicket(CStr _LaunchID)
	{
		auto pThis = m_pThis;

		CCallingHostInfo CallingHostInfo = fg_GetCallingHostInfo();

		auto pEnvironment = pThis->fp_EnvironmentFromHostID(CallingHostInfo.f_GetRealHostID());
		if (!pEnvironment)
			co_return DErrorInstance("Environment agent not associated with your host");

		if (_LaunchID.f_IsEmpty())
			co_return DErrorInstance("A launch id is required to request an application connection ticket");

		auto pApplication = pThis->fp_ApplicationFromLaunchID(_LaunchID);
		if (!pApplication || pApplication->m_pLaunchedEnvironment != pEnvironment)
			co_return DErrorInstance("No application with the launch id is launched in your environment");

		auto Address = co_await pThis->fp_EnsureEnvironmentListen(pEnvironment);

		if (pApplication->m_bDeleted)
			co_return DErrorInstance("Application deleted");

		DMibLogWithCategory
			(
				Malterlib/Cloud/AppManager
				, Info
				, "Generating connection ticket for '{}' in environment '{}'"
				, pApplication->m_Name
				, pEnvironment->m_Name
			)
		;

		CStr NotificationID = NCryptography::fg_RandomID(pThis->mp_EnvironmentTicketNotifications);

		auto Ticket = co_await pThis->mp_State.m_TrustManager
			(
				&CDistributedActorTrustManager::f_GenerateConnectionTicket
				, Address
				, g_ActorFunctor
				(
					g_ActorSubscription / [pThis, NotificationID]
					{
						pThis->mp_EnvironmentTicketNotifications.f_Remove(NotificationID);
					}
				)
				/ [pThis, NotificationID, pApplication](CStr _HostID, CCallingHostInfo _HostInfo, CByteVector _CertificateRequest) -> TCFuture<void>
				{
					pThis->mp_EnvironmentTicketNotifications.f_Remove(NotificationID);

					if (pApplication->m_bDeleted)
						co_return DMibErrorInstance("Application deleted");

					if (pApplication->m_AssociatedHostID != _HostID)
					{
						pApplication->m_AssociatedHostID = _HostID;
						pThis->fp_SendAppChange_AddedOrChanged(*pApplication);
					}

					auto Result = co_await pThis->fp_UpdateApplicationJson(pApplication).f_Wrap();

					if (!Result)
					{
						DMibLogWithCategory
							(
								Malterlib/Cloud/AppManager
								, Info
								, "Failed to update application JSON when granting connection ticket for '{}': {}"
								, pApplication->m_Name
								, Result.f_GetExceptionStr()
							)
						;
						co_return DMibErrorInstance("Failed to update application JSON, see AppManager log for details");
					}

					co_return {};
				}
				, nullptr
			)
		;

		pThis->mp_EnvironmentTicketNotifications[NotificationID] = fg_Move(Ticket.m_NotificationsSubscription);

		co_return Ticket.m_Ticket.f_ToStringTicket();
	}
}
