// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Encoding/JsonShortcuts>
#include <Mib/CommandLine/TableRenderer>
#include <Mib/Cryptography/RandomID>
#include <Mib/Cryptography/UUID>

#include <Mib/Concurrency/LogError>
#include <Mib/File/FileBeneath>

#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud
{
	// Returns the encoded AutoUpdate settings of the host for the agent of an environment of _Type, or empty for no automatic updates, with which
	// the agent only monitors. A local agent shares the OS of the host, whose own AppManager patches it and handles its reboots, and a read-only
	// container can't change its packages
	NStr::CStr fg_AppManager_EnvironmentAgentAutoUpdateConfig(CAppManagerInterface::EEnvironmentType _Type, bool _bReadOnly, NEncoding::CEJsonSortedYaml const &_HostConfig)
	{
		using namespace NMib::NEncoding;

		if (_Type == CAppManagerInterface::EEnvironmentType_Local || (_Type == CAppManagerInterface::EEnvironmentType_Container && _bReadOnly))
			return {};

		if (auto pAutoUpdate = _HostConfig.f_GetMember("AutoUpdate", EJsonType_Object))
			return CEJsonSorted::fs_FromCompatible(*pAutoUpdate).f_ToString();

		return {};
	}

	// Text that a guest reports is shown to the operators of the host in logs, statuses and terminals, so the control characters that could rewrite
	// what a terminal shows become '?', all but line feeds and tabs, also the C1 controls in UTF-8. It is cut to at most _MaxLength bytes at the
	// start of a character
	NStr::CStr fg_AppManager_SanitizeGuestText(NStr::CStr const &_Text, umint _MaxLength)
	{
		umint Length = _Text.f_GetLen();
		if (Length > _MaxLength)
		{
			Length = _MaxLength;
			while (Length && (uint8(_Text.f_GetStr()[Length]) & 0xC0) == 0x80)
				--Length;
		}

		ch8 const *pText = _Text.f_GetStr();
		NContainer::TCVector<ch8> Sanitized;
		Sanitized.f_SetLen(Length);
		umint nSanitized = 0;
		for (umint iText = 0; iText < Length; ++iText)
		{
			uint8 Byte = uint8(pText[iText]);
			if (Byte == 0xC2 && iText + 1 < Length && uint8(pText[iText + 1]) >= 0x80 && uint8(pText[iText + 1]) <= 0x9F)
			{
				Sanitized[nSanitized++] = '?';
				++iText;
			}
			else if ((Byte < 0x20 && Byte != '\n' && Byte != '\t') || Byte == 0x7F)
				Sanitized[nSanitized++] = '?';
			else
				Sanitized[nSanitized++] = ch8(Byte);
		}

		return NStr::CStr(Sanitized.f_GetArray(), nSanitized);
	}

	// Host accounts can't create the directories first, which would block the listen: the root directory is the AppManager's, and only root
	// writes /var/run. An AppManager that doesn't run as root uses its temporary directory when a socket path in its root directory is too long,
	// where they can. The socket directory is dedicated to the socket, so mounting it into a container exposes no other AppManager files. A VM
	// guest agent listens on the same path in the guest, so that the connection tickets of the listen name a path that works on both sides
	auto fg_AppManager_EnvironmentSocketDirectory(NStr::CStr const &_RootDirectory, NStr::CStr const &_EnvironmentName, bool _bContainer, bool _bRunningAsRoot)
		-> CAppManagerEnvironmentSocketDirectory
	{
		using namespace NMib::NStr;
		using namespace NMib::NFile;

		static NCryptography::CUniversallyUniqueIdentifier s_SocketDirectoryUUID("8F90EF58-4429-4746-B84B-E681D694E60E", NCryptography::EUniversallyUniqueIdentifierFormat_Bare);

		// Tells the environments of all AppManagers of a host apart, and is short enough for socket paths in most root directories
		CStr Name = NCryptography::fg_GetHashedUuidString(_RootDirectory / _EnvironmentName, s_SocketDirectoryUUID, NCryptography::EUniversallyUniqueIdentifierFormat_AlphaNum).f_Left(16);

		auto fDirectory = [&](CStr const &_Parent, bool _bShared)
			{
				CAppManagerEnvironmentSocketDirectory Directory;
				if (_bShared)
					Directory.m_SharedDirectory = _Parent;
				Directory.m_EnvironmentDirectory = _Parent / Name;
				Directory.m_SocketDirectory = _bContainer ? Directory.m_EnvironmentDirectory / "Socket" : Directory.m_EnvironmentDirectory;
				Directory.m_SocketPath = Directory.m_SocketDirectory / "Agent.socket";
				return Directory;
			}
		;

		auto Directory = fDirectory(_RootDirectory / "Sockets", true);
		if (Directory.m_SocketPath.f_GetLen() <= NSys::NNetwork::fg_GetMaxUnixSocketNameLength())
			return Directory;

		if (_bRunningAsRoot)
			return fDirectory("/var/run/MalterlibAppManager", true);

		return fDirectory(CFile::fs_GetRawTemporaryDirectory(), false);
	}

	// Blocking. Root in a docker container is root on what it writes in its read-write mount, like set-user-ID executables and device nodes, so
	// the environment directory that holds the mounted socket directory is never traversable by other users, and is checked without following
	// links. The shared and socket directories are traversable, for clients of other users like VM windows
	void fg_AppManager_PrepareEnvironmentSocketDirectory(CAppManagerEnvironmentSocketDirectory const &_Directory)
	{
		using namespace NMib::NStr;
		using namespace NMib::NFile;

		if (!_Directory.m_SharedDirectory.f_IsEmpty())
			NConcurrency::CDistributedAppActor_Settings::fs_PrepareLocalSocketDirectory(_Directory.m_SharedDirectory);

		if (_Directory.m_SocketDirectory != _Directory.m_EnvironmentDirectory)
		{
			CStr const &PrivateDirectory = _Directory.m_EnvironmentDirectory;
			constexpr EFileAttrib c_PrivateAttributes = EFileAttrib_UnixAttributesValid | EFileAttrib_UserRead | EFileAttrib_UserWrite | EFileAttrib_UserExecute;

			if (!CFile::fs_FileExists(PrivateDirectory, EFileAttrib_Directory | EFileAttrib_File | EFileAttrib_Link))
				CFile::fs_CreateDirectory(PrivateDirectory, c_PrivateAttributes);

			EFileAttrib Attributes = CFile::fs_GetAttributesOnLink(PrivateDirectory);
			if ((Attributes & EFileAttrib_Link) || !(Attributes & EFileAttrib_Directory))
				DMibError("Environment socket directory '{}' is not a directory"_f << PrivateDirectory);

#ifndef DPlatformFamily_Windows
			CStr Owner = CFile::fs_GetOwnerOnLink(PrivateDirectory);
			CStr User = NSys::fg_UserManagement_GetProcessEffectiveUserName();
			if (Owner != User)
				DMibError("Environment socket directory '{}' is owned by '{}' instead of '{}'"_f << PrivateDirectory << Owner << User);
#endif

			// Only this user writes the shared directory, and others can't replace an entry of this user in the sticky temporary directory, so the
			// directory is still the one checked
			CFile::fs_SetAttributes(PrivateDirectory, c_PrivateAttributes);
		}

		NConcurrency::CDistributedAppActor_Settings::fs_PrepareLocalSocketDirectory(_Directory.m_SocketDirectory);
	}
}

namespace NMib::NCloud::NAppManager
{
	void CAppManagerActor::CEnvironment::f_SetStatus(CStr const &_Status, CAppManagerInterface::EStatusSeverity _Severity)
	{
		m_Status = _Status;
		m_StatusSeverity = _Severity;

		m_pThis->fp_SetEnvironmentSensorStatus(TCSharedPointer<CEnvironment>(fg_Explicit(this)), _Status, _Severity)
			> fg_LogError("Malterlib/Cloud/AppManager", "Failed to report environment sensor status")
		;
	}

	TCFuture<void> CAppManagerActor::fp_SetEnvironmentSensorStatus(TCSharedPointer<CEnvironment> _pEnvironment, CStr _Status, CAppManagerInterface::EStatusSeverity _Severity)
	{
		if (!mp_bEnableApplicationStatusSensors)
			co_return {};

		auto OnResume = co_await fg_OnResume
			(
				[_pEnvironment]() -> CExceptionPointer
				{
					if (_pEnvironment->m_bDeleted)
						return DMibErrorInstance("Environment was deleted");

					return nullptr;
				}
			)
		;

		if (!_pEnvironment->m_StatusSensorReporter.m_fReportReadings)
		{
			auto SequenceSubscription = co_await _pEnvironment->m_StatusSensorReporterSequencer.f_Sequence();
			if (!_pEnvironment->m_StatusSensorReporter.m_fReportReadings)
			{
				CDistributedAppSensorReporter::CSensorInfo SensorInfo;
				SensorInfo.m_Identifier = "org.malterlib.appmanager.environment.status";
				SensorInfo.m_Name = "Environment Status";
				SensorInfo.m_IdentifierScope = _pEnvironment->m_Name;
				SensorInfo.m_Type = NConcurrency::CDistributedAppSensorReporter::ESensorDataType_Status;

				_pEnvironment->m_StatusSensorReporter = co_await fp_OpenSensorReporter(fg_Move(SensorInfo));
			}
		}

		if (!_pEnvironment->m_StatusSensorReporter.m_fReportReadings)
			co_return {};

		auto NewStatus = CDistributedAppSensorReporter::CStatus
			{
				.m_Severity = [&]
				{
					switch (_Severity)
					{
					case CAppManagerInterface::EStatusSeverity_None: return CDistributedAppSensorReporter::EStatusSeverity_Ok;
					case CAppManagerInterface::EStatusSeverity_Warning: return CDistributedAppSensorReporter::EStatusSeverity_Info;
					case CAppManagerInterface::EStatusSeverity_Error: return CDistributedAppSensorReporter::EStatusSeverity_Error;
					}

					return CDistributedAppSensorReporter::EStatusSeverity_Info;
				}
				()
				, .m_Description = _Status
			}
		;

		if (_pEnvironment->m_LastReporterSensorStatus && NewStatus == *_pEnvironment->m_LastReporterSensorStatus)
			co_return {};

		_pEnvironment->m_LastReporterSensorStatus = NewStatus;

		TCVector<CDistributedAppSensorReporter::CSensorReading> Readings;
		Readings.f_Insert().m_Data = fg_Move(NewStatus);

		co_await _pEnvironment->m_StatusSensorReporter.m_fReportReadings(fg_Move(Readings));

		co_return {};
	}

	bool CAppManagerActor::CEnvironment::f_IsStarted() const
	{
		return m_bStarted && m_AgentInterface;
	}

	// The agent of a container or VM is a guest, so its updates defer host reboots and environment stops only within the limit of
	// CAppManagerGuestDeferral. A Local agent runs on the host, like the host AppManager's own updates
	bool CAppManagerActor::CEnvironment::f_AgentUpdateDefers() const
	{
		if (m_AgentUpdateInProgress.f_IsEmpty())
			return false;

		if (m_Settings.m_Type == CAppManagerInterface::EEnvironmentType_Local)
			return true;

		return m_AgentUpdateDeferral.f_Defers(CAppManagerGuestDeferral::fs_Now());
	}

	// An empty description ends the update
	void CAppManagerActor::CEnvironment::f_SetAgentUpdateInProgress(CStr &&_Description)
	{
		m_AgentUpdateDeferral.f_Update(!_Description.f_IsEmpty(), CAppManagerGuestDeferral::fs_Now());
		m_AgentUpdateInProgress = fg_Move(_Description);
	}

	void CAppManagerActor::fp_BuildCommandLine_Environments(CDistributedAppCommandLineSpecification &o_CommandLine)
	{
		auto EnvironmentManagement = o_CommandLine.f_AddSection("Environment Management", "Commands to manage AppManager launch environments (containers and virtual machines).");

		auto NameOption = "Name"_o=
			{
				"Names"_o= _o["--name"]
				, "Type"_o= ""
				, "Description"_o= "Unique name of the environment."
			}
		;
		auto SettingsOption_AgentApplication = "AgentApplication?"_o=
			{
				"Names"_o= _o["--agent-application"]
				, "Type"_o= ""
				, "Default"_o= ""
				, "Description"_o= "Name of the application that provides the AppManager agent executable for the environment.\n"
				"When empty, container and VM environments default to the SelfUpdate.<Platform> application\n"
				"matching the guest platform, and local environments run the AppManager executable itself.\n"
				"The SelfUpdate.<Platform> name for the host platform resolves to the self update source\n"
				"application, since host platform distributions install as the self update application.\n"
				"See --application-enable-self-update for installing agent executables for other platforms."
			}
		;
		auto SettingsOption_ParentApplication = "ParentApplication?"_o=
			{
				"Names"_o= _o["--parent-application"]
				, "Type"_o= ""
				, "Default"_o= ""
				, "Description"_o= "Name of the application whose storage confines the environment storage.\n"
				"The environment storage, with the agent files and the application data of Linux containers, and VM\n"
				"images live inside that application's directory, so they can be placed on encrypted storage by using\n"
				"an application with encryption storage."
			}
		;
		auto SettingsOption_AutoStart = "AutoStart?"_o=
			{
				"Names"_o= _o["--auto-start"]
				, "Type"_o= true
				, "Default"_o= true
				, "Description"_o= "Start the environment automatically when the AppManager starts."
			}
		;
		auto SettingsOption_ContainerRuntime = "ContainerRuntime?"_o=
			{
				"Names"_o= _o["--container-runtime"]
#ifdef DPlatformFamily_macOS
				, "Type"_o= COneOf{"AppleContainer", "Colima"}
				, "Default"_o= "AppleContainer"
#else
				, "Type"_o= COneOf{"Docker"}
				, "Default"_o= "Docker"
#endif
				, "Description"_o= "The container runtime to use.\n"
				"'Colima' runs docker containers in a colima virtual machine owned by the AppManager."
			}
		;
		auto SettingsOption_ContainerImage = "ContainerImage?"_o=
			{
				"Names"_o= _o["--container-image"]
				, "Type"_o= ""
				, "Default"_o= ""
				, "Description"_o= "The container image reference to run the environment from."
			}
		;
		auto SettingsOption_ContainerNetwork = "ContainerNetwork?"_o=
			{
				"Names"_o= _o["--container-network"]
				, "Type"_o= ""
				, "Default"_o= ""
				, "Description"_o= "The container network mode.\n"
				"Leave empty to use the container runtime's default network, which is shared with the containers of other environments and\n"
				"reaches host services as the runtime allows, as bridge and default do.\n"
				"Other networks than bridge, none and default, like host, container:<Name> or a named network, need the\n"
				"AppManager/EnvironmentHostAccess permission."
			}
		;
		auto SettingsOption_ContainerExtraMounts = "ContainerExtraMounts?"_o=
			{
				"Names"_o= _o["--container-extra-mounts"]
				, "Type"_o=
				{
					"*"_o= ""
				}
				, "Description"_o= "Additional bind mounts for the environment beyond its agent and data storage.\n"
				"Maps host paths to paths inside the environment. Needs the AppManager/EnvironmentHostAccess permission.\n"
				"Example: '{\"/opt/Data\": \"/opt/Data\"}'\n"
			}
		;
		auto SettingsOption_ContainerReadOnly = "ContainerReadOnly?"_o=
			{
				"Names"_o= _o["--container-read-only"]
				, "Type"_o= true
				, "Default"_o= false
				, "Description"_o= "Run the container with a read-only filesystem, so all writes are confined to the\n"
				"data storage of the environment. Writes outside the mounted paths would otherwise go to\n"
				"the container runtime's own storage, which is not confined by --parent-application."
			}
		;
		auto SettingsOption_ContainerExtraArguments = "ContainerExtraArguments?"_o=
			{
				"Names"_o= _o["--container-extra-arguments"]
				, "Default"_o= _o[]
				, "Type"_o= _o[""]
				, "Description"_o= "Additional arguments appended to the container run command.\n"
				"Needs the AppManager/EnvironmentHostAccess permission."
			}
		;
		auto SettingsOption_OSDependencies = "OSDependencies?"_o=
			{
				"Names"_o= _o["--os-dependencies"]
				, "Default"_o= _o[]
				, "Type"_o= _o[""]
				, "Description"_o= "Additional OS packages installed inside the environment by the agent, in addition\n"
				"to the OS dependencies of the agent application.\n"
				"Example: '[\"htop\", \"strace\"]'\n"
				"A Local environment installs them on the host, which needs the AppManager/EnvironmentHostAccess permission."
			}
		;
		auto SettingsOption_MemoryLimitMB = "MemoryLimitMB?"_o=
			{
				"Names"_o= _o["--memory-limit"]
				, "Type"_o= 0
				, "Default"_o= 0
				, "Description"_o= "Memory limit in megabytes for the environment. Set to 0 for no limit."
			}
		;
		auto SettingsOption_CPULimit = "CPULimit?"_o=
			{
				"Names"_o= _o["--cpu-limit"]
				, "Type"_o= 0.0
				, "Default"_o= 0.0
				, "Description"_o= "Number of CPUs the environment may use. Set to 0 for no limit."
			}
		;
		auto SettingsOption_VMImage = "VMImage?"_o=
			{
				"Names"_o= _o["--vm-image"]
				, "Type"_o= ""
				, "Default"_o= ""
				, "Description"_o= "Name of the prepared guest image bundle to run the VM environment from."
			}
		;
		auto SettingsOption_VMBackend = "VMBackend?"_o=
			{
				"Names"_o= _o["--vm-backend"]
#ifdef DPlatformFamily_macOS
				, "Type"_o= COneOf{"MacOSVirtualization", ""}
				, "Default"_o= "MacOSVirtualization"
#else
				, "Type"_o= COneOf{""}
				, "Default"_o= ""
#endif
				, "Description"_o= "The virtualization backend to use."
			}
		;
		auto SettingsOption_VMCPUCount = "VMCPUCount?"_o=
			{
				"Names"_o= _o["--vm-cpu-count"]
				, "Type"_o= 0
				, "Default"_o= 0
				, "Description"_o= "Number of CPUs for the VM. Set to 0 to use the backend default."
			}
		;
		auto SettingsOption_VMMemoryMB = "VMMemoryMB?"_o=
			{
				"Names"_o= _o["--vm-memory"]
				, "Type"_o= 0
				, "Default"_o= 0
				, "Description"_o= "Memory in megabytes for the VM. Set to 0 to use the backend default."
			}
		;
		auto SettingsOption_VMDataDiskGB = "VMDataDiskGB?"_o=
			{
				"Names"_o= _o["--vm-data-disk-size"]
				, "Type"_o= 0
				, "Default"_o= 0
				, "Description"_o= "Logical size in gigabytes of the persistent data disk holding the application\n"
				"storage of the VM environment. The disk image is created sparse on the first\n"
				"start and keeps its size afterwards; set to 0 to use the default of 64."
			}
		;

		auto fStripDefault = [](auto &&_Template)
			{
				auto Return = _Template;
				Return.m_Value.f_RemoveMember("Default");
				return Return;
			}
		;

		EnvironmentManagement.f_RegisterCommand
			(
				{
					"Names"_o= _o["--environment-add"]
					, "Description"_o= "Adds a launch environment.\n"
					"Applications reference the environment with --launch-environment. Several applications can share one environment."
					, "Options"_o=
					{
						NameOption
						, "Type?"_o=
						{
							"Names"_o= _o["--type"]
							, "Type"_o= COneOf{"Local", "Container", "VM"}
							, "Default"_o= "Container"
							, "Description"_o= "The environment type."
						}
						, SettingsOption_AgentApplication
						, SettingsOption_ParentApplication
						, SettingsOption_AutoStart
						, SettingsOption_ContainerRuntime
						, SettingsOption_ContainerImage
						, SettingsOption_ContainerNetwork
						, SettingsOption_ContainerExtraMounts
						, SettingsOption_ContainerExtraArguments
						, SettingsOption_ContainerReadOnly
						, SettingsOption_OSDependencies
						, SettingsOption_MemoryLimitMB
						, SettingsOption_CPULimit
						, SettingsOption_VMImage
						, SettingsOption_VMBackend
						, SettingsOption_VMCPUCount
						, SettingsOption_VMMemoryMB
						, SettingsOption_VMDataDiskGB
					}
				}
				, [this](CEJsonSorted &&_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_AddEnvironment(fg_Move(_Params), fg_Move(_pCommandLine));
				}
			)
		;

		EnvironmentManagement.f_RegisterCommand
			(
				{
					"Names"_o= _o["--environment-change-settings"]
					, "Description"_o= "Changes settings for an environment."
					, "Options"_o=
					{
						NameOption
						, fStripDefault(SettingsOption_AgentApplication)
						, fStripDefault(SettingsOption_ParentApplication)
						, fStripDefault(SettingsOption_AutoStart)
						, fStripDefault(SettingsOption_ContainerRuntime)
						, fStripDefault(SettingsOption_ContainerImage)
						, fStripDefault(SettingsOption_ContainerNetwork)
						, fStripDefault(SettingsOption_ContainerExtraMounts)
						, fStripDefault(SettingsOption_ContainerExtraArguments)
						, fStripDefault(SettingsOption_ContainerReadOnly)
						, fStripDefault(SettingsOption_OSDependencies)
						, fStripDefault(SettingsOption_MemoryLimitMB)
						, fStripDefault(SettingsOption_CPULimit)
						, fStripDefault(SettingsOption_VMImage)
						, fStripDefault(SettingsOption_VMBackend)
						, fStripDefault(SettingsOption_VMCPUCount)
						, fStripDefault(SettingsOption_VMMemoryMB)
						, fStripDefault(SettingsOption_VMDataDiskGB)
					}
				}
				, [this](CEJsonSorted &&_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_ChangeEnvironmentSettings(fg_Move(_Params), fg_Move(_pCommandLine));
				}
			)
		;

		EnvironmentManagement.f_RegisterCommand
			(
				{
					"Names"_o= _o["--environment-remove"]
					, "Description"_o= "Removes an environment.\n"
					"The environment cannot be removed while applications reference it."
					, "Options"_o=
					{
						NameOption
						, "DeleteApplicationData?"_o=
						{
							"Names"_o= _o["--delete-application-data"]
							, "Default"_o= false
							, "Description"_o= "Delete the storage of the environment, with the application data of a container or VM environment and the\n"
							"files that removed applications kept there. Without it the storage is kept, and an environment added later\n"
							"with the same name, parent application and container runtime uses it."
						}
					}
				}
				, [this](CEJsonSorted &&_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_RemoveEnvironment(fg_Move(_Params), fg_Move(_pCommandLine));
				}
			)
		;

		EnvironmentManagement.f_RegisterCommand
			(
				{
					"Names"_o= _o["--environment-list"]
					, "Description"_o= "List environments."
					, "Options"_o=
					{
						"Name?"_o=
						{
							"Names"_o= _o["--name"]
							, "Type"_o= ""
							, "Default"_o= ""
							, "Description"_o= "Only list the environment with this name."
						}
						, "Verbose?"_o=
						{
							"Names"_o= _o["--verbose"]
							, "Default"_o= false
							, "Description"_o= "Show more details."
						}
						, CTableRenderHelper::fs_OutputTypeOption()
					}
				}
				, [this](CEJsonSorted &&_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_EnumEnvironments(fg_Move(_Params), fg_Move(_pCommandLine));
				}
			)
		;

		EnvironmentManagement.f_RegisterCommand
			(
				{
					"Names"_o= _o["--environment-start"]
					, "Description"_o= "Starts an environment."
					, "Options"_o=
					{
						NameOption
					}
				}
				, [this](CEJsonSorted &&_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_StartEnvironment(fg_Move(_Params), fg_Move(_pCommandLine));
				}
			)
		;

		EnvironmentManagement.f_RegisterCommand
			(
				{
					"Names"_o= _o["--environment-stop"]
					, "Description"_o= "Stops an environment.\n"
					"Applications running in the environment are stopped first."
					, "Options"_o=
					{
						NameOption
					}
				}
				, [this](CEJsonSorted &&_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_StopEnvironment(fg_Move(_Params), fg_Move(_pCommandLine));
				}
			)
		;

		EnvironmentManagement.f_RegisterCommand
			(
				{
					"Names"_o= _o["--environment-restart"]
					, "Description"_o= "Restarts an environment."
					, "Options"_o=
					{
						NameOption
					}
				}
				, [this](CEJsonSorted &&_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_RestartEnvironment(fg_Move(_Params), fg_Move(_pCommandLine));
				}
			)
		;

		EnvironmentManagement.f_RegisterCommand
			(
				{
					"Names"_o= _o["--environment-pull"]
					, "Description"_o= "Pulls the newest container image for the environment and recreates the container from it.\n"
					"This resets changes made inside the container filesystem, including OS updates made by an\n"
					"agent inside it; data in the mounted storage is unaffected. The environment is restarted\n"
					"when it was running."
					, "Options"_o=
					{
						NameOption
					}
				}
				, [this](CEJsonSorted &&_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_PullEnvironment(fg_Move(_Params), fg_Move(_pCommandLine));
				}
			)
		;

		EnvironmentManagement.f_RegisterCommand
			(
				{
					"Names"_o= _o["--environment-bash"]
					, "Description"_o= "Prints the command that opens an interactive bash in the environment's container.\n"
					"Run it directly with: `sudo ./AppManager --environment-bash --name Name`"
					, "Options"_o=
					{
						NameOption
						, "Shell?"_o=
						{
							"Names"_o= _o["--shell"]
							, "Type"_o= ""
							, "Default"_o= "/bin/bash"
							, "Description"_o= "The shell to run inside the environment."
						}
					}
				}
				, [this](CEJsonSorted &&_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_EnvironmentBash(fg_Move(_Params), fg_Move(_pCommandLine));
				}
			)
		;

#ifdef DPlatformFamily_macOS
		EnvironmentManagement.f_RegisterCommand
			(
				{
					"Names"_o= _o["--environment-window"]
					, "Description"_o= "Opens a window showing the display of a VM environment, for setting up and\n"
					"debugging the guest system interactively.\n"
					"The AppManager hands the virtual machine over to this command while the window is\n"
					"open: the environment is stopped first, the window hosts the virtual machine, and\n"
					"the environment is started again when the window closes.\n"
					"Closing the window asks the guest to shut down; closing it again forces the stop."
					, "Options"_o=
					{
						NameOption
					}
				}
				, [this](CEJsonSorted &&_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_EnvironmentWindow(fg_Move(_Params), fg_Move(_pCommandLine));
				}
				, EDistributedAppCommandFlag_None
				, &fg_AppManager_HandleEnvironmentWindowRequest
			)
		;

		EnvironmentManagement.f_RegisterCommand
			(
				{
					"Names"_o= _o["--vm-settings"]
					, "Description"_o= "Shows or changes the settings shared by all VM environments.\n"
					"Changing a value restarts the running VM environments."
					, "Options"_o=
					{
						"Window?"_o=
						{
							"Names"_o= _o["--window"]
							, "Type"_o= true
							, "Description"_o= "Host each VM environment with a window showing the guest display.\n"
							"The window opens in the graphical session of the AppManager; closing the window\n"
							"hides it while the machine keeps running, and clicking the host application in\n"
							"the Dock shows it again. Starts fall back to no window when the AppManager has\n"
							"no graphical session."
						}
					}
				}
				, [this](CEJsonSorted &&_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_VMSettings(fg_Move(_Params), fg_Move(_pCommandLine));
				}
			)
		;

		EnvironmentManagement.f_RegisterDirectCommand
			(
				{
					"Names"_o= _o["--vm-window-run"]
					, "Description"_o= "Hosts a virtual machine with a window from a window host configuration.\n"
					"Used internally by the AppManager for VM environments with the window setting."
					, "Options"_o=
					{
						"Config"_o=
						{
							"Names"_o= _o["--config"]
							, "Type"_o= ""
							, "Description"_o= "Path to the window host configuration file."
						}
					}
				}
				, [](CEJsonSorted &&_Params, CDistributedAppCommandLineClient &_CommandLineClient) -> uint32
				{
					return fg_AppManager_RunVMWindowHost(_Params["Config"].f_String());
				}
			)
		;

		EnvironmentManagement.f_RegisterCommand
			(
				{
					"Names"_o= _o["--colima-settings"]
					, "Description"_o= "Shows or changes the sizing of the colima virtual machine shared by all Colima environments.\n"
					"Changing a value restarts the virtual machine and the running Colima environments."
					, "Options"_o=
					{
						"CPUCount?"_o=
						{
							"Names"_o= _o["--cpu-count"]
							, "Type"_o= 0
							, "Default"_o= -1
							, "Description"_o= "Number of CPUs for the virtual machine. Set to 0 to use the colima default."
						}
						, "MemoryMB?"_o=
						{
							"Names"_o= _o["--memory"]
							, "Type"_o= 0
							, "Default"_o= -1
							, "Description"_o= "Memory for the virtual machine in megabytes. Set to 0 to use the colima default."
						}
						, "DiskGB?"_o=
						{
							"Names"_o= _o["--disk-size"]
							, "Type"_o= 0
							, "Default"_o= -1
							, "Description"_o= "Disk size for the virtual machine in gigabytes. The disk can only grow.\n"
							"Set to 0 to use the colima default."
						}
					}
				}
				, [this](CEJsonSorted &&_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_ColimaSettings(fg_Move(_Params), fg_Move(_pCommandLine));
				}
			)
		;
#endif

		EnvironmentManagement.f_RegisterCommand
			(
				{
					"Names"_o= _o["--vm-image-create"]
					, "Description"_o= "Creates a macOS guest VM image bundle and installs macOS into it from a restore image (IPSW).\n"
					"The image is created under .VMImages in the AppManager root and is referenced from VM environments with --vm-image.\n"
					"Only supported on Apple silicon macOS hosts."
					, "Options"_o=
					{
						NameOption
						, SettingsOption_ParentApplication
						, "RestoreImage"_o=
						{
							"Names"_o= _o["--restore-image"]
							, "Type"_o= ""
							, "Description"_o= "Path to the macOS IPSW restore image to install from."
						}
						, SettingsOption_VMCPUCount
						, SettingsOption_VMMemoryMB
						, "DiskSizeGB?"_o=
						{
							"Names"_o= _o["--disk-size"]
							, "Type"_o= 0
							, "Default"_o= 0
							, "Description"_o= "Size of the guest disk image in gigabytes. Set to 0 to use the default."
						}
						, "ProvisionUsername?"_o=
						{
							"Names"_o= _o["--provision-username"]
							, "Type"_o= ""
							, "Default"_o= "admin"
							, "Description"_o= "Automatically create this user account in the guest on its first boot instead\n"
							"of running the interactive setup assistant, and use it to set up the environment\n"
							"agent through SSH.\n"
							"The provisioning is stored in the image bundle and applied by the first start\n"
							"of an environment or environment window using the image.\n"
							"Set to an empty string to disable provisioning and set the guest up manually\n"
							"with --environment-window. An environment then starts only with an agent that\n"
							"is already installed in the guest.\n"
							"Requires a macOS 27 host and a macOS 27 restore image; earlier guests ignore\n"
							"the provisioning and boot into the setup assistant."
						}
						, "ProvisionPassword?"_o=
						{
							"Names"_o= _o["--provision-password"]
							, "Type"_o= ""
							, "Default"_o= ""
							, "Description"_o= "The password for the provisioned user account.\n"
							"A random password is generated when left empty.\n"
							"Stored in Provisioning.json inside the image bundle."
						}
						, "ProvisionFullName?"_o=
						{
							"Names"_o= _o["--provision-full-name"]
							, "Type"_o= ""
							, "Default"_o= ""
							, "Description"_o= "The full name for the provisioned user account. Defaults to the username."
						}
						, "ProvisionAutoLogin?"_o=
						{
							"Names"_o= _o["--provision-auto-login"]
							, "Type"_o= true
							, "Default"_o= true
							, "Description"_o= "Log the provisioned user in automatically when the guest starts."
						}
						, "ProvisionEnableSSH?"_o=
						{
							"Names"_o= _o["--provision-enable-ssh"]
							, "Type"_o= true
							, "Default"_o= true
							, "Description"_o= "Enable Remote Login (SSH) in the guest for the provisioned user."
						}
					}
				}
				, [this](CEJsonSorted &&_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_CreateVMImage(fg_Move(_Params), fg_Move(_pCommandLine));
				}
			)
		;
	}

