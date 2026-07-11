// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <Mib/Cloud/AppManager>

namespace NMib::NCloud::NAppManager
{
	/// Unpublished interface an environment agent shares with each environment host actor its trust manager allows
	struct CAppManagerEnvironmentInterface : public NConcurrency::CActor
	{
		// Every host and agent with this interface comes from the release that introduces environments, which streams the layouts of 0x107.
		// A later change of a layout takes a new version, which the stream functions check for the fields they add
		enum EProtocolVersion : uint32
		{
			EProtocolVersion_Min       = 0x107
			, EProtocolVersion_Current = 0x107
		};

		struct CAgentInfo
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NStr::CStr m_Platform;
			NStr::CStr m_PlatformFamily;
		};

		struct CAgentConfig
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NContainer::TCMap<NStr::CStr, NContainer::TCVector<NStr::CStr>> m_OSDependencies; /// OS dependencies of the agent application and the environment by OS selector
		};

		struct CEnvironmentLaunch
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NStr::CStr m_Name;
			NStr::CStr m_Directory;                                                           /// Absolute application directory. The path is the same inside the environment as on the host.
			NStr::CStr m_Executable;
			NContainer::TCVector<NStr::CStr> m_Parameters;
			NStr::CStr m_RunAsUser;
			NStr::CStr m_RunAsGroup;
			bool m_bRunAsUserHasShell = false;
			NStr::CStrSecure m_RunAsUserPassword;                                             /// Set on Windows, where a local agent logs on with the account the host created
			bool m_bDistributedApp = false;
			NStr::CStr m_InterfaceAddress;                                                    /// Host AppManager address the application connects its distributed app interface to
			NStr::CStr m_LaunchID;                                                            /// Launch ID the host assigned to the application, which it registers with
			NContainer::TCMap<NStr::CStr, NContainer::TCVector<NStr::CStr>> m_OSDependencies; /// OS dependencies of the application, installed by the agent inside the environment
		};

		struct CEnvironmentScript
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NStr::CStr m_Description;
			NStr::CStr m_Script;                  /// Script path, expanded against m_Directory inside the environment
			NStr::CStr m_Directory;
			NStr::CStr m_Parameter;
			NContainer::TCMap<NStr::CStr, NStr::CStr> m_Environment;
			NStr::CStr m_RunAsUser;               /// Application user name as configured; the agent resolves the account name
			NStr::CStr m_RunAsGroup;              /// Application group name as configured; the agent resolves the group name
			NStr::CStrSecure m_RunAsUserPassword; /// Set on Windows, where a local agent logs on with the account the host created
		};

		struct CApplicationStateChange
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NStr::CStr m_Application;
			NStr::CStr m_Status;
			CAppManagerInterface::EStatusSeverity m_StatusSeverity = CAppManagerInterface::EStatusSeverity_None;
		};

		CAppManagerEnvironmentInterface();
		~CAppManagerEnvironmentInterface();

		virtual NConcurrency::TCFuture<CAgentInfo> f_GetAgentInfo() = 0;

		virtual NConcurrency::TCFuture<NStr::CStr> f_LaunchApplication(CEnvironmentLaunch _Launch) = 0;
		virtual NConcurrency::TCFuture<uint32> f_StopApplication(NStr::CStr _Name) = 0;
		virtual NConcurrency::TCFuture<void> f_RunScript(CEnvironmentScript _Script) = 0;

		/// Configures the agent with the OS dependencies to install inside the environment
		virtual NConcurrency::TCFuture<void> f_ConfigureAgent(CAgentConfig _Config) = 0;

		static constexpr ch8 const *mc_pDefaultNamespace = "com.malterlib/Cloud/AppManagerEnvironment";
	};

	/// Interface published by the host AppManager for its environment agents
	struct CAppManagerEnvironmentHostInterface : public NConcurrency::CActor
	{
		enum EProtocolVersion : uint32
		{
			EProtocolVersion_Min       = 0x105
			, EProtocolVersion_Current = 0x105
		};

		CAppManagerEnvironmentHostInterface();
		~CAppManagerEnvironmentHostInterface();

		virtual auto f_RegisterEnvironmentAgent(NStr::CStr _LaunchID, NConcurrency::TCDistributedActorInterfaceWithID<CAppManagerEnvironmentInterface> _Interface)
			-> NConcurrency::TCFuture<NConcurrency::TCActorSubscriptionWithID<>> = 0
		;
		virtual NConcurrency::TCFuture<void> f_ReportApplicationState(CAppManagerEnvironmentInterface::CApplicationStateChange _Change) = 0;

		virtual NConcurrency::TCFuture<NStr::CStr> f_RequestApplicationConnectionTicket(NStr::CStr _LaunchID) = 0;

		static constexpr ch8 const *mc_pDefaultNamespace = "com.malterlib/Cloud/AppManagerEnvironmentHost";
	};
}
