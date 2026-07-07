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
				, "Description"_o= "Name of the application that provides the AppManager agent executable for the environment.\n"
				"See --application-enable-self-update for installing agent executables for other platforms."
			}
		;
		auto SettingsOption_AutoStart = "AutoStart?"_o=
			{
				"Names"_o= _o["--auto-start"]
				, "Type"_o= true
				, "Description"_o= "Start the environment automatically when the AppManager starts. Defaults to true."
			}
		;
		auto SettingsOption_ContainerRuntime = "ContainerRuntime?"_o=
			{
				"Names"_o= _o["--container-runtime"]
				, "Type"_o= ""
				, "Description"_o= "The container runtime to use: Docker or AppleContainer.\n"
				"Leave empty to select the default runtime for the host platform."
			}
		;
		auto SettingsOption_ContainerImage = "ContainerImage?"_o=
			{
				"Names"_o= _o["--container-image"]
				, "Type"_o= ""
				, "Description"_o= "The container image reference to run the environment from."
			}
		;
		auto SettingsOption_ContainerNetwork = "ContainerNetwork?"_o=
			{
				"Names"_o= _o["--container-network"]
				, "Type"_o= ""
				, "Description"_o= "The container network mode.\n"
				"Leave empty to select the default network mode for the host platform and runtime."
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
		auto SettingsOption_ContainerExtraArguments = "ContainerExtraArguments?"_o=
			{
				"Names"_o= _o["--container-extra-arguments"]
				, "Type"_o= _o[""]
				, "Description"_o= "Additional arguments appended to the container run command."
			}
		;
		auto SettingsOption_MemoryLimit = "MemoryLimit?"_o=
			{
				"Names"_o= _o["--memory-limit"]
				, "Type"_o= ""
				, "Description"_o= "Memory limit for the environment, for example 512m or 4g."
			}
		;
		auto SettingsOption_CPULimit = "CPULimit?"_o=
			{
				"Names"_o= _o["--cpu-limit"]
				, "Type"_o= 0.0
				, "Description"_o= "Number of CPUs the environment may use. Set to 0 for no limit."
			}
		;
		auto SettingsOption_VMImage = "VMImage?"_o=
			{
				"Names"_o= _o["--vm-image"]
				, "Type"_o= ""
				, "Description"_o= "Name of the prepared guest image bundle to run the VM environment from."
			}
		;
		auto SettingsOption_VMBackend = "VMBackend?"_o=
			{
				"Names"_o= _o["--vm-backend"]
				, "Type"_o= ""
				, "Description"_o= "The virtualization backend to use.\n"
				"Leave empty to select the default backend for the host platform."
			}
		;
		auto SettingsOption_VMCPUCount = "VMCPUCount?"_o=
			{
				"Names"_o= _o["--vm-cpu-count"]
				, "Type"_o= 0
				, "Description"_o= "Number of CPUs for the VM. Set to 0 to use the backend default."
			}
		;
		auto SettingsOption_VMMemoryMB = "VMMemoryMB?"_o=
			{
				"Names"_o= _o["--vm-memory"]
				, "Type"_o= 0
				, "Description"_o= "Memory in megabytes for the VM. Set to 0 to use the backend default."
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
							, "Type"_o= ""
							, "Description"_o= "The environment type: Local, Container or VM.\n"
							"Defaults to Container."
						}
						, SettingsOption_AgentApplication
						, SettingsOption_AutoStart
						, SettingsOption_ContainerRuntime
						, SettingsOption_ContainerImage
						, SettingsOption_ContainerNetwork
						, SettingsOption_ContainerExtraMounts
						, SettingsOption_ContainerExtraArguments
						, SettingsOption_MemoryLimit
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
						, SettingsOption_AgentApplication
						, SettingsOption_AutoStart
						, SettingsOption_ContainerRuntime
						, SettingsOption_ContainerImage
						, SettingsOption_ContainerNetwork
						, SettingsOption_ContainerExtraMounts
						, SettingsOption_ContainerExtraArguments
						, SettingsOption_MemoryLimit
						, SettingsOption_CPULimit
						, SettingsOption_VMImage
						, SettingsOption_VMBackend
						, SettingsOption_VMCPUCount
						, SettingsOption_VMMemoryMB
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
							, "Description"_o= "Only list the environment with this name."
						}
						, "Verbose?"_o=
						{
							"Names"_o= _o["--verbose"]
							, "Default"_o= false
							, "Description"_o= "Show more details."
						}
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
					"Names"_o= _o["--vm-image-create"]
					, "Description"_o=
						"Creates a macOS guest VM image bundle and installs macOS into it from a restore image (IPSW).\n"
						"The image is created under VMImages in the AppManager root and is referenced from VM environments with --vm-image.\n"
						"Only supported on Apple silicon macOS hosts."
					, "Options"_o=
					{
						NameOption
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

		if (auto *pValue = _Params.f_GetMember("MemoryLimit"))
		{
			o_ChangedSettings |= EEnvironmentSetting_MemoryLimit;
			m_MemoryLimit = pValue->f_String();
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
			m_VMMemoryMB = (uint32)pValue->f_AsInteger();
		}

		return true;
	}

	void CAppManagerActor::CEnvironmentSettings::f_ApplySettings(EEnvironmentSetting _ChangedSettings, CEnvironmentSettings const &_Source)
	{
		if (_ChangedSettings & EEnvironmentSetting_Type)
			m_Type = _Source.m_Type;
		if (_ChangedSettings & EEnvironmentSetting_AgentApplication)
			m_AgentApplication = _Source.m_AgentApplication;
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
		if (_ChangedSettings & EEnvironmentSetting_MemoryLimit)
			m_MemoryLimit = _Source.m_MemoryLimit;
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
		if (_Settings.m_MemoryLimit)
		{
			m_MemoryLimit = *_Settings.m_MemoryLimit;
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
		if (m_MemoryLimit != _Other.m_MemoryLimit)
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

			if (auto pValue = EnvironmentJson.f_GetMember("MemoryLimit", EJsonType_String))
				Settings.m_MemoryLimit = pValue->f_String();
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
				Settings.m_VMMemoryMB = (uint32)pValue->f_Integer();
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

		EnvironmentJson["MemoryLimit"] = Settings.m_MemoryLimit;
		EnvironmentJson["CPULimit"] = Settings.m_CPULimit;

		EnvironmentJson["VMImage"] = Settings.m_VMImage;
		EnvironmentJson["VMBackend"] = Settings.m_VMBackend;
		EnvironmentJson["VMCPUCount"] = Settings.m_VMCPUCount;
		EnvironmentJson["VMMemoryMB"] = Settings.m_VMMemoryMB;

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
		OutEnvironment.m_bAutoStart = Settings.m_bAutoStart;

		OutEnvironment.m_ContainerRuntime = Settings.m_ContainerRuntime;
		OutEnvironment.m_ContainerImage = Settings.m_ContainerImage;
		OutEnvironment.m_ContainerNetwork = Settings.m_ContainerNetwork;
		OutEnvironment.m_ContainerExtraMounts = Settings.m_ContainerExtraMounts;
		OutEnvironment.m_ContainerExtraArguments = Settings.m_ContainerExtraArguments;

		OutEnvironment.m_MemoryLimit = Settings.m_MemoryLimit;
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

		{
			CStr Error;
			if (!NewSettings.f_Validate(Error))
				co_return Auditor.f_Exception(Error);
		}

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

		Auditor.f_Info(fg_Format("Started environment '{}'", _Name));

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
			fAddProperty(Settings, "Auto start", Environment.m_bAutoStart);

			if (Environment.m_Type == CAppManagerInterface::EEnvironmentType_Container)
			{
				fAddProperty(Settings, "Image", Environment.m_ContainerImage);
				fAddProperty(Settings, "Runtime", Environment.m_ContainerRuntime);
				fAddProperty(Settings, "Network", Environment.m_ContainerNetwork);
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

			if (!Environment.m_MemoryLimit.f_IsEmpty())
				fAddProperty(Settings, "Memory limit", Environment.m_MemoryLimit);
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

	CStr CAppManagerActor::fp_GetEnvironmentAgentExecutable(TCSharedPointer<CEnvironment> const &_pEnvironment, CStr &o_Error)
	{
		if (_pEnvironment->m_Settings.m_AgentApplication.f_IsEmpty())
		{
			if (_pEnvironment->m_Settings.m_Type == CAppManagerInterface::EEnvironmentType_Local)
				return CFile::fs_GetProgramPath();

			o_Error = "Environment '{}' has no agent application configured. Set one with --agent-application."_f << _pEnvironment->m_Name;
			return {};
		}

		auto *pFindApplication = mp_Applications.f_FindEqual(_pEnvironment->m_Settings.m_AgentApplication);
		if (!pFindApplication)
		{
			o_Error = "Agent application '{}' for environment '{}' is not installed"_f << _pEnvironment->m_Settings.m_AgentApplication << _pEnvironment->m_Name;
			return {};
		}

		CStr Directory = (*pFindApplication)->f_GetDirectory();

		TCVector<CStr> Candidates;
		if (!(*pFindApplication)->m_Settings.m_Executable.f_IsEmpty())
			Candidates.f_Insert((*pFindApplication)->m_Settings.m_Executable);
		Candidates.f_Insert(CFile::fs_GetFile(CFile::fs_GetProgramPath()));
#ifdef DPlatformFamily_Windows
		Candidates.f_Insert("AppManager.exe");
#else
		Candidates.f_Insert("AppManager");
#endif

		for (auto &Candidate : Candidates)
		{
			CStr Executable = Directory / Candidate;
			if (CFile::fs_FileExists(Executable))
				return Executable;
		}

		o_Error = "Found no agent executable in '{}' for environment '{}'"_f << Directory << _pEnvironment->m_Name;
		return {};
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

		bool bContainer = _pEnvironment->m_Settings.m_Type == CAppManagerInterface::EEnvironmentType_Container;
		bool bVM = _pEnvironment->m_Settings.m_Type == CAppManagerInterface::EEnvironmentType_VM;

		if (bVM)
			co_return co_await fp_StartEnvironmentVM(_pEnvironment);

		CStr Error;
		CStr AgentExecutable = fp_GetEnvironmentAgentExecutable(_pEnvironment, Error);
		if (AgentExecutable.f_IsEmpty())
		{
			_pEnvironment->f_SetStatus(Error, CAppManagerInterface::EStatusSeverity_Error);
			co_return DMibErrorInstance(Error);
		}

		_pEnvironment->m_bStarting = true;
		_pEnvironment->m_bStopping = false;
		_pEnvironment->f_SetStatus("Starting", CAppManagerInterface::EStatusSeverity_Warning);

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

		CStr AgentRootDirectory = fg_Format("{}/Environment/{}", mp_State.m_RootDirectory, _pEnvironment->m_Name);

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

		if (bContainer)
		{
			// Remove any stale container left behind by an earlier agent
			co_await fp_RemoveEnvironmentContainer(_pEnvironment);

			if (_pEnvironment->m_bDeleted)
				co_return DMibErrorInstance("Environment has been deleted");
		}

		_pEnvironment->m_LaunchID = fg_RandomID();

		TCPromiseFuturePair<void> LaunchedPromise;

		CStr LaunchExecutable = AgentExecutable;
		TCVector<CStr> LaunchParameters = {"--daemon-run-standalone"};

		if (bContainer)
		{
			LaunchExecutable = fp_GetContainerRuntimeExecutable(_pEnvironment);
			LaunchParameters = fg_AppManager_BuildContainerRunArguments(fp_BuildEnvironmentContainerLaunch(_pEnvironment, AgentExecutable, AgentRootDirectory));
		}

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
		Launch.m_LogName = fg_Format("Environment/{}", _pEnvironment->m_Name);

		auto &LaunchParams = Launch.m_Params;
		LaunchParams.m_bAllowExecutableLocate = bContainer;
		LaunchParams.m_Environment["MalterlibAppManagerEnvironmentAgentRoot"] = AgentRootDirectory;
		LaunchParams.m_Environment["MalterlibAppManagerEnvironmentHostID"] = mp_State.m_HostID;

		// The container runtime client must keep the launching environment (for example
		// the docker context configuration in HOME); the agent environment inside the
		// container is set with explicit --env values instead
		if (!bContainer)
		{
			LaunchParams.m_Environment["HOME"] = AgentRootDirectory + "/.home";
			LaunchParams.m_Environment["TMPDIR"] = AgentRootDirectory + "/.tmp";
		}
		LaunchParams.m_bMergeEnvironment = true;
		LaunchParams.m_bCreateNewProcessGroup = true;
		LaunchParams.m_bShowLaunched = false;

		_pEnvironment->m_AgentLaunch = fg_ConstructActor<CDistributedAppInterfaceLaunchActor>
			(
				mp_State.m_LocalAddress
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

	TCFuture<void> CAppManagerActor::fp_OnEnvironmentAgentConnected(TCSharedPointer<CEnvironment> _pEnvironment, TCDistributedActorInterface<CAppManagerEnvironmentInterface> _Interface)
	{
		if (_pEnvironment->m_bDeleted || _pEnvironment->m_bStopping)
			co_return {};

		_pEnvironment->m_AgentInterface = fg_Move(_Interface);

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

		if (_pEnvironment->m_Settings.m_Type == CAppManagerInterface::EEnvironmentType_Container)
			co_await fp_RemoveEnvironmentContainer(_pEnvironment);

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

		fp_SetAppLaunchStatus(pApplication, _Change.m_Status, _Change.m_StatusSeverity);
	}

	auto CAppManagerActor::fp_LaunchAppInEnvironment(TCSharedPointer<CApplication> _pApplication, TCSharedPointer<CEnvironment> _pEnvironment) -> TCFuture<CAppLaunchResult>
	{
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

		CAppManagerEnvironmentInterface::CEnvironmentLaunch Launch;
		Launch.m_Name = _pApplication->m_Name;
		Launch.m_Directory = _pApplication->f_GetDirectory();
		Launch.m_Executable = _pApplication->m_Settings.m_Executable;
		Launch.m_Parameters = _pApplication->m_Settings.m_ExecutableParameters;
		Launch.m_RunAsUser = _pApplication->m_Settings.m_RunAsUser;
		Launch.m_RunAsGroup = _pApplication->m_Settings.m_RunAsGroup;
		Launch.m_bRunAsUserHasShell = _pApplication->m_Settings.m_bRunAsUserHasShell;
		Launch.m_bDistributedApp = _pApplication->m_Settings.m_bDistributedApp;

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

			if (!_pApplication->m_bStopped)
				fp_ScheduleRelaunchApp(_pApplication);

			co_return LaunchResult.f_GetException();
		}

		_pApplication->m_pLaunchedEnvironment = _pEnvironment;
		_pApplication->m_bLaunched = true;

		fp_AppLaunchStateChanged(_pApplication, "Launched", CAppManagerInterface::EStatusSeverity_None);

		co_return CAppLaunchResult{};
	}
}