	bool CAppManagerActor::CEnvironmentSettings::f_ParseSettings(CEJsonSorted const &_Params, EEnvironmentSetting &o_ChangedSettings, CStr &o_Error)
	{
		if (auto *pValue = _Params.f_GetMember("Type"))
		{
			auto Type = CAppManagerInterface::fs_EnvironmentTypeFromStr(pValue->f_String());
			if (!Type)
			{
				o_Error = fg_Format("'{}' is not a valid environment type. Valid types are: Local, Container, VM", pValue->f_String());
				return false;
			}

			o_ChangedSettings |= EEnvironmentSetting_Type;
			m_Type = *Type;
		}

		if (auto *pValue = _Params.f_GetMember("AgentApplication"))
		{
			o_ChangedSettings |= EEnvironmentSetting_AgentApplication;
			m_AgentApplication = pValue->f_String();
		}

		if (auto *pValue = _Params.f_GetMember("ParentApplication"))
		{
			o_ChangedSettings |= EEnvironmentSetting_ParentApplication;
			m_ParentApplication = pValue->f_String();
		}

		if (auto *pValue = _Params.f_GetMember("AutoStart"))
		{
			o_ChangedSettings |= EEnvironmentSetting_AutoStart;
			m_bAutoStart = pValue->f_Boolean();
		}

		if (auto *pValue = _Params.f_GetMember("ContainerRuntime"))
		{
			o_ChangedSettings |= EEnvironmentSetting_ContainerRuntime;
			m_ContainerRuntime = pValue->f_String();
		}

		if (auto *pValue = _Params.f_GetMember("ContainerImage"))
		{
			o_ChangedSettings |= EEnvironmentSetting_ContainerImage;
			m_ContainerImage = pValue->f_String();
		}

		if (auto *pValue = _Params.f_GetMember("ContainerNetwork"))
		{
			o_ChangedSettings |= EEnvironmentSetting_ContainerNetwork;
			m_ContainerNetwork = pValue->f_String();
		}

		if (auto *pValue = _Params.f_GetMember("ContainerExtraMounts"))
		{
			o_ChangedSettings |= EEnvironmentSetting_ContainerExtraMounts;
			m_ContainerExtraMounts.f_Clear();
			for (auto &Mount : pValue->f_Object())
				m_ContainerExtraMounts[Mount.f_Name()] = Mount.f_Value().f_String();
		}

		if (auto *pValue = _Params.f_GetMember("ContainerExtraArguments"))
		{
			o_ChangedSettings |= EEnvironmentSetting_ContainerExtraArguments;
			m_ContainerExtraArguments.f_Clear();
			for (auto &Argument : pValue->f_Array())
				m_ContainerExtraArguments.f_Insert(Argument.f_String());
		}

		if (auto *pValue = _Params.f_GetMember("ContainerReadOnly"))
		{
			o_ChangedSettings |= EEnvironmentSetting_ContainerReadOnly;
			m_bContainerReadOnly = pValue->f_Boolean();
		}

		if (auto *pValue = _Params.f_GetMember("OSDependencies"))
		{
			o_ChangedSettings |= EEnvironmentSetting_OSDependencies;
			m_OSDependencies.f_Clear();
			for (auto &Package : pValue->f_Array())
				m_OSDependencies.f_Insert(Package.f_String());
		}

		if (auto *pValue = _Params.f_GetMember("MemoryLimitMB"))
		{
			o_ChangedSettings |= EEnvironmentSetting_MemoryLimit;
			m_MemoryLimitMB = (uint64)pValue->f_AsInteger();
		}

		if (auto *pValue = _Params.f_GetMember("CPULimit"))
		{
			o_ChangedSettings |= EEnvironmentSetting_CPULimit;
			m_CPULimit = pValue->f_AsFloat();
		}

		if (auto *pValue = _Params.f_GetMember("VMImage"))
		{
			o_ChangedSettings |= EEnvironmentSetting_VMImage;
			m_VMImage = pValue->f_String();
		}

		if (auto *pValue = _Params.f_GetMember("VMBackend"))
		{
			o_ChangedSettings |= EEnvironmentSetting_VMBackend;
			m_VMBackend = pValue->f_String();
		}

		if (auto *pValue = _Params.f_GetMember("VMCPUCount"))
		{
			o_ChangedSettings |= EEnvironmentSetting_VMCPUCount;
			m_VMCPUCount = (uint32)pValue->f_AsInteger();
		}

		if (auto *pValue = _Params.f_GetMember("VMMemoryMB"))
		{
			o_ChangedSettings |= EEnvironmentSetting_VMMemoryMB;
			m_VMMemoryMB = (uint64)pValue->f_AsInteger();
		}

		if (auto *pValue = _Params.f_GetMember("VMDataDiskGB"))
		{
			o_ChangedSettings |= EEnvironmentSetting_VMDataDiskGB;
			m_VMDataDiskGB = (uint64)pValue->f_AsInteger();
		}

		return true;
	}

	void CAppManagerActor::CEnvironmentSettings::f_ApplySettings(EEnvironmentSetting _ChangedSettings, CEnvironmentSettings const &_Source)
	{
		if (_ChangedSettings & EEnvironmentSetting_Type)
			m_Type = _Source.m_Type;
		if (_ChangedSettings & EEnvironmentSetting_AgentApplication)
			m_AgentApplication = _Source.m_AgentApplication;
		if (_ChangedSettings & EEnvironmentSetting_ParentApplication)
			m_ParentApplication = _Source.m_ParentApplication;
		if (_ChangedSettings & EEnvironmentSetting_AutoStart)
			m_bAutoStart = _Source.m_bAutoStart;
		if (_ChangedSettings & EEnvironmentSetting_ContainerRuntime)
			m_ContainerRuntime = _Source.m_ContainerRuntime;
		if (_ChangedSettings & EEnvironmentSetting_ContainerImage)
			m_ContainerImage = _Source.m_ContainerImage;
		if (_ChangedSettings & EEnvironmentSetting_ContainerNetwork)
			m_ContainerNetwork = _Source.m_ContainerNetwork;
		if (_ChangedSettings & EEnvironmentSetting_ContainerExtraMounts)
			m_ContainerExtraMounts = _Source.m_ContainerExtraMounts;
		if (_ChangedSettings & EEnvironmentSetting_ContainerExtraArguments)
			m_ContainerExtraArguments = _Source.m_ContainerExtraArguments;
		if (_ChangedSettings & EEnvironmentSetting_ContainerReadOnly)
			m_bContainerReadOnly = _Source.m_bContainerReadOnly;
		if (_ChangedSettings & EEnvironmentSetting_OSDependencies)
			m_OSDependencies = _Source.m_OSDependencies;
		if (_ChangedSettings & EEnvironmentSetting_MemoryLimit)
			m_MemoryLimitMB = _Source.m_MemoryLimitMB;
		if (_ChangedSettings & EEnvironmentSetting_CPULimit)
			m_CPULimit = _Source.m_CPULimit;
		if (_ChangedSettings & EEnvironmentSetting_VMImage)
			m_VMImage = _Source.m_VMImage;
		if (_ChangedSettings & EEnvironmentSetting_VMBackend)
			m_VMBackend = _Source.m_VMBackend;
		if (_ChangedSettings & EEnvironmentSetting_VMCPUCount)
			m_VMCPUCount = _Source.m_VMCPUCount;
		if (_ChangedSettings & EEnvironmentSetting_VMMemoryMB)
			m_VMMemoryMB = _Source.m_VMMemoryMB;
		if (_ChangedSettings & EEnvironmentSetting_VMDataDiskGB)
			m_VMDataDiskGB = _Source.m_VMDataDiskGB;
	}

	void CAppManagerActor::CEnvironmentSettings::f_FromInterfaceSettings(CAppManagerInterface::CEnvironmentSettings const &_Settings, EEnvironmentSetting &o_ChangedSettings)
	{
		if (_Settings.m_Type)
		{
			m_Type = *_Settings.m_Type;
			o_ChangedSettings |= EEnvironmentSetting_Type;
		}
		if (_Settings.m_AgentApplication)
		{
			m_AgentApplication = *_Settings.m_AgentApplication;
			o_ChangedSettings |= EEnvironmentSetting_AgentApplication;
		}
		if (_Settings.m_ParentApplication)
		{
			m_ParentApplication = *_Settings.m_ParentApplication;
			o_ChangedSettings |= EEnvironmentSetting_ParentApplication;
		}
		if (_Settings.m_AutoStart)
		{
			m_bAutoStart = *_Settings.m_AutoStart;
			o_ChangedSettings |= EEnvironmentSetting_AutoStart;
		}
		if (_Settings.m_ContainerRuntime)
		{
			m_ContainerRuntime = *_Settings.m_ContainerRuntime;
			o_ChangedSettings |= EEnvironmentSetting_ContainerRuntime;
		}
		if (_Settings.m_ContainerImage)
		{
			m_ContainerImage = *_Settings.m_ContainerImage;
			o_ChangedSettings |= EEnvironmentSetting_ContainerImage;
		}
		if (_Settings.m_ContainerNetwork)
		{
			m_ContainerNetwork = *_Settings.m_ContainerNetwork;
			o_ChangedSettings |= EEnvironmentSetting_ContainerNetwork;
		}
		if (_Settings.m_ContainerExtraMounts)
		{
			m_ContainerExtraMounts = *_Settings.m_ContainerExtraMounts;
			o_ChangedSettings |= EEnvironmentSetting_ContainerExtraMounts;
		}
		if (_Settings.m_ContainerExtraArguments)
		{
			m_ContainerExtraArguments = *_Settings.m_ContainerExtraArguments;
			o_ChangedSettings |= EEnvironmentSetting_ContainerExtraArguments;
		}
		if (_Settings.m_ContainerReadOnly)
		{
			m_bContainerReadOnly = *_Settings.m_ContainerReadOnly;
			o_ChangedSettings |= EEnvironmentSetting_ContainerReadOnly;
		}
		if (_Settings.m_OSDependencies)
		{
			m_OSDependencies = *_Settings.m_OSDependencies;
			o_ChangedSettings |= EEnvironmentSetting_OSDependencies;
		}
		if (_Settings.m_MemoryLimitMB)
		{
			m_MemoryLimitMB = *_Settings.m_MemoryLimitMB;
			o_ChangedSettings |= EEnvironmentSetting_MemoryLimit;
		}
		if (_Settings.m_CPULimit)
		{
			m_CPULimit = *_Settings.m_CPULimit;
			o_ChangedSettings |= EEnvironmentSetting_CPULimit;
		}
		if (_Settings.m_VMImage)
		{
			m_VMImage = *_Settings.m_VMImage;
			o_ChangedSettings |= EEnvironmentSetting_VMImage;
		}
		if (_Settings.m_VMBackend)
		{
			m_VMBackend = *_Settings.m_VMBackend;
			o_ChangedSettings |= EEnvironmentSetting_VMBackend;
		}
		if (_Settings.m_VMCPUCount)
		{
			m_VMCPUCount = *_Settings.m_VMCPUCount;
			o_ChangedSettings |= EEnvironmentSetting_VMCPUCount;
		}
		if (_Settings.m_VMMemoryMB)
		{
			m_VMMemoryMB = *_Settings.m_VMMemoryMB;
			o_ChangedSettings |= EEnvironmentSetting_VMMemoryMB;
		}
		if (_Settings.m_VMDataDiskGB)
		{
			m_VMDataDiskGB = *_Settings.m_VMDataDiskGB;
			o_ChangedSettings |= EEnvironmentSetting_VMDataDiskGB;
		}
	}

	auto CAppManagerActor::CEnvironmentSettings::f_ChangedSettings(CEnvironmentSettings const &_Other) const -> EEnvironmentSetting
	{
		EEnvironmentSetting ChangedSettings = EEnvironmentSetting_None;
		if (m_Type != _Other.m_Type)
			ChangedSettings |= EEnvironmentSetting_Type;
		if (m_AgentApplication != _Other.m_AgentApplication)
			ChangedSettings |= EEnvironmentSetting_AgentApplication;
		if (m_ParentApplication != _Other.m_ParentApplication)
			ChangedSettings |= EEnvironmentSetting_ParentApplication;
		if (m_bAutoStart != _Other.m_bAutoStart)
			ChangedSettings |= EEnvironmentSetting_AutoStart;
		// An empty runtime is the default runtime of the platform, which naming it doesn't change
		auto fEffectiveRuntime = [](CStr const &_Runtime) -> CStr
			{
				if (!_Runtime.f_IsEmpty())
					return _Runtime;
#ifdef DPlatformFamily_macOS
				return "AppleContainer";
#else
				return "Docker";
#endif
			}
		;
		if (fEffectiveRuntime(m_ContainerRuntime) != fEffectiveRuntime(_Other.m_ContainerRuntime))
			ChangedSettings |= EEnvironmentSetting_ContainerRuntime;
		if (m_ContainerImage != _Other.m_ContainerImage)
			ChangedSettings |= EEnvironmentSetting_ContainerImage;
		if (m_ContainerNetwork != _Other.m_ContainerNetwork)
			ChangedSettings |= EEnvironmentSetting_ContainerNetwork;
		if (m_ContainerExtraMounts != _Other.m_ContainerExtraMounts)
			ChangedSettings |= EEnvironmentSetting_ContainerExtraMounts;
		if (m_ContainerExtraArguments != _Other.m_ContainerExtraArguments)
			ChangedSettings |= EEnvironmentSetting_ContainerExtraArguments;
		if (m_bContainerReadOnly != _Other.m_bContainerReadOnly)
			ChangedSettings |= EEnvironmentSetting_ContainerReadOnly;
		if (m_OSDependencies != _Other.m_OSDependencies)
			ChangedSettings |= EEnvironmentSetting_OSDependencies;
		if (m_MemoryLimitMB != _Other.m_MemoryLimitMB)
			ChangedSettings |= EEnvironmentSetting_MemoryLimit;
		if (m_CPULimit != _Other.m_CPULimit)
			ChangedSettings |= EEnvironmentSetting_CPULimit;
		if (m_VMImage != _Other.m_VMImage)
			ChangedSettings |= EEnvironmentSetting_VMImage;
		if (m_VMBackend != _Other.m_VMBackend)
			ChangedSettings |= EEnvironmentSetting_VMBackend;
		if (m_VMCPUCount != _Other.m_VMCPUCount)
			ChangedSettings |= EEnvironmentSetting_VMCPUCount;
		if (m_VMMemoryMB != _Other.m_VMMemoryMB)
			ChangedSettings |= EEnvironmentSetting_VMMemoryMB;
		if (m_VMDataDiskGB != _Other.m_VMDataDiskGB)
			ChangedSettings |= EEnvironmentSetting_VMDataDiskGB;

		return ChangedSettings;
	}

	bool CAppManagerActor::CEnvironmentSettings::f_Validate(CStr &o_Error) const
	{
		auto fError = [&](CStr const &_Error)
			{
				o_Error = _Error;
				return false;
			}
		;

		// A streamed enum can hold values outside the environment types
		if (m_Type > CAppManagerInterface::EEnvironmentType_Container)
			return fError("Unknown environment type");

		// The runtime selects the executable the AppManager runs for container commands, so only known runtimes are accepted. On macOS a docker
		// daemon of the user runs its containers in a virtual machine that the agent socket of the host doesn't reach
		{
#ifdef DPlatformFamily_macOS
			bool bKnownRuntime = m_ContainerRuntime.f_IsEmpty() || m_ContainerRuntime == "AppleContainer" || m_ContainerRuntime == "Colima";
#else
			bool bKnownRuntime = m_ContainerRuntime.f_IsEmpty() || m_ContainerRuntime == "Docker";
#endif
			if (!bKnownRuntime)
				return fError(fg_Format("'{}' is not a supported container runtime on this host", m_ContainerRuntime));
		}

		if (m_Type == CAppManagerInterface::EEnvironmentType_Container)
		{
			if (m_ContainerImage.f_IsEmpty())
				return fError("For container environments you must specify a container image");
			else if (!m_VMImage.f_IsEmpty())
				return fError("For container environments you cannot specify a VM image");

			bool bAppleContainer = m_ContainerRuntime == "AppleContainer";
#ifdef DPlatformFamily_macOS
			bAppleContainer = bAppleContainer || m_ContainerRuntime.f_IsEmpty();
#endif
			if (bAppleContainer && m_CPULimit.f_Fraction() != 0.0)
				return fError("The Apple container runtime only allocates a whole number of CPUs");

			// The Apple container runtime keeps its volumes outside the directory of the parent application
			if (bAppleContainer && !m_ParentApplication.f_IsEmpty())
				return fError("Environments with a parent application need the Colima container runtime on this host");
		}
		else if (m_Type == CAppManagerInterface::EEnvironmentType_VM)
		{
			if (m_VMImage.f_IsEmpty())
				return fError("For VM environments you must specify a VM image");
			else if (!CVersionManager::fs_IsValidApplicationName(m_VMImage))
				return fError(fg_Format("'{}' is not a valid VM image name", m_VMImage));
			else if (!m_ContainerImage.f_IsEmpty())
				return fError("For VM environments you cannot specify a container image");
		}
		else if (m_Type == CAppManagerInterface::EEnvironmentType_Local)
		{
			if (!m_ContainerImage.f_IsEmpty())
				return fError("For local environments you cannot specify a container image");
			else if (!m_VMImage.f_IsEmpty())
				return fError("For local environments you cannot specify a VM image");
		}

		return true;
	}

	// Adds the permissions that _ChangedSettings of these settings need beyond access to the environment of type _Type. Extra mounts, extra runtime
	// arguments, networks other than the runtime's defaults and the OS dependencies of a Local environment, which install on the host, reach past
	// the environment boundary, so they need access to the host itself. The agent application provides the files the environment runs, and the parent
	// application holds the environment storage
	void CAppManagerActor::CEnvironmentSettings::f_AddPermissionQueries
		(
			NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> &o_Permissions
			, EEnvironmentSetting _ChangedSettings
			, EEnvironmentType _Type
		)
		const
	{
		// The default networks of the runtimes, bridge and default, are shared by the containers of all environments and reach the services of
		// the host as the runtime allows, which the threat model accepts. Other networks can be the network of the host, of another container or
		// one that an administrator set up
		bool bOtherNetwork = !m_ContainerNetwork.f_IsEmpty() && m_ContainerNetwork != "bridge" && m_ContainerNetwork != "none" && m_ContainerNetwork != "default";

		bool bHostAccess
			= ((_ChangedSettings & EEnvironmentSetting_ContainerExtraMounts) && !m_ContainerExtraMounts.f_IsEmpty())
			|| ((_ChangedSettings & EEnvironmentSetting_ContainerExtraArguments) && !m_ContainerExtraArguments.f_IsEmpty())
			|| ((_ChangedSettings & EEnvironmentSetting_ContainerNetwork) && bOtherNetwork)
			|| ((_ChangedSettings & EEnvironmentSetting_OSDependencies) && !m_OSDependencies.f_IsEmpty() && _Type == CAppManagerInterface::EEnvironmentType_Local)
		;

		if (bHostAccess)
			o_Permissions["HostAccess"] = {CPermissionQuery{"AppManager/EnvironmentHostAccess"}.f_Description("Give environments access to the host in AppManager")};

		auto fAddApplication = [&](CStr const &_Name, EEnvironmentSetting _Setting, CStr const &_Application)
			{
				if (!(_ChangedSettings & _Setting) || _Application.f_IsEmpty())
					return;

				o_Permissions[_Name] = {CPermissionQuery{"AppManager/AppAll", "AppManager/App/{}"_f << _Application}.f_Description("Access application {} in AppManager"_f << _Application)};
			}
		;

		fAddApplication("AgentApplication", EEnvironmentSetting_AgentApplication, m_AgentApplication);
		fAddApplication("ParentApplication", EEnvironmentSetting_ParentApplication, m_ParentApplication);
	}

	void CAppManagerActor::fp_ReadEnvironmentsState()
	{
		if (auto pColima = mp_State.m_StateDatabase.m_Data.f_GetMember("ColimaSettings"))
		{
			if (auto pValue = pColima->f_GetMember("CPUCount", EJsonType_Integer))
				mp_ColimaCPUCount = (uint32)pValue->f_Integer();
			if (auto pValue = pColima->f_GetMember("MemoryMB", EJsonType_Integer))
				mp_ColimaMemoryMB = (uint64)pValue->f_Integer();
			if (auto pValue = pColima->f_GetMember("DiskGB", EJsonType_Integer))
				mp_ColimaDiskGB = (uint64)pValue->f_Integer();
		}

		if (auto pVMSettings = mp_State.m_StateDatabase.m_Data.f_GetMember("VMSettings"))
		{
			if (auto pValue = pVMSettings->f_GetMember("Window", EJsonType_Boolean))
				mp_bVMWindow = pValue->f_Boolean();
		}

		auto pEnvironments = mp_State.m_StateDatabase.m_Data.f_GetMember("Environments");
		if (!pEnvironments)
			return;

		for (auto &EnvironmentEntry : pEnvironments->f_Object())
		{
			CStr const &Name = EnvironmentEntry.f_Name();
			auto &EnvironmentJson = EnvironmentEntry.f_Value();

			auto &Environment = *(mp_Environments[Name] = fg_Construct(Name, this));

			auto &Settings = Environment.m_Settings;

			if (auto pValue = EnvironmentJson.f_GetMember("Type", EJsonType_String))
			{
				if (auto Type = CAppManagerInterface::fs_EnvironmentTypeFromStr(pValue->f_String()))
					Settings.m_Type = *Type;
			}

			if (auto pValue = EnvironmentJson.f_GetMember("AgentApplication", EJsonType_String))
				Settings.m_AgentApplication = pValue->f_String();
			if (auto pValue = EnvironmentJson.f_GetMember("ParentApplication", EJsonType_String))
				Settings.m_ParentApplication = pValue->f_String();
			if (auto pValue = EnvironmentJson.f_GetMember("AutoStart", EJsonType_Boolean))
				Settings.m_bAutoStart = pValue->f_Boolean();
			if (auto pValue = EnvironmentJson.f_GetMember("StoppedManually", EJsonType_Boolean))
				Environment.m_bStoppedManually = pValue->f_Boolean();

			if (auto pValue = EnvironmentJson.f_GetMember("ContainerRuntime", EJsonType_String))
				Settings.m_ContainerRuntime = pValue->f_String();
			if (auto pValue = EnvironmentJson.f_GetMember("ContainerImage", EJsonType_String))
				Settings.m_ContainerImage = pValue->f_String();
			if (auto pValue = EnvironmentJson.f_GetMember("ContainerNetwork", EJsonType_String))
				Settings.m_ContainerNetwork = pValue->f_String();

			if (auto pValue = EnvironmentJson.f_GetMember("ContainerExtraMounts", EJsonType_Object))
			{
				for (auto &Mount : pValue->f_Object())
					Settings.m_ContainerExtraMounts[Mount.f_Name()] = Mount.f_Value().f_String();
			}

			if (auto pValue = EnvironmentJson.f_GetMember("ContainerExtraArguments", EJsonType_Array))
			{
				for (auto &Argument : pValue->f_Array())
					Settings.m_ContainerExtraArguments.f_Insert(Argument.f_String());
			}

			if (auto pValue = EnvironmentJson.f_GetMember("ContainerReadOnly", EJsonType_Boolean))
				Settings.m_bContainerReadOnly = pValue->f_Boolean();

			if (auto pValue = EnvironmentJson.f_GetMember("OSDependencies", EJsonType_Array))
			{
				for (auto &Package : pValue->f_Array())
					Settings.m_OSDependencies.f_Insert(Package.f_String());
			}

			if (auto pValue = EnvironmentJson.f_GetMember("MemoryLimitMB", EJsonType_Integer))
				Settings.m_MemoryLimitMB = (uint64)pValue->f_Integer();
			{
				auto pValue = EnvironmentJson.f_GetMember("CPULimit", EJsonType_Float);
				if (!pValue)
					pValue = EnvironmentJson.f_GetMember("CPULimit", EJsonType_Integer);
				if (pValue)
					Settings.m_CPULimit = pValue->f_AsFloat();
			}

			if (auto pValue = EnvironmentJson.f_GetMember("VMImage", EJsonType_String))
				Settings.m_VMImage = pValue->f_String();
			if (auto pValue = EnvironmentJson.f_GetMember("VMBackend", EJsonType_String))
				Settings.m_VMBackend = pValue->f_String();
			if (auto pValue = EnvironmentJson.f_GetMember("VMCPUCount", EJsonType_Integer))
				Settings.m_VMCPUCount = (uint32)pValue->f_Integer();
			if (auto pValue = EnvironmentJson.f_GetMember("VMMemoryMB", EJsonType_Integer))
				Settings.m_VMMemoryMB = (uint64)pValue->f_Integer();
			if (auto pValue = EnvironmentJson.f_GetMember("VMDataDiskGB", EJsonType_Integer))
				Settings.m_VMDataDiskGB = (uint64)pValue->f_Integer();

			if (auto pValue = EnvironmentJson.f_GetMember("ContainerFingerprint", EJsonType_String))
				Environment.m_ContainerFingerprint = pValue->f_String();
			if (auto pValue = EnvironmentJson.f_GetMember("ContainerLaunchID", EJsonType_String))
				Environment.m_ContainerLaunchID = pValue->f_String();
			if (auto pValue = EnvironmentJson.f_GetMember("ContainerRequestTicketMagic", EJsonType_String))
				Environment.m_ContainerRequestTicketMagic = pValue->f_String();
			if (auto pValue = EnvironmentJson.f_GetMember("ContainerMounts", EJsonType_Array))
			{
				for (auto &Mount : pValue->f_Array())
					Environment.m_ContainerMounts.f_Insert(fg_TempCopy(Mount.f_String()));
			}
			if (auto pValue = EnvironmentJson.f_GetMember("ListenPort", EJsonType_Integer))
				Environment.m_ListenPort = (uint32)pValue->f_Integer();
			if (auto pValue = EnvironmentJson.f_GetMember("VMMACAddress", EJsonType_String))
				Environment.m_VMMACAddress = pValue->f_String();
			if (auto pValue = EnvironmentJson.f_GetMember("VMAgentLaunchID", EJsonType_String))
				Environment.m_VMAgentLaunchID = pValue->f_String();
			if (auto pValue = EnvironmentJson.f_GetMember("VMAgentFingerprint", EJsonType_String))
				Environment.m_VMAgentFingerprint = pValue->f_String();

			// Restore the container's creation-time launch ID so an agent that survived the restart can reconnect
			Environment.m_LaunchID = Environment.m_ContainerLaunchID;

			// A VM agent reconnects with the launch ID from its guest installation
			if (Environment.m_LaunchID.f_IsEmpty())
				Environment.m_LaunchID = Environment.m_VMAgentLaunchID;
		}
	}

