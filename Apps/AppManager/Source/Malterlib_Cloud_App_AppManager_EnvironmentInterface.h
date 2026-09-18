// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <Mib/Cloud/AppManager>
#include <Mib/Cloud/VersionManager>

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
			NStr::CStr m_Directory;                                                           /// Host directory in a Local environment; empty for environment-managed storage
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
			NContainer::TCVector<NStr::CStr> m_LowPortExecutables;                            /// Executables relative to the directory that may bind low ports
		};

		struct CEnvironmentScript
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NStr::CStr m_Description;
			NStr::CStr m_Script;                  /// Script path as configured; the agent expands it against the directory inside the environment
			NStr::CStr m_Application;             /// Application the script belongs to; used to derive the directory when m_Directory is empty
			NStr::CStr m_Directory;               /// Working directory; empty when the environment manages the application storage
			NStr::CStr m_Parameter;
			NContainer::TCMap<NStr::CStr, NStr::CStr> m_Environment;
			NStr::CStr m_RunAsUser;               /// Application user name as configured; the agent resolves the account name
			NStr::CStr m_RunAsGroup;              /// Application group name as configured; the agent resolves the group name
			NStr::CStrSecure m_RunAsUserPassword; /// Set on Windows, where a local agent logs on with the account the host created
			bool m_bRunAsUserHasShell = false;
		};

		struct CStageFileContents
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NConcurrency::TCAsyncGenerator<NContainer::CIOByteVector> m_DataGenerator;
			NConcurrency::TCActorSubscriptionWithID<> m_Subscription;
			uint64 m_StartPosition;
		};

		struct CStageFile
		{
			using CDownloadFileContents = CStageFileContents;

			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NStr::CStr m_FilePath;
			NFile::EFileAttrib m_FileAttributes = NFile::EFileAttrib_None;
			NTime::CTime m_WriteTime;
			uint64 m_FileSize = 0;
			NStr::CStr m_SymlinkContents;
			NConcurrency::TCActorFunctorWithID<NConcurrency::TCFuture<CStageFileContents> (uint64 _StartPosition, NCryptography::CHashDigest_SHA256 _StartDigest)> m_fGetDataGenerator;
		};

		struct CApplicationStage
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NStr::CStr m_Name;
			uint64 m_QueueSize = NFile::gc_IdealNetworkQueueSize;
			NStorage::TCOptional<NConcurrency::TCAsyncGeneratorWithID<CStageFile>> m_FilesGenerator;
			NStr::CStr m_RunAsUser; /// Owner of the staged files: the account the new version runs as
			NStr::CStr m_RunAsGroup;
			bool m_bRunAsUserHasShell = false;
		};

		struct CApplicationStageResult
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NStr::CStr m_StageID;
			NStr::CStr m_Directory;                                                        /// Application directory inside the environment
			NStr::CStr m_StageDirectory;                                                   /// Directory holding the staged files inside the environment
			NConcurrency::TCActorFunctorWithID<NConcurrency::TCFuture<void> ()> m_fFinish; /// Call to await the receive; dropping it uncalled aborts an in-flight receive
		};

		struct CApplicationApply
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NStr::CStr m_Name;
			NStr::CStr m_StageID;
			NContainer::TCVector<NStr::CStr> m_Files;    /// Paths relative to the stage; files and links move to the same paths in the application directory, directories are created there
			NContainer::TCVector<NStr::CStr> m_OldFiles; /// Paths of the previously installed version, removed before the staged files move in; nonempty directories stay
			NStr::CStr m_RunAsUser;
			NStr::CStr m_RunAsGroup;
			bool m_bRunAsUserHasShell = false;
			bool m_bFailOnExisting = false;              /// When set, only TempVersion, lost+found, .home and .tmp may already exist
			NStr::CStr m_VersionExtraInfo;               /// Encoded version extra info for platform post-processing, for example low port executables
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

		virtual NConcurrency::TCFuture<CApplicationStageResult> f_StageApplicationFiles(CApplicationStage _Stage) = 0;
		virtual NConcurrency::TCFuture<void> f_ApplyApplicationFiles(CApplicationApply _Apply) = 0;
		virtual NConcurrency::TCFuture<void> f_DiscardApplicationStage(NStr::CStr _Name, NStr::CStr _StageID) = 0;
		virtual NConcurrency::TCFuture<void> f_RemoveApplicationStorage(NStr::CStr _Name) = 0;

		/// Configures the agent with the OS dependencies to install inside the environment
		virtual NConcurrency::TCFuture<void> f_ConfigureAgent(CAgentConfig _Config) = 0;

		virtual NConcurrency::TCFuture<void> f_ShutdownEnvironment() = 0;

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
