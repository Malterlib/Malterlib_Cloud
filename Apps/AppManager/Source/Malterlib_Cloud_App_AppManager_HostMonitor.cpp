// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Encoding/JsonShortcuts>
#include <Mib/Concurrency/ActorSubscription>
#include <Mib/Concurrency/LogError>
#include <Mib/CommandLine/TableRenderer>

#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud::NAppManager
{
	TCFuture<void> CAppManagerActor::fp_InitHostMonitor(CEJsonSorted _AutoUpdateConfig)
	{
		++mp_HostMonitorGeneration;
		mp_HostMonitor = fg_Construct(mp_SensorStore, mp_LogStore, mp_DatabaseActor, mp_LogMetadata, mp_SensorMetadata);

		auto OnResume = co_await fg_OnResume
			(
				[&]() -> CExceptionPointer
				{
					if (mp_State.m_bStoppingApp || f_IsDestroyed())
						return DMibErrorInstance("Startup aborted");
					return {};
				}
			)
		;

		CHostMonitor::EInitFlag Flags = CHostMonitor::EInitFlag_None;

		if (mp_State.m_ConfigDatabase.m_Data.f_GetMemberValue("MonitorAllMounts", false).f_Boolean())
			Flags |= CHostMonitor::EInitFlag_MonitorAllMounts;

		CHostMonitor::CConfig Config
			{
				.m_Flags = Flags
				, .m_Interval = mp_HostMonitorInterval
				, .m_PatchInterval = mp_HostMonitorPatchInterval
				, .m_MemoryInterval = mp_HostMonitorMemoryInterval
			}
		;

		if (_AutoUpdateConfig.f_IsObject())
		{
			if (_AutoUpdateConfig.f_GetMemberValue("NormalUpdates", false).f_Boolean())
				Config.m_AutomaticUpdateFlags |= CHostMonitor::EAutomaticUpdatesFlag::mc_NormalUpdates;

			if (_AutoUpdateConfig.f_GetMemberValue("SecurityUpdates", false).f_Boolean())
				Config.m_AutomaticUpdateFlags |= CHostMonitor::EAutomaticUpdatesFlag::mc_SecurityUpdates;

			if (_AutoUpdateConfig.f_GetMemberValue("AutomaticReboot", false).f_Boolean())
			{
				Config.m_AutomaticUpdateFlags |= CHostMonitor::EAutomaticUpdatesFlag::mc_AutomaticReboot;
				Config.m_fOnRebootNeeded = fp_HostMonitorRebootNeededFunctor(_AutoUpdateConfig);
			}
		}

		// Package managers hold exclusive locks, so patching and OS dependency installations take turns. An update in the environment must
		// prevent host reboots and environment stops, so the agent patches only once the environment host has admitted the update, which it
		// refuses once it stops the environment. The monitor is only replaced while it doesn't patch, so a call of a replaced monitor changes
		// nothing after a suspension
		Config.m_fOnPatchingChanged = g_ActorFunctor / [this, Generation = mp_HostMonitorGeneration](bool _bPatching) -> TCFuture<void>
			{
				if (Generation != mp_HostMonitorGeneration)
					co_return DMibErrorInstance("The host monitor was replaced");

				if (_bPatching)
				{
					auto Sequence = co_await mp_OSDependenciesSequencer.f_Sequence();
					if (Generation != mp_HostMonitorGeneration)
					{
						co_await Sequence->f_Destroy();
						co_return DMibErrorInstance("The host monitor was replaced");
					}

					mp_PatchingOSDependenciesSequence = fg_Move(Sequence);
				}
				else if (mp_PatchingOSDependenciesSequence)
				{
					co_await fg_Exchange(mp_PatchingOSDependenciesSequence, nullptr)->f_Destroy();
					if (Generation != mp_HostMonitorGeneration)
						co_return DMibErrorInstance("The host monitor was replaced");
				}

				if (!mp_bEnvironmentAgent)
					co_return {};

				if (_bPatching && !mp_EnvironmentHostActor)
				{
					co_await fg_Exchange(mp_PatchingOSDependenciesSequence, nullptr)->f_Destroy();
					fp_ApplyPendingHostMonitorConfig();
					co_return DMibErrorInstance("Not connected to the environment host");
				}

				mp_bEnvironmentPatching = _bPatching;
				if (!_bPatching)
					fp_ApplyPendingHostMonitorConfig();

				auto Result = co_await fp_ReportEnvironmentBusyToHost().f_Wrap();
				if (!Result && _bPatching && Generation == mp_HostMonitorGeneration)
				{
					mp_bEnvironmentPatching = false;
					if (mp_PatchingOSDependenciesSequence)
						co_await fg_Exchange(mp_PatchingOSDependenciesSequence, nullptr)->f_Destroy();

					// A report that timed out can have reached the host
					fp_ReportEnvironmentBusyToHost() > fg_LogError("Malterlib/Cloud/AppManager", "Failed to report the environment update state");
					fp_ApplyPendingHostMonitorConfig();
				}

				co_return Result;
			}
		;

		if (mp_bEnvironmentAgent)
		{
			// A container shares the kernel of whatever runs it, so its kernel reboot check is skipped
			Config.m_bKernelManagedExternally = mp_bEnvironmentContainer;

			// Only apt-check tells the patches a container lacks, which the agent installs in Ubuntu as an OS dependency of its distribution, and a
			// read-only container can't
			Config.m_bPatchCheckRequired = mp_bEnvironmentContainer;

			Config.m_fPackageManagerTurn = g_ActorFunctor / [this]() -> TCFuture<CActorSubscription>
				{
					co_return co_await mp_OSDependenciesSequencer.f_Sequence();
				}
			;

			// A container has no timer that refreshes the package indexes, which the patch check reads
			Config.m_bRefreshPackageIndexes
				= mp_bEnvironmentContainer
				&&
				(
					fg_IsSet(Config.m_AutomaticUpdateFlags, CHostMonitor::EAutomaticUpdatesFlag::mc_NormalUpdates)
					|| fg_IsSet(Config.m_AutomaticUpdateFlags, CHostMonitor::EAutomaticUpdatesFlag::mc_SecurityUpdates)
				)
			;
		}

#ifndef DPlatformFamily_Linux
		if
		(
			mp_bEnvironmentAgent
			&&
			(
				fg_IsSet(Config.m_AutomaticUpdateFlags, CHostMonitor::EAutomaticUpdatesFlag::mc_NormalUpdates)
				|| fg_IsSet(Config.m_AutomaticUpdateFlags, CHostMonitor::EAutomaticUpdatesFlag::mc_SecurityUpdates)
			)
		)
		{
			DMibLogWithCategory(Malterlib/Cloud/AppManager, Warning, "The environment host configures automatic OS updates, which only Linux environments install");
		}
#endif

		auto InitResult = co_await mp_HostMonitor(&CHostMonitor::f_Init, fg_Move(Config));
		mp_OsName = InitResult.m_OsName;

		{
			CHostMonitor::CMonitorPathOptions PathOptions;

			// The environment's own root filesystem is the one that OS updates and writes outside the application storage fill up
			if (mp_bEnvironmentAgent)
				PathOptions.m_Path = "/";
			else
				PathOptions.m_Path = mp_State.m_RootDirectory;

			auto MonitorResult = co_await mp_HostMonitor(&CHostMonitor::f_MonitorPath, PathOptions).f_Wrap();
			if (MonitorResult)
				mp_MainDirectoryMonitorSubscription = fg_Move(*MonitorResult);
			else
				DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Failed to monitor main directory: {}", MonitorResult.f_GetExceptionStr());
		}

		// The applications in environment-managed storage fill a volume of their own
		if (mp_bEnvironmentAgent && !mp_EnvironmentDataRoot.f_IsEmpty())
		{
			auto DataRootResult = co_await fp_EnsureEnvironmentDataRoot().f_Wrap();
			if (DataRootResult)
			{
				CHostMonitor::CMonitorPathOptions PathOptions;
				PathOptions.m_Path = mp_EnvironmentDataRoot;

				auto MonitorResult = co_await mp_HostMonitor(&CHostMonitor::f_MonitorPath, PathOptions).f_Wrap();
				if (MonitorResult)
					mp_EnvironmentDataRootMonitorSubscription = fg_Move(*MonitorResult);
				else
					DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Failed to monitor the environment data root: {}", MonitorResult.f_GetExceptionStr());
			}
			else
				DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Failed to prepare the environment data root for monitoring: {}", DataRootResult.f_GetExceptionStr());
		}

		{
			NConcurrency::CDistributedAppInterfaceServer::CConfigFiles ConfigFiles;

			auto const &ConfigFileName = mp_State.m_ConfigDatabase.f_GetFileName();
			bool bYamlConfig = ConfigFileName.f_EndsWith(".yaml") || ConfigFileName.f_EndsWith(".yml");
			ConfigFiles.m_Files[ConfigFileName].m_Type
				= bYamlConfig ? NConcurrency::CDistributedAppInterfaceServer::EMonitorConfigType_Yaml : NConcurrency::CDistributedAppInterfaceServer::EMonitorConfigType_Json
			;

			auto ConfigResult = co_await mp_HostMonitor(&CHostMonitor::f_MonitorConfigs, fg_Move(ConfigFiles)).f_Wrap();
			if (ConfigResult)
				mp_MainConfigFileMonitorSubscription = fg_Move(*ConfigResult);
			else
				DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Failed to monitor config files: {}", ConfigResult.f_GetExceptionStr());
		}

		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_ConfigureHostMonitorFromHost(CStr _AutoUpdateConfig)
	{
		auto SequenceSubscription = co_await mp_HostMonitorConfigureSequencer.f_Sequence();

		if (mp_HostMonitor && _AutoUpdateConfig == mp_AppliedHostMonitorConfig)
		{
			mp_PendingHostMonitorConfig.f_Clear();
			co_return {};
		}

		// Destroying the monitor stops the package manager that its patching runs and leaves the packages broken, so the configuration
		// replaces the monitor when the patching ends
		if (mp_HostMonitor && (mp_bEnvironmentPatching || mp_PatchingOSDependenciesSequence))
		{
			DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Restarting the host monitor with a changed configuration from the environment host once its patching ends");
			mp_PendingHostMonitorConfig = fg_Move(_AutoUpdateConfig);
			co_return {};
		}

		mp_PendingHostMonitorConfig.f_Clear();

		CEJsonSorted Config;
		if (!_AutoUpdateConfig.f_IsEmpty())
		{
			auto CaptureScope = co_await (g_CaptureExceptions % "Failed to parse the host monitor configuration from the environment host");

			Config = CEJsonSorted::fs_FromString(_AutoUpdateConfig);
		}

		// The host monitor configuration is fixed at initialization, so a changed
		// configuration recreates the monitor
		if (mp_HostMonitor)
		{
			DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Restarting the host monitor with a changed configuration from the environment host");

			// The calls of the replaced monitor that are still running give up what they get from here on
			++mp_HostMonitorGeneration;

			mp_MainDirectoryMonitorSubscription = {};
			mp_EnvironmentDataRootMonitorSubscription = {};
			mp_MainConfigFileMonitorSubscription = {};

			// A restart timer set under the previous configuration does not apply anymore
			if (mp_RebootScheduleTimerSubscrption)
			{
				co_await fg_Exchange(mp_RebootScheduleTimerSubscrption, nullptr)->f_Destroy().f_Wrap()
					> fg_LogError("Malterlib/Cloud/AppManager", "Failed to cancel the scheduled environment restart")
				;
			}
			mp_bRebootScheduled = false;

			co_await fg_Move(mp_HostMonitor).f_Destroy().f_Wrap() > fg_LogError("Malterlib/Cloud/AppManager", "Failed to destroy the host monitor");
		}

		auto InitResult = co_await fp_InitHostMonitor(fg_Move(Config)).f_Wrap();
		if (!InitResult)
		{
			// A failed initialization stays retryable with the same configuration
			if (mp_HostMonitor)
				co_await fg_Move(mp_HostMonitor).f_Destroy().f_Wrap() > fg_LogError("Malterlib/Cloud/AppManager", "Failed to destroy the host monitor");

			co_return InitResult.f_GetException();
		}

		mp_AppliedHostMonitorConfig = _AutoUpdateConfig;

		DMibLogWithCategory
			(
				Malterlib/Cloud/AppManager
				, Info
				, "Configured the host monitor from the environment host{}"
				, _AutoUpdateConfig.f_IsEmpty() ? "" : " with automatic update settings"
			)
		;

		co_return {};
	}

	void CAppManagerActor::fp_ApplyPendingHostMonitorConfig()
	{
		if (!mp_PendingHostMonitorConfig)
			return;

		CStr Config = fg_Move(*mp_PendingHostMonitorConfig);
		mp_PendingHostMonitorConfig.f_Clear();

		fp_ConfigureHostMonitorFromHost(fg_Move(Config)) > fg_LogError("Malterlib/Cloud/AppManager", "Failed to configure the host monitor from the environment host");
	}

	void CAppManagerActor::fp_BuildCommandLine_HostMonitor(CDistributedAppCommandLineSpecification &o_CommandLine)
	{
		auto HostMonitor = o_CommandLine.f_AddSection("Host Monitor", "Commands to manage host monitor.");

		HostMonitor.f_RegisterCommand
			(
				{
					"Names"_o= _o["--host-monitor-config-list"]
					, "Description"_o= "List monitored config files."
					, "Options"_o=
					{
						CTableRenderHelper::fs_OutputTypeOption()
					}
				}
				, [this](CEJsonSorted &&_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_HostMonitorConfigList(fg_Move(_Params), fg_Move(_pCommandLine));
				}
			)
		;

		HostMonitor.f_RegisterCommand
			(
				{
					"Names"_o= _o["--host-monitor-config-version-list"]
					, "Description"_o= "List monitored config files."
					, "Parameters"_o=
					{
						"FileName?"_o=
						{
							"Type"_o= ""
							, "Description"_o= "The name of the config file to list versions for. If not specified all files are displayed."
						}
					}
					, "Options"_o=
					{
						"Verbose?"_o=
						{
							"Names"_o= _o["--verbose", "-v"]
							, "Default"_o= false
							, "Description"_o= "Display more extensive information about the config file versions."
						}
						, CTableRenderHelper::fs_OutputTypeOption()
					}
				}
				, [this](CEJsonSorted &&_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_HostMonitorConfigVersionList(fg_Move(_Params), fg_Move(_pCommandLine));
				}
			)
		;

		HostMonitor.f_RegisterCommand
			(
				{
					"Names"_o= _o["--host-monitor-config-contents-get"]
					, "Description"_o= "List monitored config files."
					, "Parameters"_o=
					{
						"FileName"_o=
						{
							"Type"_o= ""
							, "Description"_o= "The name of the config file to get contents for."
						}
					}
					, "Options"_o=
					{
						"Sequence?"_o=
						{
							"Names"_o= _o["--sequence", "-s"]
							, "Type"_o= 0
							, "Description"_o= "The sequence of the config file to get contents for. If not specified latest version is retrieved."
						}
					}
				}
				, [this](CEJsonSorted &&_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_HostMonitorConfigContentsGet(fg_Move(_Params), fg_Move(_pCommandLine));
				}
			)
		;
	}

	TCFuture<uint32> CAppManagerActor::fp_CommandLine_HostMonitorConfigList(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		if (!mp_HostMonitor)
			co_return DMibErrorInstance("Host monitor not yet initialized");

		auto OnResume = co_await fg_OnResume
			(
				[&]() -> CExceptionPointer
				{
					if (!mp_HostMonitor)
						return DMibErrorInstance("Host monitor destroyed");
					return {};
				}
			)
		;

		auto AnsiEncoding = _pCommandLine->f_AnsiEncoding();
		CTableRenderHelper TableRenderer = _pCommandLine->f_TableRenderer();
		CTableRenderHelper::CColumnHelper Columns(0);
		Columns.f_AddHeading("File name", 0);

		TableRenderer.f_AddHeadings(&Columns);

		auto ConfigFiles = co_await mp_HostMonitor(&CHostMonitor::f_EnumConfigFiles);

		for (auto &FileName : ConfigFiles)
			TableRenderer.f_AddRow(FileName);

		TableRenderer.f_Output(_Params);

		co_return 0;
	}

	TCFuture<uint32> CAppManagerActor::fp_CommandLine_HostMonitorConfigVersionList(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		if (!mp_HostMonitor)
			co_return DMibErrorInstance("Host monitor not yet initialized");

		auto OnResume = co_await fg_OnResume
			(
				[&]() -> CExceptionPointer
				{
					if (!mp_HostMonitor)
						return DMibErrorInstance("Host monitor destroyed");
					return {};
				}
			)
		;

		auto AnsiEncoding = _pCommandLine->f_AnsiEncoding();
		CTableRenderHelper TableRenderer = _pCommandLine->f_TableRenderer();
		CTableRenderHelper::CColumnHelper Columns(_Params["Verbose"].f_Boolean() ? 1 : 0);
		Columns.f_AddHeading("File Name", 0);
		Columns.f_AddHeading("Sequence", 0);
		Columns.f_AddHeading("Timestamp", 0);
		Columns.f_AddHeading("Config Type", 0);
		Columns.f_AddHeading("Exists", 0);
		Columns.f_AddHeading("Parse Error", 0);
		Columns.f_AddHeading("Digest", 1);
		Columns.f_AddHeading("Owner", 1);
		Columns.f_AddHeading("Group", 1);
		Columns.f_AddHeading("Size", 1);
		Columns.f_AddHeading("Attributes", 1);

		TableRenderer.f_AddHeadings(&Columns);

		TCSet<CStr> ConfigFiles;
		if (auto *pValue = _Params.f_GetMember("FileName"))
		{
			Columns.f_SetVerbose("File Name");
			ConfigFiles[pValue->f_String()];
		}
		else
			ConfigFiles = co_await mp_HostMonitor(&CHostMonitor::f_EnumConfigFiles);

		bool bAnyNoExists = false;
		bool bHasParseError = false;
		for (auto &FileName : ConfigFiles)
		{
			auto Versions = co_await mp_HostMonitor(&CHostMonitor::f_EnumConfigFileVersions, FileName);
			for (auto &Version : Versions)
			{
				auto &VersionKey = Versions.fs_GetKey(Version);

				if (Version.m_UniqueProperties.m_bExists)
					bAnyNoExists = true;

				if (Version.m_UniqueProperties.m_ParseError)
					bHasParseError = true;

				TableRenderer.f_AddRow
					(
						VersionKey.m_FileName
						, VersionKey.m_Sequence
						, "{tc6}"_f << Version.m_Timestamp
						, CDistributedAppInterfaceServer::fs_MonitorConfigTypeToString(Version.m_UniqueProperties.m_ConfigType)
						, Version.m_UniqueProperties.m_bExists
						, Version.m_UniqueProperties.m_Digest
						, Version.m_UniqueProperties.m_ParseError
						, Version.m_UniqueProperties.m_Owner
						, Version.m_UniqueProperties.m_Group
						, "{ns }"_f << Version.m_UniqueProperties.m_Size
						, CStr::fs_Join(CFile::fs_AttribToJson(Version.m_UniqueProperties.m_Attributes).f_StringArray(), ", ")
					)
				;
			}
		}

		if (!bHasParseError)
			Columns.f_SetVerbose("Parse Error");

		if (!bAnyNoExists)
			Columns.f_SetVerbose("Exists");

		TableRenderer.f_Output(_Params);

		co_return 0;
	}

	TCFuture<uint32> CAppManagerActor::fp_CommandLine_HostMonitorConfigContentsGet(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		NHostMonitor::CConfigFileVersionKey Key;
		Key.m_FileName = _Params["FileName"].f_String();
		if (auto *pValue = _Params.f_GetMember("Sequence"))
			Key.m_Sequence = pValue->f_Integer();
		else
			Key.m_Sequence = TCLimitsInt<uint64>::mc_Max;

		auto Contents = co_await mp_HostMonitor(&CHostMonitor::f_GetConfigFileContents, Key);

		co_await _pCommandLine->f_StdOutBinary(Contents.m_Raw);

		co_return 0;
	}
}