	TCFuture<void> CAppManagerActor::fp_UpdateEnvironmentJson(TCSharedPointer<CEnvironment> _pEnvironment)
	{
		auto &Environment = *_pEnvironment;
		if (Environment.m_bDeleted)
			co_return DMibErrorInstance("Environment has been deleted");

		auto &Settings = Environment.m_Settings;

		auto &EnvironmentJson = mp_State.m_StateDatabase.m_Data["Environments"][Environment.m_Name];
		EnvironmentJson["Type"] = CAppManagerInterface::fs_EnvironmentTypeToStr(Settings.m_Type);
		EnvironmentJson["AgentApplication"] = Settings.m_AgentApplication;
		EnvironmentJson["ParentApplication"] = Settings.m_ParentApplication;
		EnvironmentJson["AutoStart"] = Settings.m_bAutoStart;
		EnvironmentJson["StoppedManually"] = Environment.m_bStoppedManually;

		EnvironmentJson["ContainerRuntime"] = Settings.m_ContainerRuntime;
		EnvironmentJson["ContainerImage"] = Settings.m_ContainerImage;
		EnvironmentJson["ContainerNetwork"] = Settings.m_ContainerNetwork;
		{
			auto &Mounts = EnvironmentJson["ContainerExtraMounts"] = CEJsonSortedYaml();
			Mounts.f_Object();
			for (auto &Mount : Settings.m_ContainerExtraMounts)
				Mounts[Settings.m_ContainerExtraMounts.fs_GetKey(Mount)] = Mount;
		}
		{
			auto &Arguments = EnvironmentJson["ContainerExtraArguments"].f_Array();
			Arguments.f_Clear();
			for (auto &Argument : Settings.m_ContainerExtraArguments)
				Arguments.f_Insert(Argument);
		}

		EnvironmentJson["ContainerReadOnly"] = Settings.m_bContainerReadOnly;
		{
			auto &DependenciesJson = EnvironmentJson["OSDependencies"].f_Array();
			DependenciesJson.f_Clear();
			for (auto &Package : Settings.m_OSDependencies)
				DependenciesJson.f_Insert(Package);
		}
		EnvironmentJson["MemoryLimitMB"] = Settings.m_MemoryLimitMB;
		EnvironmentJson["CPULimit"] = Settings.m_CPULimit;

		EnvironmentJson["VMImage"] = Settings.m_VMImage;
		EnvironmentJson["VMBackend"] = Settings.m_VMBackend;
		EnvironmentJson["VMCPUCount"] = Settings.m_VMCPUCount;
		EnvironmentJson["VMMemoryMB"] = Settings.m_VMMemoryMB;
		EnvironmentJson["VMDataDiskGB"] = Settings.m_VMDataDiskGB;

		EnvironmentJson["ContainerFingerprint"] = Environment.m_ContainerFingerprint;
		EnvironmentJson["ContainerLaunchID"] = Environment.m_ContainerLaunchID;
		EnvironmentJson["ContainerRequestTicketMagic"] = Environment.m_ContainerRequestTicketMagic;
		{
			auto &MountsJson = EnvironmentJson["ContainerMounts"].f_Array();
			MountsJson.f_Clear();
			for (auto &Mount : Environment.m_ContainerMounts)
				MountsJson.f_Insert(Mount);
		}
		EnvironmentJson["ListenPort"] = Environment.m_ListenPort;
		EnvironmentJson["VMMACAddress"] = Environment.m_VMMACAddress;
		EnvironmentJson["VMAgentLaunchID"] = Environment.m_VMAgentLaunchID;
		EnvironmentJson["VMAgentFingerprint"] = Environment.m_VMAgentFingerprint;

		co_return co_await mp_State.m_StateDatabase.f_Save();
	}

	TCFuture<void> CAppManagerActor::fp_SetEnvironmentStoppedManually(TCSharedPointer<CEnvironment> _pEnvironment, bool _bStoppedManually)
	{
		if (_pEnvironment->m_bStoppedManually == _bStoppedManually)
			co_return {};

		_pEnvironment->m_bStoppedManually = _bStoppedManually;

		co_return co_await fp_UpdateEnvironmentJson(_pEnvironment);
	}

	CAppManagerInterface::CEnvironmentInfo CAppManagerActor::fp_GetEnvironmentInfo(CEnvironment const &_Environment)
	{
		auto &Settings = _Environment.m_Settings;

		CAppManagerInterface::CEnvironmentInfo OutEnvironment;

		OutEnvironment.m_Status = _Environment.m_Status;
		OutEnvironment.m_StatusSeverity = _Environment.m_StatusSeverity;
		OutEnvironment.m_HostID = _Environment.m_AgentHostID;

		// The environment runs after a failed configuration, like an installation of its OS dependencies, but its launches and update scripts
		// send the configuration again and fail until it succeeds
		if
		(
			_Environment.m_AgentInterface
			&& !_Environment.m_AgentConfigurationError.f_IsEmpty()
			&& !_Environment.m_nPendingAgentConfigurations
			&& OutEnvironment.m_StatusSeverity == CAppManagerInterface::EStatusSeverity_None
		)
		{
			OutEnvironment.m_Status = "{}, configuration failed: {}"_f << _Environment.m_Status << _Environment.m_AgentConfigurationError;
			OutEnvironment.m_StatusSeverity = CAppManagerInterface::EStatusSeverity_Warning;
		}

		OutEnvironment.m_Type = Settings.m_Type;
		OutEnvironment.m_AgentApplication = Settings.m_AgentApplication;
		OutEnvironment.m_ParentApplication = Settings.m_ParentApplication;
		OutEnvironment.m_bAutoStart = Settings.m_bAutoStart;

		OutEnvironment.m_ContainerRuntime = Settings.m_ContainerRuntime;
		OutEnvironment.m_ContainerImage = Settings.m_ContainerImage;
		OutEnvironment.m_ContainerNetwork = Settings.m_ContainerNetwork;
		OutEnvironment.m_ContainerExtraMounts = Settings.m_ContainerExtraMounts;
		OutEnvironment.m_ContainerExtraArguments = Settings.m_ContainerExtraArguments;
		OutEnvironment.m_bContainerReadOnly = Settings.m_bContainerReadOnly;

		OutEnvironment.m_MemoryLimitMB = Settings.m_MemoryLimitMB;
		OutEnvironment.m_CPULimit = Settings.m_CPULimit;

		OutEnvironment.m_VMImage = Settings.m_VMImage;
		OutEnvironment.m_VMBackend = Settings.m_VMBackend;
		OutEnvironment.m_VMCPUCount = Settings.m_VMCPUCount;
		OutEnvironment.m_VMMemoryMB = Settings.m_VMMemoryMB;
		OutEnvironment.m_VMDataDiskGB = Settings.m_VMDataDiskGB;

		OutEnvironment.m_OSDependencies = Settings.m_OSDependencies;

		for (auto &pApplication : mp_Applications)
		{
			if (pApplication->m_Settings.m_LaunchEnvironment == _Environment.m_Name)
				OutEnvironment.m_Applications[pApplication->m_Name];
		}

		return OutEnvironment;
	}

	TCFuture<void> CAppManagerActor::fp_RegisterEnvironmentPermissions(TCSharedPointer<CEnvironment> _pEnvironment)
	{
		auto Permissions = fg_CreateSet<CStr>(fg_Format("AppManager/Environment/{}", _pEnvironment->m_Name));
		co_return co_await mp_State.m_TrustManager(&CDistributedActorTrustManager::f_RegisterPermissions, fg_Move(Permissions));
	}

	TCFuture<void> CAppManagerActor::fp_UnregisterEnvironmentPermissions(TCSharedPointer<CEnvironment> _pEnvironment)
	{
		auto Permissions = fg_CreateSet<CStr>(fg_Format("AppManager/Environment/{}", _pEnvironment->m_Name));
		co_return co_await mp_State.m_TrustManager(&CDistributedActorTrustManager::f_UnregisterPermissions, fg_Move(Permissions));
	}

	auto CAppManagerActor::fp_AddEnvironment
		(
			CStr _Name
			, CEnvironmentSettings _Settings
			, EEnvironmentSetting _ChangedSettings
			, TCFunction<void (CStr const &_Info)> _fOnInfo
			, CCallingHostInfo _CallingHostInfo
		)
		-> TCFuture<void>
	{
		auto Auditor = f_Auditor({}, _CallingHostInfo);

		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;

		Permissions["Command"] = {{"AppManager/CommandAll", "AppManager/Command/EnvironmentAdd"}};
		Permissions["Environment"] =
			{
				CPermissionQuery{"AppManager/EnvironmentAll", fg_Format("AppManager/Environment/{}", _Name)}.f_Description("Access environment {} in AppManager"_f << _Name)
			}
		;
		_Settings.f_AddPermissionQueries(Permissions, _ChangedSettings, _Settings.m_Type);

		NContainer::TCMap<NStr::CStr, bool> HasPermissions = co_await
			(
				mp_Permissions.f_HasPermissions("Add environment in AppManager", Permissions, _CallingHostInfo) % "Permission denied adding environment" % Auditor
			)
		;

		if (!HasPermissions["Command"])
			co_return Auditor.f_AccessDenied("(Environment add, command)", Permissions["Command"]);

		if (!HasPermissions["Environment"])
			co_return Auditor.f_AccessDenied("(Environment add, environment name)", Permissions["Environment"]);

		for (auto *pName : {"HostAccess", "AgentApplication", "ParentApplication"})
		{
			if (auto *pHasPermission = HasPermissions.f_FindEqual(pName); pHasPermission && !*pHasPermission)
				co_return Auditor.f_AccessDenied("(Environment add, {})"_f << pName, Permissions[pName]);
		}

		if (!CVersionManager::fs_IsValidApplicationName(_Name))
			co_return Auditor.f_Exception(fg_Format("'{}' is not a valid environment name", _Name));

		if (mp_Environments.f_FindEqual(_Name))
			co_return Auditor.f_Exception(fg_Format("Environment '{}' already exists", _Name));

		// The name is a directory of the environment storage, which can be on a case-insensitive file system
		for (auto &pEnvironment : mp_Environments)
		{
			if (pEnvironment->m_Name.f_CmpNoCase(_Name) == 0)
				co_return Auditor.f_Exception(fg_Format("Environment '{}' differs from environment '{}' only in case", _Name, pEnvironment->m_Name));
		}

		CEnvironmentSettings NewSettings;
		NewSettings.f_ApplySettings(_ChangedSettings, _Settings);

		if (NewSettings.m_AgentApplication.f_IsEmpty() && NewSettings.m_Type != CAppManagerInterface::EEnvironmentType_Local)
			NewSettings.m_AgentApplication = "SelfUpdate.{}"_f << fsp_GetEnvironmentGuestPlatform(NewSettings.m_Type);

		{
			CStr Error;
			if (!NewSettings.f_Validate(Error))
				co_return Auditor.f_Exception(Error);
		}

		if (!NewSettings.m_ParentApplication.f_IsEmpty() && !mp_Applications.f_FindEqual(NewSettings.m_ParentApplication))
			co_return Auditor.f_Exception(fg_Format("Parent application '{}' does not exist", NewSettings.m_ParentApplication));

		if (CStr Error = fp_CheckLocalEnvironmentAccounts(NewSettings); !Error.f_IsEmpty())
			co_return Auditor.f_Exception(Error);

		// The environment storage is created in the directory of the parent application on the host
		if (auto *pParent = mp_Applications.f_FindEqual(NewSettings.m_ParentApplication); pParent && fp_ApplicationRemoteStorageEnvironment(**pParent))
			co_return Auditor.f_Exception(fg_Format("Parent application '{}' keeps its files in environment-managed storage", NewSettings.m_ParentApplication));

		if (CStr Sharing = fp_FindEnvironmentSharingVMImage(_Name, NewSettings); !Sharing.f_IsEmpty())
			co_return Auditor.f_Exception(fg_Format("VM image '{}' is used by environment '{}'", NewSettings.m_VMImage, Sharing));

		auto pEnvironment = mp_Environments[_Name] = fg_Construct(_Name, this);
		pEnvironment->m_Settings = NewSettings;

		co_await (fp_RegisterEnvironmentPermissions(pEnvironment) % "Failed to register environment permissions" % Auditor);

		_fOnInfo("Saving environment state");
		co_await (fp_UpdateEnvironmentJson(pEnvironment) % "Failed to save environment state" % Auditor);

		_fOnInfo(fg_Format("Environment '{}' was successfully added", _Name));
		Auditor.f_Info(fg_Format("Added environment '{}'", _Name));

		co_return {};
	}

	// The environment storage, with the data of environment-managed application storage, is kept unless _bDeleteApplicationData is set, like the
	// application files that removing an application keeps, and an environment added again with the same name uses it
	auto CAppManagerActor::fp_RemoveEnvironment(CStr _Name, bool _bDeleteApplicationData, TCFunction<void (CStr const &_Info)> _fOnInfo, CCallingHostInfo _CallingHostInfo) -> TCFuture<void>
	{
		auto Auditor = f_Auditor({}, _CallingHostInfo);

		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;

		Permissions["Command"] = {{"AppManager/CommandAll", "AppManager/Command/EnvironmentRemove"}};
		Permissions["Environment"] =
			{
				CPermissionQuery{"AppManager/EnvironmentAll", fg_Format("AppManager/Environment/{}", _Name)}.f_Description("Access environment {} in AppManager"_f << _Name)
			}
		;

		NContainer::TCMap<NStr::CStr, bool> HasPermissions = co_await
			(
				mp_Permissions.f_HasPermissions("Remove environment from AppManager", Permissions, _CallingHostInfo) % "Permission denied removing environment" % Auditor
			)
		;

		if (!HasPermissions["Command"])
			co_return Auditor.f_AccessDenied("(Environment remove, command)", Permissions["Command"]);

		if (!HasPermissions["Environment"])
			co_return Auditor.f_AccessDenied("(Environment remove, environment name)", Permissions["Environment"]);

		auto *pFindEnvironment = mp_Environments.f_FindEqual(_Name);
		if (!pFindEnvironment)
			co_return Auditor.f_Exception(fg_Format("No such environment '{}'", _Name));

		for (auto &pApplication : mp_Applications)
		{
			if (pApplication->m_Settings.m_LaunchEnvironment == _Name)
				co_return Auditor.f_Exception(fg_Format("Environment '{}' is used by application '{}'. Change the application settings first.", _Name, pApplication->m_Name));
		}

		auto pEnvironment = *pFindEnvironment;

		if (pEnvironment->m_bRemoving)
			co_return Auditor.f_Exception(fg_Format("Environment '{}' is already being removed", _Name));

		if (pEnvironment->m_nPendingInstalls)
			co_return Auditor.f_Exception(fg_Format("Applications are being installed in environment '{}'", _Name));

		// The window client hosts the machine and holds the image bundle until it closes
		if (pEnvironment->m_bWindowOpen)
			co_return Auditor.f_Exception(fg_Format("Close the window of environment '{}' before removing it", _Name));

		pEnvironment->m_bRemoving = true;
		auto ClearRemoving = g_OnScopeExit / [pEnvironment]
			{
				pEnvironment->m_bRemoving = false;
			}
		;

		// An environment without applications can still be installing OS packages, which a stop in the middle breaks in a
		// virtual machine image or on the host of a local environment
		co_await (fp_WaitForEnvironmentIdle(pEnvironment) % "Failed to wait for the environment operations" % Auditor);

		// The stop suspends before it starts, and an update admitted meanwhile would be cut off
		++pEnvironment->m_nUpdateAdmissionClosed;
		auto ReopenUpdateAdmission = g_OnScopeExit / [pEnvironment]
			{
				--pEnvironment->m_nUpdateAdmissionClosed;
			}
		;

		bool bContainer = pEnvironment->m_Settings.m_Type == CAppManagerInterface::EEnvironmentType_Container;

		// The storage of an environment with a parent application is in the parent's directory, which is missing while its encryption is closed
		if (_bDeleteApplicationData)
		{
			CStr StorageError;
			CAppManagerInterface::EStatusSeverity StorageSeverity;
			if (!fp_EnvironmentStorageReady(*pEnvironment, StorageError, StorageSeverity))
				co_return Auditor.f_Exception(fg_Format("Cannot delete the data of environment '{}': {}", _Name, StorageError));
		}

		// The runtime that AppManager manages is not running after a host restart until an environment starts, and the stop and removal of
		// a container need it
		if (bContainer && (!pEnvironment->m_ContainerFingerprint.f_IsEmpty() || _bDeleteApplicationData))
			co_await (fp_EnsureContainerSystem(pEnvironment) % "Failed to start the container system" % Auditor);

		co_await (fp_StopEnvironmentInternal(pEnvironment) % "Failed to stop environment" % Auditor);

		if (bContainer)
			co_await fp_RemoveEnvironmentContainer(pEnvironment);

		// The container or virtual machine is gone, so nothing in the environment changes the data while it is deleted
		if (_bDeleteApplicationData)
		{
			if (bContainer)
				co_await (fp_RemoveEnvironmentDataVolume(pEnvironment) % Auditor);

			co_await (fp_DeleteEnvironmentStorage(pEnvironment) % Auditor);
		}
		else if (fp_EnvironmentUsesRemoteStorage(*pEnvironment))
			_fOnInfo("Kept the application data of environment '{}'. To delete it, remove the environment with --delete-application-data."_f << _Name);

		pEnvironment->m_bDeleted = true;
		mp_Environments.f_Remove(_Name);

		if (auto *pEnvironmentsState = mp_State.m_StateDatabase.m_Data.f_GetMember("Environments"))
		{
			if (pEnvironmentsState->f_GetMember(_Name))
				pEnvironmentsState->f_RemoveMember(_Name);
		}

		co_await (mp_State.m_StateDatabase.f_Save() % "Failed to save state" % Auditor);

		// The trust manager persists the listen apart from the environment state
		co_await fp_RemoveEnvironmentListen(pEnvironment).f_Wrap() > fg_LogError("Malterlib/Cloud/AppManager", "Failed to remove environment listen");

		fp_UnregisterEnvironmentPermissions(pEnvironment) > fg_LogError("Malterlib/Cloud/AppManager", "Failed to unregister environment permissions");

		_fOnInfo(fg_Format("Environment '{}' was successfully removed", _Name));
		Auditor.f_Info(fg_Format("Removed environment '{}'", _Name));

		co_return {};
	}

