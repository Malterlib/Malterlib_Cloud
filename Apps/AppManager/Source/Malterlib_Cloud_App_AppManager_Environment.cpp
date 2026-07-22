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
		auto EnvironmentManagement = o_CommandLine.f_AddSection("Environment Management", "Commands to manage AppManager launch environments.");

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
				"When empty, local environments run the AppManager executable itself.\n"
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
				"The environment agent root lives inside that application's directory, so it can be\n"
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
							, "Type"_o= COneOf{"Local"}
							, "Default"_o= "Local"
							, "Description"_o= "The environment type."
						}
						, SettingsOption_AgentApplication
						, SettingsOption_ParentApplication
						, SettingsOption_AutoStart
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
	}

	bool CAppManagerActor::CEnvironmentSettings::f_ParseSettings(CEJsonSorted const &_Params, EEnvironmentSetting &o_ChangedSettings, CStr &o_Error)
	{
		if (auto *pValue = _Params.f_GetMember("Type"))
		{
			auto Type = CAppManagerInterface::fs_EnvironmentTypeFromStr(pValue->f_String());
			if (!Type)
			{
				o_Error = fg_Format("'{}' is not a valid environment type. Valid types are: Local", pValue->f_String());
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

		return ChangedSettings;
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
			if (auto pValue = EnvironmentJson.f_GetMember("ParentApplication", EJsonType_String))
				Settings.m_ParentApplication = pValue->f_String();
			if (auto pValue = EnvironmentJson.f_GetMember("AutoStart", EJsonType_Boolean))
				Settings.m_bAutoStart = pValue->f_Boolean();
			if (auto pValue = EnvironmentJson.f_GetMember("StoppedManually", EJsonType_Boolean))
				Environment.m_bStoppedManually = pValue->f_Boolean();
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

		OutEnvironment.m_Type = Settings.m_Type;
		OutEnvironment.m_AgentApplication = Settings.m_AgentApplication;
		OutEnvironment.m_ParentApplication = Settings.m_ParentApplication;
		OutEnvironment.m_bAutoStart = Settings.m_bAutoStart;

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

		// The name is a directory of the environment storage, which can be on a case-insensitive file system
		for (auto &pEnvironment : mp_Environments)
		{
			if (pEnvironment->m_Name.f_CmpNoCase(_Name) == 0)
				co_return Auditor.f_Exception(fg_Format("Environment '{}' differs from environment '{}' only in case", _Name, pEnvironment->m_Name));
		}

		CEnvironmentSettings NewSettings;
		NewSettings.f_ApplySettings(_ChangedSettings, _Settings);

		if (!NewSettings.m_ParentApplication.f_IsEmpty() && !mp_Applications.f_FindEqual(NewSettings.m_ParentApplication))
			co_return Auditor.f_Exception(fg_Format("Parent application '{}' does not exist", NewSettings.m_ParentApplication));

		if (CStr Error = fp_CheckLocalEnvironmentAccounts(NewSettings); !Error.f_IsEmpty())
			co_return Auditor.f_Exception(Error);

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

		pEnvironment->m_bRemoving = true;
		auto ClearRemoving = g_OnScopeExit / [pEnvironment]
			{
				pEnvironment->m_bRemoving = false;
			}
		;

		// An environment without applications can still be installing OS packages, which a stop in the middle breaks in a
		// virtual machine image or on the host of a local environment
		co_await (fp_WaitForEnvironmentIdle(pEnvironment) % "Failed to wait for the environment operations" % Auditor);

		co_await (fp_StopEnvironmentInternal(pEnvironment) % "Failed to stop environment" % Auditor);

		pEnvironment->m_bDeleted = true;
		mp_Environments.f_Remove(_Name);

		if (auto *pEnvironmentsState = mp_State.m_StateDatabase.m_Data.f_GetMember("Environments"))
		{
			if (pEnvironmentsState->f_GetMember(_Name))
				pEnvironmentsState->f_RemoveMember(_Name);
		}

		co_await (mp_State.m_StateDatabase.f_Save() % "Failed to save state" % Auditor);

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

		auto OldSettings = pEnvironment->m_Settings;
		auto NewSettings = OldSettings;
		NewSettings.f_ApplySettings(_ChangedSettings, _Settings);

		EEnvironmentSetting ChangedSettings = pEnvironment->m_Settings.f_ChangedSettings(NewSettings);

		if (ChangedSettings == EEnvironmentSetting_None)
		{
			_fOnInfo("No settings were changed");
			co_return {};
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

		co_await (fp_SetEnvironmentStoppedManually(pEnvironment, false) % "Failed to save environment state" % Auditor);

		// The launches of the applications the stop stops are refused while it runs, and wait for this also when the restart fails
		auto UpdateDependencies = g_OnScopeExit / [this]
			{
				if (!mp_bStoppingApp)
					fp_UpdateApplicationDependencies();
			}
		;

		co_await (fp_StopEnvironmentInternal(pEnvironment) % "Failed to stop environment" % Auditor);
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

			if (!bInProgress)
				co_return {};

			if (Stopwatch.f_GetTime() > 1_hours)
				co_return DMibErrorInstance("Operations kept the applications of environment '{}' busy for an hour"_f << _pEnvironment->m_Name);

			co_await fg_Timeout(10.0);
		}
	}

	TCFuture<void> CAppManagerActor::fp_RestartEnvironmentWhenIdle(TCSharedPointer<CEnvironment> _pEnvironment)
	{
		auto CheckDestroy = co_await f_CheckDestroyedOnResume();

		// The applications prevent automatic restarts of their environment like they prevent host reboots. An application
		// can prevent the restart while the operations of the others finish, so both are checked together
		auto fRestartPrevented = [&]
			{
				for (auto &pApplication : mp_Applications)
				{
					if (pApplication->m_Settings.m_LaunchEnvironment != _pEnvironment->m_Name)
						continue;

					if (fg_IsSet(pApplication->m_PreventRebootSensorFlags, CDistributedAppSensorReporter::ESensorInfoFlag::mc_AllPreventRebootFlags))
						return true;
				}

				return false;
			}
		;

		CStopwatch Stopwatch{true};
		while (true)
		{
			if (_pEnvironment->m_bDeleted || _pEnvironment->m_bStopping || _pEnvironment->m_bStoppedManually)
				co_return {};

			if (!fRestartPrevented())
			{
				co_await (fp_WaitForEnvironmentIdle(_pEnvironment) % CStr("Aborted the restart of environment '{}'"_f << _pEnvironment->m_Name));

				if (_pEnvironment->m_bDeleted || _pEnvironment->m_bStopping || _pEnvironment->m_bStoppedManually)
					co_return {};

				if (!fRestartPrevented())
					break;
			}

			if (Stopwatch.f_GetTime() > 1_hours)
				co_return DMibErrorInstance("Aborted the restart of environment '{}': its applications prevented the restart for an hour"_f << _pEnvironment->m_Name);

			co_await fg_Timeout(10.0);
		}

		DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Restarting environment '{}'", _pEnvironment->m_Name);

		// The launches of the applications the stop stops are refused while it runs, and wait for this also when the restart fails
		auto UpdateDependencies = g_OnScopeExit / [this]
			{
				if (!mp_bStoppingApp)
					fp_UpdateApplicationDependencies();
			}
		;

		co_await fp_StopEnvironmentInternal(_pEnvironment);

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

	TCFuture<CStr> CAppManagerActor::fp_GetEnvironmentAgentExecutable(TCSharedPointer<CEnvironment> _pEnvironment, TCSharedPointer<CStr> _pError)
	{
		if (CStr Error = fp_CheckLocalEnvironmentAccounts(_pEnvironment->m_Settings); !Error.f_IsEmpty())
		{
			*_pError = fg_Move(Error);
			co_return {};
		}

		CStr Directory;
		TCVector<CStr> Candidates;

		if (_pEnvironment->m_Settings.m_AgentApplication.f_IsEmpty())
			Candidates.f_Insert(CFile::fs_GetProgramPath());
		else
		{
			auto pAgentApplication = fp_FindEnvironmentAgentApplication(*_pEnvironment);
			if (!pAgentApplication)
			{
				*_pError = "Agent application '{}' for environment '{}' is not installed"_f << _pEnvironment->m_Settings.m_AgentApplication << _pEnvironment->m_Name;
				co_return {};
			}

			Directory = pAgentApplication->f_GetDirectory();

			if (!pAgentApplication->m_Settings.m_Executable.f_IsEmpty())
				Candidates.f_Insert(Directory / pAgentApplication->m_Settings.m_Executable);
			Candidates.f_Insert(Directory / CFile::fs_GetFile(CFile::fs_GetProgramPath()));
#ifdef DPlatformFamily_Windows
			Candidates.f_Insert(Directory / "AppManager.exe");
#else
			Candidates.f_Insert(Directory / "AppManager");
#endif
		}

		CStr StorageDirectory = fp_GetEnvironmentStorageDirectory(*_pEnvironment);

		// The deployment settings next to the agent executable identify the installation as this environment's agent, since
		// environment variables can be inherited by other processes in the environment
		CStr DeploymentSettings;
		{
			CEJsonSorted SettingsJson(EJsonType_Object);
			SettingsJson["EnvironmentAgent"] = true;
			SettingsJson["EnvironmentHostID"] = mp_State.m_HostID;
			SettingsJson["UserGroupNameTransform"] = mp_pUniqueUserGroup->m_UserGroupNameTransform;
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

						// Each environment runs its own copy, in the layout of the agent application: environments can share an agent application, and self updates replace its files.
						// Deletions are skipped since the storage also holds the agent state
						CStr AgentExecutable = Directory.f_IsEmpty() ? StorageDirectory / CFile::fs_GetFile(Source) : StorageDirectory / Source.f_Extract(Directory.f_GetLen() + 1);

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
								// An agent application can also be the parent application, which holds environment storage
								TCVector<CStr> ExcludePatterns = {"*/.home", "*/.tmp", "*/TempVersion", "*/TempVersionDownload", Directory / "Environment", StorageDirectory};

								CFile::fs_DiffCopyFileOrDirectory
									(
										Directory
										, StorageDirectory
										, []
										(CFile::EDiffCopyChange _Change, NStr::CStr const &_Source, NStr::CStr const &_Destination, NStr::CStr const &_Link) -> CFile::EDiffCopyChangeAction
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
										, ExcludePatterns
										, 0.0
									)
								;
							}

							CStr SettingsFile = CFile::fs_GetPath(AgentExecutable) / "AppManagerSettings.json";
							if (!CFile::fs_FileExists(SettingsFile) || CFile::fs_ReadStringFromFile(SettingsFile, true) != DeploymentSettings)
								CFile::fs_WriteStringToFile(SettingsFile, DeploymentSettings);

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

	// Starts the auto start environments whose parent application's storage became ready. The AppManager startup starts every auto start
	// environment, also those without a parent application, which are ready from the start, so only environments with a parent are handled here
	void CAppManagerActor::fp_AutoStartEnvironments()
	{
		if (mp_bEnvironmentAgent)
			return;

		for (auto &pEnvironment : mp_Environments)
		{
			auto &Environment = *pEnvironment;
			if (!Environment.m_Settings.m_bAutoStart || Environment.m_bStoppedManually || Environment.m_Settings.m_ParentApplication.f_IsEmpty())
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

	bool CAppManagerActor::fp_EnvironmentAcceptsApplications(CStr const &_Name)
	{
		auto *pFindEnvironment = mp_Environments.f_FindEqual(_Name);
		return pFindEnvironment && !(*pFindEnvironment)->m_bRemoving;
	}

	TCFuture<void> CAppManagerActor::fp_EnsureEnvironmentStarted(TCSharedPointer<CEnvironment> _pEnvironment)
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

		co_return co_await fp_StartEnvironmentInternal(_pEnvironment);
	}

	TCFuture<void> CAppManagerActor::fp_StartEnvironmentInternal(TCSharedPointer<CEnvironment> _pEnvironment)
	{
		auto CheckDestroy = co_await f_CheckDestroyedOnResume();

		for (;;)
		{
			if (_pEnvironment->m_bDeleted)
				co_return DMibErrorInstance("Environment has been deleted");

			if (_pEnvironment->m_bRemoving)
				co_return DMibErrorInstance("Environment is being removed");

			if (_pEnvironment->f_IsStarted() && !_pEnvironment->m_bStopping)
				co_return {};

			if (_pEnvironment->m_bStarting)
			{
				co_await _pEnvironment->m_OnAgentConnected.f_Insert().f_Future();
				co_return {};
			}

			if (!_pEnvironment->m_bStopping)
				break;

			// A stop can still be tearing down the previous agent; starting on top of
			// it would race that teardown
			(void)co_await _pEnvironment->m_OnStopFinished.f_Insert().f_Future().f_Wrap();
		}

		// A manual stop wins over a start that passed its caller's check before the stop
		if (_pEnvironment->m_bStoppedManually)
			co_return DMibErrorInstance("Environment '{}' was stopped. Start it with --environment-start."_f << _pEnvironment->m_Name);

		{
			CStr Error;
			CAppManagerInterface::EStatusSeverity Severity;
			if (!fp_EnvironmentStorageReady(*_pEnvironment, Error, Severity))
			{
				_pEnvironment->f_SetStatus(Error, Severity);
				co_return DMibErrorInstance("Cannot start environment '{}': {}"_f << _pEnvironment->m_Name << Error);
			}
		}

		// Set before any further suspension, so a concurrent start waits instead of launching the environment again
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

		CProcessLaunchActor::CLaunch Launch = CProcessLaunchParams::fs_LaunchExecutable
			(
				AgentExecutable
				, fg_CreateVector<CStr>("--daemon-run-standalone", "--log-to-stderr")
				, AgentRootDirectory
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
		LaunchParams.m_Environment["HOME"] = AgentRootDirectory + "/.home";
		LaunchParams.m_Environment["TMPDIR"] = AgentRootDirectory + "/.tmp";
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

		auto LaunchSubscription = co_await _pEnvironment->m_AgentLaunch(&CProcessLaunchActor::f_Launch, Launch, fg_ThisActor(this)).f_Wrap();

		if (!LaunchSubscription)
		{
			_pEnvironment->f_SetStatus(fg_Format("Failed to launch agent: {}", LaunchSubscription.f_GetExceptionStr()), CAppManagerInterface::EStatusSeverity_Error);

			if (_pEnvironment->m_AgentLaunch)
				co_await fg_Move(_pEnvironment->m_AgentLaunch).f_Destroy().f_Wrap() > fg_LogError("Malterlib/Cloud/AppManager", "Failed to destroy environment agent launch");

			co_return LaunchSubscription.f_GetException();
		}

		_pEnvironment->m_AgentLaunchSubscription = fg_Move(*LaunchSubscription);

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

	// Returns once the environment is down
	TCFuture<void> CAppManagerActor::fp_StopEnvironmentInternal(TCSharedPointer<CEnvironment> _pEnvironment)
	{
		// The start finishes first, since it would otherwise launch the environment after the stop finished
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

		if (_pEnvironment->m_AgentLaunch)
		{
			auto StopResult = co_await fg_TempCopy(_pEnvironment->m_AgentLaunch)(&CProcessLaunchActor::f_StopProcess).f_Wrap();
			if (!StopResult)
				DMibLogWithCategory(Malterlib/Cloud/AppManager, Warning, "Failed to stop agent for environment '{}': {}", _pEnvironment->m_Name, StopResult.f_GetExceptionStr());

			co_await fg_Move(_pEnvironment->m_AgentLaunch).f_Destroy().f_Wrap() > fg_LogError("Malterlib/Cloud/AppManager", "Failed to destroy environment agent launch");
		}

		_pEnvironment->m_AgentLaunchSubscription.f_Clear();

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

		// A stop that arrived while the environment started had no agent process to stop
		if (_pApplication->m_bStopped)
			co_return DMibErrorInstance("Application was stopped while its environment was prepared");

		CStr NewLaunchID = fg_RandomID();
		_pApplication->m_LaunchID = NewLaunchID;
		umint AppInterfaceAssignSequence = _pApplication->m_AppInterfaceAssignSequence;

		CAppManagerEnvironmentInterface::CEnvironmentLaunch Launch;
		Launch.m_Name = _pApplication->m_Name;
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
		Launch.m_InterfaceAddress = mp_State.m_LocalAddress.f_Encode();
		Launch.m_LaunchID = _pApplication->m_LaunchID;

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
					, "Failed launch in environment '{}': {}"_f << _pEnvironment->m_Name << LaunchResult.f_GetExceptionStr()
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
		// AppManager through the local address
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
