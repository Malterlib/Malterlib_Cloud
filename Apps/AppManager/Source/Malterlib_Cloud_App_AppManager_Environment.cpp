// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Encoding/JsonShortcuts>
#include <Mib/CommandLine/TableRenderer>
#include <Mib/Cryptography/RandomID>

#include <Mib/Concurrency/LogError>

#include "Malterlib_Cloud_App_AppManager.h"

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
				"See --application-enable-self-update for installing agent executables for other platforms."
			}
		;
		auto SettingsOption_ParentApplication = "ParentApplication?"_o=
			{
				"Names"_o= _o["--parent-application"]
				, "Type"_o= ""
				, "Default"_o= ""
				, "Description"_o= "Name of the application whose storage confines the environment storage.\n"
				"The environment agent root and VM images live inside that application's directory, so they can be\n"
				"placed on encrypted storage by using an application with encryption storage."
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
				, "Type"_o= COneOf{"Docker", "AppleContainer", "Colima"}
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
#ifdef DPlatformFamily_Linux
				, "Default"_o= "host"
#else
				, "Default"_o= ""
#endif
				, "Description"_o= "The container network mode.\n"
				"Leave empty to use the container runtime's default network."
			}
		;
		auto SettingsOption_ContainerExtraMounts = "ContainerExtraMounts?"_o=
			{
				"Names"_o= _o["--container-extra-mounts"]
				, "Type"_o=
				{
					"*"_o= ""
				}
				, "Description"_o= "Additional bind mounts for the environment beyond the application directories.\n"
				"Maps host paths to paths inside the environment.\n"
				"Example: '{\"/opt/Data\": \"/opt/Data\"}'\n"
			}
		;
		auto SettingsOption_ContainerReadOnly = "ContainerReadOnly?"_o=
			{
				"Names"_o= _o["--container-read-only"]
				, "Type"_o= true
				, "Default"_o= false
				, "Description"_o= "Run the container with a read-only filesystem, so all writes are confined to the\n"
				"mounted storage. Writes outside the mounted paths would otherwise go to the container\n"
				"runtime's own storage, which is not confined by --parent-application."
			}
		;
		auto SettingsOption_ContainerExtraArguments = "ContainerExtraArguments?"_o=
			{
				"Names"_o= _o["--container-extra-arguments"]
				, "Default"_o= _o[]
				, "Type"_o= _o[""]
				, "Description"_o= "Additional arguments appended to the container run command."
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
					, "Description"_o=
						"Adds a launch environment.\n"
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
						, SettingsOption_MemoryLimitMB
						, SettingsOption_CPULimit
						, SettingsOption_VMImage
						, SettingsOption_VMBackend
						, SettingsOption_VMCPUCount
						, SettingsOption_VMMemoryMB
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
						, fStripDefault(SettingsOption_MemoryLimitMB)
						, fStripDefault(SettingsOption_CPULimit)
						, fStripDefault(SettingsOption_VMImage)
						, fStripDefault(SettingsOption_VMBackend)
						, fStripDefault(SettingsOption_VMCPUCount)
						, fStripDefault(SettingsOption_VMMemoryMB)
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
					, "Description"_o=
						"Pulls the newest container image for the environment and recreates the container from it.\n"
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
					, "Description"_o=
						"Prints the command that opens an interactive bash in the environment's container.\n"
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
					"Names"_o= _o["--colima-settings"]
					, "Description"_o=
						"Shows or changes the sizing of the colima virtual machine shared by all Colima environments.\n"
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
					, "Description"_o=
						"Creates a macOS guest VM image bundle and installs macOS into it from a restore image (IPSW).\n"
						"The image is created under VMImages in the AppManager root and is referenced from VM environments with --vm-image.\n"
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
		if (_Settings.m_bAutoStart)
		{
			m_bAutoStart = *_Settings.m_bAutoStart;
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
		if (_Settings.m_bContainerReadOnly)
		{
			m_bContainerReadOnly = *_Settings.m_bContainerReadOnly;
			o_ChangedSettings |= EEnvironmentSetting_ContainerReadOnly;
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
		if (m_ContainerRuntime != _Other.m_ContainerRuntime)
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
		if (m_Type == CAppManagerInterface::EEnvironmentType_Container)
		{
			if (m_ContainerImage.f_IsEmpty())
				return fError("For container environments you must specify a container image");
			else if (!m_VMImage.f_IsEmpty())
				return fError("For container environments you cannot specify a VM image");
		}
		else if (m_Type == CAppManagerInterface::EEnvironmentType_VM)
		{
			if (m_VMImage.f_IsEmpty())
				return fError("For VM environments you must specify a VM image");
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

			// An agent in a persistent container keeps running across AppManager
			// restarts and reconnects with the launch id frozen into the container, so
			// expect that launch id right away
			Environment.m_LaunchID = Environment.m_ContainerLaunchID;
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
		EnvironmentJson["MemoryLimitMB"] = Settings.m_MemoryLimitMB;
		EnvironmentJson["CPULimit"] = Settings.m_CPULimit;

		EnvironmentJson["VMImage"] = Settings.m_VMImage;
		EnvironmentJson["VMBackend"] = Settings.m_VMBackend;
		EnvironmentJson["VMCPUCount"] = Settings.m_VMCPUCount;
		EnvironmentJson["VMMemoryMB"] = Settings.m_VMMemoryMB;

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

		co_return co_await mp_State.m_StateDatabase.f_Save();
	}

	CAppManagerInterface::CEnvironmentInfo CAppManagerActor::fp_GetEnvironmentInfo(CEnvironment const &_Environment)
	{
		auto &Settings = _Environment.m_Settings;

		CAppManagerInterface::CEnvironmentInfo OutEnvironment;

		OutEnvironment.m_Status = _Environment.m_Status;
		OutEnvironment.m_StatusSeverity = _Environment.m_StatusSeverity;

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

	TCFuture<void> CAppManagerActor::fp_AddEnvironment
		(
			CStr _Name
			, CEnvironmentSettings _Settings
			, EEnvironmentSetting _ChangedSettings
			, TCFunction<void (CStr const &_Info)> _fOnInfo
			, CCallingHostInfo _CallingHostInfo
		)
	{
		auto Auditor = f_Auditor({}, _CallingHostInfo);

		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;

		Permissions["Command"] = {{"AppManager/CommandAll", "AppManager/Command/EnvironmentAdd"}};
		Permissions["Environment"] = {CPermissionQuery{"AppManager/EnvironmentAll", fg_Format("AppManager/Environment/{}", _Name)}.f_Description("Access environment {} in AppManager"_f << _Name)};

		NContainer::TCMap<NStr::CStr, bool> HasPermissions = co_await
			(
				mp_Permissions.f_HasPermissions("Add environment in AppManager", Permissions, _CallingHostInfo) % "Permission denied adding environment" % Auditor
			)
		;

		if (!HasPermissions["Command"])
			co_return Auditor.f_AccessDenied("(Environment add, command)", Permissions["Command"]);

		if (!HasPermissions["Environment"])
			co_return Auditor.f_AccessDenied("(Environment add, environment name)", Permissions["Environment"]);

		if (!CVersionManager::fs_IsValidApplicationName(_Name))
			co_return Auditor.f_Exception(fg_Format("'{}' is not a valid environment name", _Name));

		if (mp_Environments.f_FindEqual(_Name))
			co_return Auditor.f_Exception(fg_Format("Environment '{}' already exists", _Name));

		CEnvironmentSettings NewSettings;
		NewSettings.f_ApplySettings(_ChangedSettings, _Settings);

		// Containers run Linux guests and VM images run macOS guests, both with the host
		// architecture, so the matching self-update agent application is the default.
		// Local environments run the AppManager's own executable when no agent application is set
		if (NewSettings.m_AgentApplication.f_IsEmpty() && NewSettings.m_Type != CAppManagerInterface::EEnvironmentType_Local)
		{
			CStr HostPlatform = DMalterlibCloudPlatform;
			CStr Architecture = HostPlatform.f_Extract(HostPlatform.f_FindReverse("-") + 1);

			if (NewSettings.m_Type == CAppManagerInterface::EEnvironmentType_Container)
				NewSettings.m_AgentApplication = "SelfUpdate.Linux-{}"_f << Architecture;
			else
				NewSettings.m_AgentApplication = "SelfUpdate.macOS-{}"_f << Architecture;
		}

		{
			CStr Error;
			if (!NewSettings.f_Validate(Error))
				co_return Auditor.f_Exception(Error);
		}

		if (!NewSettings.m_ParentApplication.f_IsEmpty() && !mp_Applications.f_FindEqual(NewSettings.m_ParentApplication))
			co_return Auditor.f_Exception(fg_Format("Parent application '{}' does not exist", NewSettings.m_ParentApplication));

		auto pEnvironment = mp_Environments[_Name] = fg_Construct(_Name, this);
		pEnvironment->m_Settings = NewSettings;

		co_await (fp_RegisterEnvironmentPermissions(pEnvironment) % "Failed to register environment permissions" % Auditor);

		_fOnInfo("Saving environment state");
		co_await (fp_UpdateEnvironmentJson(pEnvironment) % "Failed to save environment state" % Auditor);

		_fOnInfo(fg_Format("Environment '{}' was successfully added", _Name));
		Auditor.f_Info(fg_Format("Added environment '{}'", _Name));

		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_RemoveEnvironment(CStr _Name, TCFunction<void (CStr const &_Info)> _fOnInfo, CCallingHostInfo _CallingHostInfo)
	{
		auto Auditor = f_Auditor({}, _CallingHostInfo);

		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;

		Permissions["Command"] = {{"AppManager/CommandAll", "AppManager/Command/EnvironmentRemove"}};
		Permissions["Environment"] = {CPermissionQuery{"AppManager/EnvironmentAll", fg_Format("AppManager/Environment/{}", _Name)}.f_Description("Access environment {} in AppManager"_f << _Name)};

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

		co_await (fp_StopEnvironmentInternal(pEnvironment) % "Failed to stop environment" % Auditor);

		if (pEnvironment->m_Settings.m_Type == CAppManagerInterface::EEnvironmentType_Container)
			co_await fp_RemoveEnvironmentContainer(pEnvironment);

		pEnvironment->m_bDeleted = true;
		mp_Environments.f_Remove(_Name);

		if (auto *pEnvironmentsState = mp_State.m_StateDatabase.m_Data.f_GetMember("Environments"))
		{
			if (pEnvironmentsState->f_GetMember(_Name))
				pEnvironmentsState->f_RemoveMember(_Name);
		}

		co_await (mp_State.m_StateDatabase.f_Save() % "Failed to save state" % Auditor);

		fp_UnregisterEnvironmentPermissions(pEnvironment).f_DiscardResult();

		_fOnInfo(fg_Format("Environment '{}' was successfully removed", _Name));
		Auditor.f_Info(fg_Format("Removed environment '{}'", _Name));

		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_ChangeEnvironmentSettings
		(
			CStr _Name
			, CEnvironmentSettings _Settings
			, EEnvironmentSetting _ChangedSettings
			, TCFunction<void (CStr const &_Info)> _fOnInfo
			, CCallingHostInfo _CallingHostInfo
		)
	{
		auto Auditor = f_Auditor({}, _CallingHostInfo);

		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;

		Permissions["Command"] = {{"AppManager/CommandAll", "AppManager/Command/EnvironmentChangeSettings"}};
		Permissions["Environment"] = {CPermissionQuery{"AppManager/EnvironmentAll", fg_Format("AppManager/Environment/{}", _Name)}.f_Description("Access environment {} in AppManager"_f << _Name)};

		NContainer::TCMap<NStr::CStr, bool> HasPermissions = co_await
			(
				mp_Permissions.f_HasPermissions("Change environment settings in AppManager", Permissions, _CallingHostInfo) % "Permission denied changing environment settings" % Auditor
			)
		;

		if (!HasPermissions["Command"])
			co_return Auditor.f_AccessDenied("(Environment change settings, command)", Permissions["Command"]);

		if (!HasPermissions["Environment"])
			co_return Auditor.f_AccessDenied("(Environment change settings, environment name)", Permissions["Environment"]);

		auto *pFindEnvironment = mp_Environments.f_FindEqual(_Name);
		if (!pFindEnvironment)
			co_return Auditor.f_Exception(fg_Format("No such environment '{}'", _Name));

		auto pEnvironment = *pFindEnvironment;

		auto NewSettings = pEnvironment->m_Settings;
		NewSettings.f_ApplySettings(_ChangedSettings, _Settings);

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

		if (ChangedSettings & EEnvironmentSetting_ParentApplication)
		{
			if (!NewSettings.m_ParentApplication.f_IsEmpty() && !mp_Applications.f_FindEqual(NewSettings.m_ParentApplication))
				co_return Auditor.f_Exception(fg_Format("Parent application '{}' does not exist", NewSettings.m_ParentApplication));

			if (pEnvironment->f_IsStarted() || pEnvironment->m_bStarting)
				co_return Auditor.f_Exception("Stop the environment before changing the parent application, because it moves the environment storage");
		}

		pEnvironment->m_Settings = NewSettings;

		_fOnInfo("Saving environment state");
		co_await (fp_UpdateEnvironmentJson(pEnvironment) % "Failed to save environment state" % Auditor);

		_fOnInfo("Environment settings were successfully changed");
		Auditor.f_Info(fg_Format("Updated settings for environment '{}'", _Name));

		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_StartEnvironment(CStr _Name, CCallingHostInfo _CallingHostInfo)
	{
		auto Auditor = f_Auditor({}, _CallingHostInfo);

		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;

		Permissions["Command"] = {{"AppManager/CommandAll", "AppManager/Command/EnvironmentStart"}};
		Permissions["Environment"] = {CPermissionQuery{"AppManager/EnvironmentAll", fg_Format("AppManager/Environment/{}", _Name)}.f_Description("Access environment {} in AppManager"_f << _Name)};

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

		co_await (fp_EnsureEnvironmentStarted(*pFindEnvironment) % "Failed to start environment" % Auditor);

		// Relaunch the applications a previous environment stop stopped with the
		// auto start flag
		fp_UpdateApplicationDependencies();

		Auditor.f_Info(fg_Format("Started environment '{}'", _Name));

		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_PullEnvironment(CStr _Name, CCallingHostInfo _CallingHostInfo)
	{
		auto Auditor = f_Auditor({}, _CallingHostInfo);

		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;

		Permissions["Command"] = {{"AppManager/CommandAll", "AppManager/Command/EnvironmentPull"}};
		Permissions["Environment"] = {CPermissionQuery{"AppManager/EnvironmentAll", fg_Format("AppManager/Environment/{}", _Name)}.f_Description("Access environment {} in AppManager"_f << _Name)};

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

		bool bWasStarted = pEnvironment->f_IsStarted() || pEnvironment->m_bStarting;

		co_await (fp_EnsureContainerSystem(pEnvironment) % "Failed to start the container system" % Auditor);

		co_await (fp_StopEnvironmentInternal(pEnvironment) % "Failed to stop environment" % Auditor);

		co_await fp_RemoveEnvironmentContainer(pEnvironment);

		co_await (fp_PullEnvironmentContainerImage(pEnvironment) % "Failed to pull environment image" % Auditor);

		// The container is recreated from the pulled image on the next start
		pEnvironment->m_ContainerFingerprint = {};

		co_await (fp_UpdateEnvironmentJson(pEnvironment) % "Failed to save environment state" % Auditor);

		if (bWasStarted)
		{
			co_await (fp_EnsureEnvironmentStarted(pEnvironment) % "Failed to start environment" % Auditor);

			// Relaunch the applications the environment stop stopped with the auto
			// start flag
			fp_UpdateApplicationDependencies();
		}

		Auditor.f_Info(fg_Format("Pulled environment '{}'", _Name));

		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_StopEnvironment(CStr _Name, CCallingHostInfo _CallingHostInfo)
	{
		auto Auditor = f_Auditor({}, _CallingHostInfo);

		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;

		Permissions["Command"] = {{"AppManager/CommandAll", "AppManager/Command/EnvironmentStop"}};
		Permissions["Environment"] = {CPermissionQuery{"AppManager/EnvironmentAll", fg_Format("AppManager/Environment/{}", _Name)}.f_Description("Access environment {} in AppManager"_f << _Name)};

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

		co_await (fp_StopEnvironmentInternal(*pFindEnvironment) % "Failed to stop environment" % Auditor);

		Auditor.f_Info(fg_Format("Stopped environment '{}'", _Name));

		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_RestartEnvironment(CStr _Name, CCallingHostInfo _CallingHostInfo)
	{
		auto Auditor = f_Auditor({}, _CallingHostInfo);

		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;

		Permissions["Command"] = {{"AppManager/CommandAll", "AppManager/Command/EnvironmentRestart"}};
		Permissions["Environment"] = {CPermissionQuery{"AppManager/EnvironmentAll", fg_Format("AppManager/Environment/{}", _Name)}.f_Description("Access environment {} in AppManager"_f << _Name)};

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

		co_await (fp_StopEnvironmentInternal(pEnvironment) % "Failed to stop environment" % Auditor);
		co_await (fp_EnsureEnvironmentStarted(pEnvironment) % "Failed to start environment" % Auditor);

		Auditor.f_Info(fg_Format("Restarted environment '{}'", _Name));

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

		for (auto &pEnvironment : pThis->mp_Environments)
		{
			auto &Environment = *pEnvironment;

			Permissions[Environment.m_Name]
				= {CPermissionQuery{"AppManager/EnvironmentAll", fg_Format("AppManager/Environment/{}", Environment.m_Name)}.f_Description("Access environment {} in AppManager"_f << Environment.m_Name)}
			;
		}

		auto HasPermissions = co_await
			(
				pThis->mp_Permissions.f_HasPermissions("Enumerate environments in AppManager", Permissions) % "Permission denied enumerating environments" % Auditor
			)
		;

		if (!HasPermissions["//Command//"])
			co_return Auditor.f_AccessDenied("(Environment enum)", Permissions["//Command//"]);

		TCMap<CStr, CEnvironmentInfo> OutputEnvironments;
		for (auto &pEnvironment : pThis->mp_Environments)
		{
			auto &Environment = *pEnvironment;

			auto pHasPermission = HasPermissions.f_FindEqual(Environment.m_Name);
			if (!pHasPermission || !*pHasPermission)
				continue;

			OutputEnvironments[Environment.m_Name] = pThis->fp_GetEnvironmentInfo(Environment);
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

		// The AppManager-owned container systems are reached with the data location
		// their daemons were started with, which needs an elevated client
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

		// The output is meant to be executed with backticks, where the shell only
		// splits words and does not interpret quotes, so the parts are printed as
		// they are
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
		int64 CPUCount = _Params["CPUCount"].f_Integer();
		int64 MemoryMB = _Params["MemoryMB"].f_Integer();
		int64 DiskGB = _Params["DiskGB"].f_Integer();

		bool bChanged = false;
		if (CPUCount >= 0 && uint32(CPUCount) != mp_ColimaCPUCount)
		{
			mp_ColimaCPUCount = uint32(CPUCount);
			bChanged = true;
		}
		if (MemoryMB >= 0 && uint64(MemoryMB) != mp_ColimaMemoryMB)
		{
			mp_ColimaMemoryMB = uint64(MemoryMB);
			bChanged = true;
		}
		if (DiskGB >= 0 && uint64(DiskGB) != mp_ColimaDiskGB)
		{
			mp_ColimaDiskGB = uint64(DiskGB);
			bChanged = true;
		}

		auto fFormatValue = [](uint64 _Value) -> CStr
			{
				if (!_Value)
					return "default";

				return CStr::fs_ToStr(_Value);
			}
		;

		co_await _pCommandLine->f_StdOut
			(
				"CPU count: {}\nMemory MB: {}\nDisk GB: {}\n"_f
					<< fFormatValue(mp_ColimaCPUCount)
					<< fFormatValue(mp_ColimaMemoryMB)
					<< fFormatValue(mp_ColimaDiskGB)
			)
		;

		if (!bChanged)
			co_return 0;

		{
			auto &ColimaJson = mp_State.m_StateDatabase.m_Data["ColimaSettings"];
			ColimaJson["CPUCount"] = int64(mp_ColimaCPUCount);
			ColimaJson["MemoryMB"] = int64(mp_ColimaMemoryMB);
			ColimaJson["DiskGB"] = int64(mp_ColimaDiskGB);
		}

		co_await mp_State.m_StateDatabase.f_Save();

		// Restart the running colima environments; the first start after the change
		// restarts the virtual machine with the new configuration
		TCVector<TCSharedPointer<CEnvironment>> RestartEnvironments;
		for (auto &pEnvironment : mp_Environments)
		{
			if (!fp_UseOwnColimaSystem(pEnvironment))
				continue;

			if (!pEnvironment->f_IsStarted() && !pEnvironment->m_bStarting)
				continue;

			RestartEnvironments.f_Insert(fg_TempCopy(pEnvironment));
		}

		for (auto &pEnvironment : RestartEnvironments)
		{
			co_await _pCommandLine->f_StdOut("Restarting environment '{}'\n"_f << pEnvironment->m_Name);

			co_await fp_StopEnvironmentInternal(pEnvironment);
			co_await fp_EnsureEnvironmentStarted(pEnvironment);
		}

		if (!RestartEnvironments.f_IsEmpty())
		{
			// Relaunch the applications the environment stops stopped with the auto
			// start flag
			fp_UpdateApplicationDependencies();
		}

		co_return 0;
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
			}

			if (Environment.m_MemoryLimitMB != 0)
				fAddProperty(Settings, "Memory limit MB", Environment.m_MemoryLimitMB);
			if (Environment.m_CPULimit != 0.0)
				fAddProperty(Settings, "CPU limit", Environment.m_CPULimit);

			CStr Status;
			if (Environment.m_StatusSeverity == CAppManagerInterface::EStatusSeverity_Error)
				Status = AnsiEncoding.f_StatusError(Environment.m_Status);
			else if (Environment.m_StatusSeverity == CAppManagerInterface::EStatusSeverity_Warning)
				Status = AnsiEncoding.f_StatusWarning(Environment.m_Status);
			else
				Status = AnsiEncoding.f_StatusNormal(Environment.m_Status);

			TableRenderer.f_AddRow
				(
					EnvironmentName
					, CAppManagerInterface::fs_EnvironmentTypeToStr(Environment.m_Type)
					, Status
					, "{vs}"_f << Environment.m_Applications
					, Settings
				)
			;
		}

		TableRenderer.f_Output(_Params);

		co_return 0;
	}

	TCFuture<CStr> CAppManagerActor::fp_GetEnvironmentAgentExecutable(TCSharedPointer<CEnvironment> _pEnvironment, TCSharedPointer<CStr> _pError)
	{
		CStr Directory;
		TCVector<CStr> Candidates;

		if (_pEnvironment->m_Settings.m_AgentApplication.f_IsEmpty())
		{
			if (_pEnvironment->m_Settings.m_Type != CAppManagerInterface::EEnvironmentType_Local)
			{
				*_pError = "Environment '{}' has no agent application configured. Set one with --agent-application."_f << _pEnvironment->m_Name;
				co_return {};
			}

			// A local environment without an agent application runs a copy of this
			// AppManager
			Candidates.f_Insert(CFile::fs_GetProgramPath());
		}
		else
		{
			auto *pFindApplication = mp_Applications.f_FindEqual(_pEnvironment->m_Settings.m_AgentApplication);
			if (!pFindApplication)
			{
				*_pError = "Agent application '{}' for environment '{}' is not installed"_f << _pEnvironment->m_Settings.m_AgentApplication << _pEnvironment->m_Name;
				co_return {};
			}

			Directory = (*pFindApplication)->f_GetDirectory();

			if (!(*pFindApplication)->m_Settings.m_Executable.f_IsEmpty())
				Candidates.f_Insert(Directory / (*pFindApplication)->m_Settings.m_Executable);
			Candidates.f_Insert(Directory / CFile::fs_GetFile(CFile::fs_GetProgramPath()));
#ifdef DPlatformFamily_Windows
			Candidates.f_Insert(Directory / "AppManager.exe");
#else
			Candidates.f_Insert(Directory / "AppManager");
#endif
		}

		CStr StorageDirectory = fp_GetEnvironmentStorageDirectory(*_pEnvironment);

		// The deployment settings written next to the agent executable identify the
		// installation as this environment's agent; the process environment cannot,
		// since it is inherited by everything running in the environment
		CStr DeploymentSettings;
		{
			CEJsonSorted SettingsJson(EJsonType_Object);
			SettingsJson["EnvironmentAgent"] = true;
			SettingsJson["EnvironmentHostID"] = mp_State.m_HostID;
			SettingsJson["EnvironmentHostName"] = fp_GetEnvironmentHostName(*_pEnvironment);
			DeploymentSettings = SettingsJson.f_ToString();
		}

		CStr Executable;
		{
			auto BlockingActorCheckout = fg_BlockingActor();

			Executable = co_await
				(
					g_Dispatch(BlockingActorCheckout) / [Candidates = fg_Move(Candidates), Directory, StorageDirectory, DeploymentSettings, _pError]() -> CStr
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

						// The agent application directory is only a source for the
						// distribution files: several environments can share the same agent
						// application and self updates replace its files, so each
						// environment runs its own copy inside its storage directory, next
						// to the agent state like the root AppManager layout. Deletions are
						// skipped since the storage also holds the state
						CStr AgentExecutable = StorageDirectory / CFile::fs_GetFile(Source);

						try
						{
							CFile::fs_CreateDirectory(StorageDirectory);

							if (Directory.f_IsEmpty())
							{
								// This AppManager's directory holds the whole host
								// installation, so only the agent files are copied
								CStr ProgramDirectory = CFile::fs_GetPath(Source);
								CStr ExecutableName = CFile::fs_GetFile(Source);

								for (auto &File : {ExecutableName, CStr("MalterlibHelper"), CFile::fs_GetFileNoExt(ExecutableName) + "VersionInfo.json"})
								{
									if (CFile::fs_FileExists(ProgramDirectory / File))
										CFile::fs_DiffCopyFileOrDirectory(ProgramDirectory / File, StorageDirectory / File, nullptr);
								}
							}
							else
							{
								CFile::fs_DiffCopyFileOrDirectory
									(
										Directory
										, StorageDirectory
										, [](CFile::EDiffCopyChange _Change, NStr::CStr const &, NStr::CStr const &, NStr::CStr const &) -> CFile::EDiffCopyChangeAction
										{
											if
												(
													_Change == CFile::EDiffCopyChange_DirectoryDeleted
													|| _Change == CFile::EDiffCopyChange_FileDeleted
													|| _Change == CFile::EDiffCopyChange_LinkDeleted
												)
											{
												return CFile::EDiffCopyChangeAction_Skip;
											}

											return CFile::EDiffCopyChangeAction_Perform;
										}
										, {"*/.home", "*/.tmp", "*/TempVersion", "*/TempVersionDownload"}
										, 0.0
									)
								;
							}

							CStr SettingsFile = StorageDirectory / "AppManagerSettings.json";
							if (!CFile::fs_FileExists(SettingsFile) || CFile::fs_ReadStringFromFile(SettingsFile, true) != DeploymentSettings)
								CFile::fs_WriteStringToFile(SettingsFile, DeploymentSettings);

							// Remove the directory earlier versions copied the agent to
							CStr LegacyAgentDirectory = StorageDirectory / ".agent";
							if (CFile::fs_FileExists(LegacyAgentDirectory))
								CFile::fs_DeleteDirectoryRecursive(LegacyAgentDirectory);
						}
						catch (CException const &_Exception)
						{
							*_pError = "Failed to copy the agent files to '{}': {}"_f << StorageDirectory << _Exception;
							return {};
						}

						return AgentExecutable;
					}
				)
			;
		}

		if (Executable.f_IsEmpty() && _pError->f_IsEmpty())
			*_pError = "Found no agent executable for environment '{}'"_f << _pEnvironment->m_Name;

		co_return Executable;
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
			return fg_Format("{}/Environment/{}", pParentApplication->f_GetDirectory(), _Environment.m_Name);

		return fg_Format("{}/Environment/{}", mp_State.m_RootDirectory, _Environment.m_Name);
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

		if (pParentApplication->f_NeedsEncryption() && !pParentApplication->f_EncryptionOpened())
		{
			o_Error = "Parent application encryption not yet opened";
			o_Severity = CAppManagerInterface::EStatusSeverity_Warning;
			return false;
		}

		return true;
	}

	void CAppManagerActor::fp_AutoStartEnvironments()
	{
		if (mp_bEnvironmentAgent)
			return;

		for (auto &pEnvironment : mp_Environments)
		{
			auto &Environment = *pEnvironment;
			if (!Environment.m_Settings.m_bAutoStart || Environment.m_Settings.m_ParentApplication.f_IsEmpty())
				continue;

			if (Environment.f_IsStarted() || Environment.m_bStarting || Environment.m_bStopping)
				continue;

			CStr Error;
			CAppManagerInterface::EStatusSeverity Severity;
			if (!fp_EnvironmentStorageReady(Environment, Error, Severity))
			{
				if (Environment.m_Status != Error)
					Environment.f_SetStatus(Error, Severity);
				continue;
			}

			fp_EnsureEnvironmentStarted(pEnvironment) > fg_LogError("Malterlib/Cloud/AppManager", "Failed to auto-start environment");
		}
	}

	TCFuture<NWeb::NHTTP::CURL> CAppManagerActor::fp_EnsureEnvironmentListen(TCSharedPointer<CEnvironment> _pEnvironment)
	{
		bool bVM = _pEnvironment->m_Settings.m_Type == CAppManagerInterface::EEnvironmentType_VM;
		bool bAppleContainer
			= _pEnvironment->m_Settings.m_Type == CAppManagerInterface::EEnvironmentType_Container
			&& fp_GetContainerRuntimeExecutable(_pEnvironment) == "container"
		;
		bool bColima
			= _pEnvironment->m_Settings.m_Type == CAppManagerInterface::EEnvironmentType_Container
			&& fp_UseOwnColimaSystem(_pEnvironment)
		;

		if (!bVM && !bAppleContainer && !bColima)
			co_return mp_State.m_LocalAddress;

		// Unix domain sockets cannot cross the virtiofs shares that containers and VMs
		// use, so guests connect to a listen address on the host side of the shared
		// network instead of the primary local address
		CStr HostAddress;
		if (bColima)
		{
			// The lima user-mode network places the host at a fixed address, published
			// to guests as host.lima.internal; containers reach it through the docker
			// bridge and the virtual machine's outbound network
			HostAddress = "192.168.5.2";
		}
		else if (bAppleContainer)
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
		else
		{
			// The virtualization framework NAT network is the macOS shared network; its
			// host address is kept in the vmnet system configuration
			CProcessLaunchParams LaunchParams = CProcessLaunchParams::fs_LaunchExecutable
				(
					"/usr/bin/defaults"
					, fg_CreateVector<CStr>("read", "/Library/Preferences/SystemConfiguration/com.apple.vmnet", "Shared_Net_Address")
					, mp_State.m_RootDirectory
					, {}
				)
			;

			CProcessLaunchActor::CSimpleLaunch SimpleLaunch(LaunchParams, CProcessLaunchActor::ESimpleLaunchFlag_None);
			SimpleLaunch.m_LogName = "Environment/{}/VMNetAddress"_f << _pEnvironment->m_Name;

			auto Result = co_await CProcessLaunchActor::fs_LaunchSimple(SimpleLaunch).f_Wrap();
			if (Result && Result->m_ExitCode == 0)
				HostAddress = CStr(Result->f_GetStdOut().f_Trim());
			if (HostAddress.f_IsEmpty())
				HostAddress = "192.168.64.1";
		}

		for (aint Attempt = 0;; ++Attempt)
		{
			if (!_pEnvironment->m_ListenPort)
			{
				CStr Random = fg_RandomID();
				uint32 Seed = 0;
				ch8 const *pRandom = Random.f_GetStr();
				for (umint i = 0; i < Random.f_GetLen(); ++i)
					Seed = Seed * 31 + uint32(uint8(pRandom[i]));

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

			// Remove a listen bound directly to the shared network host address left
			// behind by earlier versions; it cannot bind while the network is inactive
			{
				CDistributedActorTrustManager_Address DirectAddress;
				DirectAddress.m_URL = AgentUrl;

				bool bHasDirectListen = co_await mp_State.m_TrustManager(&CDistributedActorTrustManager::f_HasListen, DirectAddress);
				if (bHasDirectListen)
					co_await mp_State.m_TrustManager(&CDistributedActorTrustManager::f_RemoveListen, DirectAddress);
			}

			CDistributedActorTrustManager_Address Address;
			Address.m_URL = ListenUrl;

			bool bHasListen = co_await mp_State.m_TrustManager(&CDistributedActorTrustManager::f_HasListen, Address);
			if (bHasListen)
				co_return AgentUrl;

			auto AddResult = co_await mp_State.m_TrustManager(&CDistributedActorTrustManager::f_AddListen, Address).f_Wrap();
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

				co_await fp_UpdateEnvironmentJson(_pEnvironment).f_Wrap()
					> fg_LogError("Malterlib/Cloud/AppManager", "Failed to save environment listen state")
				;

				co_return AgentUrl;
			}

			if (Attempt == 2)
				co_return DMibErrorInstance("Failed to add environment listen '{}': {}"_f << ListenUrl.f_Encode() << AddResult.f_GetExceptionStr());

			// The chosen port can collide with another service; retry with a new one
			_pEnvironment->m_ListenPort = 0;
		}
	}

	TCFuture<void> CAppManagerActor::fp_EnsureEnvironmentStarted(TCSharedPointer<CEnvironment> _pEnvironment)
	{
		auto CheckDestroy = co_await f_CheckDestroyedOnResume();

		if (_pEnvironment->f_IsStarted())
			co_return {};

		if (_pEnvironment->m_bStarting)
		{
			co_await _pEnvironment->m_OnAgentConnected.f_Insert().f_Future();
			co_return {};
		}

		co_return co_await fp_StartEnvironmentInternal(_pEnvironment);
	}

	TCFuture<void> CAppManagerActor::fp_StartEnvironmentInternal(TCSharedPointer<CEnvironment> _pEnvironment)
	{
		auto CheckDestroy = co_await f_CheckDestroyedOnResume();

		if (_pEnvironment->m_bDeleted)
			co_return DMibErrorInstance("Environment has been deleted");

		if (_pEnvironment->f_IsStarted())
			co_return {};

		// The starting flag must be set before the first suspension: a concurrent start
		// would otherwise slip past the guard and attach to the same container twice
		if (_pEnvironment->m_bStarting)
		{
			co_await _pEnvironment->m_OnAgentConnected.f_Insert().f_Future();
			co_return {};
		}

		{
			CStr Error;
			CAppManagerInterface::EStatusSeverity Severity;
			if (!fp_EnvironmentStorageReady(*_pEnvironment, Error, Severity))
			{
				_pEnvironment->f_SetStatus(Error, Severity);
				co_return DMibErrorInstance("Cannot start environment '{}': {}"_f << _pEnvironment->m_Name << Error);
			}
		}

		bool bContainer = _pEnvironment->m_Settings.m_Type == CAppManagerInterface::EEnvironmentType_Container;
		bool bVM = _pEnvironment->m_Settings.m_Type == CAppManagerInterface::EEnvironmentType_VM;

		_pEnvironment->m_bStarting = true;
		_pEnvironment->m_bStopping = false;

		bool bConnected = false;
		auto Cleanup = g_OnScopeExit / [&, _pEnvironment]
			{
				_pEnvironment->m_bStarting = false;
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

		_pEnvironment->f_SetStatus("Starting", CAppManagerInterface::EStatusSeverity_Warning);

		TCSharedPointer<CStr> pError = fg_Construct();
		CStr AgentExecutable = co_await fp_GetEnvironmentAgentExecutable(_pEnvironment, pError);
		if (AgentExecutable.f_IsEmpty())
		{
			_pEnvironment->f_SetStatus(*pError, CAppManagerInterface::EStatusSeverity_Error);
			co_return DMibErrorInstance(*pError);
		}

		CStr AgentRootDirectory = fp_GetEnvironmentStorageDirectory(*_pEnvironment);

		{
			auto BlockingActorCheckout = fg_BlockingActor();

			co_await
				(
					(
						g_Dispatch(BlockingActorCheckout) / [AgentRootDirectory]()
						{
							CFile::fs_CreateDirectory(AgentRootDirectory);
						}
					)
					% "Failed to create environment agent root directory"
				)
			;
		}

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
			}

			// Everything a colima environment writes through its mounts is written on
			// the host by the colima user, so it must own the environment storage
			if (fp_UseOwnColimaSystem(_pEnvironment))
			{
				auto Result = co_await fp_EnsureColimaOwnership(AgentRootDirectory).f_Wrap();
				if (!Result)
				{
					_pEnvironment->f_SetStatus(Result.f_GetExceptionStr(), CAppManagerInterface::EStatusSeverity_Error);
					co_return Result.f_GetException();
				}
			}

			{
				auto Result = co_await fp_EnsureEnvironmentListen(_pEnvironment).f_Wrap();
				if (!Result)
				{
					_pEnvironment->f_SetStatus(Result.f_GetExceptionStr(), CAppManagerInterface::EStatusSeverity_Error);
					co_return Result.f_GetException();
				}

				AgentAddress = *Result;
			}

			LaunchExecutable = fp_GetContainerRuntimeExecutable(_pEnvironment);
			CAppManagerContainerLaunch ContainerLaunch = fp_BuildEnvironmentContainerLaunch(_pEnvironment, AgentExecutable, AgentRootDirectory);
			TCVector<CStr> RunArguments = fg_AppManager_BuildContainerRunArguments(ContainerLaunch);

			// The container keeps its creation-time process environment, so it can only be
			// reused while everything that shaped it is unchanged; the fingerprint covers
			// the full run command plus the values forwarded through the environment
			CStr FingerprintData = LaunchExecutable;
			for (auto &Argument : RunArguments)
				FingerprintData += "\n" + Argument;
			FingerprintData += "\n" + AgentAddress.f_Encode();
			FingerprintData += "\n" + mp_State.m_HostID;
			CStr Fingerprint = NCryptography::CHash_SHA256::fs_DigestFromData(FingerprintData.f_GetStr(), FingerprintData.f_GetLen()).f_GetString();

			bool bReuse
				= Fingerprint == _pEnvironment->m_ContainerFingerprint
				&& !_pEnvironment->m_ContainerLaunchID.f_IsEmpty()
				&& co_await fp_EnvironmentContainerExists(_pEnvironment)
			;

			if (_pEnvironment->m_bDeleted)
				co_return DMibErrorInstance("Environment has been deleted");

			if (bReuse)
			{
				// Stop a still-running instance left behind by an earlier AppManager, then
				// restart it with attached standard streams for the ticket handshake. The
				// container filesystem survives, keeping OS updates made inside it.
				co_await fp_StopEnvironmentContainer(_pEnvironment);

				LaunchParameters = fg_CreateVector<CStr>("start", "--attach", "--interactive", fp_GetContainerName(_pEnvironment));
			}
			else
			{
				// Recreating the container resets its filesystem to the image, so take the
				// chance to pull the newest image for the reference first
				co_await fp_RemoveEnvironmentContainer(_pEnvironment);

				co_await fp_PullEnvironmentContainerImage(_pEnvironment).f_Wrap()
					> fg_LogError("Malterlib/Cloud/AppManager", "Failed to pull environment container image")
				;

				_pEnvironment->m_ContainerLaunchID = fg_RandomID();
				_pEnvironment->m_ContainerRequestTicketMagic = fg_RandomID();
				_pEnvironment->m_ContainerFingerprint = Fingerprint;
				_pEnvironment->m_ContainerMounts.f_Clear();
				for (auto &Mount : ContainerLaunch.m_Mounts)
					_pEnvironment->m_ContainerMounts.f_Insert(fg_TempCopy(ContainerLaunch.m_Mounts.fs_GetKey(Mount)));

				co_await fp_UpdateEnvironmentJson(_pEnvironment).f_Wrap()
					> fg_LogError("Malterlib/Cloud/AppManager", "Failed to save environment container state")
				;

				LaunchParameters = fg_Move(RunArguments);
			}

			if (_pEnvironment->m_bDeleted)
				co_return DMibErrorInstance("Environment has been deleted");

			_pEnvironment->m_LaunchID = _pEnvironment->m_ContainerLaunchID;
		}

		bool bOwnAppleContainerSystem = bContainer && LaunchExecutable == "container" && fp_UseOwnAppleContainerSystem();

		CProcessLaunchActor::CLaunch Launch = CProcessLaunchParams::fs_LaunchExecutable
			(
				LaunchExecutable
				, LaunchParameters
				, AgentRootDirectory
				, [this, _pEnvironment, LaunchedPromise = LaunchedPromise.m_Promise](CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)
				{
					if (_pEnvironment->m_bDeleted)
						return;

					switch (_State.f_GetTypeID())
					{
					case NProcess::EProcessLaunchState_Launched:
						break;
					case NProcess::EProcessLaunchState_LaunchFailed:
						{
							auto &LaunchError = _State.f_Get<NProcess::EProcessLaunchState_LaunchFailed>();
							_pEnvironment->f_SetStatus(fg_Format("Failed to launch agent: {}", LaunchError), CAppManagerInterface::EStatusSeverity_Error);

							if (!LaunchedPromise.f_IsSet())
								LaunchedPromise.f_SetException(DMibErrorInstance(LaunchError));

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
			LaunchParams.m_Environment["HOME"] = AgentRootDirectory + "/.home";
			LaunchParams.m_Environment["TMPDIR"] = AgentRootDirectory + "/.tmp";
		}
		if (bOwnAppleContainerSystem)
			fp_ApplyAppleContainerLaunchEnvironment(LaunchParams);
		else if (bContainer && fp_UseOwnColimaSystem(_pEnvironment))
			fp_ApplyColimaLaunchEnvironment(LaunchParams);
		LaunchParams.m_bMergeEnvironment = true;
		LaunchParams.m_bCreateNewProcessGroup = true;
		LaunchParams.m_bShowLaunched = false;

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

		auto LaunchSubscription = co_await _pEnvironment->m_AgentLaunch
			(
				&CProcessLaunchActor::f_Launch
				, Launch
				, fg_ThisActor(this)
			)
			.f_Wrap()
		;

		if (!LaunchSubscription)
		{
			_pEnvironment->f_SetStatus(fg_Format("Failed to launch agent: {}", LaunchSubscription.f_GetExceptionStr()), CAppManagerInterface::EStatusSeverity_Error);

			if (_pEnvironment->m_AgentLaunch)
			{
				co_await fg_Move(_pEnvironment->m_AgentLaunch).f_Destroy().f_Wrap()
					> fg_LogError("Malterlib/Cloud/AppManager", "Failed to destroy environment agent launch")
				;
			}

			co_return LaunchSubscription.f_GetException();
		}

		_pEnvironment->m_AgentLaunchSubscription = fg_Move(*LaunchSubscription);

		// The agent can already have connected while the launch was being set up
		if (!_pEnvironment->f_IsStarted())
		{
			// Wait until the agent has connected and its environment interface has been registered.
			// The launch promise resolves the wait early when the agent fails to launch or exits.
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

			auto ConnectedResult = co_await fg_Move(ConnectedFuture)
				.f_Timeout(120.0, "Timed out waiting for the environment agent to connect")
				.f_Wrap()
			;

			if (!ConnectedResult)
			{
				_pEnvironment->f_SetStatus(fg_Format("Agent failed to connect: {}", ConnectedResult.f_GetExceptionStr()), CAppManagerInterface::EStatusSeverity_Error);
				fp_StopEnvironmentInternal(_pEnvironment).f_DiscardResult();
				co_return ConnectedResult.f_GetException();
			}
		}
		else
			fg_Move(LaunchedPromise.m_Future).f_DiscardResult();

		bConnected = true;

		co_return {};
	}

	CStr CAppManagerActor::fp_GetEnvironmentHostName(CEnvironment const &_Environment)
	{
		CStr HostName = "{}-{}"_f << NProcess::NPlatform::fg_Process_GetComputerName() << _Environment.m_Name;

		// Keep the name a valid host name label: letters, digits and hyphens
		CStr Sanitized;
		for (ch8 const *pChar = HostName.f_GetStr(); *pChar; ++pChar)
		{
			ch8 Char = *pChar;
			bool bValid
				= (Char >= 'a' && Char <= 'z')
				|| (Char >= 'A' && Char <= 'Z')
				|| (Char >= '0' && Char <= '9')
				|| Char == '-'
			;

			ch8 Append[2] = {bValid ? Char : ch8('-'), 0};
			Sanitized += Append;
		}

		while (Sanitized.f_StartsWith("-"))
			Sanitized = Sanitized.f_Extract(1);
		while (Sanitized.f_EndsWith("-"))
			Sanitized = Sanitized.f_Left(Sanitized.f_GetLen() - 1);

		if (Sanitized.f_GetLen() > 63)
			Sanitized = Sanitized.f_Left(63);

		return Sanitized;
	}

	TCFuture<void> CAppManagerActor::fp_RestartEnvironmentWhenIdle(TCSharedPointer<CEnvironment> _pEnvironment)
	{
		auto CheckDestroy = co_await f_CheckDestroyedOnResume();

		// A restart in the middle of an update operation on an application would
		// corrupt it, so the restart waits until the environment applications are idle
		CStopwatch Stopwatch{true};
		while (true)
		{
			if (_pEnvironment->m_bDeleted || _pEnvironment->m_bStopping)
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

			if (!bInProgress)
				break;

			if (Stopwatch.f_GetTime() > 1_hours)
				co_return DMibErrorInstance("Aborted the restart of environment '{}': operations kept its applications busy for an hour"_f << _pEnvironment->m_Name);

			co_await fg_Timeout(10.0);
		}

		DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Restarting environment '{}'", _pEnvironment->m_Name);

		co_await fp_StopEnvironmentInternal(_pEnvironment);
		co_await fp_EnsureEnvironmentStarted(_pEnvironment);

		// Relaunch the applications the environment stop stopped with the auto
		// start flag
		fp_UpdateApplicationDependencies();

		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_OnEnvironmentAgentConnected(TCSharedPointer<CEnvironment> _pEnvironment, TCDistributedActorInterface<CAppManagerEnvironmentInterface> _Interface)
	{
		if (_pEnvironment->m_bDeleted || _pEnvironment->m_bStopping)
			co_return {};

		_pEnvironment->m_AgentInterface = fg_Move(_Interface);

		// The agent names the environment host after this host and monitors the
		// environment with the update settings inherited from this AppManager
		{
			CAppManagerEnvironmentInterface::CAgentConfig AgentConfig;
			AgentConfig.m_HostName = fp_GetEnvironmentHostName(*_pEnvironment);

			if (auto pAutoUpdate = mp_State.m_ConfigDatabase.m_Data.f_GetMember("AutoUpdate", EJsonType_Object))
				AgentConfig.m_AutoUpdateConfig = CEJsonSorted::fs_FromCompatible(*pAutoUpdate).f_ToString();

			// The agent installs the OS dependencies of its own application inside
			// the environment
			if (auto *pFindAgentApplication = mp_Applications.f_FindEqual(_pEnvironment->m_Settings.m_AgentApplication))
				AgentConfig.m_OSDependencies = (*pFindAgentApplication)->m_Settings.m_OSDependencies;

			_pEnvironment->m_AgentInterface.f_CallActor(&CAppManagerEnvironmentInterface::f_ConfigureAgent)(fg_Move(AgentConfig))
				> fg_LogError("Malterlib/Cloud/AppManager", "Failed to configure the environment agent")
			;
		}

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

		// An agent that disappears mid-update must not keep preventing reboots
		_pEnvironment->m_AgentUpdateInProgress = {};

		if (!_pEnvironment->m_bStopping && bWasStarted)
			_pEnvironment->f_SetStatus("Agent disconnected", CAppManagerInterface::EStatusSeverity_Error);

		for (auto &pApplication : mp_Applications)
		{
			if (pApplication->m_pLaunchedEnvironment != _pEnvironment)
				continue;

			pApplication->m_pLaunchedEnvironment = nullptr;
			pApplication->f_Clear();

			if (!_pEnvironment->m_bStopping && !pApplication->m_bStopped && !pApplication->m_bDeleted)
			{
				fp_AppLaunchStateChanged
					(
						pApplication
						, "Environment '{}' stopped unexpectedly. Waiting to retry launching."_f << _pEnvironment->m_Name
						, CAppManagerInterface::EStatusSeverity_Error
					)
				;
				fp_ScheduleRelaunchApp(pApplication);
			}
		}
	}

	TCFuture<void> CAppManagerActor::fp_StopEnvironmentInternal(TCSharedPointer<CEnvironment> _pEnvironment)
	{
		if (_pEnvironment->m_bStopping)
			co_return {};

		_pEnvironment->m_bStopping = true;

		auto Cleanup = g_OnScopeExit / [_pEnvironment]
			{
				_pEnvironment->m_bStopping = false;
			}
		;

		// Stop applications launched in the environment first
		{
			TCFutureVector<uint32> ApplicationStops;
			for (auto &pApplication : mp_Applications)
			{
				if (pApplication->m_pLaunchedEnvironment == _pEnvironment)
					pApplication->f_Stop(EStopFlag_AutoStart) > ApplicationStops;
			}

			auto StopResults = co_await fg_AllDoneWrapped(ApplicationStops);
			for (auto &StopResult : StopResults)
			{
				if (!StopResult)
					DMibLogWithCategory(Malterlib/Cloud/AppManager, Warning, "Failed to stop application in environment '{}': {}", _pEnvironment->m_Name, StopResult.f_GetExceptionStr());
			}
		}

		_pEnvironment->m_bStarted = false;

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

			co_await fg_AllDone(InterfaceDestroys).f_Wrap()
				> fg_LogError("Malterlib/Cloud/AppManager", "Failed to destroy environment agent interfaces")
			;
		}

		_pEnvironment->m_AgentHostID = {};

		// Stop the container through the runtime first: the attached client then exits by
		// itself. Signalling the client instead does not stop the container; the Apple
		// container client cannot even forward the signal, which leaves it running until
		// the stop escalates to killing it
		if (_pEnvironment->m_Settings.m_Type == CAppManagerInterface::EEnvironmentType_Container && _pEnvironment->m_AgentLaunch)
			co_await fp_StopEnvironmentContainer(_pEnvironment);

		if (_pEnvironment->m_AgentLaunch)
		{
			auto StopResult = co_await fg_TempCopy(_pEnvironment->m_AgentLaunch)(&CProcessLaunchActor::f_StopProcess).f_Wrap();
			if (!StopResult)
				DMibLogWithCategory(Malterlib/Cloud/AppManager, Warning, "Failed to stop agent for environment '{}': {}", _pEnvironment->m_Name, StopResult.f_GetExceptionStr());

			co_await fg_Move(_pEnvironment->m_AgentLaunch).f_Destroy().f_Wrap()
				> fg_LogError("Malterlib/Cloud/AppManager", "Failed to destroy environment agent launch")
			;
		}

		_pEnvironment->m_AgentLaunchSubscription.f_Clear();

		if (_pEnvironment->m_VMActor)
		{
			co_await fg_TempCopy(_pEnvironment->m_VMActor)(&NVirtualization::CVirtualMachineActor::f_Stop).f_Wrap()
				> fg_LogError("Malterlib/Cloud/AppManager", "Failed to stop environment virtual machine")
			;

			co_await fg_Move(_pEnvironment->m_VMActor).f_Destroy().f_Wrap()
				> fg_LogError("Malterlib/Cloud/AppManager", "Failed to destroy environment virtual machine")
			;
		}

		// The container is stopped but kept, so its filesystem (including OS updates
		// made inside it) survives across environment restarts
		if (_pEnvironment->m_Settings.m_Type == CAppManagerInterface::EEnvironmentType_Container)
			co_await fp_StopEnvironmentContainer(_pEnvironment);

		_pEnvironment->f_SetStatus("Stopped", CAppManagerInterface::EStatusSeverity_Warning);

		co_return {};
	}

	void CAppManagerActor::fp_OnEnvironmentApplicationStateChange(CAppManagerEnvironmentInterface::CApplicationStateChange const &_Change)
	{
		auto *pFindApplication = mp_Applications.f_FindEqual(_Change.m_Application);
		if (!pFindApplication)
			return;

		auto pApplication = *pFindApplication;

		if (!pApplication->m_pLaunchedEnvironment)
			return;

		if (_Change.m_StatusSeverity == CAppManagerInterface::EStatusSeverity_Error)
		{
			// A failed launch attempt inside the environment aborts any pending wait
			// for the application to register its distributed app interface
			for (auto &Promise : pApplication->m_OnRegisterDistributedApp)
				Promise.f_SetException(DMibErrorInstance(_Change.m_Status));

			for (auto &Promise : pApplication->m_OnStartedDistributedApp)
				Promise.f_SetException(DMibErrorInstance(_Change.m_Status));

			pApplication->m_OnRegisterDistributedApp.f_Clear();
			pApplication->m_OnStartedDistributedApp.f_Clear();
		}

		fp_SetAppLaunchStatus(pApplication, _Change.m_Status, _Change.m_StatusSeverity);
	}

	TCFuture<void> CAppManagerActor::fp_AbortEnvironmentLaunch(TCSharedPointer<CApplication> _pApplication, TCSharedPointer<CEnvironment> _pEnvironment)
	{
		// Stop the application inside the environment so a later relaunch attempt
		// does not hit the already-launched check in the agent
		if (_pEnvironment->m_AgentInterface && !_pApplication->m_bDeleted)
		{
			co_await _pEnvironment->m_AgentInterface.f_CallActor(&CAppManagerEnvironmentInterface::f_StopApplication)(_pApplication->m_Name)
				.f_Timeout(60.0 * 60.0, "Timed out stopping application in environment (1 hour)")
				.f_Wrap()
				> fg_LogError("Malterlib/Cloud/AppManager", "Failed to stop application after failed environment launch")
			;
		}

		if (_pApplication->m_pLaunchedEnvironment == _pEnvironment)
			_pApplication->m_pLaunchedEnvironment = nullptr;

		_pApplication->m_bLaunched = false;

		if (!_pApplication->m_bStopped && !_pApplication->m_bDeleted)
			fp_ScheduleRelaunchApp(_pApplication);

		co_return {};
	}

	auto CAppManagerActor::fp_LaunchAppInEnvironment(TCSharedPointer<CApplication> _pApplication, TCSharedPointer<CEnvironment> _pEnvironment) -> TCFuture<CAppLaunchResult>
	{
		// A container only sees the mounts it was created with, so when the
		// application directory is not covered the environment is restarted and the
		// container recreated with the new mount set
		bool bRestartedEnvironment = false;
		if
		(
			_pEnvironment->m_Settings.m_Type == CAppManagerInterface::EEnvironmentType_Container
			&& _pEnvironment->f_IsStarted()
		)
		{
			CStr Directory = _pApplication->f_GetDirectory();

			bool bCovered = false;
			for (auto &Mount : _pEnvironment->m_ContainerMounts)
			{
				if (Directory == Mount || Directory.f_StartsWith(Mount + "/"))
				{
					bCovered = true;
					break;
				}
			}

			if (!bCovered)
			{
				DMibLogWithCategory
					(
						Malterlib/Cloud/AppManager
						, Info
						, "Restarting environment '{}' to mount the directory of application '{}'"
						, _pEnvironment->m_Name
						, _pApplication->m_Name
					)
				;

				co_await fp_StopEnvironmentInternal(_pEnvironment);
				bRestartedEnvironment = true;

				if (_pApplication->m_bDeleted)
					co_return DMibErrorInstance("Application deleted");
			}
		}

		auto EnsureResult = co_await fp_EnsureEnvironmentStarted(_pEnvironment).f_Wrap();

		if (_pApplication->m_bDeleted)
			co_return DMibErrorInstance("Application deleted");

		if (!EnsureResult)
		{
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

		// Relaunch the applications the environment restart stopped with the auto
		// start flag
		if (bRestartedEnvironment)
			fp_UpdateApplicationDependencies();

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

		// Everything the application writes through the colima mounts is written on
		// the host by the colima user, so it must own the application directory
		if (fp_UseOwnColimaSystem(_pEnvironment))
		{
			auto OwnershipResult = co_await fp_EnsureColimaOwnership(_pApplication->f_GetDirectory()).f_Wrap();

			if (_pApplication->m_bDeleted)
				co_return DMibErrorInstance("Application deleted");

			if (!OwnershipResult)
			{
				fp_AppLaunchStateChanged
					(
						_pApplication
						, "Failed to prepare directory for environment '{}': {}"_f << _pEnvironment->m_Name << OwnershipResult.f_GetExceptionStr()
						, CAppManagerInterface::EStatusSeverity_Error
					)
				;

				if (!_pApplication->m_bStopped)
					fp_ScheduleRelaunchApp(_pApplication);

				co_return OwnershipResult.f_GetException();
			}
		}

		_pApplication->m_LaunchID = fg_RandomID();

		CAppManagerEnvironmentInterface::CEnvironmentLaunch Launch;
		Launch.m_Name = _pApplication->m_Name;
		Launch.m_Directory = _pApplication->f_GetDirectory();
		Launch.m_Executable = _pApplication->m_Settings.m_Executable;
		Launch.m_Parameters = _pApplication->m_Settings.m_ExecutableParameters;
		Launch.m_RunAsUser = _pApplication->m_Settings.m_RunAsUser;
		Launch.m_RunAsGroup = _pApplication->m_Settings.m_RunAsGroup;
		Launch.m_bRunAsUserHasShell = _pApplication->m_Settings.m_bRunAsUserHasShell;
		Launch.m_bDistributedApp = _pApplication->m_Settings.m_bDistributedApp;
		Launch.m_InterfaceAddress = InterfaceAddress->f_Encode();
		Launch.m_LaunchID = _pApplication->m_LaunchID;

		auto LaunchResult = co_await _pEnvironment->m_AgentInterface.f_CallActor(&CAppManagerEnvironmentInterface::f_LaunchApplication)(fg_Move(Launch))
			.f_Timeout(60.0 * 60.0, "Timed out launching application in environment (1 hour)")
			.f_Wrap()
		;

		if (_pApplication->m_bDeleted)
			co_return DMibErrorInstance("Application deleted");

		if (!LaunchResult)
		{
			fp_AppLaunchStateChanged
				(
					_pApplication
					, "Failed launch in environment '{}': {}"_f << _pEnvironment->m_Name << LaunchResult.f_GetExceptionStr()
					, CAppManagerInterface::EStatusSeverity_Error
				)
			;

			// The agent can have launched the application even though the launch
			// call failed, so stop it in the environment before the retry
			co_await fp_AbortEnvironmentLaunch(_pApplication, _pEnvironment);

			co_return LaunchResult.f_GetException();
		}

		// An application that kept running in the environment across a restart is
		// adopted with the launch id it already runs with
		_pApplication->m_LaunchID = fg_Move(*LaunchResult);

		_pApplication->m_pLaunchedEnvironment = _pEnvironment;
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

			auto RegisterResult = co_await _pApplication->m_OnRegisterDistributedApp.f_Insert().f_Future()
				.f_Timeout(60.0 * 60.0, "Timed out waiting for application to register (1 hour)")
				.f_Wrap()
			;

			if (_pApplication->m_bDeleted)
				co_return {};

			if (!RegisterResult)
			{
				DMibLogWithCategory
					(
						Malterlib/Cloud/AppManager
						, Error
						, "Launched app '{}' failed to register: {}"
						, _pApplication->m_Name
						, RegisterResult.f_GetExceptionStr()
					)
				;

				fp_AppLaunchStateChanged
					(
						_pApplication
						, "Launched (app register failed: '{}')"_f << RegisterResult.f_GetExceptionStr()
						, CAppManagerInterface::EStatusSeverity_Error
					)
				;

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
			DMibLogWithCategory
				(
					Malterlib/Cloud/AppManager
					, Error
					, "Launched app '{}' failed to start up: {}"
					, _pApplication->m_Name
					, StartResult.f_GetExceptionStr()
				)
			;

			fp_AppLaunchStateChanged
				(
					_pApplication
					, "Launched (app startup failed: '{}')"_f << StartResult.f_GetExceptionStr()
					, CAppManagerInterface::EStatusSeverity_Error
				)
			;

			co_await fp_AbortEnvironmentLaunch(_pApplication, _pEnvironment);

			co_return {StartResult.f_GetExceptionStr()};
		}

		for (auto &fOnStartedDistributedApp : _pApplication->m_OnStartedDistributedApp)
			fOnStartedDistributedApp.f_SetResult();

		_pApplication->m_OnStartedDistributedApp.f_Clear();

		fp_AppLaunchStateChanged(_pApplication, "Launched", CAppManagerInterface::EStatusSeverity_None);

		co_return CAppLaunchResult{};
	}
}