	auto CAppManagerActor::fp_ChangeEnvironmentSettings
		(
			CStr _Name
			, CEnvironmentSettings _Settings
			, EEnvironmentSetting _ChangedSettings
			, TCFunction<void (CStr const &_Info)> _fOnInfo
			, CCallingHostInfo _CallingHostInfo
		)
		-> TCFuture<void>
	{
		auto Auditor = f_Auditor({}, _CallingHostInfo);

		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;

		Permissions["Command"] = {{"AppManager/CommandAll", "AppManager/Command/EnvironmentChangeSettings"}};
		Permissions["Environment"] =
			{
				CPermissionQuery{"AppManager/EnvironmentAll", fg_Format("AppManager/Environment/{}", _Name)}.f_Description("Access environment {} in AppManager"_f << _Name)
			}
		;
		// The type of an environment doesn't change, but the environment can be removed and added again with another type while the permissions
		// are checked, so the environment found after the check must have the type the queries were made for
		CAppManagerInterface::EEnvironmentType QueriedType = _Settings.m_Type;
		if (auto *pFindEnvironment = mp_Environments.f_FindEqual(_Name))
			QueriedType = (*pFindEnvironment)->m_Settings.m_Type;

		_Settings.f_AddPermissionQueries(Permissions, _ChangedSettings, QueriedType);

		NContainer::TCMap<NStr::CStr, bool> HasPermissions = co_await
			(
				mp_Permissions.f_HasPermissions("Change environment settings in AppManager", Permissions, _CallingHostInfo) % "Permission denied changing environment settings" % Auditor
			)
		;

		if (!HasPermissions["Command"])
			co_return Auditor.f_AccessDenied("(Environment change settings, command)", Permissions["Command"]);

		if (!HasPermissions["Environment"])
			co_return Auditor.f_AccessDenied("(Environment change settings, environment name)", Permissions["Environment"]);

		for (auto *pName : {"HostAccess", "AgentApplication", "ParentApplication"})
		{
			if (auto *pHasPermission = HasPermissions.f_FindEqual(pName); pHasPermission && !*pHasPermission)
				co_return Auditor.f_AccessDenied("(Environment change settings, {})"_f << pName, Permissions[pName]);
		}

		auto *pFindEnvironment = mp_Environments.f_FindEqual(_Name);
		if (!pFindEnvironment)
			co_return Auditor.f_Exception(fg_Format("No such environment '{}'", _Name));

		auto pEnvironment = *pFindEnvironment;

		if (pEnvironment->m_Settings.m_Type != QueriedType)
			co_return Auditor.f_Exception(fg_Format("Environment '{}' was replaced while its settings changed", _Name));

		auto OldSettings = pEnvironment->m_Settings;
		auto NewSettings = OldSettings;
		NewSettings.f_ApplySettings(_ChangedSettings, _Settings);

		if (NewSettings.m_AgentApplication.f_IsEmpty() && NewSettings.m_Type != CAppManagerInterface::EEnvironmentType_Local)
			NewSettings.m_AgentApplication = "SelfUpdate.{}"_f << fsp_GetEnvironmentGuestPlatform(NewSettings.m_Type);

		{
			CStr Error;
			if (!NewSettings.f_Validate(Error))
				co_return Auditor.f_Exception(Error);
		}

		EEnvironmentSetting ChangedSettings = pEnvironment->m_Settings.f_ChangedSettings(NewSettings);

		if (ChangedSettings == EEnvironmentSetting_None)
		{
			_fOnInfo("No settings were changed");
			co_return {};
		}

		// Only the runtime that created the data volume reaches it, also to delete it with the environment. A running environment is refused
		// below
		if ((ChangedSettings & EEnvironmentSetting_ContainerRuntime) && !(pEnvironment->f_IsStarted() || pEnvironment->m_bStarting || pEnvironment->m_bStopping))
		{
			bool bDataVolumeExists = co_await (fp_EnvironmentDataVolumeExists(pEnvironment) % "Failed to check the data volume of the current container runtime" % Auditor);

			if (pEnvironment->m_bDeleted || pEnvironment->m_bRemoving || pEnvironment->m_Settings.f_ChangedSettings(OldSettings) != EEnvironmentSetting_None)
				co_return Auditor.f_Exception(fg_Format("Environment '{}' was changed or removed while its container runtime changed", _Name));

			if (bDataVolumeExists)
			{
				co_return Auditor.f_Exception
					(
						fg_Format
						(
							"The data volume of environment '{}' exists in the current container runtime. Remove the environment with --delete-application-data "
							"and add it again to change its container runtime."
							, _Name
						)
					)
				;
			}
		}

		if (ChangedSettings & (EEnvironmentSetting_ParentApplication | EEnvironmentSetting_AgentApplication))
		{
			if (CStr Error = fp_CheckLocalEnvironmentAccounts(NewSettings); !Error.f_IsEmpty())
				co_return Auditor.f_Exception(Error);
		}

		if (ChangedSettings & EEnvironmentSetting_ParentApplication)
		{
			// Closed encrypted storage of the current parent hides existing storage from the check below
			auto fStorageUnavailableError = [&]() -> CStr
				{
					CStr StorageError;
					CAppManagerInterface::EStatusSeverity StorageSeverity;
					if (!fp_EnvironmentStorageReady(*pEnvironment, StorageError, StorageSeverity))
						return "The parent application cannot change while the environment storage is unavailable: {}"_f << StorageError;

					return {};
				}
			;

			if (CStr Error = fStorageUnavailableError(); !Error.f_IsEmpty())
				co_return Auditor.f_Exception(Error);

			if (pEnvironment->f_IsStarted() || pEnvironment->m_bStarting)
				co_return Auditor.f_Exception("Stop the environment before changing the parent application, because it moves the environment storage");

			// The storage is not moved, so existing storage would be left behind
			CStr StorageDirectory = fp_GetEnvironmentStorageDirectory(*pEnvironment);
			bool bHasStorage;
			{
				auto BlockingActorCheckout = fg_BlockingActor();

				bHasStorage = co_await
					(
						g_Dispatch(BlockingActorCheckout) / [StorageDirectory]()
						{
							return CFile::fs_FileExists(StorageDirectory, EFileAttrib_Directory);
						}
					)
				;
			}

			// The environment can have started, changed or been removed, and the new parent removed, while the storage was checked,
			// so the checks of the parent follow the suspension
			if (pEnvironment->m_bDeleted || pEnvironment->m_bRemoving || pEnvironment->m_Settings.f_ChangedSettings(OldSettings) != EEnvironmentSetting_None)
				co_return Auditor.f_Exception(fg_Format("Environment '{}' was changed or removed while its parent application changed", _Name));

			if (bHasStorage)
				co_return Auditor.f_Exception(fg_Format("The environment storage at '{}' exists. Remove and add the environment to change its parent application.", StorageDirectory));

			if (CStr Error = fStorageUnavailableError(); !Error.f_IsEmpty())
				co_return Auditor.f_Exception(Error);

			if (pEnvironment->f_IsStarted() || pEnvironment->m_bStarting)
				co_return Auditor.f_Exception("Stop the environment before changing the parent application, because it moves the environment storage");

			if (!NewSettings.m_ParentApplication.f_IsEmpty() && !mp_Applications.f_FindEqual(NewSettings.m_ParentApplication))
				co_return Auditor.f_Exception(fg_Format("Parent application '{}' does not exist", NewSettings.m_ParentApplication));

			// The environment storage is created in the directory of the parent application on the host
			if (auto *pParent = mp_Applications.f_FindEqual(NewSettings.m_ParentApplication); pParent && fp_ApplicationRemoteStorageEnvironment(**pParent))
				co_return Auditor.f_Exception(fg_Format("Parent application '{}' keeps its files in environment-managed storage", NewSettings.m_ParentApplication));
		}

		if ((ChangedSettings & (EEnvironmentSetting_VMImage | EEnvironmentSetting_ParentApplication)))
		{
			if (CStr Sharing = fp_FindEnvironmentSharingVMImage(_Name, NewSettings); !Sharing.f_IsEmpty())
				co_return Auditor.f_Exception(fg_Format("VM image '{}' is used by environment '{}'", NewSettings.m_VMImage, Sharing));
		}

		// The image bundle of a VM is in the directory of its parent application, so a changed parent selects another bundle, like a changed image
		EEnvironmentSetting VMBundleSettings = EEnvironmentSetting_VMImage;
		if (NewSettings.m_Type == CAppManagerInterface::EEnvironmentType_VM)
			VMBundleSettings |= EEnvironmentSetting_ParentApplication;

		// The image sharing check only sees the settings, so a running machine keeps its image until it stops
		if
		(
			(ChangedSettings & VMBundleSettings)
			&& (pEnvironment->f_IsStarted() || pEnvironment->m_bStarting || pEnvironment->m_bStopping || pEnvironment->m_bWindowOpen || pEnvironment->m_VMActor)
		)
		{
			co_return Auditor.f_Exception("Stop the environment before changing its VM image or parent application");
		}

		// The type selects how the environment is stopped and removed and where the application files live, so a
		// changed type would leave the old environment and the installed applications behind
		if (ChangedSettings & EEnvironmentSetting_Type)
			co_return Auditor.f_Exception("Remove and add the environment to change its type");

		// The stop goes through the runtime that started the container
		if ((ChangedSettings & EEnvironmentSetting_ContainerRuntime) && (pEnvironment->f_IsStarted() || pEnvironment->m_bStarting || pEnvironment->m_bStopping))
			co_return Auditor.f_Exception("Stop the environment before changing the container runtime");

		// Only the runtime that created the container removes it
		if ((ChangedSettings & EEnvironmentSetting_ContainerRuntime) && !pEnvironment->m_ContainerFingerprint.f_IsEmpty())
			co_return Auditor.f_Exception("The container of the environment exists. Remove and add the environment to change its container runtime.");

		// The application files live in storage of the runtime, which the new runtime does not see
		if ((ChangedSettings & EEnvironmentSetting_ContainerRuntime) && fp_EnvironmentUsesRemoteStorage(*pEnvironment))
		{
			for (auto &pApplication : mp_Applications)
			{
				if (pApplication->m_Settings.m_LaunchEnvironment == _Name)
				{
					co_return Auditor.f_Exception
						(
							fg_Format("Application '{}' keeps its files in the storage of the container runtime. Change the application settings first.", pApplication->m_Name)
						)
					;
				}
			}
		}

		pEnvironment->m_Settings = NewSettings;

		// The agent installed in the old image keeps the launch ID and would register with this environment when that image boots for another one
		if (ChangedSettings & VMBundleSettings)
		{
			pEnvironment->m_LaunchID = {};
			pEnvironment->m_VMAgentLaunchID = {};
			pEnvironment->m_VMAgentFingerprint = {};
		}

		_fOnInfo("Saving environment state");
		auto SaveResult = co_await fp_UpdateEnvironmentJson(pEnvironment).f_Wrap();

		// The settings apply whether or not they are saved, and a repeated change finds nothing changed, so the running agent gets them either
		// way. An agent that is not running gets the configuration when it connects
		if ((ChangedSettings & EEnvironmentSetting_OSDependencies) && pEnvironment->f_IsStarted())
		{
			_fOnInfo("Configuring the running environment agent with the changed OS dependencies");
			fp_SendEnvironmentAgentConfig(*pEnvironment);
		}

		if (!SaveResult)
			co_return Auditor.f_Exception(fg_Format("Failed to save environment state: {}", SaveResult.f_GetExceptionStr()));

		_fOnInfo("Environment settings were successfully changed");
		Auditor.f_Info(fg_Format("Updated settings for environment '{}'", _Name));

		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_StartEnvironment(CStr _Name, CCallingHostInfo _CallingHostInfo)
	{
		auto Auditor = f_Auditor({}, _CallingHostInfo);

		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;

		Permissions["Command"] = {{"AppManager/CommandAll", "AppManager/Command/EnvironmentStart"}};
		Permissions["Environment"] =
			{
				CPermissionQuery{"AppManager/EnvironmentAll", fg_Format("AppManager/Environment/{}", _Name)}.f_Description("Access environment {} in AppManager"_f << _Name)
			}
		;

		NContainer::TCMap<NStr::CStr, bool> HasPermissions = co_await
			(
				mp_Permissions.f_HasPermissions("Start environment in AppManager", Permissions, _CallingHostInfo) % "Permission denied starting environment" % Auditor
			)
		;

		if (!HasPermissions["Command"])
			co_return Auditor.f_AccessDenied("(Environment start, command)", Permissions["Command"]);

		if (!HasPermissions["Environment"])
			co_return Auditor.f_AccessDenied("(Environment start, environment name)", Permissions["Environment"]);

		auto *pFindEnvironment = mp_Environments.f_FindEqual(_Name);
		if (!pFindEnvironment)
			co_return Auditor.f_Exception(fg_Format("No such environment '{}'", _Name));

		auto pEnvironment = *pFindEnvironment;

		co_await (fp_SetEnvironmentStoppedManually(pEnvironment, false) % "Failed to save environment state" % Auditor);
		if (pEnvironment->m_bDeleted)
			co_return Auditor.f_Exception(fg_Format("Environment '{}' was removed", _Name));

		co_await (fp_EnsureEnvironmentStarted(pEnvironment) % "Failed to start environment" % Auditor);
		fp_UpdateApplicationDependencies();

		Auditor.f_Info(fg_Format("Started environment '{}'", _Name));

		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_PullEnvironment(CStr _Name, CCallingHostInfo _CallingHostInfo)
	{
		auto Auditor = f_Auditor({}, _CallingHostInfo);

		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;

		Permissions["Command"] = {{"AppManager/CommandAll", "AppManager/Command/EnvironmentPull"}};
		Permissions["Environment"] =
			{
				CPermissionQuery{"AppManager/EnvironmentAll", fg_Format("AppManager/Environment/{}", _Name)}.f_Description("Access environment {} in AppManager"_f << _Name)
			}
		;

		NContainer::TCMap<NStr::CStr, bool> HasPermissions = co_await
			(
				mp_Permissions.f_HasPermissions("Pull environment in AppManager", Permissions, _CallingHostInfo) % "Permission denied pulling environment" % Auditor
			)
		;

		if (!HasPermissions["Command"])
			co_return Auditor.f_AccessDenied("(Environment pull, command)", Permissions["Command"]);

		if (!HasPermissions["Environment"])
			co_return Auditor.f_AccessDenied("(Environment pull, environment name)", Permissions["Environment"]);

		auto *pFindEnvironment = mp_Environments.f_FindEqual(_Name);
		if (!pFindEnvironment)
			co_return Auditor.f_Exception(fg_Format("No such environment '{}'", _Name));

		auto pEnvironment = *pFindEnvironment;

		if (pEnvironment->m_Settings.m_Type != CAppManagerInterface::EEnvironmentType_Container)
			co_return Auditor.f_Exception(fg_Format("Environment '{}' is not a container environment", _Name));

		co_await (fp_EnsureContainerSystem(pEnvironment) % "Failed to start the container system" % Auditor);

		// Pull before replacing the container, so a failing pull leaves the current container in place
		co_await (fp_PullEnvironmentContainerImage(pEnvironment) % "Failed to pull environment image" % Auditor);

		co_await (fp_WaitForEnvironmentIdle(pEnvironment) % "Failed to wait for the environment applications" % Auditor);
		if (pEnvironment->m_bDeleted)
			co_return Auditor.f_Exception(fg_Format("Environment '{}' was removed", _Name));

		// The environment can have started or stopped while the image was pulled
		bool bWasStarted = pEnvironment->f_IsStarted() || pEnvironment->m_bStarting;

		// The removal by name of a second replacement would remove the container that the first one starts
		if (pEnvironment->m_bReplacingContainerAfterPull)
			co_return Auditor.f_Exception(fg_Format("Environment '{}' is already being replaced after a pull", _Name));

		// The launches of the applications the stop stops are refused until the replacement ends, and wait for this also when it fails
		auto UpdateDependencies = g_OnScopeExit / [this]
			{
				if (!mp_bStoppingApp)
					fp_UpdateApplicationDependencies();
			}
		;

		{
			pEnvironment->m_bReplacingContainerAfterPull = true;
			auto ClearReplacing = g_OnScopeExit / [pEnvironment]
				{
					pEnvironment->m_bReplacingContainerAfterPull = false;
				}
			;

			co_await (fp_StopEnvironmentInternal(pEnvironment) % "Failed to stop environment" % Auditor);

			co_await fp_RemoveEnvironmentContainer(pEnvironment);

			pEnvironment->m_ContainerFingerprint = {};

			co_await (fp_UpdateEnvironmentJson(pEnvironment) % "Failed to save environment state" % Auditor);
		}

		if (bWasStarted && !pEnvironment->m_bStoppedManually)
			co_await (fp_EnsureEnvironmentStarted(pEnvironment) % "Failed to start environment" % Auditor);

		Auditor.f_Info(fg_Format("Pulled environment '{}'", _Name));

		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_StopEnvironment(CStr _Name, CCallingHostInfo _CallingHostInfo)
	{
		auto Auditor = f_Auditor({}, _CallingHostInfo);

		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;

		Permissions["Command"] = {{"AppManager/CommandAll", "AppManager/Command/EnvironmentStop"}};
		Permissions["Environment"] =
			{
				CPermissionQuery{"AppManager/EnvironmentAll", fg_Format("AppManager/Environment/{}", _Name)}.f_Description("Access environment {} in AppManager"_f << _Name)
			}
		;

		NContainer::TCMap<NStr::CStr, bool> HasPermissions = co_await
			(
				mp_Permissions.f_HasPermissions("Stop environment in AppManager", Permissions, _CallingHostInfo) % "Permission denied stopping environment" % Auditor
			)
		;

		if (!HasPermissions["Command"])
			co_return Auditor.f_AccessDenied("(Environment stop, command)", Permissions["Command"]);

		if (!HasPermissions["Environment"])
			co_return Auditor.f_AccessDenied("(Environment stop, environment name)", Permissions["Environment"]);

		auto *pFindEnvironment = mp_Environments.f_FindEqual(_Name);
		if (!pFindEnvironment)
			co_return Auditor.f_Exception(fg_Format("No such environment '{}'", _Name));

		auto pEnvironment = *pFindEnvironment;

		co_await (fp_WaitForEnvironmentIdle(pEnvironment) % "Failed to wait for the environment applications" % Auditor);
		if (pEnvironment->m_bDeleted)
			co_return Auditor.f_Exception(fg_Format("Environment '{}' was removed", _Name));

		// The stop suspends before it starts, and an update admitted meanwhile would be cut off
		++pEnvironment->m_nUpdateAdmissionClosed;
		auto ReopenUpdateAdmission = g_OnScopeExit / [pEnvironment]
			{
				--pEnvironment->m_nUpdateAdmissionClosed;
			}
		;

		// The window client hosts the virtual machine, which only a shutdown through the agent in the guest reaches from here
		if (pEnvironment->m_bWindowOpen && !pEnvironment->m_AgentInterface)
			co_return Auditor.f_Exception(fg_Format("Environment '{}' runs in a window and its agent is not connected. Close the window to stop it.", _Name));

		// Set before stopping, so the applications the stop stops are not relaunched into a new start
		co_await (fp_SetEnvironmentStoppedManually(pEnvironment, true) % "Failed to save environment state" % Auditor);
		if (pEnvironment->m_bDeleted)
			co_return Auditor.f_Exception(fg_Format("Environment '{}' was removed", _Name));

		co_await (fp_StopEnvironmentInternal(pEnvironment) % "Failed to stop environment" % Auditor);

		Auditor.f_Info(fg_Format("Stopped environment '{}'", _Name));

		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_RestartEnvironment(CStr _Name, CCallingHostInfo _CallingHostInfo)
	{
		auto Auditor = f_Auditor({}, _CallingHostInfo);

		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;

		Permissions["Command"] = {{"AppManager/CommandAll", "AppManager/Command/EnvironmentRestart"}};
		Permissions["Environment"] =
			{
				CPermissionQuery{"AppManager/EnvironmentAll", fg_Format("AppManager/Environment/{}", _Name)}.f_Description("Access environment {} in AppManager"_f << _Name)
			}
		;

		NContainer::TCMap<NStr::CStr, bool> HasPermissions = co_await
			(
				mp_Permissions.f_HasPermissions("Restart environment in AppManager", Permissions, _CallingHostInfo) % "Permission denied restarting environment" % Auditor
			)
		;

		if (!HasPermissions["Command"])
			co_return Auditor.f_AccessDenied("(Environment restart, command)", Permissions["Command"]);

		if (!HasPermissions["Environment"])
			co_return Auditor.f_AccessDenied("(Environment restart, environment name)", Permissions["Environment"]);

		auto *pFindEnvironment = mp_Environments.f_FindEqual(_Name);
		if (!pFindEnvironment)
			co_return Auditor.f_Exception(fg_Format("No such environment '{}'", _Name));

		auto pEnvironment = *pFindEnvironment;

		co_await (fp_WaitForEnvironmentIdle(pEnvironment) % "Failed to wait for the environment applications" % Auditor);
		if (pEnvironment->m_bDeleted)
			co_return Auditor.f_Exception(fg_Format("Environment '{}' was removed", _Name));

		// The launches of the applications the stop stops are refused while it runs, and wait for this also when the restart fails
		auto UpdateDependencies = g_OnScopeExit / [this]
			{
				if (!mp_bStoppingApp)
					fp_UpdateApplicationDependencies();
			}
		;

		{
			// The stop suspends before it starts, and an update admitted meanwhile would be cut off
			++pEnvironment->m_nUpdateAdmissionClosed;
			auto ReopenUpdateAdmission = g_OnScopeExit / [pEnvironment]
				{
					--pEnvironment->m_nUpdateAdmissionClosed;
				}
			;

			co_await (fp_SetEnvironmentStoppedManually(pEnvironment, false) % "Failed to save environment state" % Auditor);
			co_await (fp_StopEnvironmentInternal(pEnvironment) % "Failed to stop environment" % Auditor);
		}

		co_await (fp_EnsureEnvironmentStarted(pEnvironment) % "Failed to start environment" % Auditor);

		Auditor.f_Info(fg_Format("Restarted environment '{}'", _Name));

		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_WaitForEnvironmentIdle(TCSharedPointer<CEnvironment> _pEnvironment)
	{
		auto CheckDestroy = co_await f_CheckDestroyedOnResume();

		// Stopping the agent in the middle of an update operation on an application would corrupt it
		CStopwatch Stopwatch{true};
		while (true)
		{
			if (_pEnvironment->m_bDeleted)
				co_return {};

			bool bInProgress = false;
			for (auto &pApplication : mp_Applications)
			{
				if (pApplication->m_Settings.m_LaunchEnvironment != _pEnvironment->m_Name)
					continue;

				if (pApplication->f_IsInProgress())
				{
					bInProgress = true;
					break;
				}
			}

			// The agent configuration installs the OS dependencies of the environment
			if (_pEnvironment->m_nPendingAgentConfigurations)
				bInProgress = true;

			if (_pEnvironment->m_nPendingInstalls)
				bInProgress = true;

			// Stopping a container in the middle of an OS package installation leaves the packages broken
			if (_pEnvironment->f_AgentUpdateDefers())
				bInProgress = true;

			// An error script can outlive the operation of its application
			if (_pEnvironment->m_nRunningScripts)
				bInProgress = true;

			if (!bInProgress)
				co_return {};

			if (Stopwatch.f_GetTime() > 1_hours)
				co_return DMibErrorInstance("Operations kept the applications of environment '{}' busy for an hour"_f << _pEnvironment->m_Name);

			co_await fg_Timeout(10.0);
		}
	}

	// The applications prevent automatic restarts of their environment like they prevent host reboots. Returns the first application that prevents
	// restarts of _Environment, or empty
	CStr CAppManagerActor::fp_FindApplicationPreventingEnvironmentRestart(CEnvironment const &_Environment)
	{
		for (auto &pApplication : mp_Applications)
		{
			if (pApplication->m_Settings.m_LaunchEnvironment != _Environment.m_Name)
				continue;

			if (fg_IsSet(pApplication->m_PreventRebootSensorFlags, CDistributedAppSensorReporter::ESensorInfoFlag::mc_AllPreventRebootFlags))
				return pApplication->m_Name;
		}

		return {};
	}

	TCFuture<void> CAppManagerActor::fp_RestartEnvironmentWhenIdle(TCSharedPointer<CEnvironment> _pEnvironment)
	{
		auto CheckDestroy = co_await f_CheckDestroyedOnResume();

		// A guest can request restarts at any rate, so the requests that come while one waits join it
		if (_pEnvironment->m_bRestartWhenIdlePending)
			co_return {};

		_pEnvironment->m_bRestartWhenIdlePending = true;
		auto ClearRestartPending = g_OnScopeExit / [_pEnvironment]
			{
				_pEnvironment->m_bRestartWhenIdlePending = false;
			}
		;

		// A stop while this waits, like a manual restart, gives the requests their restart, and a restart of the environment it started would be
		// one too many
		umint nStops = _pEnvironment->m_nStops;
		auto fSuperseded = [&]
			{
				return _pEnvironment->m_bDeleted || _pEnvironment->m_bStopping || _pEnvironment->m_bStoppedManually || _pEnvironment->m_nStops != nStops;
			}
		;

		// An application can prevent the restart while the operations of the others finish, so both are checked together
		auto fRestartPrevented = [&]
			{
				return !fp_FindApplicationPreventingEnvironmentRestart(*_pEnvironment).f_IsEmpty();
			}
		;

		CStopwatch Stopwatch{true};
		while (true)
		{
			if (fSuperseded())
				co_return {};

			if (!fRestartPrevented())
			{
				co_await (fp_WaitForEnvironmentIdle(_pEnvironment) % CStr("Aborted the restart of environment '{}'"_f << _pEnvironment->m_Name));

				if (fSuperseded())
					co_return {};

				if (!fRestartPrevented())
					break;
			}

			if (Stopwatch.f_GetTime() > 1_hours)
				co_return DMibErrorInstance("Aborted the restart of environment '{}': its applications prevented the restart for an hour"_f << _pEnvironment->m_Name);

			co_await fg_Timeout(10.0);
		}

		// A request from now on can need a newer agent than the start below runs, so it waits for a restart of its own
		ClearRestartPending.f_Clear();
		_pEnvironment->m_bRestartWhenIdlePending = false;

		DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Restarting environment '{}'", _pEnvironment->m_Name);

		// The launches of the applications the stop stops are refused while it runs, and wait for this also when the restart fails
		auto UpdateDependencies = g_OnScopeExit / [this]
			{
				if (!mp_bStoppingApp)
					fp_UpdateApplicationDependencies();
			}
		;

		{
			// The stop suspends before it starts, and an update admitted meanwhile would be cut off
			++_pEnvironment->m_nUpdateAdmissionClosed;
			auto ReopenUpdateAdmission = g_OnScopeExit / [_pEnvironment]
				{
					--_pEnvironment->m_nUpdateAdmissionClosed;
				}
			;

			co_await fp_StopEnvironmentInternal(_pEnvironment);
		}

		// The stop suspends, so a manual stop or a removal can supersede this restart
		if (_pEnvironment->m_bDeleted || _pEnvironment->m_bStoppedManually)
			co_return {};

		co_await fp_EnsureEnvironmentStarted(_pEnvironment);

		co_return {};
	}

	TCFuture<void> CAppManagerActor::CAppManagerInterfaceImplementation::f_EnvironmentAdd(CStr _Name, CEnvironmentSettings _Settings)
	{
		CAppManagerActor::CEnvironmentSettings EnvironmentSettings;
		EEnvironmentSetting ChangedSettings = EEnvironmentSetting_None;
		EnvironmentSettings.f_FromInterfaceSettings(_Settings, ChangedSettings);

		co_return co_await m_pThis->fp_AddEnvironment
			(
				_Name
				, EnvironmentSettings
				, ChangedSettings
				, [](CStr const &_Info)
				{
					DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Environment Add: {}", _Info);
				}
				, fg_GetCallingHostInfo()
			)
		;
	}

	TCFuture<void> CAppManagerActor::CAppManagerInterfaceImplementation::f_EnvironmentRemove(CStr _Name)
	{
		co_return co_await m_pThis->fp_RemoveEnvironment
			(
				_Name
				, false
				, [](CStr const &_Info)
				{
					DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Environment Remove: {}", _Info);
				}
				, fg_GetCallingHostInfo()
			)
		;
	}

	TCFuture<void> CAppManagerActor::CAppManagerInterfaceImplementation::f_EnvironmentChangeSettings(CStr _Name, CEnvironmentSettings _Settings)
	{
		CAppManagerActor::CEnvironmentSettings EnvironmentSettings;
		EEnvironmentSetting ChangedSettings = EEnvironmentSetting_None;
		EnvironmentSettings.f_FromInterfaceSettings(_Settings, ChangedSettings);

		co_return co_await m_pThis->fp_ChangeEnvironmentSettings
			(
				_Name
				, EnvironmentSettings
				, ChangedSettings
				, [](CStr const &_Info)
				{
					DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Environment Change Settings: {}", _Info);
				}
				, fg_GetCallingHostInfo()
			)
		;
	}

	TCFuture<void> CAppManagerActor::CAppManagerInterfaceImplementation::f_EnvironmentStart(CStr _Name)
	{
		co_return co_await m_pThis->fp_StartEnvironment(_Name, fg_GetCallingHostInfo());
	}

	TCFuture<void> CAppManagerActor::CAppManagerInterfaceImplementation::f_EnvironmentPull(CStr _Name)
	{
		co_return co_await m_pThis->fp_PullEnvironment(_Name, fg_GetCallingHostInfo());
	}

	TCFuture<void> CAppManagerActor::CAppManagerInterfaceImplementation::f_EnvironmentStop(CStr _Name)
	{
		co_return co_await m_pThis->fp_StopEnvironment(_Name, fg_GetCallingHostInfo());
	}

	TCFuture<void> CAppManagerActor::CAppManagerInterfaceImplementation::f_EnvironmentRestart(CStr _Name)
	{
		co_return co_await m_pThis->fp_RestartEnvironment(_Name, fg_GetCallingHostInfo());
	}

	auto CAppManagerActor::CAppManagerInterfaceImplementation::f_GetEnvironments() -> TCFuture<TCMap<CStr, CEnvironmentInfo>>
	{
		auto pThis = m_pThis;
		auto Auditor = pThis->f_Auditor();

		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;

		Permissions["//Command//"] = {{"AppManager/CommandAll", "AppManager/Command/EnvironmentEnum"}};

		// Environments and applications can share names, so their queries are kept apart by prefixes that names can't contain
		for (auto &pEnvironment : pThis->mp_Environments)
		{
			auto &Environment = *pEnvironment;

			Permissions[CStr("Environment//{}"_f << Environment.m_Name)] =
				{
					CPermissionQuery{"AppManager/EnvironmentAll", fg_Format("AppManager/Environment/{}", Environment.m_Name)}
					.f_Description("Access environment {} in AppManager"_f << Environment.m_Name)
				}
			;
		}

		// The information names applications, which are listed only to callers that can access them, as f_GetInstalled lists them. The agent and
		// parent applications of an environment can be named before they are installed
		TCSet<CStr> ApplicationNames;
		for (auto &pApplication : pThis->mp_Applications)
			ApplicationNames[pApplication->m_Name];

		for (auto &pEnvironment : pThis->mp_Environments)
		{
			for (auto *pName : {&pEnvironment->m_Settings.m_AgentApplication, &pEnvironment->m_Settings.m_ParentApplication})
			{
				if (!pName->f_IsEmpty())
					ApplicationNames[*pName];
			}
		}

		for (auto &ApplicationName : ApplicationNames)
		{
			Permissions[CStr("Application//{}"_f << ApplicationName)] =
				{
					CPermissionQuery{"AppManager/AppAll", fg_Format("AppManager/App/{}", ApplicationName)}.f_Description("Access application {} in AppManager"_f << ApplicationName)
				}
			;
		}

		auto HasPermissions = co_await (pThis->mp_Permissions.f_HasPermissions("Enumerate environments in AppManager", Permissions) % "Permission denied enumerating environments" % Auditor);

		if (!HasPermissions["//Command//"])
			co_return Auditor.f_AccessDenied("(Environment enum)", Permissions["//Command//"]);

		auto fHasApplicationPermission = [&](CStr const &_Application)
			{
				auto pHasPermission = HasPermissions.f_FindEqual(CStr("Application//{}"_f << _Application));
				return pHasPermission && *pHasPermission;
			}
		;

		TCMap<CStr, CEnvironmentInfo> OutputEnvironments;
		for (auto &pEnvironment : pThis->mp_Environments)
		{
			auto &Environment = *pEnvironment;

			auto pHasPermission = HasPermissions.f_FindEqual(CStr("Environment//{}"_f << Environment.m_Name));
			if (!pHasPermission || !*pHasPermission)
				continue;

			auto &OutputEnvironment = OutputEnvironments[Environment.m_Name] = pThis->fp_GetEnvironmentInfo(Environment);

			if (!OutputEnvironment.m_AgentApplication.f_IsEmpty() && !fHasApplicationPermission(OutputEnvironment.m_AgentApplication))
				OutputEnvironment.m_AgentApplication.f_Clear();

			if (!OutputEnvironment.m_ParentApplication.f_IsEmpty() && !fHasApplicationPermission(OutputEnvironment.m_ParentApplication))
				OutputEnvironment.m_ParentApplication.f_Clear();

			for (auto iApplication = OutputEnvironment.m_Applications.f_GetIterator(); iApplication;)
			{
				if (fHasApplicationPermission(*iApplication))
					++iApplication;
				else
					iApplication.f_Remove();
			}
		}

		Auditor.f_Info("Enum environments");
		co_return fg_Move(OutputEnvironments);
	}

	TCFuture<uint32> CAppManagerActor::fp_CommandLine_AddEnvironment(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		CStr Name = _Params["Name"].f_String();

		CEnvironmentSettings Settings;
		EEnvironmentSetting ChangedSettings = EEnvironmentSetting_None;

		{
			CStr Error;
			if (!Settings.f_ParseSettings(_Params, ChangedSettings, Error))
				co_return DMibErrorInstance(Error);
		}

		auto Result = co_await fp_AddEnvironment
			(
				Name
				, Settings
				, ChangedSettings
				, [=](CStr const &_Info)
				{
					*_pCommandLine += _Info + DMibNewLine;
					DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "{}", _Info);
				}
				, fg_GetCallingHostInfo()
			)
			.f_Wrap()
		;

		co_return _pCommandLine->f_AddAsyncResult(Result);
	}

	TCFuture<uint32> CAppManagerActor::fp_CommandLine_ChangeEnvironmentSettings(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		CStr Name = _Params["Name"].f_String();

		CEnvironmentSettings Settings;
		EEnvironmentSetting ChangedSettings = EEnvironmentSetting_None;

		{
			CStr Error;
			if (!Settings.f_ParseSettings(_Params, ChangedSettings, Error))
				co_return DMibErrorInstance(Error);
		}

		auto Result = co_await fp_ChangeEnvironmentSettings
			(
				Name
				, Settings
				, ChangedSettings
				, [=](CStr const &_Info)
				{
					*_pCommandLine += _Info + DMibNewLine;
					DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "{}", _Info);
				}
				, fg_GetCallingHostInfo()
			)
			.f_Wrap()
		;

		co_return _pCommandLine->f_AddAsyncResult(Result);
	}

	TCFuture<uint32> CAppManagerActor::fp_CommandLine_RemoveEnvironment(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		CStr Name = _Params["Name"].f_String();

		auto Result = co_await fp_RemoveEnvironment
			(
				Name
				, _Params["DeleteApplicationData"].f_Boolean()
				, [=](CStr const &_Info)
				{
					*_pCommandLine += _Info + DMibNewLine;
					DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "{}", _Info);
				}
				, fg_GetCallingHostInfo()
			)
			.f_Wrap()
		;

		co_return _pCommandLine->f_AddAsyncResult(Result);
	}

	TCFuture<uint32> CAppManagerActor::fp_CommandLine_StartEnvironment(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		auto Result = co_await fp_StartEnvironment(_Params["Name"].f_String(), fg_GetCallingHostInfo()).f_Wrap();

		co_return _pCommandLine->f_AddAsyncResult(Result);
	}

	TCFuture<uint32> CAppManagerActor::fp_CommandLine_StopEnvironment(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		auto Result = co_await fp_StopEnvironment(_Params["Name"].f_String(), fg_GetCallingHostInfo()).f_Wrap();

		co_return _pCommandLine->f_AddAsyncResult(Result);
	}

	TCFuture<uint32> CAppManagerActor::fp_CommandLine_RestartEnvironment(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		auto Result = co_await fp_RestartEnvironment(_Params["Name"].f_String(), fg_GetCallingHostInfo()).f_Wrap();

		co_return _pCommandLine->f_AddAsyncResult(Result);
	}

	TCFuture<uint32> CAppManagerActor::fp_CommandLine_PullEnvironment(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		auto Result = co_await fp_PullEnvironment(_Params["Name"].f_String(), fg_GetCallingHostInfo()).f_Wrap();

		co_return _pCommandLine->f_AddAsyncResult(Result);
	}

	// Only prints a command that the caller runs on the host, where it needs host access through sudo to do anything, and acts through nothing in
	// the AppManager, so it needs no AppManager permission beyond the permission of the command line itself
	TCFuture<uint32> CAppManagerActor::fp_CommandLine_EnvironmentBash(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		CStr Name = _Params["Name"].f_String();
		CStr Shell = _Params["Shell"].f_String();

		auto *pFindEnvironment = mp_Environments.f_FindEqual(Name);
		if (!pFindEnvironment)
		{
			co_await _pCommandLine->f_StdErr("No such environment '{}'\n"_f << Name);
			co_return 1;
		}

		auto pEnvironment = *pFindEnvironment;

		if (pEnvironment->m_Settings.m_Type != CAppManagerInterface::EEnvironmentType_Container)
		{
			co_await _pCommandLine->f_StdErr("Environment '{}' is not a container environment\n"_f << Name);
			co_return 1;
		}

		CStr Executable = fp_GetContainerRuntimeExecutable(pEnvironment);
		bool bOwnAppleContainerSystem = Executable == "container" && fp_UseOwnAppleContainerSystem();
		bool bOwnColimaSystem = fp_UseOwnColimaSystem(pEnvironment);

		TCVector<CStr> Parts;

		// The AppManager-owned container systems are reached through the data location their daemons were started with; sudo covers an AppManager running as another user
		if (bOwnAppleContainerSystem || bOwnColimaSystem)
		{
			Parts.f_Insert("sudo");
			Parts.f_Insert("env");
			if (bOwnAppleContainerSystem)
				Parts.f_Insert("CONTAINER_APP_ROOT=" + fp_GetAppleContainerAppRoot());
			else
				Parts.f_Insert("DOCKER_HOST=unix://" + (fp_GetColimaAppRoot() / "colima" / "default" / "docker.sock"));
		}

		Parts.f_Insert(fg_Move(Executable));
		Parts.f_Insert("exec");
		Parts.f_Insert("--interactive");
		Parts.f_Insert("--tty");
		Parts.f_Insert(fp_GetContainerName(pEnvironment));
		Parts.f_Insert(fg_Move(Shell));

		// Backtick substitution splits words without interpreting quotes, so the parts are printed unquoted and an AppManager root with whitespace is unsupported
		CStr Command;
		for (auto &Part : Parts)
		{
			if (!Command.f_IsEmpty())
				Command += " ";

			Command += Part;
		}

		co_await _pCommandLine->f_StdOut("{}\n"_f << Command);

		co_return 0;
	}

	TCFuture<uint32> CAppManagerActor::fp_CommandLine_ColimaSettings(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		auto CallingHostInfo = fg_GetCallingHostInfo();
		auto Auditor = f_Auditor({}, CallingHostInfo);

		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;

		Permissions["Command"] = {{"AppManager/CommandAll", "AppManager/Command/ColimaSettings"}};

		NContainer::TCMap<NStr::CStr, bool> HasPermissions = co_await
			(
				mp_Permissions.f_HasPermissions("Change colima settings in AppManager", Permissions, CallingHostInfo) % "Permission denied changing colima settings" % Auditor
			)
		;

		if (!HasPermissions["Command"])
			co_return Auditor.f_AccessDenied("(Colima settings, command)", Permissions["Command"]);

		int64 nCPUs = _Params["CPUCount"].f_Integer();
		int64 MemoryMB = _Params["MemoryMB"].f_Integer();
		int64 DiskGB = _Params["DiskGB"].f_Integer();

		bool bChanged = (nCPUs >= 0 && uint32(nCPUs) != mp_ColimaCPUCount) || (MemoryMB >= 0 && uint64(MemoryMB) != mp_ColimaMemoryMB) || (DiskGB >= 0 && uint64(DiskGB) != mp_ColimaDiskGB);

		// The first colima start applies the new configuration to the shared VM, so this command stops every running colima environment before it
		// starts any
		TCVector<TCSharedPointer<CEnvironment>> RestartEnvironments;
		if (bChanged)
		{
			TCVector<CStr> RestartNames;
			for (auto &pEnvironment : mp_Environments)
			{
				if (!fp_UseOwnColimaSystem(pEnvironment))
					continue;

				if (!pEnvironment->f_IsStarted() && !pEnvironment->m_bStarting)
					continue;

				RestartEnvironments.f_Insert(fg_TempCopy(pEnvironment));
				RestartNames.f_Insert(pEnvironment->m_Name);
			}

			co_await fp_CheckEnvironmentRestartAccess(fg_Move(RestartNames), "Restart environments to apply colima settings in AppManager", CallingHostInfo);

			if (nCPUs >= 0)
				mp_ColimaCPUCount = uint32(nCPUs);
			if (MemoryMB >= 0)
				mp_ColimaMemoryMB = uint64(MemoryMB);
			if (DiskGB >= 0)
				mp_ColimaDiskGB = uint64(DiskGB);
		}

		auto fFormatValue = [](uint64 _Value) -> CStr
			{
				if (!_Value)
					return "default";

				return CStr::fs_ToStr(_Value);
			}
		;

		CStr Output = "CPU count: {}\nMemory MB: {}\nDisk GB: {}\n"_f << fFormatValue(mp_ColimaCPUCount) << fFormatValue(mp_ColimaMemoryMB) << fFormatValue(mp_ColimaDiskGB);

		if (!bChanged)
		{
			co_await _pCommandLine->f_StdOut(Output);
			co_return 0;
		}

		{
			auto &ColimaJson = mp_State.m_StateDatabase.m_Data["ColimaSettings"];
			ColimaJson["CPUCount"] = int64(mp_ColimaCPUCount);
			ColimaJson["MemoryMB"] = int64(mp_ColimaMemoryMB);
			ColimaJson["DiskGB"] = int64(mp_ColimaDiskGB);
		}

		co_await mp_State.m_StateDatabase.f_Save();

		// The changed settings are saved and the environments restart to apply them also when the client is gone
		bool bConnected = co_await fp_CommandLineProgress(_pCommandLine, true, Output, false);

		co_return co_await fp_RestartEnvironmentsForSettings(fg_Move(RestartEnvironments), _pCommandLine, bConnected);
	}

	TCFuture<uint32> CAppManagerActor::fp_CommandLine_EnumEnvironments(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		bool bVerbose = _Params["Verbose"].f_Boolean();
		CStr Name = _Params["Name"].f_String();

		auto AnsiEncoding = _pCommandLine->f_AnsiEncoding();
		CTableRenderHelper TableRenderer = _pCommandLine->f_TableRenderer();
		TableRenderer.f_AddHeadings("Environment", "Type", "Status", "Applications", "Settings");

		TCMap<CStr, CAppManagerInterface::CEnvironmentInfo> EnvironmentInfo = co_await mp_AppManagerInterface.m_Actor(&CAppManagerInterfaceImplementation::f_GetEnvironments);
		for (auto &Environment : EnvironmentInfo)
		{
			auto &EnvironmentName = EnvironmentInfo.fs_GetKey(Environment);
			if (!Name.f_IsEmpty() && EnvironmentName != Name)
				continue;

			auto fAddProperty = [&](CStr &o_String, CStr const &_Property, auto const &_Value)
				{
					o_String += "{}{}:{} "_f << AnsiEncoding.f_Prompt() << _Property << AnsiEncoding.f_Default();
					o_String += CStr::fs_ToStr(_Value);
					o_String += "\n";
				}
			;

			CStr Settings;
			fAddProperty(Settings, "Agent application", Environment.m_AgentApplication);
			if (!Environment.m_ParentApplication.f_IsEmpty())
				fAddProperty(Settings, "Parent application", Environment.m_ParentApplication);
			fAddProperty(Settings, "Auto start", Environment.m_bAutoStart);

			if (Environment.m_Type == CAppManagerInterface::EEnvironmentType_Container)
			{
				fAddProperty(Settings, "Image", Environment.m_ContainerImage);
				fAddProperty(Settings, "Runtime", Environment.m_ContainerRuntime);
				fAddProperty(Settings, "Network", Environment.m_ContainerNetwork);
				if (Environment.m_bContainerReadOnly)
					fAddProperty(Settings, "Read only", Environment.m_bContainerReadOnly);
				if (bVerbose)
				{
					fAddProperty(Settings, "Extra mounts", "{}"_f << Environment.m_ContainerExtraMounts);
					fAddProperty(Settings, "Extra arguments", "{vs}"_f << Environment.m_ContainerExtraArguments);
				}
			}
			else if (Environment.m_Type == CAppManagerInterface::EEnvironmentType_VM)
			{
				fAddProperty(Settings, "Image", Environment.m_VMImage);
				fAddProperty(Settings, "Backend", Environment.m_VMBackend);
				fAddProperty(Settings, "CPU count", Environment.m_VMCPUCount);
				fAddProperty(Settings, "Memory MB", Environment.m_VMMemoryMB);
				fAddProperty(Settings, "Data disk GB", Environment.m_VMDataDiskGB);
			}

			if (Environment.m_MemoryLimitMB != 0)
				fAddProperty(Settings, "Memory limit MB", Environment.m_MemoryLimitMB);
			if (Environment.m_CPULimit != 0.0)
				fAddProperty(Settings, "CPU limit", Environment.m_CPULimit);

			if (!Environment.m_OSDependencies.f_IsEmpty())
				fAddProperty(Settings, "OS dependencies", "{vs}"_f << Environment.m_OSDependencies);

			CStr Status;
			if (Environment.m_StatusSeverity == CAppManagerInterface::EStatusSeverity_Error)
				Status = AnsiEncoding.f_StatusError(Environment.m_Status);
			else if (Environment.m_StatusSeverity == CAppManagerInterface::EStatusSeverity_Warning)
				Status = AnsiEncoding.f_StatusWarning(Environment.m_Status);
			else
				Status = AnsiEncoding.f_StatusNormal(Environment.m_Status);

			TableRenderer.f_AddRow(EnvironmentName, CAppManagerInterface::fs_EnvironmentTypeToStr(Environment.m_Type), Status, "{vs}"_f << Environment.m_Applications, Settings);
		}

		TableRenderer.f_Output(_Params);

		co_return 0;
	}

	// A child application lives in the directory of its root application, so an environment confined to the child stores
	// its data in the root application too
	CStr CAppManagerActor::fp_FindEnvironmentStoringDataInApplication(CStr const &_Application)
	{
		for (auto &pEnvironment : mp_Environments)
		{
			if (pEnvironment->m_Settings.m_ParentApplication == _Application)
				return pEnvironment->m_Name;

			auto *pParent = mp_Applications.f_FindEqual(pEnvironment->m_Settings.m_ParentApplication);
			if (pParent && (*pParent)->m_Settings.m_ParentApplication == _Application)
				return pEnvironment->m_Name;
		}

		return {};
	}

	// The agents of local environments run with the privileges of this AppManager from the directory of an application or of
	// its children, and can store their data there, which an account of the application could write
	CStr CAppManagerActor::fp_CheckLocalEnvironmentApplicationAccount(CStr const &_Application, CApplicationSettings const &_NewSettings)
	{
		bool bAccount = !_NewSettings.m_RunAsUser.f_IsEmpty();
		if (auto *pParent = mp_Applications.f_FindEqual(_NewSettings.m_ParentApplication); pParent && !(*pParent)->m_Settings.m_RunAsUser.f_IsEmpty())
			bAccount = true;

		if (!bAccount)
			return {};

		auto fInDirectoryTree = [&](CStr const &_UsedApplication)
			{
				if (_UsedApplication == _Application)
					return true;

				auto *pUsedApplication = mp_Applications.f_FindEqual(_UsedApplication);
				return pUsedApplication && (*pUsedApplication)->m_Settings.m_ParentApplication == _Application;
			}
		;

		for (auto &pEnvironment : mp_Environments)
		{
			if (pEnvironment->m_Settings.m_Type != CAppManagerInterface::EEnvironmentType_Local)
				continue;

			// A SelfUpdate.<host platform> agent application can resolve to the self update source, as in fp_CheckLocalEnvironmentAccounts
			CStr AgentApplication = pEnvironment->m_Settings.m_AgentApplication;
			if (auto pAgentApplication = fp_FindAgentApplication(AgentApplication))
				AgentApplication = pAgentApplication->m_Name;

			if (fInDirectoryTree(pEnvironment->m_Settings.m_ParentApplication) || fInDirectoryTree(AgentApplication))
				return fg_Format("Local environment '{}' uses the directory of the application, which therefore cannot run as its own user", pEnvironment->m_Name);
		}

		return {};
	}

	// A local agent runs on the host with the privileges of this AppManager, so its executable, its source and its state must
	// not live in a directory an application account is configured to write. A child application lives in the directory of its root
	// application. A SelfUpdate.<host platform> agent application can resolve to the self update source, which cannot run as its own user.
	// This checks the configuration, not the files: application accounts are trusted like the packages and scripts of their applications,
	// and a Local environment isn't a boundary, so files that a former run as user still owns aren't verified
	CStr CAppManagerActor::fp_CheckLocalEnvironmentAccounts(CEnvironmentSettings const &_Settings)
	{
		if (_Settings.m_Type != CAppManagerInterface::EEnvironmentType_Local)
			return {};

		auto fCheckDirectoryTree = [&](CStr const &_Application, ch8 const *_pUse) -> CStr
			{
				CStr Application = _Application;

				// Child applications are only one level deep
				for (umint iLevel = 0; iLevel < 2 && !Application.f_IsEmpty(); ++iLevel)
				{
					auto *pApplication = mp_Applications.f_FindEqual(Application);
					if (!pApplication)
						break;

					if (!(*pApplication)->m_Settings.m_RunAsUser.f_IsEmpty())
					{
						return fg_Format
							(
								"Local environments cannot {} the directory of application '{}', which runs as its own user, since the agent runs with the "
								"AppManager privileges"
								, _pUse
								, Application
							)
						;
					}

					Application = (*pApplication)->m_Settings.m_ParentApplication;
				}

				return {};
			}
		;

		if (CStr Error = fCheckDirectoryTree(_Settings.m_ParentApplication, "store their data in"); !Error.f_IsEmpty())
			return Error;

		// The self update source that the alias resolves to can be the child of an application with its own user
		CStr AgentApplication = _Settings.m_AgentApplication;
		if (auto pAgentApplication = fp_FindAgentApplication(AgentApplication))
			AgentApplication = pAgentApplication->m_Name;

		return fCheckDirectoryTree(AgentApplication, "run their agent from");
	}

	// Installs the agent files in a directory of their own in the agent directory of the environment, named after their contents, and returns the
	// executable there. A container mounts the agent directory read-only and runs the agent through the Current link, which a rename swaps to a
	// new directory, so a running agent never sees a partial update and the new files take effect when the container restarts. Only the files
	// the agent runs are installed, never other files of the agent application
	TCFuture<CStr> CAppManagerActor::fp_GetEnvironmentAgentExecutable(TCSharedPointer<CEnvironment> _pEnvironment, TCSharedPointer<CStr> _pError)
	{
		if (CStr Error = fp_CheckLocalEnvironmentAccounts(_pEnvironment->m_Settings); !Error.f_IsEmpty())
		{
			*_pError = fg_Move(Error);
			co_return {};
		}

		TCVector<CStr> Candidates;

		if (_pEnvironment->m_Settings.m_AgentApplication.f_IsEmpty())
		{
			if (_pEnvironment->m_Settings.m_Type != CAppManagerInterface::EEnvironmentType_Local)
			{
				*_pError = "Environment '{}' has no agent application configured. Set one with --agent-application."_f << _pEnvironment->m_Name;
				co_return {};
			}

			Candidates.f_Insert(CFile::fs_GetProgramPath());
		}
		else
		{
			auto pAgentApplication = fp_FindEnvironmentAgentApplication(*_pEnvironment);
			if (!pAgentApplication)
			{
				*_pError = "Agent application '{}' for environment '{}' is not installed"_f << _pEnvironment->m_Settings.m_AgentApplication << _pEnvironment->m_Name;
				co_return {};
			}

			CStr Directory = pAgentApplication->f_GetDirectory();

			if (!pAgentApplication->m_Settings.m_Executable.f_IsEmpty())
				Candidates.f_Insert(Directory / pAgentApplication->m_Settings.m_Executable);
			Candidates.f_Insert(Directory / CFile::fs_GetFile(CFile::fs_GetProgramPath()));
#ifdef DPlatformFamily_Windows
			Candidates.f_Insert(Directory / "AppManager.exe");
#else
			Candidates.f_Insert(Directory / "AppManager");
#endif
		}

		bool bContainer = _pEnvironment->m_Settings.m_Type == CAppManagerInterface::EEnvironmentType_Container;

		// The deployment settings next to the agent executable identify the installation as this environment's agent, since
		// environment variables can be inherited by other processes in the environment
		CStr DeploymentSettings;
		{
			CEJsonSorted SettingsJson(EJsonType_Object);
			SettingsJson["EnvironmentAgent"] = true;
			SettingsJson["EnvironmentContainer"] = bContainer;
			SettingsJson["EnvironmentHostID"] = mp_State.m_HostID;
			SettingsJson["UserGroupNameTransform"] = mp_pUniqueUserGroup->m_UserGroupNameTransform;

			if (fp_EnvironmentUsesRemoteStorage(*_pEnvironment))
			{
				if (_pEnvironment->m_Settings.m_Type == CAppManagerInterface::EEnvironmentType_VM)
					SettingsJson["EnvironmentDataRoot"] = mcp_pEnvironmentDataRootVM;
				else
					SettingsJson["EnvironmentDataRoot"] = mcp_pEnvironmentDataRootContainer;
			}

			// The agent keeps everything it writes out of the agent directory: in the environment's data storage in a container, which mounts the agent
			// directory read-only, and beside the agent directory for a Local agent. A VM agent writes its own installation in the guest
			if (bContainer)
				SettingsJson["EnvironmentStateRoot"] = CStr(mcp_pEnvironmentDataRootContainer) / "Agent";
			else if (_pEnvironment->m_Settings.m_Type == CAppManagerInterface::EEnvironmentType_Local)
				SettingsJson["EnvironmentStateRoot"] = fp_GetEnvironmentStorageDirectory(*_pEnvironment) / "State";

			DeploymentSettings = SettingsJson.f_ToString();
		}

		CStr AgentDirectory = fp_GetEnvironmentAgentDirectory(*_pEnvironment);

		CStr Executable;
		{
			auto BlockingActorCheckout = fg_BlockingActor();

			Executable = co_await
				(
					g_Dispatch(BlockingActorCheckout) / [Candidates = fg_Move(Candidates), AgentDirectory, DeploymentSettings, bContainer, _pError]() -> CStr
					{
						CStr Source;
						for (auto &Candidate : Candidates)
						{
							if (CFile::fs_FileExists(Candidate))
							{
								Source = Candidate;
								break;
							}
						}

						if (Source.f_IsEmpty())
							return {};

						CStr SourceDirectory = CFile::fs_GetPath(Source);
						CStr ExecutableName = CFile::fs_GetFile(Source);

						TCVector<CStr> Files;
						for (auto &File : {ExecutableName, CStr("MalterlibHelper"), CFile::fs_GetFileNoExt(ExecutableName) + "VersionInfo.json"})
						{
							if (CFile::fs_FileExists(SourceDirectory / File, EFileAttrib_File))
								Files.f_Insert(File);
						}

						CStr VersionName;
						try
						{
							CStr VersionData = DeploymentSettings;
							for (auto &File : Files)
								VersionData += "\n{}:{}:{}"_f << File << CFile::fs_GetFileSize(SourceDirectory / File) << CFile::fs_GetWriteTime(SourceDirectory / File).f_GetSeconds();

							VersionName = NCryptography::CHash_SHA256::fs_DigestFromData(VersionData.f_GetStr(), VersionData.f_GetLen()).f_GetString().f_Left(32);
						}
						catch (CException const &_Exception)
						{
							*_pError = "Failed to read the agent files in '{}': {}"_f << SourceDirectory << _Exception;
							return {};
						}

						CStr VersionDirectory = AgentDirectory / VersionName;
						CStr CurrentLink = AgentDirectory / "Current";

						// Readable by everyone, since a container runtime can access the mounted files as its own user
						EFileAttrib DirectoryAttributes
							= EFileAttrib_UnixAttributesValid
							| EFileAttrib_UserRead
							| EFileAttrib_UserWrite
							| EFileAttrib_UserExecute
							| EFileAttrib_GroupRead
							| EFileAttrib_GroupExecute
							| EFileAttrib_EveryoneRead
							| EFileAttrib_EveryoneExecute
						;

						CStr PreviousVersion;
						try
						{
							if (!CFile::fs_FileExists(AgentDirectory, EFileAttrib_Directory))
								CFile::fs_CreateDirectory(AgentDirectory);
							CFile::fs_SetAttributes(AgentDirectory, DirectoryAttributes);

							if (!CFile::fs_FileExists(VersionDirectory, EFileAttrib_Directory))
							{
								// Completed under a temporary name, so a version directory is always whole
								CStr NewDirectory = AgentDirectory / (".New." + VersionName);
								if (CFile::fs_FileExists(NewDirectory, EFileAttrib_Directory | EFileAttrib_File | EFileAttrib_Link))
									CFile::fs_DeleteDirectoryRecursive(NewDirectory);

								CFile::fs_CreateDirectory(NewDirectory);
								CFile::fs_SetAttributes(NewDirectory, DirectoryAttributes);

								for (auto &File : Files)
									CFile::fs_CopyFile(SourceDirectory / File, NewDirectory / File);

								CFile::fs_WriteStringToFile(NewDirectory / "AppManagerSettings.json", DeploymentSettings);

								CFile::fs_RenameFile(NewDirectory, VersionDirectory);
							}

							if (bContainer)
							{
								if (CFile::fs_FileExists(CurrentLink, EFileAttrib_Link))
									PreviousVersion = CFile::fs_GetFile(CFile::fs_ResolveSymbolicLink(CurrentLink));

								if (PreviousVersion != VersionName)
								{
									CStr NewLink = AgentDirectory / ".NewCurrent";
									if (CFile::fs_FileExists(NewLink, EFileAttrib_Directory | EFileAttrib_File | EFileAttrib_Link))
										CFile::fs_DeleteFile(NewLink);

									CFile::fs_CreateSymbolicLink(VersionName, NewLink, EFileAttrib_Directory, ESymbolicLinkFlag_Relative);
									CFile::fs_AtomicReplaceFile(NewLink, CurrentLink);
								}
							}
						}
						catch (CException const &_Exception)
						{
							*_pError = "Failed to install the agent files in '{}': {}"_f << AgentDirectory << _Exception;
							return {};
						}

						// The previous version can still run in a container that has not restarted yet
						for (auto &Entry : CFile::fs_FindFiles(AgentDirectory / "*", EFileAttrib_Directory | EFileAttrib_File | EFileAttrib_Link, false, false))
						{
							CStr Name = CFile::fs_GetFile(Entry);
							if (Name == VersionName || Name == PreviousVersion || Entry == CurrentLink)
								continue;

							try
							{
								if (CFile::fs_FileExists(Entry, EFileAttrib_Link) || !CFile::fs_FileExists(Entry, EFileAttrib_Directory))
									CFile::fs_DeleteFile(Entry);
								else
									CFile::fs_DeleteDirectoryRecursive(Entry);
							}
							catch (CException const &_Exception)
							{
								DMibLogWithCategory(Malterlib/Cloud/AppManager, Warning, "Failed to delete the old agent files '{}': {}", Entry, _Exception);
							}
						}

						return VersionDirectory / ExecutableName;
					}
				)
			;
		}

		if (Executable.f_IsEmpty() && _pError->f_IsEmpty())
			*_pError = "Found no agent executable for environment '{}'"_f << _pEnvironment->m_Name;

		co_return Executable;
	}

	TCSharedPointer<CAppManagerActor::CApplication> CAppManagerActor::fp_FindEnvironmentAgentApplication(CEnvironment const &_Environment)
	{
		return fp_FindAgentApplication(_Environment.m_Settings.m_AgentApplication);
	}

	TCSharedPointer<CAppManagerActor::CApplication> CAppManagerActor::fp_FindAgentApplication(CStr const &_AgentApplication)
	{
		// The names SelfUpdate.<Platform> belong to the agent distributions, so an application of such a name whose installed version is of
		// another platform provides no agent, see CApplication::f_AgentDistributionPlatform
		auto *pFindApplication = mp_Applications.f_FindEqual(_AgentApplication);
		if
		(
			pFindApplication
			&& (!_AgentApplication.f_StartsWith("SelfUpdate.") || (*pFindApplication)->m_Settings.m_bSelfUpdateSource || !(*pFindApplication)->f_AgentDistributionPlatform().f_IsEmpty())
		)
		{
			return *pFindApplication;
		}

		// Without a SelfUpdate.<host platform> agent distribution, that name resolves to the self update source. Access to the name suffices: the
		// self update source is the AppManager distribution that environments of the host platform get as their default agent without any
		// application access
		if (_AgentApplication == CStr("SelfUpdate.{}"_f << DMalterlibCloudPlatform))
		{
			for (auto &pApplication : mp_Applications)
			{
				if (pApplication->m_Settings.m_bSelfUpdateSource && !pApplication->m_bDeleted)
					return pApplication;
			}
		}

		return {};
	}

	// A container or VM is a boundary, so the files of its applications live in storage the environment manages, which the host never reads or
	// writes inside, and never in host application directories. A Local environment runs on the host and uses the host directories
	bool CAppManagerActor::fp_EnvironmentUsesRemoteStorage(CEnvironment const &_Environment)
	{
		return _Environment.m_Settings.m_Type != CAppManagerInterface::EEnvironmentType_Local;
	}

	// The guest disk in an image bundle is writable, so one environment uses a bundle
	CStr CAppManagerActor::fp_FindEnvironmentSharingVMImage(CStr const &_Name, CEnvironmentSettings const &_Settings)
	{
		if (_Settings.m_Type != CAppManagerInterface::EEnvironmentType_VM)
			return {};

		for (auto &pEnvironment : mp_Environments)
		{
			if (pEnvironment->m_Name == _Name || pEnvironment->m_Settings.m_Type != CAppManagerInterface::EEnvironmentType_VM)
				continue;

			// Bundle directories can be on a case-insensitive file system
			if (pEnvironment->m_Settings.m_VMImage.f_CmpNoCase(_Settings.m_VMImage) == 0 && pEnvironment->m_Settings.m_ParentApplication == _Settings.m_ParentApplication)
				return pEnvironment->m_Name;
		}

		return {};
	}

	// The agent keeps every application in its own directory of the environment storage, which can be on a
	// case-insensitive file system, so a name differing only in case would share the directory of another application
	CStr CAppManagerActor::fp_FindApplicationSharingEnvironmentStorage(CStr const &_Name, CApplicationSettings const &_Settings)
	{
		auto pEnvironment = fp_LaunchEnvironmentRemoteStorage(_Settings.m_LaunchEnvironment);
		if (!pEnvironment)
			return {};

		// Applications being added stage their files in the storage before they are added
		for (auto *pApplications : {&mp_Applications, &mp_AddingApplicationsInStorage})
		{
			for (auto &pApplication : *pApplications)
			{
				if (pApplication->m_Name != _Name && pApplication->m_Name.f_CmpNoCase(_Name) == 0 && fp_ApplicationRemoteStorageEnvironment(*pApplication) == pEnvironment)
					return pApplication->m_Name;
			}
		}

		return {};
	}

	// The agent keeps every application in environment storage in its own directory, so the files of a child application
	// cannot be inside the directory of its parent there
	CStr CAppManagerActor::fp_CheckParentApplicationStorage(CApplicationSettings const &_Settings)
	{
		if (_Settings.m_ParentApplication.f_IsEmpty())
			return {};

		if (fp_LaunchEnvironmentRemoteStorage(_Settings.m_LaunchEnvironment))
			return "Applications in environment-managed storage cannot have a parent application";

		if (auto *pParent = mp_Applications.f_FindEqual(_Settings.m_ParentApplication); pParent && fp_ApplicationRemoteStorageEnvironment(**pParent))
			return fg_Format("Parent application '{}' keeps its files in environment-managed storage", _Settings.m_ParentApplication);

		return {};
	}

	auto CAppManagerActor::fp_ApplicationRemoteStorageEnvironment(CApplication const &_Application) -> TCSharedPointer<CEnvironment>
	{
		return fp_LaunchEnvironmentRemoteStorage(_Application.m_Settings.m_LaunchEnvironment);
	}

	auto CAppManagerActor::fp_LaunchEnvironmentRemoteStorage(CStr const &_LaunchEnvironment) -> TCSharedPointer<CEnvironment>
	{
		if (_LaunchEnvironment.f_IsEmpty())
			return {};

		auto *pFindEnvironment = mp_Environments.f_FindEqual(_LaunchEnvironment);
		if (!pFindEnvironment)
			return {};

		if (!fp_EnvironmentUsesRemoteStorage(**pFindEnvironment))
			return {};

		return *pFindEnvironment;
	}

	TCFuture<void> CAppManagerActor::fp_RemoveApplicationEnvironmentStorage(TCSharedPointer<CApplication> _pApplication)
	{
		auto pEnvironment = fp_ApplicationRemoteStorageEnvironment(*_pApplication);
		if (!pEnvironment)
			co_return {};

		// Only the agent can delete the storage, which is removed even when no files may have reached it. A manually stopped environment
		// runs only for the removals, and the last one to finish stops it again, also when the stop that marked it was still running when
		// a removal started it
		++pEnvironment->m_nStorageRemovals;
		auto StorageRemoval = g_OnScopeExit / [pEnvironment]
			{
				--pEnvironment->m_nStorageRemovals;
			}
		;

		auto StartResult = co_await fp_EnsureEnvironmentStarted(pEnvironment, EEnvironmentStart::mc_EvenIfStoppedManually).f_Wrap();

		// The agent can disconnect between the start and this call
		TCAsyncResult<void> Result;
		if (!StartResult)
			Result.f_SetException(StartResult.f_GetException());
		else if (pEnvironment->m_AgentInterface)
		{
			Result = co_await pEnvironment->m_AgentInterface.f_CallActor(&CAppManagerEnvironmentInterface::f_RemoveApplicationStorage)
				(fg_TempCopy(_pApplication->m_Name))
				.f_Timeout(600.0, "Timed out removing the application storage in the environment")
				.f_Wrap()
			;
		}
		else
			Result.f_SetException(DMibErrorInstance("The agent of environment '{}' disconnected"_f << pEnvironment->m_Name));

		StorageRemoval.f_Clear();
		--pEnvironment->m_nStorageRemovals;

		// The window session hosts the virtual machine of an environment that is stopped manually
		auto fLastRemovalInStoppedEnvironment = [&]
			{
				return pEnvironment->m_nStorageRemovals == 0
					&& pEnvironment->m_bStoppedManually
					&& !pEnvironment->m_bDeleted
					&& !pEnvironment->m_bStopping
					&& !pEnvironment->m_bWindowOpen
					&& (pEnvironment->f_IsStarted() || pEnvironment->m_bStarting)
				;
			}
		;

		if (fLastRemovalInStoppedEnvironment())
		{
			// The start configured the agent, which can be installing OS packages
			if (pEnvironment->m_nPendingAgentConfigurations)
			{
				co_await pEnvironment->m_OnAgentConfigured.f_Insert().f_Future().f_Wrap()
					> fg_LogError("Malterlib/Cloud/AppManager", "The environment agent configuration failed before the stop after removing the application storage")
				;
			}

			// Another removal can have started while the configuration finished
			if (fLastRemovalInStoppedEnvironment())
			{
				co_await fp_StopEnvironmentInternal(pEnvironment).f_Wrap()
					> fg_LogError("Malterlib/Cloud/AppManager", "Failed to stop the environment after removing the application storage")
				;
			}
		}

		if (!Result)
			co_return Result.f_GetException();

		co_return {};
	}

	TCSharedPointer<CAppManagerActor::CApplication> CAppManagerActor::fp_GetEnvironmentParentApplication(CEnvironment const &_Environment)
	{
		if (_Environment.m_Settings.m_ParentApplication.f_IsEmpty())
			return {};

		auto *pFindApplication = mp_Applications.f_FindEqual(_Environment.m_Settings.m_ParentApplication);
		if (!pFindApplication)
			return {};

		return *pFindApplication;
	}

	CStr CAppManagerActor::fp_GetEnvironmentStorageDirectory(CEnvironment const &_Environment)
	{
		if (auto pParentApplication = fp_GetEnvironmentParentApplication(_Environment))
			return fg_Format("{}/{}/{}", pParentApplication->f_GetDirectory(), mcp_pEnvironmentStorageDirectory, _Environment.m_Name);

		return fg_Format("{}/{}/{}", mp_State.m_RootDirectory, mcp_pEnvironmentStorageDirectory, _Environment.m_Name);
	}

	// Written only by the host, and mounted read-only in containers. See fp_GetEnvironmentAgentExecutable
	CStr CAppManagerActor::fp_GetEnvironmentAgentDirectory(CEnvironment const &_Environment)
	{
		return fp_GetEnvironmentStorageDirectory(_Environment) / "Agent";
	}

	// The application data of a container on the host, which only the environment writes inside. See fp_EnsureEnvironmentDataVolume
	CStr CAppManagerActor::fp_GetEnvironmentDataDirectory(CEnvironment const &_Environment)
	{
		return fp_GetEnvironmentStorageDirectory(_Environment) / "Data";
	}

	// Deletes the host storage of a stopped environment: its agent files, the staging of its application updates and its application data. The
	// data directory of a Colima environment stays a writable share of the Colima VM, which other environments keep running, so it is deleted
	// beneath the storage directory without following anything placed in it. Only the host writes the rest, which the recursive deletion removes
	// without following links
	TCFuture<void> CAppManagerActor::fp_DeleteEnvironmentStorage(TCSharedPointer<CEnvironment> _pEnvironment)
	{
		CStr StorageDirectory = fp_GetEnvironmentStorageDirectory(*_pEnvironment);

		auto BlockingActorCheckout = fg_BlockingActor();

		co_await
			(
				(
					g_Dispatch(BlockingActorCheckout) / [StorageDirectory]()
					{
#ifndef DPlatformFamily_Windows
						if (CFile::fs_FileExists(StorageDirectory, EFileAttrib_Directory))
							CFileBeneath::fs_DeleteRecursive(StorageDirectory, "Data");
#endif
						if (CFile::fs_FileExists(StorageDirectory, EFileAttrib_Directory | EFileAttrib_File | EFileAttrib_Link))
							CFile::fs_DeleteDirectoryRecursive(StorageDirectory);
					}
				)
				% "Failed to delete the environment storage, so the application data is kept"
			)
		;

		co_return {};
	}

	bool CAppManagerActor::fp_EnvironmentStorageReady(CEnvironment const &_Environment, CStr &o_Error, CAppManagerInterface::EStatusSeverity &o_Severity)
	{
		if (_Environment.m_Settings.m_ParentApplication.f_IsEmpty())
			return true;

		auto pParentApplication = fp_GetEnvironmentParentApplication(_Environment);
		if (!pParentApplication)
		{
			o_Error = fg_Format("Parent application '{}' does not exist", _Environment.m_Settings.m_ParentApplication);
			o_Severity = CAppManagerInterface::EStatusSeverity_Error;
			return false;
		}

		if (pParentApplication->f_NeedsEncryption() && (!pParentApplication->f_EncryptionOpened() || pParentApplication->f_EncryptionClosing()))
		{
			o_Error = "Parent application encryption not yet opened";
			o_Severity = CAppManagerInterface::EStatusSeverity_Warning;
			return false;
		}

		return true;
	}

	bool fg_AppManager_PathsOverlap(CStr const &_Left, CStr const &_Right)
	{
		return _Left == _Right || _Left.f_StartsWith(_Right + "/") || _Right.f_StartsWith(_Left + "/");
	}

	// A container started while the encryption of an application is closed binds the bare mount point and writes to the unencrypted file system
	// below it, so a mount of the storage of such an application keeps the environment from starting. The mounts of the last launch count too,
	// since a stopped container restarts with them
	bool CAppManagerActor::fp_EnvironmentMountsReady(CEnvironment const &_Environment, CStr &o_Error, CAppManagerInterface::EStatusSeverity &o_Severity)
	{
		if (_Environment.m_Settings.m_Type != CAppManagerInterface::EEnvironmentType_Container)
			return true;

		for (auto &pApplication : mp_Applications)
		{
			auto &Application = *pApplication;
			if (!Application.f_NeedsEncryption() || (Application.f_EncryptionOpened() && !Application.f_EncryptionClosing()))
				continue;

			CStr Directory = Application.f_GetDirectory();
			CStr const *pMount = nullptr;

			for (auto &Mount : _Environment.m_Settings.m_ContainerExtraMounts)
			{
				auto &HostPath = _Environment.m_Settings.m_ContainerExtraMounts.fs_GetKey(Mount);
				if (fg_AppManager_PathsOverlap(HostPath, Directory))
					pMount = &HostPath;
			}

			for (auto &Mount : _Environment.m_ContainerMounts)
			{
				if (fg_AppManager_PathsOverlap(Mount, Directory))
					pMount = &Mount;
			}

			if (pMount)
			{
				o_Error = "Mount '{}' is in the storage of application '{}', whose encryption is not open"_f << *pMount << Application.m_Name;
				o_Severity = CAppManagerInterface::EStatusSeverity_Warning;
				return false;
			}
		}

		return true;
	}

	// Starts the auto start environments whose storage or mounts became ready when an encryption opened. The AppManager startup starts every auto
	// start environment, and the others are ready from the start, so only environments with a parent application or extra mounts are handled here
	void CAppManagerActor::fp_AutoStartEnvironments()
	{
		if (mp_bEnvironmentAgent)
			return;

		for (auto &pEnvironment : mp_Environments)
		{
			auto &Environment = *pEnvironment;
			if (!Environment.m_Settings.m_bAutoStart || Environment.m_bStoppedManually)
				continue;

			if (Environment.m_Settings.m_ParentApplication.f_IsEmpty() && Environment.m_Settings.m_ContainerExtraMounts.f_IsEmpty())
				continue;

			if (Environment.f_IsStarted() || Environment.m_bStarting || Environment.m_bStopping)
				continue;

			CStr Error;
			CAppManagerInterface::EStatusSeverity Severity;
			if (!fp_EnvironmentStorageReady(Environment, Error, Severity) || !fp_EnvironmentMountsReady(Environment, Error, Severity))
			{
				if (Environment.m_Status != Error)
					Environment.f_SetStatus(Error, Severity);
				continue;
			}

			fp_EnsureEnvironmentStarted(pEnvironment) > fg_LogError("Malterlib/Cloud/AppManager", "Failed to auto-start environment");
		}
	}

	// Assigning an application to an environment runs it there, so application permissions alone do not suffice
	TCFuture<void> CAppManagerActor::fp_CheckEnvironmentAccess(CStr _Name, CCallingHostInfo _CallingHostInfo)
	{
		auto Auditor = f_Auditor({}, _CallingHostInfo);

		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;
		Permissions["Environment"] =
			{
				CPermissionQuery{"AppManager/EnvironmentAll", fg_Format("AppManager/Environment/{}", _Name)}.f_Description("Access environment {} in AppManager"_f << _Name)
			}
		;

		NContainer::TCMap<NStr::CStr, bool> HasPermissions = co_await
			(
				mp_Permissions.f_HasPermissions("Launch application in environment in AppManager", Permissions, _CallingHostInfo) % "Permission denied accessing environment" % Auditor
			)
		;

		if (!HasPermissions["Environment"])
			co_return Auditor.f_AccessDenied("(Application launch environment, environment name)", Permissions["Environment"]);

		co_return {};
	}

	// Host settings that restart environments to apply them need access to each environment they restart
	TCFuture<void> CAppManagerActor::fp_CheckEnvironmentRestartAccess(TCVector<CStr> _Names, CStr _Description, CCallingHostInfo _CallingHostInfo)
	{
		if (_Names.f_IsEmpty())
			co_return {};

		auto Auditor = f_Auditor({}, _CallingHostInfo);

		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;
		for (auto &Name : _Names)
		{
			Permissions[Name] =
				{
					CPermissionQuery{"AppManager/EnvironmentAll", fg_Format("AppManager/Environment/{}", Name)}.f_Description("Access environment {} in AppManager"_f << Name)
				}
			;
		}

		NContainer::TCMap<NStr::CStr, bool> HasPermissions = co_await
			(
				mp_Permissions.f_HasPermissions(_Description, Permissions, _CallingHostInfo) % "Permission denied accessing environment" % Auditor
			)
		;

		for (auto &Name : _Names)
		{
			if (!HasPermissions[Name])
				co_return Auditor.f_AccessDenied("(Restart environment to apply settings, environment name)", Permissions[Name]);
		}

		co_return {};
	}

	// The work of a command can go on after its client disconnects, which fails the output. Returns whether the output still reaches the client;
	// once it doesn't, _bConnected is false and the output goes to the log instead
	TCFuture<bool> CAppManagerActor::fp_CommandLineProgress(NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine, bool _bConnected, CStr _Output, bool _bError)
	{
		if (_bConnected)
		{
			auto Result = co_await (_bError ? _pCommandLine->f_StdErr(_Output) : _pCommandLine->f_StdOut(_Output)).f_Wrap();
			if (Result)
				co_return true;

			DMibLogWithCategory(Malterlib/Cloud/AppManager, Warning, "Stopped reporting to the command line: {}", Result.f_GetExceptionStr());
		}

		if (_bError)
			DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "{}", _Output.f_Trim());
		else
			DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "{}", _Output.f_Trim());

		co_return false;
	}

