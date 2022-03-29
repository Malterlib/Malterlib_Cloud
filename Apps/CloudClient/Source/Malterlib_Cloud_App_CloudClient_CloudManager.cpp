// Copyright © 2019 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/ActorSubscription>
#include <Mib/Concurrency/DistributedAppSensorReader>
#include <Mib/Encoding/JSONShortcuts>
#include <Mib/CommandLine/TableRenderer>
#include <Mib/CommandLine/AnsiEncoding>

#include "Malterlib_Cloud_App_CloudClient.h"

namespace NMib::NCloud::NCloudClient
{
	void CCloudClientAppActor::fp_CloudManager_RegisterCommands(CDistributedAppCommandLineSpecification::CSection _Section)
	{
		_Section.f_RegisterSectionOptions
			(
				{
					"Host?"_=
					{
						"Names"_= {"--host"}
						, "Default"_= ""
						, "Description"_= "Limit cloud managers to only specified host ID."
					}
				}
			)
		;

		auto QuietOption = "Quiet?"_=
			{
				"Names"_= {"--quiet", "-q"}
				, "Default"_= false
				, "Description"_= "Don't output information on std out, just return the status"
			}
		;
		auto IncludeCloudManagerOption = "IncludeCloudManager?"_=
			{
				"Names"_= {"--include-cloud-manager", "-c"}
				, "Default"_= false
				, "Description"_= "Include the cloud manager column"
			}
		;
		auto FilterStatusError = "FilterStatusError?"_=
			{
				"Names"_= {"--filter-status-error"}
				, "Type"_= true
				, "Description"_= "Include applications and app managers with error status"
			}
		;
		auto FilterEnvironment = "FilterEnvironment?"_=
			{
				"Names"_= {"--filter-environment"}
				, "Type"_= ""
				, "Description"_= "Include applications and appmanagers with specified environment\n"
				"Wildcard search."
			}
		;
		auto FilterName = "FilterName?"_=
			{
				"Names"_= {"--filter-name"}
				, "Type"_= ""
				, "Description"_= "Include applications with specified application name"
			}
		;
		auto FilterVersionManagerApp = "FilterVersionManagerApp?"_=
			{
				"Names"_= {"--filter-application"}
				, "Type"_= ""
				, "Description"_= "Include applications with specified version manager app name"
			}
		;
		auto FilterOutOfDateVersion = "FilterOutOfDateVersion?"_=
			{
				"Names"_= {"--filter-out-of-date"}
				, "Type"_= true
				, "Description"_= "Include applications with out of date versions"
			}
		;

		_Section.f_RegisterCommand
			(
				{
					"Names"_= {"--cloud-manager-status"}
					, "Description"_= "List the overall status for resources controlled by the Cloud Manager"
					, "Status"_=
					{
						"0"_= "The status of all resources is OK"
						, "1"_= "One or more resources has an error status"
					}
					, "Options"_=
					{
						QuietOption
						, IncludeCloudManagerOption
						, CTableRenderHelper::fs_OutputTypeOption()
						, FilterStatusError
						, FilterEnvironment
						, FilterVersionManagerApp
						, FilterName
						, FilterOutOfDateVersion
 					}
				}
				, [this](CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine) -> TCFuture<uint32>
				{
					auto ReportFor = ECloudManagerStatusFlag_Applications | ECloudManagerStatusFlag_AppManagers;
					return g_Future <<= self(&CCloudClientAppActor::fp_CommandLine_CloudManager_Status, _Params, _pCommandLine, ReportFor);
				}
				, EDistributedAppCommandFlag_WaitForRemotes
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_= {"--cloud-manager-app-manager-status"}
					, "Description"_= "List the status for app managers controlled by the Cloud Manager"
					, "Status"_=
					{
						"0"_= "The status of all app managers is OK"
						, "1"_= "One or more app managers has an error status"
					}
					, "Options"_=
					{
						QuietOption
						, IncludeCloudManagerOption
						, CTableRenderHelper::fs_OutputTypeOption()
						, FilterStatusError
						, FilterEnvironment
  					}
				}
				, [this](CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine) -> TCFuture<uint32>
				{
					auto ReportFor = ECloudManagerStatusFlag_AppManagers;
					return g_Future <<= self(&CCloudClientAppActor::fp_CommandLine_CloudManager_Status, _Params, _pCommandLine, ReportFor);
				}
				, EDistributedAppCommandFlag_WaitForRemotes
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_= {"--cloud-manager-application-status"}
					, "Description"_= "List the status for applications in app managers controlled by the Cloud Manager"
					, "Status"_=
					{
						"0"_= "The status of all applications is OK"
						, "1"_= "One or more applications has an error status"
					}
					, "Options"_=
					{
						QuietOption
						, CTableRenderHelper::fs_OutputTypeOption()
						, FilterStatusError
						, FilterEnvironment
						, FilterVersionManagerApp
						, FilterName
						, FilterOutOfDateVersion
					}
				}
				, [this](CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine) -> TCFuture<uint32>
				{
					auto ReportFor = ECloudManagerStatusFlag_Applications;
					return g_Future <<= self(&CCloudClientAppActor::fp_CommandLine_CloudManager_Status, _Params, _pCommandLine, ReportFor);
				}
				, EDistributedAppCommandFlag_WaitForRemotes
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_= {"--cloud-manager-remove-app-manager"}
					, "Description"_= "Remove app manager from cloud manager database"
					, "Parameters"_=
					{
						"AppManagerHostID"_=
						{
							"Type"_= ""
							, "Description"_= "The host ID of the app manager to remove"
						}
					}
					, "Options"_=
					{
						QuietOption
					}
				}
				, [this](CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine) -> TCFuture<uint32>
				{
					return g_Future <<= self(&CCloudClientAppActor::fp_CommandLine_CloudManager_RemoveAppManager, _Params, _pCommandLine);
				}
				, EDistributedAppCommandFlag_WaitForRemotes
			)
		;

		fp_BuildDefaultCommandLine_Sensor_Customizable
			(
				_Section
				, "cloud-manager-"
				, g_ActorFunctor / [this]
				(
					CEJSON const &_Params
					, TCSharedPointer<CCommandLineControl> const &_pCommandLine
					, CDistributedAppSensorReader_SensorFilter const &_Filter
					, ESensorOutputFlag _Flags
					, CStr const &_TableType
				)
				-> TCFuture<uint32>
				{
					auto SensorReaders = co_await self(&CCloudClientAppActor::fp_CommandLine_CloudManager_GetSensorReaders, _Params["Host"].f_String());
					co_return co_await self
						(
							&CDistributedAppActor::f_CommandLine_SensorListOutput
							, _pCommandLine
							, co_await self(&CCloudClientAppActor::fp_CommandLine_CloudManager_GetAggregatedSensors, SensorReaders, _Filter)
							, _Flags
							, _TableType
						)
					;
				}
				, g_ActorFunctor / [this]
				(
					CEJSON const &_Params
					, TCSharedPointer<CCommandLineControl> const &_pCommandLine
					, CDistributedAppSensorReader_SensorFilter const &_Filter
					, ESensorOutputFlag _Flags
					, CStr const &_TableType
				)
				-> TCFuture<uint32>
				{
					auto SensorReaders = co_await self(&CCloudClientAppActor::fp_CommandLine_CloudManager_GetSensorReaders, _Params["Host"].f_String());
					auto SensorsGenerator = co_await self(&CCloudClientAppActor::fp_CommandLine_CloudManager_GetAggregatedSensors, SensorReaders, _Filter);
					auto ReadingsGenerator = co_await self(&CCloudClientAppActor::fp_CommandLine_CloudManager_GetAggregatedSensorStatus, SensorReaders, _Filter);

					co_return co_await self
						(
							&CDistributedAppActor::f_CommandLine_SensorReadingsOutput
							, _pCommandLine
							, fg_Move(ReadingsGenerator)
							, fg_Move(SensorsGenerator)
							, 0
							, _Flags
							, _TableType
						)
					;
				}
				, g_ActorFunctor / [this]
				(
					CEJSON const &_Params
					, TCSharedPointer<CCommandLineControl> const &_pCommandLine
					, CDistributedAppSensorReader_SensorReadingFilter const &_Filter
					, uint64 _MaxEntries
					, ESensorOutputFlag _Flags
					, CStr const &_TableType
				)
				-> TCFuture<uint32>
				{
					auto SensorReaders = co_await self(&CCloudClientAppActor::fp_CommandLine_CloudManager_GetSensorReaders, _Params["Host"].f_String());
					auto SensorsGenerator = co_await self(&CCloudClientAppActor::fp_CommandLine_CloudManager_GetAggregatedSensors, SensorReaders, _Filter.m_SensorFilter);
					auto ReadingsGenerator = co_await self(&CCloudClientAppActor::fp_CommandLine_CloudManager_GetAggregatedSensorReadings, SensorReaders, _Filter);

					co_return co_await self
						(
							&CDistributedAppActor::f_CommandLine_SensorReadingsOutput
							, _pCommandLine
							, fg_Move(ReadingsGenerator)
							, fg_Move(SensorsGenerator)
							, _MaxEntries
							, _Flags
							, _TableType
						)
					;
				}
				, EDistributedAppCommandFlag_WaitForRemotes
			)
		;
	}

	TCFuture<void> CCloudClientAppActor::fp_CloudManager_SubscribeToServers()
	{
		if (!mp_CloudManagers.f_IsEmpty())
			co_return {};

		DMibLogWithCategory(Malterlib/Cloud/CloudClient, Info, "Subscribing to cloud managers");

		auto Subscription = co_await mp_State.m_TrustManager->f_SubscribeTrustedActors<NCloud::CCloudManager>().f_Wrap();

		if (!Subscription)
		{
			DMibLogWithCategory(Malterlib/Cloud/CloudClient, Error, "Failed to subscribe to cloud managers: {}", Subscription.f_GetExceptionStr());
			co_return Subscription.f_GetException();
		}

		mp_CloudManagers = fg_Move(*Subscription);
		if (mp_CloudManagers.m_Actors.f_IsEmpty())
			co_return DMibErrorInstance("Not connected to any cloud managers, or they are not trusted for 'com.malterlib/Cloud/CloudManager' namespace");

		co_return {};
	}

	namespace
	{
		CStr fg_FormatTimespan(CTimeSpan const &_TimeSpan, CAnsiEncoding const &_AnsiEncoding)
		{
			CTimeSpanConvert TimespanConvert(_TimeSpan);

			if (TimespanConvert.f_GetSeconds() < 10)
				return "{}Now{}"_f << _AnsiEncoding.f_StatusNormal() << _AnsiEncoding.f_Default();
			else if (TimespanConvert.f_GetSeconds() < 60)
			{
				return "{}{} seconds ago{}"_f
					<< _AnsiEncoding.f_StatusNormal()
					<< TimespanConvert.f_GetSeconds()
					<< _AnsiEncoding.f_Default()
				;
			}
			else if (TimespanConvert.f_GetMinutes() < 2)
			{
				return "{}{}:{sj2,sf0} minutes ago{}"_f
					<< _AnsiEncoding.f_StatusNormal()
					<< TimespanConvert.f_GetMinutes()
					<< TimespanConvert.f_GetSecondOfMinute()
					<< _AnsiEncoding.f_Default()
				;
			}
			else if (TimespanConvert.f_GetMinutes() < 10)
				return "{}{} minutes ago{}"_f << _AnsiEncoding.f_StatusNormal() << TimespanConvert.f_GetMinutes() << _AnsiEncoding.f_Default();
			else if (TimespanConvert.f_GetMinutes() < 60)
				return "{}{} minutes ago{}"_f << _AnsiEncoding.f_StatusWarning() << TimespanConvert.f_GetMinutes() << _AnsiEncoding.f_Default();
			else if (TimespanConvert.f_GetHours() < 2)
			{
				return "{}{}:{sj2,sf0} hours ago{}"_f
					<< _AnsiEncoding.f_StatusError()
					<< TimespanConvert.f_GetHours()
					<< TimespanConvert.f_GetMinuteOfHour()
					<< _AnsiEncoding.f_Default()
				;
			}
			else
			{
				return "{}{} hours ago{}"_f
					<< _AnsiEncoding.f_StatusError()
					<< TimespanConvert.f_GetHours()
					<< _AnsiEncoding.f_Default()
				;
			}
		}

		CStr fg_FormatApplicationStatusSeverity(CStr const &_Status, CAppManagerInterface::EStatusSeverity _Severity, CAnsiEncoding const &_AnsiEncoding)
		{
			switch (_Severity)
			{
			case CAppManagerInterface::EStatusSeverity_None: return "{}{}{}"_f << _AnsiEncoding.f_StatusNormal() << _Status << _AnsiEncoding.f_Default();
			case CAppManagerInterface::EStatusSeverity_Warning: return "{}{}{}"_f << _AnsiEncoding.f_StatusWarning() << _Status << _AnsiEncoding.f_Default();
			case CAppManagerInterface::EStatusSeverity_Error: return "{}{}{}"_f << _AnsiEncoding.f_StatusError() << _Status << _AnsiEncoding.f_Default();
			default: return _Status;
			}
		}

		template <typename tf_CInfo>
		CStr fg_FormatAppManagerStatus(tf_CInfo const &_Info, CAnsiEncoding const &_AnsiEncoding)
		{
			if (!_Info.m_bActive)
			{
				CStr Status;
				if (!_Info.m_LastConnectionError.f_IsEmpty())
				{
					Status = "{}Not Connected:{} {tc6} {}"_f
						<< _AnsiEncoding.f_StatusError()
						<< _AnsiEncoding.f_Default()
						<< _Info.m_LastConnectionErrorTime
						<< _Info.m_LastConnectionError
					;
				}
				else
					Status = "{}Not Connected{}"_f << _AnsiEncoding.f_StatusError() << _AnsiEncoding.f_Default();

				return Status;
			}

			CStr Status = "{}Connected{}"_f << _AnsiEncoding.f_StatusNormal() << _AnsiEncoding.f_Default();
			if (!_Info.m_OtherErrors.f_IsEmpty())
			{
				for (auto &Error : _Info.m_OtherErrors)
					Status += "\n{}{}:{} {}"_f << _AnsiEncoding.f_StatusError() << _Info.m_OtherErrors.fs_GetKey(Error) << _AnsiEncoding.f_Default() << Error;
			}

			return Status;
		}

	}

	bool CCloudClientAppActor::CCloudManagerAppManagerInfo::f_HasErrors() const
	{
		return !m_bActive || !m_OtherErrors.f_IsEmpty();
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_CloudManager_Status_AppManagers
		(
			CEJSON const &_Params
			, TCMap<CHostInfo, TCAsyncResult<TCMap<CStr, CCloudManager::CAppManagerDynamicInfo>>> const &_AppManagers
			, TCSharedPointer<CCommandLineControl> const &_pCommandLine
		)
	{
		bool bQuiet = _Params["Quiet"].f_Boolean();
		bool bIncludeCloudManager = _Params["IncludeCloudManager"].f_Boolean();

		TCOptional<bool> FilterStatusError;
		TCOptional<CStr> FilterEnvironment;

		if (auto pValue = _Params.f_GetMember("FilterStatusError"))
			FilterStatusError = pValue->f_Boolean();

		if (auto pValue = _Params.f_GetMember("FilterEnvironment"))
			FilterEnvironment = pValue->f_String();

		CTableRenderHelper TableRenderer = _pCommandLine->f_TableRenderer();
		CAnsiEncoding AnsiEncoding = _pCommandLine->f_AnsiEncoding();

		TableRenderer.f_AddDescription("App Managers");
		TableRenderer.f_AddHeadings("Cloud Manager", "Environment", "Hostname", "Location", "Platform", "Version", "ID", "Last Seen", "Status");
		TableRenderer.f_SetOptions(CTableRenderHelper::EOption_Rounded | CTableRenderHelper::EOption_AvoidRowSeparators);
		TableRenderer.f_SetMaxColumnWidth(5, 50);

		uint32 Return = 0;

		auto Now = CTime::fs_NowUTC();

		struct CRow
		{
			auto f_SortTuple() const
			{
				return fg_TupleReferences(m_AppManagerInfo.m_Environment, m_AppManagerInfo.m_HostName, m_AppManagerInfo.m_ProgramDirectory);
			}

			CHostInfo m_HostInfo;
			CStr m_AppManagerID;
			CCloudManager::CAppManagerDynamicInfo m_AppManagerInfo;
		};

		TCVector<CRow> Rows;

		auto fIsFilteredOut = [&](CCloudManager::CAppManagerDynamicInfo const &_AppManagerInfo)
			{
				if (FilterStatusError && *FilterStatusError && !_AppManagerInfo.f_HasErrors())
					return true;

				if
					(
						FilterEnvironment
						&& fg_StrMatchWildcard(_AppManagerInfo.m_Environment.f_GetStr(), FilterEnvironment->f_GetStr()) != EMatchWildcardResult_WholeStringMatchedAndPatternExhausted
					)
				{
					return true;
				}

				return false;
			}
		;

		for (auto &AppManagersForHost : _AppManagers)
		{
			auto &HostInfo = _AppManagers.fs_GetKey(AppManagersForHost);

			if (!AppManagersForHost)
			{
				*_pCommandLine %= "{}Failed getting app managers for host{} '{}': {}\n"_f
					<< AnsiEncoding.f_StatusError()
					<< AnsiEncoding.f_Default()
					<< HostInfo.f_GetDescColored(_pCommandLine->m_AnsiFlags)
					<< AppManagersForHost.f_GetExceptionStr()
				;
				Return = 1;
				continue;
			}

			for (auto &AppManagerInfo : *AppManagersForHost)
			{
				auto &AppManagerID = (*AppManagersForHost).fs_GetKey(AppManagerInfo);

				if (fIsFilteredOut(AppManagerInfo))
					continue;

				Rows.f_Insert({HostInfo, AppManagerID, AppManagerInfo});
			}
		}

		Rows.f_Sort
			(
				[](CRow const &_Left, CRow const &_Right)
				{
					return _Left.f_SortTuple() < _Right.f_SortTuple();
				}
			)
		;

		CStr LastEnvironment;

		for (auto &Row : Rows)
		{
			auto &HostInfo = Row.m_HostInfo;
			auto &AppManagerID = Row.m_AppManagerID;
			auto &AppManagerInfo = Row.m_AppManagerInfo;

			if (AppManagerInfo.m_Environment != LastEnvironment)
			{
				TableRenderer.f_ForceRowSeparator();
				LastEnvironment = AppManagerInfo.m_Environment;
			}

			if (AppManagerInfo.f_HasErrors())
				Return = 1;

			CStr Status = fg_FormatAppManagerStatus(AppManagerInfo, AnsiEncoding);
			CStr HostName = "{}{}{}"_f << AnsiEncoding.f_Foreground256(75) << AppManagerInfo.m_HostName << AnsiEncoding.f_Default();
			CTimeSpan LastSeenTimespan(Now - AppManagerInfo.m_LastSeen);

			CStr LastSeen = fg_FormatTimespan(LastSeenTimespan, AnsiEncoding);
			if (CTimeSpanConvert(LastSeenTimespan).f_GetSeconds() >= 10)
				LastSeen += "\n{}"_f << AppManagerInfo.m_LastSeen;

			CStr Version = "{}{}{} {}{td}{}"_f
				<< AnsiEncoding.f_Foreground256(221)
				<< AppManagerInfo.m_Version
				<< AnsiEncoding.f_Default()
				<< AnsiEncoding.f_Foreground256(248)
				<< AppManagerInfo.m_VersionDate
				<< AnsiEncoding.f_Default()
			;

			TableRenderer.f_AddRow
				(
					HostInfo.f_GetDescColored(_pCommandLine->m_AnsiFlags)
					, AppManagerInfo.m_Environment
					, HostName
					, AppManagerInfo.m_ProgramDirectory
				 	, AppManagerInfo.m_PlatformFamily
				 	, Version
					, AppManagerID
					, LastSeen
					, Status
				)
			;
		}

		if (!bIncludeCloudManager)
			TableRenderer.f_RemoveColumn(0);

		if (!bQuiet)
			TableRenderer.f_Output(_Params);

		co_return Return;
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_CloudManager_Status_Applications
		(
			CEJSON const &_Params
			, TCMap<CHostInfo, TCAsyncResult<TCMap<CCloudManager::CApplicationKey, CCloudManager::CApplicationInfo>>> const &_Applications
			, TCMap<CStr, CCloudManagerAppManagerInfo> const &_AppManagerInfos
			, TCSharedPointer<CCommandLineControl> const &_pCommandLine
		)
	{
		bool bQuiet = _Params["Quiet"].f_Boolean();
		TCOptional<bool> FilterStatusError;
		TCOptional<CStr> FilterEnvironment;
		TCOptional<CStr> FilterVersionManagerApp;
		TCOptional<CStr> FilterName;
		TCOptional<bool> FilterOutOfDateVersion;

		if (auto pValue = _Params.f_GetMember("FilterStatusError"))
			FilterStatusError = pValue->f_Boolean();

		if (auto pValue = _Params.f_GetMember("FilterEnvironment"))
			FilterEnvironment = pValue->f_String();

		if (auto pValue = _Params.f_GetMember("FilterVersionManagerApp"))
			FilterVersionManagerApp = pValue->f_String();

		if (auto pValue = _Params.f_GetMember("FilterName"))
			FilterName = pValue->f_String();

		if (auto pValue = _Params.f_GetMember("FilterOutOfDateVersion"))
			FilterOutOfDateVersion = pValue->f_Boolean();

		CTableRenderHelper TableRenderer = _pCommandLine->f_TableRenderer();
		CAnsiEncoding AnsiEncoding = _pCommandLine->f_AnsiEncoding();

		TableRenderer.f_AddDescription("Applications");
		TableRenderer.f_AddHeadings
			(
			 	"Environment"
			 	, "App Manager"
			 	, "Name"
			 	, "Application"
			 	, "Update [Tags]"
			 	, "Version"
			 	, "Failed Version"
			 	, "Should Have Version"
			 	, "Newer Version Available"
			 	, "Status"
			)
		;
		TableRenderer.f_SetOptions(CTableRenderHelper::EOption_Rounded | CTableRenderHelper::EOption_AvoidRowSeparators);
		TableRenderer.f_SetMaxColumnWidth(5, 50);

		uint32 Return = 0;

		struct CRow
		{
			auto f_SortTuple() const
			{
				return fg_TupleReferences
					(
					 	m_AppManagerInfo.m_Environment
					 	, m_AppManagerInfo.m_HostName
					 	, m_AppManagerInfo.m_ProgramDirectory
					 	, m_ApplicationKey.m_AppManagerID
					 	, m_ApplicationKey.m_Name
					)
				;
			}

			CCloudManagerAppManagerInfo m_AppManagerInfo;
			CHostInfo m_HostInfo;
			CCloudManager::CApplicationKey m_ApplicationKey;
			CCloudManager::CApplicationInfo m_ApplicationInfo;
		};

		TCVector<CRow> Rows;

		auto fHasFailedVersion = [&](CAppManagerInterface::CApplicationInfo const &_ApplicationInfo)
			{
				return _ApplicationInfo.m_FailedVersion.f_IsValid();
			}
		;

		auto fShouldHaveOtherVersion = [&](CAppManagerInterface::CApplicationInfo const &_ApplicationInfo)
			{
				return _ApplicationInfo.m_WantVersion.f_IsValid()
					&& _ApplicationInfo.m_WantVersion != _ApplicationInfo.m_Version
					&& _ApplicationInfo.m_WantVersion != _ApplicationInfo.m_FailedVersion
				;
			}
		;

		auto fHasNewerVersion = [&](CAppManagerInterface::CApplicationInfo const &_ApplicationInfo)
			{
				return _ApplicationInfo.m_NewestUnconditionalVersion.f_IsValid()
					&& _ApplicationInfo.m_NewestUnconditionalVersion != _ApplicationInfo.m_Version
					&& _ApplicationInfo.m_NewestUnconditionalVersion != _ApplicationInfo.m_WantVersion
					&& _ApplicationInfo.m_NewestUnconditionalVersion != _ApplicationInfo.m_FailedVersion
				;
			}
		;

		auto fIsFilteredOut = [&](CCloudManagerAppManagerInfo const &_AppManagerInfo, CAppManagerInterface::CApplicationInfo const &_ApplicationInfo, CStr const &_ApplicationName)
			{
				if (FilterStatusError && *FilterStatusError)
				{
					if (!_AppManagerInfo.f_HasErrors() && _ApplicationInfo.m_StatusSeverity == CAppManagerInterface::EStatusSeverity_None && _AppManagerInfo.m_bActive)
						return true;
				}

				if
					(
						FilterEnvironment
						&& fg_StrMatchWildcard(_AppManagerInfo.m_Environment.f_GetStr(), FilterEnvironment->f_GetStr()) != EMatchWildcardResult_WholeStringMatchedAndPatternExhausted
					)
				{
					return true;
				}

				if (FilterVersionManagerApp && _ApplicationInfo.m_VersionManagerApplication != *FilterVersionManagerApp)
					return true;

				if (FilterName && fg_StrMatchWildcard(_ApplicationName.f_GetStr(), FilterName->f_GetStr()) != EMatchWildcardResult_WholeStringMatchedAndPatternExhausted)
					return true;

				if (FilterOutOfDateVersion && *FilterOutOfDateVersion)
				{
					if
						(
							!fHasFailedVersion(_ApplicationInfo)
 							&& !fShouldHaveOtherVersion(_ApplicationInfo)
 							&& !fHasNewerVersion(_ApplicationInfo)
						)
					{
						return true;
					}
				}

				return false;
			}
		;

		for (auto &ApplicationsForHost : _Applications)
		{
			auto &HostInfo = _Applications.fs_GetKey(ApplicationsForHost);

			if (!ApplicationsForHost)
			{
				*_pCommandLine %= "{}Failed getting applications for host{} '{}': {}\n"_f
					<< AnsiEncoding.f_StatusError()
					<< AnsiEncoding.f_Default()
					<< HostInfo.f_GetDescColored(_pCommandLine->m_AnsiFlags)
					<< ApplicationsForHost.f_GetExceptionStr()
				;
				Return = 1;
				continue;
			}

			for (auto &ApplicationInfo : *ApplicationsForHost)
			{
				auto &ApplicationKey = (*ApplicationsForHost).fs_GetKey(ApplicationInfo);
				CCloudManagerAppManagerInfo AppManagerInfo;
				if (auto pAppManagerInfo = _AppManagerInfos.f_FindEqual(ApplicationKey.m_AppManagerID))
					AppManagerInfo = *pAppManagerInfo;
				else
					AppManagerInfo = {"Unknown"};

				if (fIsFilteredOut(AppManagerInfo, ApplicationInfo.m_ApplicationInfo, ApplicationKey.m_Name))
					continue;

				Rows.f_Insert({AppManagerInfo, HostInfo, ApplicationKey, ApplicationInfo});
			}
		}

		Rows.f_Sort
			(
				[](CRow const &_Left, CRow const &_Right)
				{
					return _Left.f_SortTuple() < _Right.f_SortTuple();
				}
			)
		;

		CStr LastEnvironment;

		bool bHasFailedVersion = false;
		bool bHasWantVersion = false;
		bool bHasNewestVersion = false;

		for (auto &Row : Rows)
		{
			auto &ApplicationKey = Row.m_ApplicationKey;
			auto &ApplicationInfo = Row.m_ApplicationInfo.m_ApplicationInfo;
			auto &AppManagerInfo = Row.m_AppManagerInfo;

			if (ApplicationInfo.m_StatusSeverity > CAppManagerInterface::EStatusSeverity_None || !AppManagerInfo.m_bActive)
				Return = 1;

			if (AppManagerInfo.m_Environment != LastEnvironment)
			{
				TableRenderer.f_ForceRowSeparator();
				LastEnvironment = AppManagerInfo.m_Environment;
			}

			CStr AutoUpdate;
			if (!ApplicationInfo.m_bAutoUpdate)
				AutoUpdate = "{}Manual{} [{vs,vb}]"_f << AnsiEncoding.f_StatusWarning() << AnsiEncoding.f_Default() << ApplicationInfo.m_UpdateTags;
			else
				AutoUpdate = "{}Auto  {} [{vs,vb}]"_f << AnsiEncoding.f_StatusNormal() << AnsiEncoding.f_Default() << ApplicationInfo.m_UpdateTags;

			CStr ApplicationStatus = fg_FormatApplicationStatusSeverity(ApplicationInfo.m_Status, ApplicationInfo.m_StatusSeverity, AnsiEncoding);
			CStr Status;
			if (AppManagerInfo.f_HasErrors())
			{
				Status = "{2}Application Status{3} (outdated)\n{}\n\n{2}App Manager{3}\n{}"_f
					<< ApplicationStatus
					<< fg_FormatAppManagerStatus(AppManagerInfo, AnsiEncoding)
					<< AnsiEncoding.f_Bold()
					<< AnsiEncoding.f_Default()
				;
			}
			else
				Status = ApplicationStatus;

			CStr Version = "{}{}{} {}{td}{}"_f
				<< AnsiEncoding.f_Foreground256(221)
				<< ApplicationInfo.m_Version.m_VersionID
				<< AnsiEncoding.f_Default()
				<< AnsiEncoding.f_Foreground256(248)
				<< ApplicationInfo.m_VersionInfo.m_Time
				<< AnsiEncoding.f_Default()
			;

			CStr FailedVersion;
			if (fHasFailedVersion(ApplicationInfo))
			{
				FailedVersion = "{}{}{} {}{td}{}\n{}"_f
					<< AnsiEncoding.f_StatusError()
					<< ApplicationInfo.m_FailedVersion.m_VersionID
					<< AnsiEncoding.f_Default()
					<< AnsiEncoding.f_Foreground256(248)
					<< ApplicationInfo.m_VersionInfo.m_Time
					<< AnsiEncoding.f_Default()
					<< ApplicationInfo.m_FailedVersionError
				;
				Return = 1;
				bHasFailedVersion = true;
			}

			CStr WantVersion;
			if (fShouldHaveOtherVersion(ApplicationInfo))
			{
				WantVersion = "{}{}{} {}{td}{}"_f
					<< AnsiEncoding.f_StatusWarning()
					<< ApplicationInfo.m_WantVersion.m_VersionID
					<< AnsiEncoding.f_Default()
					<< AnsiEncoding.f_Foreground256(248)
					<< ApplicationInfo.m_WantVersionInfo.m_Time
					<< AnsiEncoding.f_Default()
				;
				bHasWantVersion = true;
			}

			CStr NewestVersion;
			if (fHasNewerVersion(ApplicationInfo))
			{
				NewestVersion = "{}{}{} {}{td}{}"_f
					<< AnsiEncoding.f_Foreground256(208)
					<< ApplicationInfo.m_NewestUnconditionalVersion.m_VersionID
					<< AnsiEncoding.f_Default()
					<< AnsiEncoding.f_Foreground256(248)
					<< ApplicationInfo.m_NewestUnconditionalVersionInfo.m_Time
					<< AnsiEncoding.f_Default()
				;
				bHasNewestVersion = true;
			}

			TableRenderer.f_AddRow
				(
					AppManagerInfo.m_Environment
				 	, "{}{}{}:{}"_f << AnsiEncoding.f_Foreground256(75) << AppManagerInfo.m_HostName << AnsiEncoding.f_Default() << AppManagerInfo.m_ProgramDirectory
					, ApplicationKey.m_Name
					, ApplicationInfo.m_VersionManagerApplication
				 	, AutoUpdate
					, Version
				 	, FailedVersion
					, WantVersion
					, NewestVersion
				 	, Status
				)
			;
		}

		if (CTableRenderHelper::fs_ParseOutputTypeOption(_Params) == CTableRenderHelper::EOutputType_HumanReadable)
		{
			if (!bHasNewestVersion)
				TableRenderer.f_RemoveColumn(8);
			if (!bHasWantVersion)
				TableRenderer.f_RemoveColumn(7);
			if (!bHasFailedVersion)
				TableRenderer.f_RemoveColumn(6);
		}

		if (!bQuiet)
			TableRenderer.f_Output(_Params);

		co_return Return;
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_CloudManager_Status
		(
		 	CEJSON const &_Params
		 	, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine
		 	, ECloudManagerStatusFlag _Flags
		)
	{
		CStr Host = _Params["Host"].f_String();

		co_await fp_CloudManager_SubscribeToServers().f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for cloud managers");

		TCActorResultMap<CHostInfo, TCMap<CStr, CCloudManager::CAppManagerDynamicInfo>> AppManagersResults;
		TCActorResultMap<CHostInfo, TCMap<CCloudManager::CApplicationKey, CCloudManager::CApplicationInfo>> ApplicationsResults;

		for (auto &TrustedCloudManager : mp_CloudManagers.m_Actors)
		{
			if (!Host.f_IsEmpty() && TrustedCloudManager.m_TrustInfo.m_HostInfo.m_HostID != Host)
				continue;
			auto &CloudManager = TrustedCloudManager.m_Actor;
			CloudManager.f_CallActor(&CCloudManager::f_EnumAppManagers)()
				.f_Timeout(mp_Timeout, "Timed out waiting for cloud manager to reply")
				> AppManagersResults.f_AddResult(TrustedCloudManager.m_TrustInfo.m_HostInfo)
			;
			if (_Flags & ECloudManagerStatusFlag_Applications)
			{
				CloudManager.f_CallActor(&CCloudManager::f_EnumApplications)()
					.f_Timeout(mp_Timeout, "Timed out waiting for cloud manager to reply")
					> ApplicationsResults.f_AddResult(TrustedCloudManager.m_TrustInfo.m_HostInfo)
				;
			}
		}

		auto AppManagers = co_await AppManagersResults.f_GetResults();

		uint32 ReturnAppManagers = 0;
		if (_Flags & ECloudManagerStatusFlag_AppManagers)
			ReturnAppManagers = co_await self(&CCloudClientAppActor::fp_CommandLine_CloudManager_Status_AppManagers, _Params, AppManagers, _pCommandLine);

		uint32 ReturnApplications = 0;
		if (_Flags & ECloudManagerStatusFlag_Applications)
		{
			auto Applications = co_await ApplicationsResults.f_GetResults();

			TCMap<CStr, CCloudManagerAppManagerInfo> AppManagerInfos;

			for (auto &AppManagersForHost : AppManagers)
			{
				if (!AppManagersForHost)
					continue;

				for (auto &AppManagerInfo : *AppManagersForHost)
				{
					auto &AppManagerID = (*AppManagersForHost).fs_GetKey(AppManagerInfo);
					auto &OutInfo = AppManagerInfos[AppManagerID];
					OutInfo.m_Environment = AppManagerInfo.m_Environment;
					OutInfo.m_ProgramDirectory = AppManagerInfo.m_ProgramDirectory;
					OutInfo.m_HostName = AppManagerInfo.m_HostName;
					OutInfo.m_bActive = AppManagerInfo.m_bActive;
					OutInfo.m_LastConnectionErrorTime = AppManagerInfo.m_LastConnectionErrorTime;
					OutInfo.m_LastConnectionError = AppManagerInfo.m_LastConnectionError;
					OutInfo.m_OtherErrors = AppManagerInfo.m_OtherErrors;
				}
			}

			ReturnApplications = co_await self(&CCloudClientAppActor::fp_CommandLine_CloudManager_Status_Applications, _Params, Applications, AppManagerInfos, _pCommandLine);
		}

		co_return fg_Max(ReturnAppManagers, ReturnApplications);
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_CloudManager_RemoveAppManager(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		CStr Host = _Params["Host"].f_String();
		CStr AppManagerHostID = _Params["AppManagerHostID"].f_String();

		co_await fp_CloudManager_SubscribeToServers().f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for cloud managers");

		TCActorResultVector<void> AppManagersResults;

		for (auto &TrustedCloudManager : mp_CloudManagers.m_Actors)
		{
			if (!Host.f_IsEmpty() && TrustedCloudManager.m_TrustInfo.m_HostInfo.m_HostID != Host)
				continue;

			auto &CloudManager = TrustedCloudManager.m_Actor;
			(CloudManager.f_CallActor(&CCloudManager::f_RemoveAppManager)(AppManagerHostID) % ("{}"_f << TrustedCloudManager.m_TrustInfo.m_HostInfo)) > AppManagersResults.f_AddResult();
		}

		co_await AppManagersResults.f_GetResults() | g_Unwrap;

		co_return 0;
	}

	auto CCloudClientAppActor::fp_CommandLine_CloudManager_GetSensorReaders(CStr const &_Host)
		-> TCFuture<TCSharedPointer<TCMap<CHostInfo, TCDistributedActorInterfaceWithID<CDistributedAppSensorReader>>>>
	{
		co_await fp_CloudManager_SubscribeToServers().f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for cloud managers");

		TCActorResultMap<CHostInfo, TCDistributedActorInterfaceWithID<CDistributedAppSensorReader>> SensorReaders;

		for (auto &TrustedCloudManager : mp_CloudManagers.m_Actors)
		{
			if (!_Host.f_IsEmpty() && TrustedCloudManager.m_TrustInfo.m_HostInfo.m_HostID != _Host)
				continue;
			auto &CloudManager = TrustedCloudManager.m_Actor;
			CloudManager.f_CallActor(&CCloudManager::f_GetSensorReader)()
				.f_Timeout(mp_Timeout, "Timed out waiting for cloud manager to reply")
				> SensorReaders.f_AddResult(TrustedCloudManager.m_TrustInfo.m_HostInfo)
			;
		}

		co_return fg_Construct(co_await SensorReaders.f_GetResults() | g_Unwrap);
	}

	TCAsyncGenerator<TCVector<CDistributedAppSensorReporter::CSensorInfo>> CCloudClientAppActor::fp_CommandLine_CloudManager_GetAggregatedSensors
		(
			TCSharedPointer<TCMap<CHostInfo, TCDistributedActorInterfaceWithID<CDistributedAppSensorReader>>> const &_pSensorReaders
			, CDistributedAppSensorReader_SensorFilter const &_Filter
		)
	{
		TCActorResultVector<TCAsyncGenerator<TCVector<CDistributedAppSensorReporter::CSensorInfo>>> SensorsResults;

		for (auto &Reader : *_pSensorReaders)
			Reader.f_CallActor(&CDistributedAppSensorReader::f_GetSensors)(fg_TempCopy(_Filter), 1024) > SensorsResults.f_AddResult();

		TCMap<CDistributedAppSensorReporter::CSensorInfoKey, CDistributedAppSensorReporter::CSensorInfo> SensorInfos;

		auto SensorGenerators = co_await SensorsResults.f_GetResults() | g_Unwrap;
		{
			for (auto &SensorGenerator : SensorGenerators)
			{
				for (auto iSensors = co_await fg_Move(SensorGenerator).f_GetIterator(); iSensors; co_await ++iSensors)
				{
					for (auto &SensorInfo : *iSensors)
						SensorInfos[SensorInfo.f_Key()] = SensorInfo;
				}
			}
		}

		TCVector<CDistributedAppSensorReporter::CSensorInfo> ToYield;

		for (auto &SensorInfo : SensorInfos)
			ToYield.f_Insert(fg_Move(SensorInfo));

		co_yield fg_Move(ToYield);

		co_return {};
	}

	TCAsyncGenerator<TCVector<CDistributedAppSensorReader_SensorKeyAndReading>> CCloudClientAppActor::fp_CommandLine_CloudManager_GetAggregatedSensorStatus
		(
			TCSharedPointer<TCMap<CHostInfo, TCDistributedActorInterfaceWithID<CDistributedAppSensorReader>>> const &_pSensorReaders
			, CDistributedAppSensorReader_SensorFilter const &_Filter
		)
	{
		TCActorResultVector<TCAsyncGenerator<TCVector<CDistributedAppSensorReader_SensorKeyAndReading>>> StatusResults;

		for (auto &Reader : *_pSensorReaders)
			Reader.f_CallActor(&CDistributedAppSensorReader::f_GetSensorStatus)(fg_TempCopy(_Filter), 1024) > StatusResults.f_AddResult();

		auto StatusGenerators = co_await StatusResults.f_GetResults() | g_Unwrap;

		TCMap<CDistributedAppSensorReporter::CSensorInfoKey, CDistributedAppSensorReader_SensorKeyAndReading> SensorStatus;

		for (auto &Generator : StatusGenerators)
		{
			for (auto iStatuses = co_await fg_Move(Generator).f_GetIterator(); iStatuses; co_await ++iStatuses)
			{
				for (auto &Status : *iStatuses)
				{
					auto pPreviousStatus = SensorStatus.f_FindEqual(Status.m_SensorInfoKey);
					if (pPreviousStatus && Status.m_Reading.m_UniqueSequence <= pPreviousStatus->m_Reading.m_UniqueSequence)
						continue;

					SensorStatus[Status.m_SensorInfoKey] = Status;
				}
			}
		}

		TCVector<CDistributedAppSensorReader_SensorKeyAndReading> ToYield;

		for (auto &Status : SensorStatus)
			ToYield.f_Insert(fg_Move(Status));

		co_yield fg_Move(ToYield);

		co_return {};
	}

	TCAsyncGenerator<TCVector<CDistributedAppSensorReader_SensorKeyAndReading>> CCloudClientAppActor::fp_CommandLine_CloudManager_GetAggregatedSensorReadings
		(
			TCSharedPointer<TCMap<CHostInfo, TCDistributedActorInterfaceWithID<CDistributedAppSensorReader>>> const &_pSensorReaders
			, CDistributedAppSensorReader_SensorReadingFilter const &_Filter
		)
	{
		TCActorResultVector<TCAsyncGenerator<TCVector<CDistributedAppSensorReader_SensorKeyAndReading>>> ReadingsResults;

		for (auto &Reader : *_pSensorReaders)
			Reader.f_CallActor(&CDistributedAppSensorReader::f_GetSensorReadings)(fg_TempCopy(_Filter), 1024) > ReadingsResults.f_AddResult();

		auto SensorGenerators = co_await ReadingsResults.f_GetResults() | g_Unwrap;
		if (SensorGenerators.f_IsEmpty())
			co_return {};
		if (SensorGenerators.f_GetLen() == 1)
		{
			for (auto iReadings = co_await fg_Move(SensorGenerators.f_GetFirst()).f_GetIterator(); iReadings; co_await ++iReadings)
				co_yield fg_Move(*iReadings);
			co_return {};
		}

		TCActorResultVector<TCAsyncGenerator<TCVector<CDistributedAppSensorReader_SensorKeyAndReading>>::CIterator> IteratorResults;

		for (auto &Generator : SensorGenerators)
			fg_Move(Generator).f_GetIterator() > IteratorResults.f_AddResult();

		TCVector<TCAsyncGenerator<CDistributedAppSensorReader_SensorKeyAndReading>::CIterator> Iterators;

		for (auto &iReadings : co_await IteratorResults.f_GetResults() | g_Unwrap)
		{
			Iterators.f_Insert
				(
					co_await
					(
						fg_CallSafe
						(
							[iReadings = fg_Move(iReadings)]() mutable -> TCAsyncGenerator<CDistributedAppSensorReader_SensorKeyAndReading>
							{
								for (; iReadings; co_await ++iReadings)
								{
									for (auto &Reading : *iReadings)
										co_yield fg_Move(Reading);
								}

								co_return {};
							}
						)
					).f_GetIterator()
				)
			;
		}

		auto fIsLess = [bNewestFirst = !!(_Filter.m_Flags & CDistributedAppSensorReader_SensorReadingFilter::ESensorReadingsFlag_ReportNewestFirst)]
			(CDistributedAppSensorReader_SensorKeyAndReading const &_Left, CDistributedAppSensorReader_SensorKeyAndReading const &_Right)
			{
				if (bNewestFirst)
				{
					return fg_TupleReferences(_Right.m_Reading.m_Timestamp, _Right.m_Reading.m_UniqueSequence)
						< fg_TupleReferences(_Left.m_Reading.m_Timestamp, _Left.m_Reading.m_UniqueSequence)
					;
				}
				else
				{
					return fg_TupleReferences(_Left.m_Reading.m_Timestamp, _Left.m_Reading.m_UniqueSequence)
						< fg_TupleReferences(_Right.m_Reading.m_Timestamp, _Right.m_Reading.m_UniqueSequence)
					;
				}
			}
		;

		bool bAtEnd = false;
		CDistributedAppSensorReader_SensorKeyAndReading LastReading;

		auto fGetNextReading = [&]() -> TCFuture<CDistributedAppSensorReader_SensorKeyAndReading>
			{
				co_await ECoroutineFlag_AllowReferences;

				TCAsyncGenerator<CDistributedAppSensorReader_SensorKeyAndReading>::CIterator *pBestReading = nullptr;

				while (true)
				{
					for (auto &iReadings : Iterators)
					{
						if (!iReadings)
							continue;

						if (!pBestReading)
						{
							pBestReading = &iReadings;
							continue;
						}

						if (fIsLess(*iReadings, **pBestReading))
							pBestReading = &iReadings;
					}

					if (pBestReading)
					{
						auto ToReturn = fg_Move(**pBestReading);
						co_await ++*pBestReading;

						if (ToReturn.m_SensorInfoKey == LastReading.m_SensorInfoKey && ToReturn.m_Reading.m_UniqueSequence == LastReading.m_Reading.m_UniqueSequence)
							continue;

						co_return fg_Move(ToReturn);
					}
					else
						break;
				}

				bAtEnd = true;
				co_return {};
			}
		;

		TCVector<CDistributedAppSensorReader_SensorKeyAndReading> ToYield;

		while (true)
		{
			auto NextReading = co_await fGetNextReading();
			if (bAtEnd)
				break;

			ToYield.f_Insert(fg_Move(NextReading));
			if (ToYield.f_GetLen() >= 1024)
				co_yield fg_Move(ToYield);
		}

		if (!ToYield.f_IsEmpty())
			co_yield fg_Move(ToYield);

		co_return {};
	}
}
