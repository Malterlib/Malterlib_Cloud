// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <Mib/Cloud/AppManager>

namespace NMib::NCloud::NAppManager
{
	/// Interface published by an AppManager running as an environment agent.
	/// The host AppManager uses it to launch and control applications inside
	/// the environment (a container, a virtual machine or a local child process).
	struct CAppManagerEnvironmentInterface : public NConcurrency::CActor
	{
		static constexpr ch8 const *mc_pDefaultNamespace = "com.malterlib/Cloud/AppManagerEnvironment";

		CAppManagerEnvironmentInterface();
		~CAppManagerEnvironmentInterface();

		enum : uint32
		{
			EProtocolVersion_Min = 0x102
			, EProtocolVersion_Current = 0x102
		};

		enum EApplicationState : uint32
		{
			EApplicationState_Status
			, EApplicationState_Launched
			, EApplicationState_LaunchFailed
			, EApplicationState_Exited
		};

		struct CAgentInfo
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NStr::CStr m_Platform;
			NStr::CStr m_PlatformFamily;
		};

		struct CEnvironmentLaunch
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NStr::CStr m_Name;
			NStr::CStr m_Directory; /// Absolute application directory. The path is the same inside the environment as on the host.
			NStr::CStr m_Executable;
			NContainer::TCVector<NStr::CStr> m_Parameters;
			NStr::CStr m_RunAsUser;
			NStr::CStr m_RunAsGroup;
			bool m_bRunAsUserHasShell = false;
			bool m_bDistributedApp = false;
			NStr::CStr m_InterfaceAddress; /// Host AppManager address the application connects its distributed app interface to
			NStr::CStr m_LaunchID; /// Host AppManager launch id the application registers with
		};

		struct CEnvironmentScript
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NStr::CStr m_Description;
			NStr::CStr m_Script; /// Script path, expanded against m_Directory inside the environment
			NStr::CStr m_Directory;
			NStr::CStr m_Parameter;
			NContainer::TCMap<NStr::CStr, NStr::CStr> m_Environment;
			NStr::CStr m_RunAsUser;
			NStr::CStr m_RunAsGroup;
		};

		struct CApplicationStateChange
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NStr::CStr m_Application;
			EApplicationState m_State = EApplicationState_Status;
			NStr::CStr m_Status;
			CAppManagerInterface::EStatusSeverity m_StatusSeverity = CAppManagerInterface::EStatusSeverity_None;
			uint32 m_ExitStatus = 0;
		};

		virtual NConcurrency::TCFuture<CAgentInfo> f_GetAgentInfo() = 0;

		/// Returns the launch id the application runs with: the provided one for a
		/// new launch, or the previous one when a matching application was already
		/// running in the environment
		virtual NConcurrency::TCFuture<NStr::CStr> f_LaunchApplication(CEnvironmentLaunch _Launch) = 0;
		virtual NConcurrency::TCFuture<uint32> f_StopApplication(NStr::CStr _Name) = 0;
		virtual NConcurrency::TCFuture<void> f_RunScript(CEnvironmentScript _Script) = 0;
	};

	/// Interface published by the host AppManager. Environment agents use it to
	/// hand over their environment interface after they have connected.
	struct CAppManagerEnvironmentHostInterface : public NConcurrency::CActor
	{
		static constexpr ch8 const *mc_pDefaultNamespace = "com.malterlib/Cloud/AppManagerEnvironmentHost";

		CAppManagerEnvironmentHostInterface();
		~CAppManagerEnvironmentHostInterface();

		enum : uint32
		{
			EProtocolVersion_Min = 0x102
			, EProtocolVersion_Current = 0x102
		};

		virtual auto f_RegisterEnvironmentAgent
			(
				NStr::CStr _LaunchID
				, NConcurrency::TCDistributedActorInterfaceWithID<CAppManagerEnvironmentInterface> _Interface
			)
			-> NConcurrency::TCFuture<NConcurrency::TCActorSubscriptionWithID<>> = 0
		;
		virtual NConcurrency::TCFuture<void> f_ReportApplicationState(CAppManagerEnvironmentInterface::CApplicationStateChange _Change) = 0;

		/// Generates a connection ticket for an application launched in the calling
		/// agent's environment; the application uses it to connect its distributed
		/// app interface to the host AppManager
		virtual NConcurrency::TCFuture<NStr::CStr> f_RequestApplicationConnectionTicket(NStr::CStr _LaunchID) = 0;
	};
}