	// Stops all of _Environments before it starts any, since the first start applies the settings that the environments share. A failure is reported
	// and the others continue; returns the exit status. The client can disconnect during the stops, which can take long, so a failed output doesn't
	// stop the restarts
	auto CAppManagerActor::fp_RestartEnvironmentsForSettings
		(
			TCVector<TCSharedPointer<CEnvironment>> _Environments
			, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine
			, bool _bConnected
		)
		-> TCFuture<uint32>
	{
		uint32 Status = 0;
		bool bConnected = _bConnected;
		TCVector<TCSharedPointer<CEnvironment>> Stopped;

		// Launches that were refused while the environments stopped wait for this
		auto UpdateDependencies = g_OnScopeExit / [this, bEnvironments = !_Environments.f_IsEmpty()]
			{
				if (bEnvironments && !mp_bStoppingApp)
					fp_UpdateApplicationDependencies();
			}
		;

		for (auto &pEnvironment : _Environments)
		{
			bConnected = co_await fp_CommandLineProgress(_pCommandLine, bConnected, "Stopping environment '{}'\n"_f << pEnvironment->m_Name, false);

			auto StopResult = co_await fp_WaitForEnvironmentIdle(pEnvironment).f_Wrap();
			if (StopResult)
				StopResult = co_await fp_StopEnvironmentInternal(pEnvironment).f_Wrap();

			if (!StopResult)
			{
				bConnected = co_await fp_CommandLineProgress
					(
						_pCommandLine
						, bConnected
						, "Failed to stop environment '{}': {}\n"_f << pEnvironment->m_Name << StopResult.f_GetExceptionStr()
						, true
					)
				;
				Status = 1;
				continue;
			}

			Stopped.f_Insert(pEnvironment);
		}

		for (auto &pEnvironment : Stopped)
		{
			// The stops suspend, so a manual stop or a removal can supersede this restart
			if (pEnvironment->m_bDeleted || pEnvironment->m_bStoppedManually)
				continue;

			bConnected = co_await fp_CommandLineProgress(_pCommandLine, bConnected, "Starting environment '{}'\n"_f << pEnvironment->m_Name, false);

			auto StartResult = co_await fp_EnsureEnvironmentStarted(pEnvironment).f_Wrap();
			if (!StartResult)
			{
				bConnected = co_await fp_CommandLineProgress
					(
						_pCommandLine
						, bConnected
						, "Failed to start environment '{}': {}\n"_f << pEnvironment->m_Name << StartResult.f_GetExceptionStr()
						, true
					)
				;
				Status = 1;
			}
		}

		co_return Status;
	}

