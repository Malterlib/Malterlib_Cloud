// Copyright © 2020 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Concurrency/ActorSubscription>
#include <Mib/Concurrency/LogError>
#include <Mib/CommandLine/TableRenderer>

#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud::NAppManager
{
	TCFuture<void> CAppManagerActor::fp_InitHostMonitor()
	{
		mp_HostMonitor = fg_Construct(mp_SensorStore, mp_LogStore, mp_DatabaseActor, mp_LogMetaData, mp_SensorMetaData);

		CHostMonitor::EInitFlag Flags = CHostMonitor::EInitFlag_None;

		if (mp_State.m_ConfigDatabase.m_Data.f_GetMemberValue("MonitorAllMounts", false).f_Boolean())
			Flags |= CHostMonitor::EInitFlag_MonitorAllMounts;

		auto InitResult = co_await mp_HostMonitor
			(
				&CHostMonitor::f_Init
				, CHostMonitor::CConfig{.m_Flags = Flags, .m_Interval = mp_HostMonitorInterval, .m_PatchInterval = mp_HostMonitorPatchInterval}
			)
		;
		mp_OsName = InitResult.m_OsName;

		{
			CHostMonitor::CMonitorPathOptions PathOptions;
			PathOptions.m_Path = mp_State.m_RootDirectory;

			auto MonitorResult = co_await mp_HostMonitor(&CHostMonitor::f_MonitorPath, PathOptions).f_Wrap();
			if (MonitorResult)
				mp_MainDirectoryMonitorSubscription = fg_Move(*MonitorResult);
			else
				DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Failed to monitor main directory: {}", MonitorResult.f_GetExceptionStr());
		}

		{
			NConcurrency::CDistributedAppInterfaceServer::CConfigFiles ConfigFiles;
			ConfigFiles.m_Files[mp_State.m_ConfigDatabase.f_GetFileName()].m_Type = NConcurrency::CDistributedAppInterfaceServer::EMonitorConfigType_Json;

			auto ConfigResult = co_await mp_HostMonitor(&CHostMonitor::f_MonitorConfigs, fg_Move(ConfigFiles)).f_Wrap();
			if (ConfigResult)
				mp_MainConfigFileMonitorSubscription = fg_Move(*ConfigResult);
			else
				DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Failed to monitor config files: {}", ConfigResult.f_GetExceptionStr());
		}

		co_return {};
	}

	void CAppManagerActor::fp_BuildCommandLine_HostMonitor(CDistributedAppCommandLineSpecification &o_CommandLine)
	{
		auto HostMonitor = o_CommandLine.f_AddSection("Host Monitor", "Commands to manage host monitor.");

		HostMonitor.f_RegisterCommand
			(
				{
					"Names"_o= {"--host-monitor-config-list"}
					, "Description"_o= "List monitored config files."
					, "Options"_o=
					{
						CTableRenderHelper::fs_OutputTypeOption()
					}
				}
				, [this](CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return g_Future <<= self(&CAppManagerActor::fp_CommandLine_HostMonitorConfigList, _Params, _pCommandLine);
				}
			)
		;

		HostMonitor.f_RegisterCommand
			(
				{
					"Names"_o= {"--host-monitor-config-version-list"}
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
							"Names"_o= {"--verbose", "-v"}
							, "Default"_o= false
							, "Description"_o= "Display more extensive information about the config file versions."
						}
						, CTableRenderHelper::fs_OutputTypeOption()
					}
				}
				, [this](CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return g_Future <<= self(&CAppManagerActor::fp_CommandLine_HostMonitorConfigVersionList, _Params, _pCommandLine);
				}
			)
		;

		HostMonitor.f_RegisterCommand
			(
				{
					"Names"_o= {"--host-monitor-config-contents-get"}
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
							"Names"_o= {"--sequence", "-s"}
							, "Type"_o= 0
							, "Description"_o= "The sequence of the config file to get contents for. If not specified latest version is retrieved."
						}
					}
				}
				, [this](CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return g_Future <<= self(&CAppManagerActor::fp_CommandLine_HostMonitorConfigContentsGet, _Params, _pCommandLine);
				}
			)
		;
	}

	TCFuture<uint32> CAppManagerActor::fp_CommandLine_HostMonitorConfigList(CEJSONSorted _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		if (!mp_HostMonitor)
			co_return DMibErrorInstance("Host monitor not yet initialized");

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

	TCFuture<uint32> CAppManagerActor::fp_CommandLine_HostMonitorConfigVersionList(CEJSONSorted _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		if (!mp_HostMonitor)
			co_return DMibErrorInstance("Host monitor not yet initialized");

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

	TCFuture<uint32> CAppManagerActor::fp_CommandLine_HostMonitorConfigContentsGet(CEJSONSorted _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		NHostMonitor::CConfigFileVersionKey Key;
		Key.m_FileName = _Params["FileName"].f_String();
		if (auto *pValue = _Params.f_GetMember("Sequence"))
			Key.m_Sequence = pValue->f_Integer();
		else
			Key.m_Sequence = TCLimitsInt<uint64>::mc_Max;

		auto Contents = co_await mp_HostMonitor(&CHostMonitor::f_GetConfigFileContents, Key);

		co_await _pCommandLine->f_StdOutBinary(Contents.m_Raw.f_ToSecure());

		co_return 0;
	}
}