	CStr CAppManagerActor::fsp_GetEnvironmentGuestPlatform(CAppManagerInterface::EEnvironmentType _Type)
	{
		CStr HostPlatform = DMalterlibCloudPlatform;
		CStr Architecture = HostPlatform.f_Extract(HostPlatform.f_FindReverse("-") + 1);

		if (_Type == CAppManagerInterface::EEnvironmentType_Container)
			return "Linux-{}"_f << Architecture;
		else if (_Type == CAppManagerInterface::EEnvironmentType_VM)
			return "macOS-{}"_f << Architecture;

		return HostPlatform;
	}

	bool CAppManagerActor::fp_EnvironmentAcceptsApplications(CStr const &_Name)
	{
		auto *pFindEnvironment = mp_Environments.f_FindEqual(_Name);
		return pFindEnvironment && !(*pFindEnvironment)->m_bRemoving;
	}

	// VM guests reach the socket through a virtio socket that the host forwards to it
	bool CAppManagerActor::fp_EnvironmentUsesAgentSocket(TCSharedPointer<CEnvironment> const &_pEnvironment)
	{
		if (_pEnvironment->m_Settings.m_Type == CAppManagerInterface::EEnvironmentType_VM)
			return true;

		return _pEnvironment->m_Settings.m_Type == CAppManagerInterface::EEnvironmentType_Container
			&& fp_GetContainerRuntimeExecutable(_pEnvironment) != "container"
			&& !fp_UseOwnColimaSystem(_pEnvironment)
		;
	}

	CAppManagerEnvironmentSocketDirectory CAppManagerActor::fp_GetEnvironmentAgentSocketDirectory(CEnvironment const &_Environment)
	{
		return fg_AppManager_EnvironmentSocketDirectory
			(
				mp_State.m_RootDirectory
				, _Environment.m_Name
				, _Environment.m_Settings.m_Type == CAppManagerInterface::EEnvironmentType_Container
				, CProcessLaunch::fs_GetElevation() == NProcess::EProcessElevation_IsRoot
			)
		;
	}

	CStr CAppManagerActor::fp_GetEnvironmentAgentSocketPath(CEnvironment const &_Environment)
	{
		return fp_GetEnvironmentAgentSocketDirectory(_Environment).m_SocketPath;
	}

	TCFuture<NWeb::NHTTP::CURL> CAppManagerActor::fp_EnsureEnvironmentListen(TCSharedPointer<CEnvironment> _pEnvironment)
	{
		bool bAppleContainer = _pEnvironment->m_Settings.m_Type == CAppManagerInterface::EEnvironmentType_Container && fp_GetContainerRuntimeExecutable(_pEnvironment) == "container";
		bool bColima = _pEnvironment->m_Settings.m_Type == CAppManagerInterface::EEnvironmentType_Container && fp_UseOwnColimaSystem(_pEnvironment);

		if (fp_EnvironmentUsesAgentSocket(_pEnvironment))
		{
#ifndef DPlatformFamily_Linux
			// Unix domain sockets cannot cross the virtual machine boundary of docker on other hosts
			if (_pEnvironment->m_Settings.m_Type != CAppManagerInterface::EEnvironmentType_VM)
				co_return DMibErrorInstance("Docker environments need a Linux host. Use the AppleContainer or Colima container runtime.");
#endif
			// A docker container only sees its own mounts, so its agent connects through a listen in a directory that
			// the container mounts. A VM guest, which can't reach the host over its network, reaches it through a virtio socket
			{
				auto BlockingActorCheckout = fg_BlockingActor();

				co_await
					(
						g_Dispatch(BlockingActorCheckout)
						/ [SocketDirectory = fp_GetEnvironmentAgentSocketDirectory(*_pEnvironment)]()
						{
							fg_AppManager_PrepareEnvironmentSocketDirectory(SocketDirectory);
						}
						% "Failed to prepare the environment agent socket directory"
					)
				;
			}

			// The root of a docker container can write the mounted directory, so the listen applies the mode of its staged socket without
			// following links
			NWeb::NHTTP::CURL ListenUrl;
			if (!ListenUrl.f_Decode("wss://[UNIX(666):{}]/"_f << fp_GetEnvironmentAgentSocketPath(*_pEnvironment)))
				co_return DMibErrorInstance("Failed to create the environment agent socket address");

			CDistributedActorTrustManager_Address Address;
			Address.m_URL = ListenUrl;

			if (!co_await mp_State.m_TrustManager(&CDistributedActorTrustManager::f_HasListen, Address))
			{
				co_await (mp_State.m_TrustManager(&CDistributedActorTrustManager::f_AddListen, Address, 0) % "Failed to add the environment agent listen");

				DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Added listen '{}' for environment '{}'", ListenUrl.f_Encode(), _pEnvironment->m_Name);
			}

			co_return ListenUrl;
		}

		if (!bAppleContainer && !bColima)
			co_return mp_State.m_LocalAddress;

		// These containers run in a VM that can't reach a host Unix domain socket, so they connect to a listen address on the host side of its network
		CStr HostAddress;
		if (bColima)
		{
			// The lima user-mode network places the host at a fixed address, published
			// to guests as host.lima.internal; containers reach it through the docker
			// bridge and the virtual machine's outbound network
			HostAddress = "192.168.5.2";
		}
		else
		{
			CStr Network = _pEnvironment->m_Settings.m_ContainerNetwork;
			if (Network.f_IsEmpty())
				Network = "default";

			CProcessLaunchActor::CSimpleLaunch SimpleLaunch
				(
					fp_BuildContainerCommandParams(_pEnvironment, fg_CreateVector<CStr>("network", "inspect", Network))
					, CProcessLaunchActor::ESimpleLaunchFlag_GenerateExceptionOnNonZeroExitCode
				)
			;
			SimpleLaunch.m_LogName = "AppleContainer/NetworkInspect";

			auto Result = co_await CProcessLaunchActor::fs_LaunchSimple(SimpleLaunch).f_Wrap();
			if (!Result)
				co_return DMibErrorInstance("Failed to inspect container network '{}': {}"_f << Network << Result.f_GetExceptionStr());

			{
				auto CaptureScope = co_await (g_CaptureExceptions % "Failed to parse the container network inspect output");

				CJsonOrdered Json = CJsonOrdered::fs_FromString(Result->f_GetStdOut());
				if (Json.f_IsArray())
				{
					for (auto &Item : Json.f_Array())
					{
						if (auto *pStatus = Item.f_GetMember("status"))
						{
							if (auto *pGateway = pStatus->f_GetMember("ipv4Gateway"))
								HostAddress = pGateway->f_AsString();
						}

						break;
					}
				}
			}

			if (HostAddress.f_IsEmpty())
				co_return DMibErrorInstance("Found no gateway address for container network '{}'"_f << Network);
		}

		for (aint Attempt = 0;; ++Attempt)
		{
			bool bNewPort = false;
			if (!_pEnvironment->m_ListenPort)
			{
				bNewPort = true;

				CStr Random = fg_RandomID();
				uint32 Seed = 0;
				ch8 const *pRandom = Random.f_GetStr();
				for (umint iChar = 0; iChar < Random.f_GetLen(); ++iChar)
					Seed = Seed * 31 + uint32(uint8(pRandom[iChar]));

				_pEnvironment->m_ListenPort = 20000 + Seed % 40000;
			}

			// The listen binds the wildcard address: the host side of the shared network
			// only has its address while the network is active, so binding it directly
			// fails when the trust manager restores the persisted listen at startup. The
			// guests are given the shared network host address to connect to
			NWeb::NHTTP::CURL ListenUrl;
			if (!ListenUrl.f_Decode("wss://0.0.0.0:{}/"_f << _pEnvironment->m_ListenPort))
				co_return DMibErrorInstance("Failed to create the environment listen address");

			NWeb::NHTTP::CURL AgentUrl;
			if (!AgentUrl.f_Decode("wss://{}:{}/"_f << HostAddress << _pEnvironment->m_ListenPort))
				co_return DMibErrorInstance("Failed to create the environment agent address for host '{}'"_f << HostAddress);

			// Environments that use the same port share its listens, which the removal of the last one removes, so the listens of a new port
			// belong to another service unless another environment uses the port
			bool bEnvironmentPort = !bNewPort;
			for (auto &pEnvironment : mp_Environments)
			{
				if (pEnvironment != _pEnvironment && pEnvironment->m_ListenPort == _pEnvironment->m_ListenPort)
					bEnvironmentPort = true;
			}

			CDistributedActorTrustManager_Address DirectAddress;
			DirectAddress.m_URL = AgentUrl;

			CDistributedActorTrustManager_Address Address;
			Address.m_URL = ListenUrl;

			bool bHasDirectListen = co_await mp_State.m_TrustManager(&CDistributedActorTrustManager::f_HasListen, DirectAddress);
			bool bHasListen = co_await mp_State.m_TrustManager(&CDistributedActorTrustManager::f_HasListen, Address);

			if (!bEnvironmentPort && (bHasDirectListen || bHasListen))
			{
				if (Attempt == 2)
					co_return DMibErrorInstance("Failed to find a free port for the environment listen: '{}' is used by another listen"_f << ListenUrl.f_Encode());

				_pEnvironment->m_ListenPort = 0;
				continue;
			}

			// Remove a persisted direct listen so startup restores only the wildcard listen
			if (bHasDirectListen)
				co_await mp_State.m_TrustManager(&CDistributedActorTrustManager::f_RemoveListen, DirectAddress);

			if (bHasListen)
				co_return AgentUrl;

			auto AddResult = co_await mp_State.m_TrustManager(&CDistributedActorTrustManager::f_AddListen, Address, 0).f_Wrap();
			if (AddResult)
			{
				DMibLogWithCategory
					(
						Malterlib/Cloud/AppManager
						, Info
						, "Added listen '{}' for environment '{}' reached through '{}'"
						, ListenUrl.f_Encode()
						, _pEnvironment->m_Name
						, AgentUrl.f_Encode()
					)
				;

				co_await fp_UpdateEnvironmentJson(_pEnvironment).f_Wrap() > fg_LogError("Malterlib/Cloud/AppManager", "Failed to save environment listen state");

				co_return AgentUrl;
			}

			if (Attempt == 2)
				co_return DMibErrorInstance("Failed to add environment listen '{}': {}"_f << ListenUrl.f_Encode() << AddResult.f_GetExceptionStr());

			// The chosen port can collide with another service; retry with a new one
			_pEnvironment->m_ListenPort = 0;
		}
	}

	TCFuture<void> CAppManagerActor::fp_RemoveEnvironmentListen(TCSharedPointer<CEnvironment> _pEnvironment)
	{
		TCVector<CStr> ListenUrls;

		// Environments using the same port share one wildcard listen
		if (_pEnvironment->m_ListenPort)
		{
			bool bShared = false;
			for (auto &pEnvironment : mp_Environments)
			{
				if (pEnvironment != _pEnvironment && pEnvironment->m_ListenPort == _pEnvironment->m_ListenPort)
					bShared = true;
			}

			if (!bShared)
				ListenUrls.f_Insert("wss://0.0.0.0:{}/"_f << _pEnvironment->m_ListenPort);
		}

		if (fp_EnvironmentUsesAgentSocket(_pEnvironment))
			ListenUrls.f_Insert("wss://[UNIX(666):{}]/"_f << fp_GetEnvironmentAgentSocketPath(*_pEnvironment));

		for (auto &ListenUrl : ListenUrls)
		{
			CDistributedActorTrustManager_Address Address;
			if (!Address.m_URL.f_Decode(ListenUrl))
				co_return DMibErrorInstance("Failed to decode the environment listen address '{}'"_f << ListenUrl);

			if (!co_await mp_State.m_TrustManager(&CDistributedActorTrustManager::f_HasListen, Address))
				continue;

			co_await (mp_State.m_TrustManager(&CDistributedActorTrustManager::f_RemoveListen, Address) % "Failed to remove the environment listen");

			DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Removed listen '{}' of environment '{}'", ListenUrl, _pEnvironment->m_Name);
		}

		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_EnsureEnvironmentStarted(TCSharedPointer<CEnvironment> _pEnvironment, EEnvironmentStart _Start)
	{
		auto CheckDestroy = co_await f_CheckDestroyedOnResume();

		// A stop keeps the started state until it has stopped the applications
		if (_pEnvironment->f_IsStarted() && !_pEnvironment->m_bStopping)
			co_return {};

		if (_pEnvironment->m_bStarting)
		{
			co_await _pEnvironment->m_OnAgentConnected.f_Insert().f_Future();
			co_return {};
		}

		co_return co_await fp_StartEnvironmentInternal(_pEnvironment, _Start);
	}

	TCFuture<void> CAppManagerActor::fp_StartEnvironmentInternal(TCSharedPointer<CEnvironment> _pEnvironment, EEnvironmentStart _Start)
	{
		auto CheckDestroy = co_await f_CheckDestroyedOnResume();

		for (;;)
		{
			if (_pEnvironment->m_bDeleted)
				co_return DMibErrorInstance("Environment has been deleted");

			if (_pEnvironment->m_bRemoving)
				co_return DMibErrorInstance("Environment is being removed");

			if (_pEnvironment->m_bReplacingContainerAfterPull)
				co_return DMibErrorInstance("Environment is being pulled");

			// The shutdown stops the environments once, so a start after it would outlive the AppManager
			if (mp_bStoppingApp)
				co_return DMibErrorInstance("The AppManager is stopping");

			if (_pEnvironment->f_IsStarted() && !_pEnvironment->m_bStopping)
				co_return {};

			if (_pEnvironment->m_bStarting)
			{
				co_await _pEnvironment->m_OnAgentConnected.f_Insert().f_Future();
				co_return {};
			}

			if (!_pEnvironment->m_bStopping)
				break;

			// A stop can still be tearing down the previous container or virtual
			// machine, which holds the shared machine state like the guest image
			// locks; starting on top of it would race that teardown
			(void)co_await _pEnvironment->m_OnStopFinished.f_Insert().f_Future().f_Wrap();
		}

		// A manual stop wins over a start that passed its caller's check before the stop
		if (_Start == EEnvironmentStart::mc_UnlessStoppedManually && _pEnvironment->m_bStoppedManually)
			co_return DMibErrorInstance("Environment '{}' was stopped. Start it with --environment-start."_f << _pEnvironment->m_Name);

		if (_pEnvironment->m_bWindowOpen)
			co_return DMibErrorInstance("Environment '{}' is open in a window"_f << _pEnvironment->m_Name);

		{
			CStr Error;
			CAppManagerInterface::EStatusSeverity Severity;
			if (!fp_EnvironmentStorageReady(*_pEnvironment, Error, Severity) || !fp_EnvironmentMountsReady(*_pEnvironment, Error, Severity))
			{
				_pEnvironment->f_SetStatus(Error, Severity);
				co_return DMibErrorInstance("Cannot start environment '{}': {}"_f << _pEnvironment->m_Name << Error);
			}
		}

		bool bContainer = _pEnvironment->m_Settings.m_Type == CAppManagerInterface::EEnvironmentType_Container;
		bool bVM = _pEnvironment->m_Settings.m_Type == CAppManagerInterface::EEnvironmentType_VM;

		// Set before any further suspension, so a concurrent start waits instead of launching the environment again
		_pEnvironment->m_bStarting = true;
		_pEnvironment->m_bStopping = false;

		bool bConnected = false;
		auto Cleanup = g_OnScopeExit / [&, _pEnvironment]
			{
				_pEnvironment->m_bStarting = false;
				_pEnvironment->m_bStartAbortRequested = false;
				_pEnvironment->m_bStartEnsuredContainerSystem = false;
				auto OnAgentConnected = fg_Move(_pEnvironment->m_OnAgentConnected);
				for (auto &Promise : OnAgentConnected)
				{
					if (bConnected)
						Promise.f_SetResult();
					else
						Promise.f_SetException(DMibErrorInstance("Environment '{}' failed to start"_f << _pEnvironment->m_Name));
				}
			}
		;

		if (bVM)
		{
			co_await fp_StartEnvironmentVM(_pEnvironment);

			bConnected = true;

			co_return {};
		}

		// A stop, a removal or the shutdown asks the start to give up, which it checks after each step that suspends, so an image pull or the
		// wait for the agent doesn't hold them up
		auto fCheckAbort = [&]() -> CExceptionPointer
			{
				if (_pEnvironment->m_bDeleted)
					return DMibErrorInstance("Environment has been deleted").f_ExceptionPointer();

				if (_pEnvironment->m_bRemoving || _pEnvironment->m_bStartAbortRequested || mp_bStoppingApp)
					return DMibErrorInstance("The start of environment '{}' was aborted"_f << _pEnvironment->m_Name).f_ExceptionPointer();

				return nullptr;
			}
		;

		_pEnvironment->f_SetStatus("Starting", CAppManagerInterface::EStatusSeverity_Warning);

		TCSharedPointer<CStr> pError = fg_Construct();
		CStr AgentExecutable = co_await fp_GetEnvironmentAgentExecutable(_pEnvironment, pError);
		if (auto pException = fCheckAbort())
			co_return pException;

		if (AgentExecutable.f_IsEmpty())
		{
			_pEnvironment->f_SetStatus(*pError, CAppManagerInterface::EStatusSeverity_Error);
			co_return DMibErrorInstance(*pError);
		}

		// A Local agent writes its state beside the agent directory, see the deployment settings in fp_GetEnvironmentAgentExecutable. The runtime
		// client of a container runs in the environment storage
		CStr AgentWorkingDirectory = bContainer ? fp_GetEnvironmentStorageDirectory(*_pEnvironment) : fp_GetEnvironmentStorageDirectory(*_pEnvironment) / "State";

		{
			auto BlockingActorCheckout = fg_BlockingActor();

			co_await
				(
					(
						g_Dispatch(BlockingActorCheckout) / [AgentWorkingDirectory]()
						{
							CFile::fs_CreateDirectory(AgentWorkingDirectory);
						}
					)
					% "Failed to create environment agent working directory"
				)
			;
		}

		if (auto pException = fCheckAbort())
			co_return pException;

		_pEnvironment->m_LaunchID = fg_RandomID();

		TCPromiseFuturePair<void> LaunchedPromise;

		CStr LaunchExecutable = AgentExecutable;
		TCVector<CStr> LaunchParameters = {"--daemon-run-standalone", "--log-to-stderr"};

		NWeb::NHTTP::CURL AgentAddress = mp_State.m_LocalAddress;

		if (bContainer)
		{
			{
				auto Result = co_await fp_EnsureContainerSystem(_pEnvironment).f_Wrap();
				if (!Result)
				{
					_pEnvironment->f_SetStatus(Result.f_GetExceptionStr(), CAppManagerInterface::EStatusSeverity_Error);
					co_return Result.f_GetException();
				}

				_pEnvironment->m_bStartEnsuredContainerSystem = true;
			}

			if (auto pException = fCheckAbort())
				co_return pException;

			// With Colima the backing image must be loop mounted in its virtual machine before the container bind mounts it
			{
				auto Result = co_await fp_EnsureEnvironmentDataVolume(_pEnvironment).f_Wrap();
				if (!Result)
				{
					_pEnvironment->f_SetStatus(Result.f_GetExceptionStr(), CAppManagerInterface::EStatusSeverity_Error);
					co_return Result.f_GetException();
				}
			}

			if (auto pException = fCheckAbort())
				co_return pException;

			{
				auto Result = co_await fp_EnsureEnvironmentListen(_pEnvironment).f_Wrap();
				if (!Result)
				{
					_pEnvironment->f_SetStatus(Result.f_GetExceptionStr(), CAppManagerInterface::EStatusSeverity_Error);
					co_return Result.f_GetException();
				}

				AgentAddress = *Result;
			}

			if (auto pException = fCheckAbort())
				co_return pException;

			LaunchExecutable = fp_GetContainerRuntimeExecutable(_pEnvironment);
			CAppManagerContainerLaunch ContainerLaunch = fp_BuildEnvironmentContainerLaunch(_pEnvironment, CFile::fs_GetFile(AgentExecutable));
			TCVector<CStr> RunArguments = fg_AppManager_BuildContainerRunArguments(ContainerLaunch);

			// A reused container keeps its creation-time process environment, so a changed agent address or host ID recreates it
			CStr FingerprintData = LaunchExecutable;
			for (auto &Argument : RunArguments)
				FingerprintData += "\n" + Argument;
			FingerprintData += "\n" + AgentAddress.f_Encode();
			FingerprintData += "\n" + mp_State.m_HostID;
			CStr Fingerprint = NCryptography::CHash_SHA256::fs_DigestFromData(FingerprintData.f_GetStr(), FingerprintData.f_GetLen()).f_GetString();

			bool bReuse = Fingerprint == _pEnvironment->m_ContainerFingerprint && !_pEnvironment->m_ContainerLaunchID.f_IsEmpty() && co_await fp_EnvironmentContainerExists(_pEnvironment);

			if (auto pException = fCheckAbort())
				co_return pException;

			if (bReuse)
			{
				// Restart a reused container attached: the ticket handshake needs its standard streams
				auto StopResult = co_await fp_StopEnvironmentContainer(_pEnvironment).f_Wrap();
				if (!StopResult)
				{
					_pEnvironment->f_SetStatus("Failed to stop the container before starting it: {}"_f << StopResult.f_GetExceptionStr(), CAppManagerInterface::EStatusSeverity_Error);
					co_return StopResult.f_GetException();
				}

				LaunchParameters = fg_CreateVector<CStr>("start", "--attach", "--interactive", fp_GetContainerName(_pEnvironment));
			}
			else
			{
				// Recreation resets the container filesystem to the image, so prefer the newest image. The removal goes by name, since
				// state from an earlier AppManager can lack the fingerprint of an existing container
				auto RemoveResult = co_await fp_RemoveEnvironmentContainerByName(_pEnvironment).f_Wrap();
				if (!RemoveResult)
				{
					_pEnvironment->f_SetStatus("Failed to remove the previous container: {}"_f << RemoveResult.f_GetExceptionStr(), CAppManagerInterface::EStatusSeverity_Error);
					co_return RemoveResult.f_GetException();
				}

				co_await fp_PullEnvironmentContainerImage(_pEnvironment).f_Wrap() > fg_LogError("Malterlib/Cloud/AppManager", "Failed to pull environment container image");

				if (auto pException = fCheckAbort())
					co_return pException;

				_pEnvironment->m_ContainerLaunchID = fg_RandomID();
				_pEnvironment->m_ContainerRequestTicketMagic = fg_RandomID();
				_pEnvironment->m_ContainerFingerprint = Fingerprint;

				LaunchParameters = fg_Move(RunArguments);
			}

			// A started container has the mounts of this launch, not those of earlier ones; mounts in the extra arguments are not tracked
			_pEnvironment->m_ContainerMounts.f_Clear();
			for (auto *pMounts : {&ContainerLaunch.m_Mounts, &ContainerLaunch.m_ReadOnlyMounts})
			{
				for (auto &Mount : *pMounts)
					_pEnvironment->m_ContainerMounts.f_Insert(fg_TempCopy(pMounts->fs_GetKey(Mount)));
			}

			// The container keeps its creation-time launch ID and ticket magic, so they are saved before it is created
			{
				auto Result = co_await fp_UpdateEnvironmentJson(_pEnvironment).f_Wrap();
				if (!Result)
				{
					CStr Error = "Failed to save environment container state: {}"_f << Result.f_GetExceptionStr();
					_pEnvironment->f_SetStatus(Error, CAppManagerInterface::EStatusSeverity_Error);
					co_return DMibErrorInstance(Error);
				}
			}

			if (auto pException = fCheckAbort())
				co_return pException;

			// An encryption can have closed since the check before the start: its close stops this start only once the mounts of this launch
			// are in m_ContainerMounts. Nothing suspends from here until the runtime launches
			{
				CStr Error;
				CAppManagerInterface::EStatusSeverity Severity;
				if (!fp_EnvironmentStorageReady(*_pEnvironment, Error, Severity) || !fp_EnvironmentMountsReady(*_pEnvironment, Error, Severity))
				{
					_pEnvironment->f_SetStatus(Error, Severity);
					co_return DMibErrorInstance("Cannot start environment '{}': {}"_f << _pEnvironment->m_Name << Error);
				}
			}

			_pEnvironment->m_LaunchID = _pEnvironment->m_ContainerLaunchID;
		}

		bool bOwnAppleContainerSystem = bContainer && LaunchExecutable == "container" && fp_UseOwnAppleContainerSystem();

		CProcessLaunchActor::CLaunch Launch = CProcessLaunchParams::fs_LaunchExecutable
			(
				LaunchExecutable
				, LaunchParameters
				, AgentWorkingDirectory
				, [this, _pEnvironment, LaunchedPromise = LaunchedPromise.m_Promise](CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)
				{
					if (_pEnvironment->m_bDeleted)
						return;

					switch (_State.f_GetTypeID())
					{
					case NProcess::EProcessLaunchState_Launched: break;
					case NProcess::EProcessLaunchState_LaunchFailed:
						{
							auto &LaunchError = _State.f_Get<NProcess::EProcessLaunchState_LaunchFailed>();
							_pEnvironment->f_SetStatus(fg_Format("Failed to launch agent: {}", LaunchError), CAppManagerInterface::EStatusSeverity_Error);

							if (!LaunchedPromise.f_IsSet())
								LaunchedPromise.f_SetException(DMibErrorInstance(LaunchError));

							_pEnvironment->m_bAgentLaunchFinished = true;

							auto Waiters = fg_Move(_pEnvironment->m_OnAgentLaunchFinished);
							for (auto &Promise : Waiters)
								Promise.f_SetResult();

							fp_OnEnvironmentAgentDisconnected(_pEnvironment);
						}
						break;
					case NProcess::EProcessLaunchState_Exited:
						{
							auto ExitStatus = _State.f_Get<NProcess::EProcessLaunchState_Exited>();

							if (!_pEnvironment->m_bStopping)
								_pEnvironment->f_SetStatus(fg_Format("Agent exited with {}", ExitStatus), CAppManagerInterface::EStatusSeverity_Error);

							if (!LaunchedPromise.f_IsSet())
								LaunchedPromise.f_SetException(DMibErrorInstance(fg_Format("Environment agent exited with '{}' before connecting", ExitStatus)));

							_pEnvironment->m_bAgentLaunchFinished = true;

							auto Waiters = fg_Move(_pEnvironment->m_OnAgentLaunchFinished);
							for (auto &Promise : Waiters)
								Promise.f_SetResult();

							fp_OnEnvironmentAgentDisconnected(_pEnvironment);
						}
						break;
					}
				}
			)
		;

		Launch.m_ToLog = CProcessLaunchActor::ELogFlag_All;
		if (mp_bLogLaunchesToStdErr)
			Launch.m_ToLog |= CProcessLaunchActor::ELogFlag_AdditionallyOutputToStdErr;
		Launch.m_LogName = fg_Format("Environment/{}/Agent", _pEnvironment->m_Name);

		auto &LaunchParams = Launch.m_Params;
		LaunchParams.m_bAllowExecutableLocate = bContainer;

		// The container runtime client must keep the launching environment (for example
		// the docker context configuration in HOME); the agent environment inside the
		// container is set with explicit --env values instead
		if (!bContainer)
		{
			LaunchParams.m_Environment["HOME"] = AgentWorkingDirectory + "/.home";
			LaunchParams.m_Environment["TMPDIR"] = AgentWorkingDirectory + "/.tmp";
		}
		if (bOwnAppleContainerSystem)
			fp_ApplyAppleContainerLaunchEnvironment(LaunchParams);
		else if (bContainer && fp_UseOwnColimaSystem(_pEnvironment))
			fp_ApplyColimaLaunchEnvironment(LaunchParams);
		LaunchParams.m_bMergeEnvironment = true;
		LaunchParams.m_bCreateNewProcessGroup = true;
		LaunchParams.m_bShowLaunched = false;

		_pEnvironment->m_bAgentLaunchFinished = false;
		_pEnvironment->m_AgentLaunch = fg_ConstructActor<CDistributedAppInterfaceLaunchActor>
			(
				AgentAddress
				, mp_State.m_TrustManager
				, g_ActorFunctor / [_pEnvironment](CStr _HostID, CCallingHostInfo _HostInfo, CByteVector _Certificate) -> TCFuture<void>
				{
					if (_pEnvironment->m_bDeleted)
						co_return DMibErrorInstance("Environment deleted");

					_pEnvironment->m_AgentHostID = _HostID;

					co_return {};
				}
				, g_ActorFunctor / [_pEnvironment](NStr::CStr _Error) -> TCFuture<void>
				{
					if (_pEnvironment->m_bDeleted || _pEnvironment->f_IsStarted())
						co_return {};

					_pEnvironment->f_SetStatus(fg_Format("Agent failed to start: {}", _Error), CAppManagerInterface::EStatusSeverity_Error);

					co_return {};
				}
				, fg_Format("Environment/{}", _pEnvironment->m_Name)
				, _pEnvironment->m_LaunchID
				, false
				, bContainer ? _pEnvironment->m_ContainerRequestTicketMagic : CStr()
			)
		;

		auto LaunchSubscription = co_await _pEnvironment->m_AgentLaunch(&CProcessLaunchActor::f_Launch, Launch, fg_ThisActor(this)).f_Wrap();

		if (!LaunchSubscription)
		{
			_pEnvironment->f_SetStatus(fg_Format("Failed to launch agent: {}", LaunchSubscription.f_GetExceptionStr()), CAppManagerInterface::EStatusSeverity_Error);

			if (_pEnvironment->m_AgentLaunch)
				co_await fg_Move(_pEnvironment->m_AgentLaunch).f_Destroy().f_Wrap() > fg_LogError("Malterlib/Cloud/AppManager", "Failed to destroy environment agent launch");

			co_return LaunchSubscription.f_GetException();
		}

		_pEnvironment->m_AgentLaunchSubscription = fg_Move(*LaunchSubscription);

		// The stop that asked to abort waits for this start to end, and then stops the launched agent
		if (auto pException = fCheckAbort())
			co_return pException;

		// The agent can already have connected while the launch was being set up
		if (!_pEnvironment->f_IsStarted())
		{
			// Registration completes this wait; a launch failure or early exit fails it without waiting for the timeout
			auto ConnectedFuture = _pEnvironment->m_OnAgentConnected.f_Insert().f_Future();

			fg_Move(LaunchedPromise.m_Future) > [_pEnvironment](TCAsyncResult<void> &&_Result)
				{
					if (_Result)
						return;

					auto OnAgentConnected = fg_Move(_pEnvironment->m_OnAgentConnected);
					for (auto &Promise : OnAgentConnected)
						Promise.f_SetException(_Result.f_GetException());
				}
			;

			auto ConnectedResult = co_await fg_Move(ConnectedFuture).f_Timeout(120.0, "Timed out waiting for the environment agent to connect").f_Wrap();

			if (!ConnectedResult)
			{
				_pEnvironment->f_SetStatus(fg_Format("Agent failed to connect: {}", ConnectedResult.f_GetExceptionStr()), CAppManagerInterface::EStatusSeverity_Error);
				fp_StopEnvironmentInternal(_pEnvironment)
					> fg_LogError("Malterlib/Cloud/AppManager", "Failed to stop environment '{}' after its agent failed to connect"_f << _pEnvironment->m_Name)
				;
				co_return ConnectedResult.f_GetException();
			}
		}
		else
			fg_Move(LaunchedPromise.m_Future) > fg_LogError("Malterlib/Cloud/AppManager", "The launch of the agent of environment '{}' failed"_f << _pEnvironment->m_Name);

		bConnected = true;

		co_return {};
	}

	void CAppManagerActor::fp_SendEnvironmentAgentConfig(CEnvironment &_Environment)
	{
		if (!_Environment.m_AgentInterface)
			return;

		CAppManagerEnvironmentInterface::CAgentConfig AgentConfig;

		AgentConfig.m_AutoUpdateConfig = fg_AppManager_EnvironmentAgentAutoUpdateConfig
			(
				_Environment.m_Settings.m_Type
				, _Environment.m_Settings.m_bContainerReadOnly
				, mp_State.m_ConfigDatabase.m_Data
			)
		;

		// The agent installs the OS dependencies of its own application inside
		// the environment. They come from the installed version info, since agent
		// distributions are installed without settings from version info. They are
		// the tools of OS patching, which cannot change a read-only container
		auto pAgentApplication = fp_FindEnvironmentAgentApplication(_Environment);
		if (pAgentApplication && !(_Environment.m_Settings.m_Type == CAppManagerInterface::EEnvironmentType_Container && _Environment.m_Settings.m_bContainerReadOnly))
		{
			try
			{
				CApplicationSettings VersionInfoSettings;
				EApplicationSetting ChangedSettings = EApplicationSetting_None;
				VersionInfoSettings.f_FromVersionInfo(pAgentApplication->m_LastInstalledVersionInfo, ChangedSettings);
				AgentConfig.m_AgentOSDependencies = fg_Move(VersionInfoSettings.m_OSDependencies);
			}
			catch (CException const &_Exception)
			{
				DMibLogWithCategory
					(
						Malterlib/Cloud/AppManager
						, Error
						, "Failed to read the OS dependencies of agent application '{}' from its version info: {}"
						, pAgentApplication->m_Name
						, _Exception
					)
				;
			}
		}

		// The environment runs one OS, so its own OS dependencies use the selector matching every OS
		if (!_Environment.m_Settings.m_OSDependencies.f_IsEmpty())
			AgentConfig.m_OSDependencies["*"].f_Insert(_Environment.m_Settings.m_OSDependencies);

		auto *pFindEnvironment = mp_Environments.f_FindEqual(_Environment.m_Name);
		if (!pFindEnvironment)
			return;

		TCSharedPointer<CEnvironment> pEnvironment = *pFindEnvironment;

		// The configuration installs OS packages, which can take long, but a guest that never replies must not hold the launches and stops that
		// wait for it
		++pEnvironment->m_nPendingAgentConfigurations;
		umint Sequence = ++pEnvironment->m_AgentConfigurationSequence;
		pEnvironment->m_AgentInterface.f_CallActor(&CAppManagerEnvironmentInterface::f_ConfigureAgent)
			(fg_Move(AgentConfig))
			.f_Timeout(60.0 * 60.0, "Timed out configuring the environment agent (1 hour)")
			> [pEnvironment, Sequence](TCAsyncResult<void> &&_Result)
			{
				// The error comes from the guest, and --environment-list shows it
				CStr Error = _Result ? CStr() : fg_AppManager_SanitizeGuestText(_Result.f_GetExceptionStr(), 16 * 1024);
				if (!_Result)
					DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Failed to configure the agent of environment '{}': {}", pEnvironment->m_Name, Error);

				// The newest configuration replaces what the older ones installed, which can finish after it
				if (Sequence == pEnvironment->m_AgentConfigurationSequence)
					pEnvironment->m_AgentConfigurationError = fg_Move(Error);

				if (--pEnvironment->m_nPendingAgentConfigurations)
					return;

				auto OnConfigured = fg_Move(pEnvironment->m_OnAgentConfigured);
				for (auto &Promise : OnConfigured)
				{
					if (pEnvironment->m_AgentConfigurationError.f_IsEmpty())
						Promise.f_SetResult();
					else
						Promise.f_SetException(DMibErrorInstance(pEnvironment->m_AgentConfigurationError));
				}
			}
		;
	}

	// Waits for pending agent configuration; when the last one failed and none is pending, it is sent again first
	TCFuture<void> CAppManagerActor::fp_WaitForEnvironmentAgentConfiguration(TCSharedPointer<CEnvironment> _pEnvironment)
	{
		if (!_pEnvironment->m_AgentConfigurationError.f_IsEmpty() && !_pEnvironment->m_nPendingAgentConfigurations)
			fp_SendEnvironmentAgentConfig(*_pEnvironment);

		if (_pEnvironment->m_nPendingAgentConfigurations)
			co_await _pEnvironment->m_OnAgentConfigured.f_Insert().f_Future();

		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_OnEnvironmentAgentConnected(TCSharedPointer<CEnvironment> _pEnvironment, TCDistributedActorInterface<CAppManagerEnvironmentInterface> _Interface)
	{
		if (_pEnvironment->m_bDeleted || _pEnvironment->m_bStopping)
			co_return {};

		_pEnvironment->m_AgentInterface = fg_Move(_Interface);

		// The agent monitors the environment with the update settings inherited
		// from this AppManager
		fp_SendEnvironmentAgentConfig(*_pEnvironment);

		_pEnvironment->m_bStarted = true;
		_pEnvironment->f_SetStatus("Running", CAppManagerInterface::EStatusSeverity_None);

		auto OnAgentConnected = fg_Move(_pEnvironment->m_OnAgentConnected);
		for (auto &Promise : OnAgentConnected)
			Promise.f_SetResult();

		co_return {};
	}

	void CAppManagerActor::fp_OnEnvironmentAgentDisconnected(TCSharedPointer<CEnvironment> _pEnvironment)
	{
		bool bWasStarted = _pEnvironment->m_bStarted;

		_pEnvironment->m_bStarted = false;
		if (_pEnvironment->m_AgentInterface)
		{
			_pEnvironment->m_AgentInterface.f_Destroy().f_DiscardResult();
			_pEnvironment->m_AgentInterface.f_Clear();
		}
		if (_pEnvironment->m_AgentAppInterface)
		{
			_pEnvironment->m_AgentAppInterface.f_Destroy().f_DiscardResult();
			_pEnvironment->m_AgentAppInterface.f_Clear();
		}
		_pEnvironment->m_AgentHostID = {};

		// An agent that disappears mid-update must not keep preventing reboots, since nothing else reports the end of the update.
		// A reconnecting agent reports its update again
		_pEnvironment->f_SetAgentUpdateInProgress({});

		if (!_pEnvironment->m_bStopping && bWasStarted)
			_pEnvironment->f_SetStatus("Agent disconnected", CAppManagerInterface::EStatusSeverity_Error);

		// The colima virtual machine can have exited with the agent, so the next start checks it again
		if (!_pEnvironment->m_bStopping && fp_UseOwnColimaSystem(_pEnvironment))
			mp_bColimaSystemReady = false;

		TCVector<TCSharedPointer<CApplication>> RelaunchApplications;
		for (auto &pApplication : mp_Applications)
		{
			if (pApplication->m_pLaunchedEnvironment != _pEnvironment)
				continue;

			pApplication->f_Clear();

			// A launch waiting for the application to register or start would otherwise wait for its timeout
			fp_AbortPendingDistributedAppWaits(*pApplication, "Environment '{}' agent disconnected"_f << _pEnvironment->m_Name);

			if (!_pEnvironment->m_bStopping && !pApplication->m_bStopped && !pApplication->m_bDeleted)
			{
				fp_ScheduleRelaunchApp(pApplication);
				RelaunchApplications.f_Insert(pApplication);
			}
		}

		// The container or virtual machine can outlive the agent connection, and the applications and mounts in it would
		// keep running unsupervised. A start runs after this stop
		if (!_pEnvironment->m_bStopping && !_pEnvironment->m_bStarting && !_pEnvironment->m_bDeleted)
		{
			fp_StopEnvironmentInternal(_pEnvironment) > [this, _pEnvironment, bRelaunch = !RelaunchApplications.f_IsEmpty()](TCAsyncResult<void> &&_Result)
				{
					if (!_Result)
					{
						DMibLogWithCategory
							(
								Malterlib/Cloud/AppManager
								, Error
								, "Failed to stop environment '{}' after its agent disconnected: {}"
								, _pEnvironment->m_Name
								, _Result.f_GetExceptionStr()
							)
						;
					}

					// The relaunch delay can end while the stop runs, and launches wait for the stop without retrying
					if (bRelaunch)
						fp_UpdateApplicationDependencies();
				}
			;
		}

		// A state change runs the dependency handling, which can launch applications, so it comes after the stop has
		// started; a start waits for the stop
		for (auto &pApplication : RelaunchApplications)
		{
			fp_AppLaunchStateChanged
				(
					pApplication
					, "Environment '{}' stopped unexpectedly. Waiting to retry launching."_f << _pEnvironment->m_Name
					, CAppManagerInterface::EStatusSeverity_Error
				)
			;
		}
	}

	// Does not wait for idle or for the updates of the agent: callers that need them finished wait for them first and refuse new ones meanwhile.
	// Agent and virtual machine teardown failures are only logged; only a failed final container stop fails the stop
	TCFuture<void> CAppManagerActor::fp_StopEnvironmentInternal(TCSharedPointer<CEnvironment> _pEnvironment)
	{
		// The start finishes first, since it would otherwise launch the environment after the stop finished. It can wait minutes for the guest, so
		// it is asked to give up
		if (_pEnvironment->m_bStarting && !_pEnvironment->m_bStopping && !_pEnvironment->m_bStartAbortRequested)
		{
			_pEnvironment->m_bStartAbortRequested = true;

			auto OnAgentConnected = fg_Move(_pEnvironment->m_OnAgentConnected);
			for (auto &Promise : OnAgentConnected)
				Promise.f_SetException(DMibErrorInstance("Environment '{}' is stopping"_f << _pEnvironment->m_Name));
		}

		while (_pEnvironment->m_bStarting && !_pEnvironment->m_bStopping)
		{
			auto StartResult = co_await _pEnvironment->m_OnAgentConnected.f_Insert().f_Future().f_Wrap();
			if (!StartResult)
				DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Environment '{}' start before stop failed: {}", _pEnvironment->m_Name, StartResult.f_GetExceptionStr());
		}

		if (_pEnvironment->m_bStopping)
		{
			co_await _pEnvironment->m_OnStopFinished.f_Insert().f_Future();
			co_return {};
		}

		_pEnvironment->m_bStopping = true;

		// Concurrent stops wait for this one and share its result
		CStr StopError;
		auto Cleanup = g_OnScopeExit / [_pEnvironment, &StopError]
			{
				_pEnvironment->m_bStopping = false;

				auto OnStopFinished = fg_Move(_pEnvironment->m_OnStopFinished);
				for (auto &Promise : OnStopFinished)
				{
					if (StopError.f_IsEmpty())
						Promise.f_SetResult();
					else
						Promise.f_SetException(DMibErrorInstance(StopError));
				}
			}
		;

		// Applications must finish their agent-mediated shutdown before the agent goes away, also those whose stop already started
		{
			TCFutureVector<uint32> ApplicationStops;
			TCFutureVector<void> StartedStops;
			for (auto &pApplication : mp_Applications)
			{
				if (pApplication->m_pLaunchedEnvironment == _pEnvironment)
					pApplication->f_Stop(EStopFlag_AutoStart) > ApplicationStops;
				else if (pApplication->m_pStoppingEnvironment == _pEnvironment)
					pApplication->m_OnEnvironmentStopFinished.f_Insert().f_Future() > StartedStops;
			}

			auto [StopResults, StartedStopResults] = co_await (fg_AllDoneWrapped(ApplicationStops) + fg_AllDoneWrapped(StartedStops));
			for (auto &StopResult : StopResults)
			{
				if (!StopResult)
					DMibLogWithCategory(Malterlib/Cloud/AppManager, Warning, "Failed to stop application in environment '{}': {}", _pEnvironment->m_Name, StopResult.f_GetExceptionStr());
			}
		}

		_pEnvironment->m_bStarted = false;
		++_pEnvironment->m_nStops;

		// A guest with a logged-in console session turns the host-side stop request
		// into a confirmation dialog, so a virtual machine is shut down from inside
		// through the agent instead, also when a window client hosts it; a machine
		// hosted here is awaited below before the host side stops it
		bool bGuestShutdown = false;
		bool bWindowGuestShutdownFailed = false;
		if (_pEnvironment->m_Settings.m_Type == CAppManagerInterface::EEnvironmentType_VM && _pEnvironment->m_AgentInterface)
		{
			// A machine a window client hosts has no actor here, so only this shutdown stops it
			bool bWindowHostedVM = _pEnvironment->m_bWindowOpen && !_pEnvironment->m_VMActor;

			auto ShutdownResult = co_await _pEnvironment->m_AgentInterface.f_CallActor(&CAppManagerEnvironmentInterface::f_ShutdownEnvironment)()
				.f_Timeout(30.0, "Timed out requesting the environment shutdown")
				.f_Wrap()
			;

			if (ShutdownResult)
				bGuestShutdown = true;
			else
			{
				DMibLogWithCategory(Malterlib/Cloud/AppManager, Warning, "Failed to shut down environment '{}' from inside: {}", _pEnvironment->m_Name, ShutdownResult.f_GetExceptionStr());
				bWindowGuestShutdownFailed = bWindowHostedVM;
			}
		}

		{
			TCFutureVector<void> InterfaceDestroys;

			if (_pEnvironment->m_AgentInterface)
			{
				_pEnvironment->m_AgentInterface.f_Destroy() > InterfaceDestroys;
				_pEnvironment->m_AgentInterface.f_Clear();
			}

			if (_pEnvironment->m_AgentAppInterface)
			{
				_pEnvironment->m_AgentAppInterface.f_Destroy() > InterfaceDestroys;
				_pEnvironment->m_AgentAppInterface.f_Clear();
			}

			co_await fg_AllDone(InterfaceDestroys).f_Wrap() > fg_LogError("Malterlib/Cloud/AppManager", "Failed to destroy environment agent interfaces");
		}

		_pEnvironment->m_AgentHostID = {};

		// The stopped agent cannot report the end of its update
		_pEnvironment->f_SetAgentUpdateInProgress({});

		// Stop the container through the runtime first: the attached client then exits by itself. Signalling the client
		// is not a reliable stop, since the Apple container client does not forward the signal to the container
		if (_pEnvironment->m_Settings.m_Type == CAppManagerInterface::EEnvironmentType_Container && _pEnvironment->m_AgentLaunch)
		{
			co_await fp_StopEnvironmentContainer(_pEnvironment).f_Wrap() > fg_LogError("Malterlib/Cloud/AppManager", "Failed to stop environment container");

			if (!_pEnvironment->m_bAgentLaunchFinished)
			{
				auto WaitResult = co_await _pEnvironment->m_OnAgentLaunchFinished.f_Insert()
					.f_Future()
					.f_Timeout(30.0, "Timed out waiting for the environment agent client to exit")
					.f_Wrap()
				;

				if (!WaitResult)
				{
					DMibLogWithCategory
						(
							Malterlib/Cloud/AppManager
							, Warning
							, "The agent client for environment '{}' did not exit with the container: {}"
							, _pEnvironment->m_Name
							, WaitResult.f_GetExceptionStr()
						)
					;
				}
			}
		}

		if (_pEnvironment->m_AgentLaunch)
		{
			if (!_pEnvironment->m_bAgentLaunchFinished)
			{
				auto StopResult = co_await fg_TempCopy(_pEnvironment->m_AgentLaunch)(&CProcessLaunchActor::f_StopProcess).f_Wrap();
				if (!StopResult)
					DMibLogWithCategory(Malterlib/Cloud/AppManager, Warning, "Failed to stop agent for environment '{}': {}", _pEnvironment->m_Name, StopResult.f_GetExceptionStr());
			}

			co_await fg_Move(_pEnvironment->m_AgentLaunch).f_Destroy().f_Wrap() > fg_LogError("Malterlib/Cloud/AppManager", "Failed to destroy environment agent launch");
		}

		_pEnvironment->m_AgentLaunchSubscription.f_Clear();
		_pEnvironment->m_AgentTicketSubscription.f_Clear();

		if (_pEnvironment->m_VMActor)
		{
			if (bGuestShutdown)
			{
				auto WaitResult = co_await fg_TempCopy(_pEnvironment->m_VMActor)(&NVirtualization::CVirtualMachineActor::f_WaitForStop, 120.0).f_Wrap();
				if (!WaitResult || !*WaitResult)
					DMibLogWithCategory(Malterlib/Cloud/AppManager, Warning, "The guest for environment '{}' did not power off after the shutdown from inside", _pEnvironment->m_Name);
			}

			co_await fg_TempCopy(_pEnvironment->m_VMActor)(&NVirtualization::CVirtualMachineActor::f_Stop).f_Wrap()
				> fg_LogError("Malterlib/Cloud/AppManager", "Failed to stop environment virtual machine")
			;

			co_await fg_Move(_pEnvironment->m_VMActor).f_Destroy().f_Wrap() > fg_LogError("Malterlib/Cloud/AppManager", "Failed to destroy environment virtual machine");
		}

		// The container is stopped but kept, so its filesystem (including OS updates
		// made inside it) survives a restart that reuses it
		if (_pEnvironment->m_Settings.m_Type == CAppManagerInterface::EEnvironmentType_Container)
		{
			auto StopResult = co_await fp_StopEnvironmentContainer(_pEnvironment).f_Wrap();
			if (!StopResult)
			{
				StopError = StopResult.f_GetExceptionStr();
				_pEnvironment->f_SetStatus(StopError, CAppManagerInterface::EStatusSeverity_Error);
				co_return StopResult.f_GetException();
			}
		}

		if (bWindowGuestShutdownFailed)
		{
			StopError = "The virtual machine of environment '{}' runs in a window and did not shut down. Close the window to stop it."_f << _pEnvironment->m_Name;
			_pEnvironment->f_SetStatus(StopError, CAppManagerInterface::EStatusSeverity_Error);
			co_return DMibErrorInstance(StopError);
		}

		// A stop that follows a failure, like an agent that failed to connect or disconnected, keeps the failure as the status, so that the status
		// and its sensor report it
		if (_pEnvironment->m_StatusSeverity != CAppManagerInterface::EStatusSeverity_Error)
			_pEnvironment->f_SetStatus("Stopped", CAppManagerInterface::EStatusSeverity_Warning);

		co_return {};
	}

	void CAppManagerActor::fp_AbortPendingDistributedAppWaits(CApplication &_Application, CStr const &_Error)
	{
		auto OnRegister = fg_Move(_Application.m_OnRegisterDistributedApp);
		for (auto &Promise : OnRegister)
			Promise.f_SetException(DMibErrorInstance(_Error));

		auto OnStarted = fg_Move(_Application.m_OnStartedDistributedApp);
		for (auto &Promise : OnStarted)
			Promise.f_SetException(DMibErrorInstance(_Error));
	}

	void CAppManagerActor::fp_OnEnvironmentApplicationStateChange(TCSharedPointer<CEnvironment> const &_pEnvironment, CAppManagerEnvironmentInterface::CApplicationStateChange const &_Change)
	{
		auto *pFindApplication = mp_Applications.f_FindEqual(_Change.m_Application);
		if (!pFindApplication)
			return;

		auto pApplication = *pFindApplication;

		if (pApplication->m_pLaunchedEnvironment != _pEnvironment)
			return;

		// A running process is not yet a ready distributed application: the host needs its interface registration and start result.
		// Only progress statuses and the exit after a requested stop are below Error: the agent reports every other exit as an Error,
		// also an exit with 0, before or after the registration, so no unrequested exit is dropped here
		if
		(
			_Change.m_StatusSeverity != CAppManagerInterface::EStatusSeverity_Error
			&& pApplication->m_Settings.m_bDistributedApp
			&& (!pApplication->m_bDistributedStartupFinished || !pApplication->m_AppInterface)
		)
		{
			return;
		}

		// An error fails pending waits for the distributed app interface, and the next process is not ready before its own start result
		if (_Change.m_StatusSeverity == CAppManagerInterface::EStatusSeverity_Error)
		{
			pApplication->m_bDistributedStartupFinished = false;
			fp_AbortPendingDistributedAppWaits(*pApplication, _Change.m_Status);
		}

		fp_AppLaunchStateChanged(pApplication, _Change.m_Status, _Change.m_StatusSeverity);
	}

	TCFuture<void> CAppManagerActor::fp_ConfirmEnvironmentAppStart(TCSharedPointer<CApplication> _pApplication)
	{
		umint AppInterfaceAssignSequence = _pApplication->m_AppInterfaceAssignSequence;

		_pApplication->m_bDistributedStartupFinished = false;
		fp_AppLaunchStateChanged(_pApplication, "Launched (waiting for app to fully start)", CAppManagerInterface::EStatusSeverity_Warning);

		auto StartResult = co_await _pApplication->m_AppInterface.f_CallActor(&CDistributedAppInterfaceClient::f_GetAppStartResult)()
			.f_Timeout(60.0 * 60.0, "Timed out waiting for application start result (1 hour)")
			.f_Wrap()
		;

		// A deletion, a new launch or a later registration supersedes this start result
		if (_pApplication->m_bDeleted || _pApplication->m_bLaunching || _pApplication->m_AppInterfaceAssignSequence != AppInterfaceAssignSequence)
			co_return {};

		_pApplication->m_bDistributedStartupFinished = true;

		if (!StartResult)
		{
			fp_AppLaunchStateChanged(_pApplication, "Launched (app startup failed: '{}')"_f << StartResult.f_GetExceptionStr(), CAppManagerInterface::EStatusSeverity_Error);

			co_return {};
		}

		auto OnStarted = fg_Move(_pApplication->m_OnStartedDistributedApp);
		for (auto &Promise : OnStarted)
			Promise.f_SetResult();

		fp_AppLaunchStateChanged(_pApplication, "Launched", CAppManagerInterface::EStatusSeverity_None);

		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_AbortEnvironmentLaunch(TCSharedPointer<CApplication> _pApplication, TCSharedPointer<CEnvironment> _pEnvironment)
	{
		// Stop the application inside the environment so a later relaunch attempt
		// does not hit the already-launched check in the agent
		if (_pEnvironment->m_AgentInterface && !_pApplication->m_bDeleted)
		{
			auto StopResult = co_await _pEnvironment->m_AgentInterface.f_CallActor(&CAppManagerEnvironmentInterface::f_StopApplication)
				(_pApplication->m_Name)
				.f_Timeout(60.0 * 60.0, "Timed out stopping application in environment (1 hour)")
				.f_Wrap()
			;

			// The process can still run, and a later stop must reach it through the agent
			if (!StopResult)
			{
				DMibLogWithCategory
					(
						Malterlib/Cloud/AppManager
						, Error
						, "Failed to stop application '{}' after failed environment launch: {}"
						, _pApplication->m_Name
						, StopResult.f_GetExceptionStr()
					)
				;

				co_return {};
			}
		}

		if (_pApplication->m_pLaunchedEnvironment == _pEnvironment)
			_pApplication->f_ClearEnvironmentLaunch();

		_pApplication->m_bLaunched = false;

		if (!_pApplication->m_bStopped && !_pApplication->m_bDeleted)
			fp_ScheduleRelaunchApp(_pApplication);

		co_return {};
	}

	auto CAppManagerActor::fp_LaunchAppInEnvironment(TCSharedPointer<CApplication> _pApplication, TCSharedPointer<CEnvironment> _pEnvironment) -> TCFuture<CAppLaunchResult>
	{
		auto fStoppedManuallyError = [&]() -> CExceptionPointer
			{
				if (!_pEnvironment->m_bStoppedManually)
					return nullptr;

				CStr Status = "Environment '{}' was stopped. Start it with --environment-start."_f << _pEnvironment->m_Name;
				fp_AppLaunchStateChanged(_pApplication, Status, CAppManagerInterface::EStatusSeverity_Warning);

				return DMibErrorInstance(Status);
			}
		;

		// A manual stop can happen while the launch is on its way here
		if (auto pException = fStoppedManuallyError())
			co_return pException;

		auto EnsureResult = co_await fp_EnsureEnvironmentStarted(_pEnvironment).f_Wrap();

		if (_pApplication->m_bDeleted)
			co_return DMibErrorInstance("Application deleted");

		if (!EnsureResult)
		{
			if (auto pException = fStoppedManuallyError())
				co_return pException;

			fp_AppLaunchStateChanged
				(
					_pApplication
					, "Failed to start environment '{}': {}"_f << _pEnvironment->m_Name << EnsureResult.f_GetExceptionStr()
					, CAppManagerInterface::EStatusSeverity_Error
				)
			;

			if (!_pApplication->m_bStopped)
				fp_ScheduleRelaunchApp(_pApplication);

			co_return EnsureResult.f_GetException();
		}

		{
			auto ConfigureResult = co_await fp_WaitForEnvironmentAgentConfiguration(_pEnvironment).f_Wrap();

			if (_pApplication->m_bDeleted)
				co_return DMibErrorInstance("Application deleted");

			if (!ConfigureResult)
			{
				fp_AppLaunchStateChanged
					(
						_pApplication
						, "Failed to configure environment '{}': {}"_f << _pEnvironment->m_Name << ConfigureResult.f_GetExceptionStr()
						, CAppManagerInterface::EStatusSeverity_Error
					)
				;

				if (!_pApplication->m_bStopped)
					fp_ScheduleRelaunchApp(_pApplication);

				co_return ConfigureResult.f_GetException();
			}
		}

		// The agent of a Local environment runs as root on the host and sets the capabilities after it installs the OS dependencies, like the
		// launch of a host application, so it gets only the files that the install wrote, see fg_AppManager_InstalledLowPortExecutablePath. The
		// agent of a container or VM sets them for all low port executables inside the environment, where changing the owners of environment
		// storage at launch clears them
		TCVector<CStr> LowPortExecutables;
		if (_pEnvironment->m_Settings.m_Type == CAppManagerInterface::EEnvironmentType_Local)
		{
			if (!_pApplication->m_LowPortExecutablesToGrant.f_IsEmpty())
			{
				auto BlockingActorCheckout = fg_BlockingActor();

				auto InstalledResult = co_await
					(
						g_Dispatch(BlockingActorCheckout) / [Directory = _pApplication->f_GetDirectory(), ToGrant = _pApplication->m_LowPortExecutablesToGrant]()
						{
							TCVector<CStr> Installed;
							for (auto &Digest : ToGrant)
							{
								auto &Executable = ToGrant.fs_GetKey(Digest);
								try
								{
									if (!fg_AppManager_InstalledLowPortExecutablePath(Directory, Executable, Digest).f_IsEmpty())
										Installed.f_Insert(Executable);
								}
								catch (CException const &_Exception)
								{
									DMibLogWithCategory(Malterlib/Cloud/AppManager, Warning, "Failed to read the low port executable '{}': {}", Directory / Executable, _Exception);
								}
							}

							return Installed;
						}
					)
					.f_Wrap()
				;

				if (_pApplication->m_bDeleted)
					co_return DMibErrorInstance("Application deleted");

				if (InstalledResult)
					LowPortExecutables = fg_Move(*InstalledResult);
				else
					fg_Move(InstalledResult) > fg_LogError("Malterlib/Cloud/AppManager", "Failed to read the low port executables");
			}
		}
		else if (auto pLowPortExecutables = _pApplication->m_LastInstalledVersionInfo.m_ExtraInfo.f_GetMember("LowPortExecutables", EJsonType_Array))
		{
			for (auto &Executable : pLowPortExecutables->f_Array())
			{
				if (Executable.f_IsString())
					LowPortExecutables.f_Insert(Executable.f_String());
			}
		}

		auto InterfaceAddress = co_await fp_EnsureEnvironmentListen(_pEnvironment).f_Wrap();

		if (_pApplication->m_bDeleted)
			co_return DMibErrorInstance("Application deleted");

		if (!InterfaceAddress)
		{
			fp_AppLaunchStateChanged
				(
					_pApplication
					, "Failed to ensure listen for environment '{}': {}"_f << _pEnvironment->m_Name << InterfaceAddress.f_GetExceptionStr()
					, CAppManagerInterface::EStatusSeverity_Error
				)
			;

			if (!_pApplication->m_bStopped)
				fp_ScheduleRelaunchApp(_pApplication);

			co_return InterfaceAddress.f_GetException();
		}

		// A stop that arrived while the environment started had no agent process to stop
		if (_pApplication->m_bStopped)
			co_return DMibErrorInstance("Application was stopped while its environment was prepared");

		CStr NewLaunchID = fg_RandomID();
		_pApplication->m_LaunchID = NewLaunchID;
		umint AppInterfaceAssignSequence = _pApplication->m_AppInterfaceAssignSequence;

		CAppManagerEnvironmentInterface::CEnvironmentLaunch Launch;
		Launch.m_Name = _pApplication->m_Name;

		if (!fp_EnvironmentUsesRemoteStorage(*_pEnvironment))
			Launch.m_Directory = _pApplication->f_GetDirectory();
		Launch.m_Executable = _pApplication->m_Settings.m_Executable;
		Launch.m_Parameters = _pApplication->m_Settings.m_ExecutableParameters;
		Launch.m_RunAsUser = _pApplication->m_Settings.m_RunAsUser;
		Launch.m_RunAsGroup = _pApplication->m_Settings.m_RunAsGroup;
		Launch.m_bRunAsUserHasShell = _pApplication->m_Settings.m_bRunAsUserHasShell;
#ifdef DPlatformFamily_Windows
		Launch.m_RunAsUserPassword = _pApplication->m_Settings.m_RunAsUserPassword;
#endif
		Launch.m_bDistributedApp = _pApplication->m_Settings.m_bDistributedApp;
		Launch.m_InterfaceAddress = InterfaceAddress->f_Encode();
		Launch.m_LaunchID = _pApplication->m_LaunchID;
		Launch.m_OSDependencies = _pApplication->m_Settings.m_OSDependencies;
		Launch.m_LowPortExecutables = fg_Move(LowPortExecutables);

		if (auto pException = fStoppedManuallyError())
			co_return pException;

		// The agent can disconnect, or a stop begin, while the launch waits for the environment. A disconnect relaunches and a stop
		// stops only the applications associated with the environment, so the association follows this check without a suspension
		if (!_pEnvironment->f_IsStarted() || _pEnvironment->m_bStopping)
		{
			CStr Error = "Environment '{}' stopped before the launch"_f << _pEnvironment->m_Name;
			fp_AppLaunchStateChanged(_pApplication, Error, CAppManagerInterface::EStatusSeverity_Error);

			if (!_pApplication->m_bStopped)
				fp_ScheduleRelaunchApp(_pApplication);

			co_return DMibErrorInstance(Error);
		}

		// The application can request its connection ticket before the launch call returns, which requires the association
		_pApplication->m_pLaunchedEnvironment = _pEnvironment;

		auto LaunchResult = co_await _pEnvironment->m_AgentInterface.f_CallActor(&CAppManagerEnvironmentInterface::f_LaunchApplication)
			(fg_Move(Launch))
			.f_Timeout(60.0 * 60.0, "Timed out launching application in environment (1 hour)")
			.f_Wrap()
		;

		if (_pApplication->m_bDeleted)
		{
			// The removal can have stopped the application before the agent created it, so the launched process is stopped
			// here unless an application added again with the same name uses the environment
			auto *pCurrent = mp_Applications.f_FindEqual(_pApplication->m_Name);
			if (_pEnvironment->m_AgentInterface && (!pCurrent || (*pCurrent)->m_pLaunchedEnvironment != _pEnvironment))
			{
				co_await _pEnvironment->m_AgentInterface.f_CallActor(&CAppManagerEnvironmentInterface::f_StopApplication)
					(_pApplication->m_Name)
					.f_Timeout(60.0 * 60.0, "Timed out stopping application in environment (1 hour)")
					.f_Wrap()
					> fg_LogError("Malterlib/Cloud/AppManager", "Failed to stop application launched after its removal")
				;
			}

			co_return DMibErrorInstance("Application deleted");
		}

		if (!LaunchResult)
		{
			fp_AppLaunchStateChanged
				(
					_pApplication
					, "Failed launch in environment '{}': {}"_f << _pEnvironment->m_Name << fg_AppManager_SanitizeGuestText(LaunchResult.f_GetExceptionStr(), 16 * 1024)
					, CAppManagerInterface::EStatusSeverity_Error
				)
			;

			// The agent can have launched the application even though the launch
			// call failed, so stop it in the environment before the retry
			co_await fp_AbortEnvironmentLaunch(_pApplication, _pEnvironment);

			co_return LaunchResult.f_GetException();
		}

		// A stop or an agent disconnect during the launch call clears the association. The stop can have come before the
		// agent created the application, so the launched process is stopped here
		if (_pApplication->m_pLaunchedEnvironment != _pEnvironment)
		{
			if (_pEnvironment->m_AgentInterface && !_pApplication->m_bDeleted)
			{
				co_await _pEnvironment->m_AgentInterface.f_CallActor(&CAppManagerEnvironmentInterface::f_StopApplication)
					(_pApplication->m_Name)
					.f_Timeout(60.0 * 60.0, "Timed out stopping application in environment (1 hour)")
					.f_Wrap()
					> fg_LogError("Malterlib/Cloud/AppManager", "Failed to stop application launched after its stop")
				;
			}

			co_return DMibErrorInstance("Application was stopped while launching in environment '{}'"_f << _pEnvironment->m_Name);
		}

		// An application that kept running in the environment across a restart is
		// adopted with the launch id it already runs with
		_pApplication->m_LaunchID = fg_Move(*LaunchResult);

		// An adopted application keeps its registration. A new process registers itself, possibly before the launch call
		// returns, so an interface that was not assigned during the launch belongs to the previous process
		if (_pApplication->m_LaunchID == NewLaunchID && _pApplication->m_AppInterfaceAssignSequence == AppInterfaceAssignSequence)
			fp_DiscardPreviousAppInterface(*_pApplication);

		_pApplication->m_bLaunched = true;

		if (!_pApplication->m_Settings.m_bDistributedApp)
		{
			fp_AppLaunchStateChanged(_pApplication, "Launched", CAppManagerInterface::EStatusSeverity_None);

			co_return CAppLaunchResult{};
		}

		// The application connects its distributed app interface directly to this
		// AppManager through the environment listen
		if (!_pApplication->m_AppInterface)
		{
			fp_AppLaunchStateChanged(_pApplication, "Launched (waiting for distributed app register)", CAppManagerInterface::EStatusSeverity_Warning);

			auto RegisterResult = co_await _pApplication->m_OnRegisterDistributedApp.f_Insert()
				.f_Future()
				.f_Timeout(60.0 * 60.0, "Timed out waiting for application to register (1 hour)")
				.f_Wrap()
			;

			if (_pApplication->m_bDeleted)
				co_return {};

			if (!RegisterResult)
			{
				DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Launched app '{}' failed to register: {}", _pApplication->m_Name, RegisterResult.f_GetExceptionStr());

				fp_AppLaunchStateChanged(_pApplication, "Launched (app register failed: '{}')"_f << RegisterResult.f_GetExceptionStr(), CAppManagerInterface::EStatusSeverity_Error);

				co_await fp_AbortEnvironmentLaunch(_pApplication, _pEnvironment);

				co_return {RegisterResult.f_GetExceptionStr()};
			}
		}

		if (!_pApplication->m_AppInterface)
			co_return DMibErrorInstance("Internal error: No app interface");

		fp_AppLaunchStateChanged(_pApplication, "Launched (waiting for app to fully start)", CAppManagerInterface::EStatusSeverity_Warning);

		auto StartResult = co_await _pApplication->m_AppInterface.f_CallActor(&CDistributedAppInterfaceClient::f_GetAppStartResult)()
			.f_Timeout(60.0 * 60.0, "Timed out waiting for application start result (1 hour)")
			.f_Wrap()
		;

		if (_pApplication->m_bDeleted)
			co_return {};

		if (_pApplication->m_bLaunching)
			_pApplication->m_bDistributedStartupFinished = true;

		if (!StartResult)
		{
			DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Launched app '{}' failed to start up: {}", _pApplication->m_Name, StartResult.f_GetExceptionStr());

			fp_AppLaunchStateChanged(_pApplication, "Launched (app startup failed: '{}')"_f << StartResult.f_GetExceptionStr(), CAppManagerInterface::EStatusSeverity_Error);

			co_await fp_AbortEnvironmentLaunch(_pApplication, _pEnvironment);

			co_return {StartResult.f_GetExceptionStr()};
		}

		for (auto &OnStartedDistributedApp : _pApplication->m_OnStartedDistributedApp)
			OnStartedDistributedApp.f_SetResult();

		_pApplication->m_OnStartedDistributedApp.f_Clear();

		fp_AppLaunchStateChanged(_pApplication, "Launched", CAppManagerInterface::EStatusSeverity_None);

		co_return CAppLaunchResult{};
	}
}
