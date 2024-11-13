// Copyright © 2019 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/ActorSubscription>
#include <Mib/Concurrency/DistributedAppSensorReader>
#include <Mib/Concurrency/DistributedAppLogReader>
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
					"Host?"_o=
					{
						"Names"_o= {"--host"}
						, "Default"_o= ""
						, "Description"_o= "Limit cloud managers to only specified host ID."
					}
				}
			)
		;

		auto QuietOption = "Quiet?"_o=
			{
				"Names"_o= {"--quiet", "-q"}
				, "Default"_o= false
				, "Description"_o= "Don't output information on std out, just return the status."
			}
		;
		auto IncludeCloudManagerOption = "IncludeCloudManager?"_o=
			{
				"Names"_o= {"--include-cloud-manager", "-c"}
				, "Default"_o= false
				, "Description"_o= "Include the cloud manager column."
			}
		;
		auto FilterStatusError = "FilterStatusError?"_o=
			{
				"Names"_o= {"--filter-status-error"}
				, "Type"_o= true
				, "Description"_o= "Include applications and app managers with error status."
			}
		;
		auto FilterEnvironment = "FilterEnvironment?"_o=
			{
				"Names"_o= {"--filter-environment"}
				, "Type"_o= ""
				, "Description"_o= "Include applications and appmanagers with specified environment.\n"
				"Wildcard search."
			}
		;
		auto FilterName = "FilterName?"_o=
			{
				"Names"_o= {"--filter-name"}
				, "Type"_o= ""
				, "Description"_o= "Include applications with specified application name."
			}
		;
		auto FilterVersionManagerApp = "FilterVersionManagerApp?"_o=
			{
				"Names"_o= {"--filter-application"}
				, "Type"_o= ""
				, "Description"_o= "Include applications with specified version manager app name."
			}
		;
		auto FilterOutOfDateVersion = "FilterOutOfDateVersion?"_o=
			{
				"Names"_o= {"--filter-out-of-date"}
				, "Type"_o= true
				, "Description"_o= "Include applications with out of date versions."
			}
		;

		_Section.f_RegisterCommand
			(
				{
					"Names"_o= {"--cloud-manager-status"}
					, "Description"_o= "List the overall status for resources controlled by the Cloud Manager."
					, "Status"_o=
					{
						"0"_o= "The status of all resources is OK."
						, "1"_o= "One or more resources has an error status."
					}
					, "Options"_o=
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
				, [this](CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine) -> TCFuture<uint32>
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
					"Names"_o= {"--cloud-manager-app-manager-status"}
					, "Description"_o= "List the status for app managers controlled by the Cloud Manager."
					, "Status"_o=
					{
						"0"_o= "The status of all app managers is OK."
						, "1"_o= "One or more app managers has an error status."
					}
					, "Options"_o=
					{
						QuietOption
						, IncludeCloudManagerOption
						, CTableRenderHelper::fs_OutputTypeOption()
						, FilterStatusError
						, FilterEnvironment
					}
				}
				, [this](CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine) -> TCFuture<uint32>
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
					"Names"_o= {"--cloud-manager-application-status"}
					, "Description"_o= "List the status for applications in app managers controlled by the Cloud Manager."
					, "Status"_o=
					{
						"0"_o= "The status of all applications is OK."
						, "1"_o= "One or more applications has an error status."
					}
					, "Options"_o=
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
				, [this](CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine) -> TCFuture<uint32>
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
					"Names"_o= {"--cloud-manager-remove-app-manager"}
					, "Description"_o= "Remove app manager from cloud manager database."
					, "Parameters"_o=
					{
						"AppManagerHostID"_o=
						{
							"Type"_o= ""
							, "Description"_o= "The host ID of the app manager to remove."
						}
					}
					, "Options"_o=
					{
						QuietOption
					}
				}
				, [this](CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine) -> TCFuture<uint32>
				{
					return g_Future <<= self(&CCloudClientAppActor::fp_CommandLine_CloudManager_RemoveAppManager, _Params, _pCommandLine);
				}
				, EDistributedAppCommandFlag_WaitForRemotes
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_o= {"--cloud-manager-remove-sensor"}
					, "Description"_o= "Remove sensor from cloud manager."
					, "Options"_o=
					{
						"SensorHostID?"_o=
						{
							"Names"_o= {"--sensor-host-id"}
							, "Type"_o= ""
							, "Description"_o= "The host ID of the sensor to remove."
						}
						, "SensorApplication?"_o=
						{
							"Names"_o= {"--sensor-application"}
							, "Type"_o= ""
							, "Description"_o= "The application of the sensor to remove. Supports wildcards."
						}
						, "SensorIdentifier?"_o=
						{
							"Names"_o= {"--sensor-identifier"}
							, "Type"_o= ""
							, "Description"_o= "The identifier of the sensor to remove. Supports wildcards."
						}
						, "SensorIdentifierScope?"_o=
						{
							"Names"_o= {"--sensor-identifier-scope"}
							, "Type"_o= ""
							, "Description"_o= "The identifier scope of the sensor to remove. Supports wildcards."
						}
						, "Force?"_o=
						{
							"Names"_o= {"--force"}
							, "Default"_o= false
							, "Description"_o= "Force removal of all sensors when no filtering options are specified."
						}
						, QuietOption
					}
				}
				, [this](CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine) -> TCFuture<uint32>
				{
					return g_Future <<= self(&CCloudClientAppActor::fp_CommandLine_CloudManager_RemoveSensor, _Params, _pCommandLine);
				}
				, EDistributedAppCommandFlag_WaitForRemotes
			)
		;

		_Section.f_RegisterCommand
			(
				{
					"Names"_o= {"--cloud-manager-remove-log"}
					, "Description"_o= "Remove log from cloud manager."
					, "Options"_o=
					{
						"LogHostID?"_o=
						{
							"Names"_o= {"--log-host-id"}
							, "Type"_o= ""
							, "Description"_o= "The host ID of the log to remove."
						}
						, "LogApplication?"_o=
						{
							"Names"_o= {"--log-application"}
							, "Type"_o= ""
							, "Description"_o= "The application of the log to remove. Supports wildcards."
						}
						, "LogIdentifier?"_o=
						{
							"Names"_o= {"--log-identifier"}
							, "Type"_o= ""
							, "Description"_o= "The identifier of the log to remove. Supports wildcards."
						}
						, "LogIdentifierScope?"_o=
						{
							"Names"_o= {"--log-identifier-scope"}
							, "Type"_o= ""
							, "Description"_o= "The identifier scope of the log to remove. Supports wildcards."
						}
						, "Force?"_o=
						{
							"Names"_o= {"--force"}
							, "Default"_o= false
							, "Description"_o= "Force removal of all logs when no filtering options are specified."
						}
						, QuietOption
					}
				}
				, [this](CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine) -> TCFuture<uint32>
				{
					return g_Future <<= self(&CCloudClientAppActor::fp_CommandLine_CloudManager_RemoveLog, _Params, _pCommandLine);
				}
				, EDistributedAppCommandFlag_WaitForRemotes
			)
		;

		_Section.f_RegisterCommand
			(
				{
					"Names"_o= {"--cloud-manager-snooze-sensor"}
					, "Description"_o= "Snooze sensor from reporting as problem in cloud manager."
					, "Options"_o=
					{
						"SensorHostID?"_o=
						{
							"Names"_o= {"--sensor-host-id"}
							, "Type"_o= ""
							, "Description"_o= "The host ID of the sensor to snooze."
						}
						, "SensorApplication?"_o=
						{
							"Names"_o= {"--sensor-application"}
							, "Type"_o= ""
							, "Description"_o= "The application of the sensor to snooze. Supports wildcards."
						}
						, "SensorIdentifier?"_o=
						{
							"Names"_o= {"--sensor-identifier"}
							, "Type"_o= ""
							, "Description"_o= "The identifier of the sensor to snooze. Supports wildcards."
						}
						, "SensorIdentifierScope?"_o=
						{
							"Names"_o= {"--sensor-identifier-scope"}
							, "Type"_o= ""
							, "Description"_o= "The identifier scope of the sensor to snooze. Supports wildcards."
						}
						, "Force?"_o=
						{
							"Names"_o= {"--force"}
							, "Default"_o= false
							, "Description"_o= "Force snoozing of all sensors when no filtering options are specified."
						}
						, "Duration?"_o=
						{
							"Names"_o= {"--duration"}
							, "Default"_o= 7.0
							, "Description"_o= "Number of days to snooze sensor."
						}
						, "UnSnooze?"_o=
						{
							"Names"_o= {"--un-snooze", "-u"}
							, "Default"_o= false
							, "Description"_o= "Remove snoozing instead of adding it."
						}
						, QuietOption
					}
				}
				, [this](CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine) -> TCFuture<uint32>
				{
					return g_Future <<= self(&CCloudClientAppActor::fp_CommandLine_CloudManager_SnoozeSensor, _Params, _pCommandLine);
				}
				, EDistributedAppCommandFlag_WaitForRemotes
			)
		;
		
		_Section.f_RegisterCommand
			(
				{
					"Names"_o= {"--cloud-manager-expected-os-version-list"}
					, "Description"_o= "List expected os version settings."
					, "Options"_o=
					{
						QuietOption
						, CTableRenderHelper::fs_OutputTypeOption()
					}
				}
				, [this](CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine) -> TCFuture<uint32>
				{
					return g_Future <<= self(&CCloudClientAppActor::fp_CommandLine_CloudManager_ExpectedOsVersionList, _Params, _pCommandLine);
				}
				, EDistributedAppCommandFlag_WaitForRemotes
			)
		;

		_Section.f_RegisterCommand
			(
				{
					"Names"_o= {"--cloud-manager-expected-os-version-set"}
					, "Description"_o= "List expected os version settings."
					, "Options"_o=
					{
						"OsName"_o=
						{
							"Names"_o= {"--os-name"}
							, "Type"_o= ""
							, "Description"_o= "The name of the OS to set for."
						}
						, "CurrentVersionMajor?"_o=
						{
							"Names"_o= {"--apply-to-version-major"}
							, "Type"_o= 0
							, "Description"_o= "The major version of the OS that this should apply for."
						}
						, "CurrentVersionMinor?"_o=
						{
							"Names"_o= {"--apply-to-version-minor"}
							, "Type"_o= 0
							, "Description"_o= "The minor version of the OS that this should apply for."
						}
						, "MinVersion?"_o=
						{
							"Names"_o= {"--min-version"}
							, "Type"_o= ""
							, "Description"_o= "The minimum version required."
						}
						, "MaxVersion?"_o=
						{
							"Names"_o= {"--max-version"}
							, "Type"_o= ""
							, "Description"_o= "The maximum version required."
						}
						, "Deprecated?"_o=
						{
							"Names"_o= {"--deprecated"}
							, "Default"_o= false
							, "Description"_o= "Deprecated the OS version, or whole OS."
						}
					}
				}
				, [this](CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine) -> TCFuture<uint32>
				{
					return g_Future <<= self(&CCloudClientAppActor::fp_CommandLine_CloudManager_ExpectedOsVersionSet, _Params, _pCommandLine);
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
					CEJSONSorted const &_Params
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
					CEJSONSorted const &_Params
					, TCSharedPointer<CCommandLineControl> const &_pCommandLine
					, CDistributedAppSensorReader_SensorStatusFilter const &_Filter
					, ESensorOutputFlag _Flags
					, CStr const &_TableType
				)
				-> TCFuture<uint32>
				{
					auto SensorReaders = co_await self(&CCloudClientAppActor::fp_CommandLine_CloudManager_GetSensorReaders, _Params["Host"].f_String());
					auto SensorsGenerator = co_await self(&CCloudClientAppActor::fp_CommandLine_CloudManager_GetAggregatedSensors, SensorReaders, _Filter.m_SensorFilter);
					auto ReadingsGenerator = co_await self(&CCloudClientAppActor::fp_CommandLine_CloudManager_GetAggregatedSensorStatus, SensorReaders, _Filter);

					CDistributedAppSensorReader_SensorReadingFilter Filter;
					Filter.m_Flags = CDistributedAppSensorReader_SensorReadingFilter::ESensorReadingsFlag_None;

					co_return co_await self
						(
							&CDistributedAppActor::f_CommandLine_SensorReadingsOutput
							, _pCommandLine
							, fg_Move(ReadingsGenerator)
							, fg_Move(SensorsGenerator)
							, 0
							, _Flags | ESensorOutputFlag_Status
							, _TableType
							, Filter
						)
					;
				}
				, g_ActorFunctor / [this]
				(
					CEJSONSorted const &_Params
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
							, _Filter
						)
					;
				}
				, EDistributedAppCommandFlag_WaitForRemotes
			)
		;

		fp_BuildDefaultCommandLine_DistributedLog_Customizable
			(
				_Section
				, "cloud-manager-"
				, g_ActorFunctor / [this]
				(
					CEJSONSorted const &_Params
					, TCSharedPointer<CCommandLineControl> const &_pCommandLine
					, CDistributedAppLogReader_LogFilter const &_Filter
					, ELogOutputFlag _Flags
					, uint32 _Verbosity
					, CStr const &_TableType
				)
				-> TCFuture<uint32>
				{
					auto LogReaders = co_await self(&CCloudClientAppActor::fp_CommandLine_CloudManager_GetLogReaders, _Params["Host"].f_String());
					co_return co_await self
						(
							&CDistributedAppActor::f_CommandLine_LogListOutput
							, _pCommandLine
							, co_await self(&CCloudClientAppActor::fp_CommandLine_CloudManager_GetAggregatedLogs, LogReaders, _Filter)
							, _Flags
							, _Verbosity
							, _TableType
						)
					;
				}
				, g_ActorFunctor / [this]
				(
					CEJSONSorted const &_Params
					, TCSharedPointer<CCommandLineControl> const &_pCommandLine
					, CDistributedAppLogReader_LogEntryFilter const &_Filter
					, uint64 _MaxEntries
					, ELogOutputFlag _Flags
					, uint32 _Verbosity
					, CStr const &_TableType
				)
				-> TCFuture<uint32>
				{
					auto LogReaders = co_await self(&CCloudClientAppActor::fp_CommandLine_CloudManager_GetLogReaders, _Params["Host"].f_String());
					auto LogsGenerator = co_await self(&CCloudClientAppActor::fp_CommandLine_CloudManager_GetAggregatedLogs, LogReaders, _Filter.m_LogFilter);
					auto EntriesGenerator = co_await self(&CCloudClientAppActor::fp_CommandLine_CloudManager_GetAggregatedLogEntries, LogReaders, _Filter);

					co_return co_await self
						(
							&CDistributedAppActor::f_CommandLine_LogEntriesOutput
							, _pCommandLine
							, fg_Move(EntriesGenerator)
							, fg_Move(LogsGenerator)
							, _MaxEntries
							, _Flags
							, _Verbosity
							, _TableType
							, _Filter
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
		CStr fg_FormatAppManagerStatus(tf_CInfo const &_Info, CAnsiEncoding const &_AnsiEncoding, CTime const &_Now)
		{
			if (!_Info.m_bActive)
			{
				if (_Info.m_PauseReportingFor == fp32::fs_Inf())
					return gc_Str<"Reporting paused">;
				else if (!_Info.m_PauseReportingFor.f_IsNan())
				{
					auto PausedTime = (_Now - _Info.m_LastSeen).f_GetSecondsFraction();
					bool bIsPaused = PausedTime < _Info.m_PauseReportingFor;

					if (bIsPaused)
						return "Reporting still paused for {}"_f << fg_SecondsDurationToHumanReadable(_Info.m_PauseReportingFor - PausedTime);
				}

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

	bool CCloudClientAppActor::CCloudManagerAppManagerInfo::f_IsPaused(CTime const &_Now) const
	{
		if (m_PauseReportingFor == fp32::fs_Inf())
			return true;
		else if (!m_PauseReportingFor.f_IsNan() && m_LastSeen.f_IsValid())
			return (_Now - m_LastSeen).f_GetSecondsFraction() < m_PauseReportingFor;

		return false;
	}

	bool CCloudClientAppActor::CCloudManagerAppManagerInfo::f_HasErrors(CTime const &_Now) const
	{
		return !f_IsPaused(_Now) && (!m_bActive || !m_OtherErrors.f_IsEmpty());
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_CloudManager_Status_AppManagers
		(
			CEJSONSorted const &_Params
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
					return _Left.f_SortTuple() <=> _Right.f_SortTuple();
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

			CStr Status = fg_FormatAppManagerStatus(AppManagerInfo, AnsiEncoding, Now);
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
			CEJSONSorted const &_Params
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

		CTime Now = CTime::fs_NowUTC();

		TableRenderer.f_AddDescription("Applications");
		CTableRenderHelper::CColumnHelper Columns(0);

		Columns.f_AddHeading("Environment", 0);
		Columns.f_AddHeading("App Manager", 0);
		Columns.f_AddHeading("Name", 0);
		Columns.f_AddHeading("Application", 0);
		Columns.f_AddHeading("ID", 0);
		Columns.f_AddHeading("Update [Tags]", 0);
		Columns.f_AddHeading("Version", 0);
		Columns.f_AddHeading("Failed Version", 0);
		Columns.f_AddHeading("Should Have Version", 0);
		Columns.f_AddHeading("Newer Version Available", 0);
		Columns.f_AddHeading("Status", 0);

		TableRenderer.f_AddHeadings(&Columns);
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
					if (!_AppManagerInfo.f_HasErrors(Now) && _ApplicationInfo.m_StatusSeverity == CAppManagerInterface::EStatusSeverity_None && _AppManagerInfo.m_bActive)
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
					return _Left.f_SortTuple() <=> _Right.f_SortTuple();
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
			if (AppManagerInfo.f_HasErrors(Now))
			{
				Status = "{2}Application Status{3} (potentially outdated)\n{}\n\n{2}App Manager{3}\n{}"_f
					<< ApplicationStatus
					<< fg_FormatAppManagerStatus(AppManagerInfo, AnsiEncoding, Now)
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
					, ApplicationInfo.m_HostID
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
				Columns.f_SetVerbose("Newer Version Available");
			if (!bHasWantVersion)
				Columns.f_SetVerbose("Should Have Version");
			if (!bHasFailedVersion)
				Columns.f_SetVerbose("Failed Version");
		}

		if (!bQuiet)
			TableRenderer.f_Output(_Params);

		co_return Return;
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_CloudManager_Status
		(
			CEJSONSorted const &_Params
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
					OutInfo.m_PauseReportingFor = AppManagerInfo.m_PauseReportingFor;
					OutInfo.m_LastConnectionErrorTime = AppManagerInfo.m_LastConnectionErrorTime;
					OutInfo.m_LastSeen = AppManagerInfo.m_LastSeen;
					OutInfo.m_LastConnectionError = AppManagerInfo.m_LastConnectionError;
					OutInfo.m_OtherErrors = AppManagerInfo.m_OtherErrors;
				}
			}

			ReturnApplications = co_await self(&CCloudClientAppActor::fp_CommandLine_CloudManager_Status_Applications, _Params, Applications, AppManagerInfos, _pCommandLine);
		}

		co_return fg_Max(ReturnAppManagers, ReturnApplications);
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_CloudManager_RemoveAppManager(CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		CStr Host = _Params["Host"].f_String();
		CStr AppManagerHostID = _Params["AppManagerHostID"].f_String();
		bool bQuiet = _Params["Quiet"].f_Boolean();

		co_await fp_CloudManager_SubscribeToServers().f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for cloud managers");

		TCActorResultVector<CCloudManager::CRemoveAppManagerReturn> AppManagersResults;

		for (auto &TrustedCloudManager : mp_CloudManagers.m_Actors)
		{
			if (!Host.f_IsEmpty() && TrustedCloudManager.m_TrustInfo.m_HostInfo.m_HostID != Host)
				continue;

			auto &CloudManager = TrustedCloudManager.m_Actor;
			(CloudManager.f_CallActor(&CCloudManager::f_RemoveAppManager)(AppManagerHostID) % ("{}"_f << TrustedCloudManager.m_TrustInfo.m_HostInfo)) > AppManagersResults.f_AddResult();
		}

		auto RemoveStatistics = co_await (co_await AppManagersResults.f_GetResults() | g_Unwrap);

		if (!bQuiet)
		{
			CCloudManager::CRemoveAppManagerReturn AggregatedStatistics;

			for (auto &Statistic : RemoveStatistics)
			{
				AggregatedStatistics.m_nRemovedAppManagers += Statistic.m_nRemovedAppManagers;
				AggregatedStatistics.m_nRemovedHostIDs += Statistic.m_nRemovedHostIDs;
			}

			*_pCommandLine %= "Removed {} app managers\n"
				"Removed {} host IDs\n"_f
				<< AggregatedStatistics.m_nRemovedAppManagers
				<< AggregatedStatistics.m_nRemovedHostIDs
			;
		}

		co_return 0;
	}

	namespace
	{
		CExceptionPointer fg_ParseSensorFilter(auto &o_Filter, CEJSONSorted const &_Params, CStr const &_Action)
		{
			if (auto pValue = _Params.f_GetMember("SensorHostID"))
				o_Filter.m_HostID = pValue->f_String();

			if (auto pValue = _Params.f_GetMember("SensorApplication"))
				o_Filter.m_Scope = CDistributedAppSensorReporter::CSensorScope_Application{.m_ApplicationName = pValue->f_String()};

			if (auto pValue = _Params.f_GetMember("SensorIdentifier"))
				o_Filter.m_Identifier = pValue->f_String();

			if (auto pValue = _Params.f_GetMember("SensorIdentifierScope"))
				o_Filter.m_IdentifierScope = pValue->f_String();

			if (!o_Filter.m_HostID && !o_Filter.m_Scope && !o_Filter.m_Identifier && !o_Filter.m_IdentifierScope)
			{
				if (!_Params["Force"].f_Boolean())
					return DMibErrorInstance("No filtering specified. To force {} of all sensors specify --force."_f << _Action).f_ExceptionPointer();
			}

			return nullptr;
		}
	}
	
	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_CloudManager_RemoveSensor(CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		CStr Host = _Params["Host"].f_String();
		bool bQuiet = _Params["Quiet"].f_Boolean();

		co_await fp_CloudManager_SubscribeToServers().f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for cloud managers");

		CCloudManager::CRemoveSensor RemoveSensor;

		if (auto pException = fg_ParseSensorFilter(RemoveSensor.m_Filter, _Params, "removal"))
			co_return fg_Move(pException);

		TCActorResultVector<uint32> AppManagersResults;

		for (auto &TrustedCloudManager : mp_CloudManagers.m_Actors)
		{
			if (!Host.f_IsEmpty() && TrustedCloudManager.m_TrustInfo.m_HostInfo.m_HostID != Host)
				continue;

			auto &CloudManager = TrustedCloudManager.m_Actor;
			(CloudManager.f_CallActor(&CCloudManager::f_RemoveSensor)(RemoveSensor) % ("{}"_f << TrustedCloudManager.m_TrustInfo.m_HostInfo)) > AppManagersResults.f_AddResult();
		}

		auto AllRemoved = co_await (co_await AppManagersResults.f_GetResults() | g_Unwrap);

		if (!bQuiet)
		{
			uint32 nRemoved = 0;
			for (auto &Removed : AllRemoved)
				nRemoved += Removed;

			*_pCommandLine %= "Removed {} sensors\n"_f << nRemoved;
		}

		co_return 0;
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_CloudManager_RemoveLog(CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		CStr Host = _Params["Host"].f_String();
		bool bQuiet = _Params["Quiet"].f_Boolean();

		co_await fp_CloudManager_SubscribeToServers().f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for cloud managers");

		CCloudManager::CRemoveLog RemoveLog;

		if (auto pValue = _Params.f_GetMember("LogHostID"))
			RemoveLog.m_Filter.m_HostID = pValue->f_String();

		if (auto pValue = _Params.f_GetMember("LogApplication"))
			RemoveLog.m_Filter.m_Scope = CDistributedAppLogReporter::CLogScope_Application{.m_ApplicationName = pValue->f_String()};

		if (auto pValue = _Params.f_GetMember("LogIdentifier"))
			RemoveLog.m_Filter.m_Identifier = pValue->f_String();

		if (auto pValue = _Params.f_GetMember("LogIdentifierScope"))
			RemoveLog.m_Filter.m_IdentifierScope = pValue->f_String();

		if (!RemoveLog.m_Filter.m_HostID && !RemoveLog.m_Filter.m_Scope && !RemoveLog.m_Filter.m_Identifier && !RemoveLog.m_Filter.m_IdentifierScope)
		{
			if (!_Params["Force"].f_Boolean())
				co_return DMibErrorInstance("No filtering specified. To force removal of all logs specify --force.");
		}
		
		NConcurrency::CDistributedAppLogReporter::CLogInfoKey LogInfoKey;

		LogInfoKey.m_HostID = _Params["LogHostID"].f_String();
		if (auto Application = _Params["LogApplication"].f_String())
			LogInfoKey.m_Scope = CDistributedAppLogReporter::CLogScope_Application{.m_ApplicationName = Application};
		LogInfoKey.m_Identifier = _Params["LogIdentifier"].f_String();
		LogInfoKey.m_IdentifierScope = _Params["LogIdentifierScope"].f_String();

		TCActorResultVector<uint32> AppManagersResults;

		for (auto &TrustedCloudManager : mp_CloudManagers.m_Actors)
		{
			if (!Host.f_IsEmpty() && TrustedCloudManager.m_TrustInfo.m_HostInfo.m_HostID != Host)
				continue;

			auto &CloudManager = TrustedCloudManager.m_Actor;
			(CloudManager.f_CallActor(&CCloudManager::f_RemoveLog)(RemoveLog) % ("{}"_f << TrustedCloudManager.m_TrustInfo.m_HostInfo)) > AppManagersResults.f_AddResult();
		}

		auto AllRemoved = co_await (co_await AppManagersResults.f_GetResults() | g_Unwrap);

		if (!bQuiet)
		{
			uint32 nRemoved = 0;
			for (auto &Removed : AllRemoved)
				nRemoved += Removed;

			*_pCommandLine %= "Removed {} logs\n"_f << nRemoved;
		}

		co_return 0;
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_CloudManager_SnoozeSensor(CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		CStr Host = _Params["Host"].f_String();
		bool bQuiet = _Params["Quiet"].f_Boolean();
		bool bUnSnooze = _Params["UnSnooze"].f_Boolean();

		co_await fp_CloudManager_SubscribeToServers().f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for cloud managers");

		auto Duration = _Params["Duration"].f_Float();

		if (Duration.f_IsInvalid() || Duration <= 0.0)
			co_return DMibErrorInstance("Duration has to be a positive number of days.");

		CCloudManager::CSnoozeSensor SnoozeSensor;
		SnoozeSensor.m_SnoozeDuration = bUnSnooze ? CTimeSpan() : CTimeSpanConvert::fs_CreateSpanFromDays(Duration);

		if (auto pException = fg_ParseSensorFilter(SnoozeSensor.m_Filter, _Params, "snoozing"))
			co_return fg_Move(pException);

		TCActorResultVector<uint32> AppManagersResults;

		for (auto &TrustedCloudManager : mp_CloudManagers.m_Actors)
		{
			if (!Host.f_IsEmpty() && TrustedCloudManager.m_TrustInfo.m_HostInfo.m_HostID != Host)
				continue;

			auto &CloudManager = TrustedCloudManager.m_Actor;
			(CloudManager.f_CallActor(&CCloudManager::f_SnoozeSensor)(SnoozeSensor) % ("{}"_f << TrustedCloudManager.m_TrustInfo.m_HostInfo)) > AppManagersResults.f_AddResult();
		}

		auto AllSnoozed = co_await (co_await AppManagersResults.f_GetResults() | g_Unwrap);

		if (!bQuiet)
		{
			uint32 nChanged = 0;
			for (auto &Changed : AllSnoozed)
				nChanged += Changed;

			if (bUnSnooze)
				*_pCommandLine %= "Un-snoozed {} sensors\n"_f << nChanged;
			else
				*_pCommandLine %= "Snoozed {} sensors\n"_f << nChanged;
		}

		co_return 0;
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_CloudManager_ExpectedOsVersionList(CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		CStr Host = _Params["Host"].f_String();
		bool bQuiet = _Params["Quiet"].f_Boolean();

		co_await fp_CloudManager_SubscribeToServers().f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for cloud managers");

		TCActorResultVector<uint32> ListResults;

		TCActorResultMap<CHostInfo, TCMap<CStr, CCloudManager::CExpectedVersions>> Results;
		for (auto &TrustedCloudManager : mp_CloudManagers.m_Actors)
		{
			if (!Host.f_IsEmpty() && TrustedCloudManager.m_TrustInfo.m_HostInfo.m_HostID != Host)
				continue;

			auto &CloudManager = TrustedCloudManager.m_Actor;
			CloudManager.f_CallActor(&CCloudManager::f_EnumExpectedOsVersions)()
				.f_Timeout(mp_Timeout, "Timed out waiting for cloud manager to reply")
				> Results.f_AddResult(TrustedCloudManager.m_TrustInfo.m_HostInfo)
			;
		}

		auto AllResults = co_await Results.f_GetUnwrappedResults();

		CTableRenderHelper TableRenderer = _pCommandLine->f_TableRenderer();
		CAnsiEncoding AnsiEncoding = _pCommandLine->f_AnsiEncoding();

		CTableRenderHelper::CColumnHelper Columns(bQuiet ? 0 : 1);

		Columns.f_AddHeading("Cloud Manager", 1);
		Columns.f_AddHeading("OS Name", 0);
		Columns.f_AddHeading("Applies Major", 0);
		Columns.f_AddHeading("Applies Minor", 0);
		Columns.f_AddHeading("Min Version", 0);
		Columns.f_AddHeading("Max Version", 0);

		TableRenderer.f_AddHeadings(&Columns);

		for (auto &Result : AllResults)
		{
			auto &HostInfo = AllResults.fs_GetKey(Result);

			auto HostInfoString = HostInfo.f_GetDescColored(_pCommandLine->m_AnsiFlags);

			for (auto &ExpectedVersions : Result)
			{
				auto &OsName = Result.fs_GetKey(ExpectedVersions);
				for (auto &Version : ExpectedVersions.m_Versions)
				{
					auto &CurrentVersion = ExpectedVersions.m_Versions.fs_GetKey(Version);

					auto fFormatOptional = [](auto &&_Optional) -> CStr
						{
							if (!_Optional)
								return {};
							return "{}"_f << *_Optional;
						}
					;

					TableRenderer.f_AddRow
						(
							HostInfoString
							, OsName
							, fFormatOptional(CurrentVersion.m_Major)
							, fFormatOptional(CurrentVersion.m_Minor)
							, fFormatOptional(Version.m_Min)
							, fFormatOptional(Version.m_Max)
						)
					;
				}
			}
		}

		TableRenderer.f_Output(_Params);

		co_return 0;
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_CloudManager_ExpectedOsVersionSet(CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		CStr Host = _Params["Host"].f_String();

		co_await fp_CloudManager_SubscribeToServers().f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for cloud managers");

		auto OsName = _Params["OsName"].f_String();

		CCloudManager::CCurrentVersion CurrentVersion;

		if (auto *pValue =_Params.f_GetMember("CurrentVersionMajor"))
			CurrentVersion.m_Major = pValue->f_Integer();

		if (auto *pValue =_Params.f_GetMember("CurrentVersionMinor"))
		{
			if (!CurrentVersion.m_Major)
				co_return DMibErrorInstance("You have to specify --apply-to-version-major if you specify --apply-to-version-minor");
			CurrentVersion.m_Minor = pValue->f_Integer();
		}

		CCloudManager::CExpectedVersionRange ExpectedVersion;
		bool bDeprecated = _Params["Deprecated"].f_Boolean();
		if (bDeprecated)
			ExpectedVersion.f_SetDeprecated();

		if (auto *pValue =_Params.f_GetMember("MinVersion"))
		{
			if (bDeprecated)
				co_return DMibErrorInstance("--min-version cannoct be specified when deprecated is set");
			ExpectedVersion.m_Min = co_await CCloudManager::CVersion::fs_ParseVersion(pValue->f_String());
		}
		
		if (auto *pValue =_Params.f_GetMember("MaxVersion"))
		{
			if (bDeprecated)
				co_return DMibErrorInstance("--max-version cannoct be specified when deprecated is set");
			ExpectedVersion.m_Max = co_await CCloudManager::CVersion::fs_ParseVersion(pValue->f_String());
		}

		TCActorResultVector<void> Results;

		for (auto &TrustedCloudManager : mp_CloudManagers.m_Actors)
		{
			if (!Host.f_IsEmpty() && TrustedCloudManager.m_TrustInfo.m_HostInfo.m_HostID != Host)
				continue;

			auto &CloudManager = TrustedCloudManager.m_Actor;
			(CloudManager.f_CallActor(&CCloudManager::f_SetExpectedOsVersions)(OsName, CurrentVersion, ExpectedVersion) % ("{}"_f << TrustedCloudManager.m_TrustInfo.m_HostInfo))
				> Results.f_AddResult()
			;
		}

		co_await Results.f_GetUnwrappedResults();

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

		co_return fg_Construct(co_await (co_await SensorReaders.f_GetResults() | g_Unwrap));
	}

	TCAsyncGenerator<TCVector<CDistributedAppSensorReporter::CSensorInfo>> CCloudClientAppActor::fp_CommandLine_CloudManager_GetAggregatedSensors
		(
			TCSharedPointer<TCMap<CHostInfo, TCDistributedActorInterfaceWithID<CDistributedAppSensorReader>>> const &_pSensorReaders
			, CDistributedAppSensorReader_SensorFilter const &_Filter
		)
	{
		TCActorResultVector<TCAsyncGenerator<TCVector<CDistributedAppSensorReporter::CSensorInfo>>> SensorsResults;

		for (auto &Reader : *_pSensorReaders)
			Reader.f_CallActor(&CDistributedAppSensorReader::f_GetSensors)(CDistributedAppSensorReader::CGetSensors{.m_Filters = {_Filter}}) > SensorsResults.f_AddResult();

		TCMap<CDistributedAppSensorReporter::CSensorInfoKey, CDistributedAppSensorReporter::CSensorInfo> SensorInfos;

		auto SensorGenerators = co_await (co_await SensorsResults.f_GetResults() | g_Unwrap);
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
			, CDistributedAppSensorReader_SensorStatusFilter const &_Filter
		)
	{
		TCActorResultVector<TCAsyncGenerator<TCVector<CDistributedAppSensorReader_SensorKeyAndReading>>> StatusResults;

		for (auto &Reader : *_pSensorReaders)
			Reader.f_CallActor(&CDistributedAppSensorReader::f_GetSensorStatus)(CDistributedAppSensorReader::CGetSensorStatus{.m_Filters = {_Filter}}) > StatusResults.f_AddResult();

		auto StatusGenerators = co_await (co_await StatusResults.f_GetResults() | g_Unwrap);

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
			Reader.f_CallActor(&CDistributedAppSensorReader::f_GetSensorReadings)(CDistributedAppSensorReader::CGetSensorReadings{.m_Filters = {_Filter}}) > ReadingsResults.f_AddResult();

		auto SensorGenerators = co_await (co_await ReadingsResults.f_GetResults() | g_Unwrap);
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

		for (auto &iReadings : co_await (co_await IteratorResults.f_GetResults() | g_Unwrap))
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

	auto CCloudClientAppActor::fp_CommandLine_CloudManager_GetLogReaders(CStr const &_Host)
		-> TCFuture<TCSharedPointer<TCMap<CHostInfo, TCDistributedActorInterfaceWithID<CDistributedAppLogReader>>>>
	{
		co_await fp_CloudManager_SubscribeToServers().f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for cloud managers");

		TCActorResultMap<CHostInfo, TCDistributedActorInterfaceWithID<CDistributedAppLogReader>> LogReaders;

		for (auto &TrustedCloudManager : mp_CloudManagers.m_Actors)
		{
			if (!_Host.f_IsEmpty() && TrustedCloudManager.m_TrustInfo.m_HostInfo.m_HostID != _Host)
				continue;
			auto &CloudManager = TrustedCloudManager.m_Actor;
			CloudManager.f_CallActor(&CCloudManager::f_GetLogReader)()
				.f_Timeout(mp_Timeout, "Timed out waiting for cloud manager to reply")
				> LogReaders.f_AddResult(TrustedCloudManager.m_TrustInfo.m_HostInfo)
			;
		}

		co_return fg_Construct(co_await (co_await LogReaders.f_GetResults() | g_Unwrap));
	}

	TCAsyncGenerator<TCVector<CDistributedAppLogReporter::CLogInfo>> CCloudClientAppActor::fp_CommandLine_CloudManager_GetAggregatedLogs
		(
			TCSharedPointer<TCMap<CHostInfo, TCDistributedActorInterfaceWithID<CDistributedAppLogReader>>> const &_pLogReaders
			, CDistributedAppLogReader_LogFilter const &_Filter
		)
	{
		TCActorResultVector<TCAsyncGenerator<TCVector<CDistributedAppLogReporter::CLogInfo>>> LogsResults;

		for (auto &Reader : *_pLogReaders)
			Reader.f_CallActor(&CDistributedAppLogReader::f_GetLogs)(CDistributedAppLogReader::CGetLogs{.m_Filters = {_Filter}}) > LogsResults.f_AddResult();

		TCMap<CDistributedAppLogReporter::CLogInfoKey, CDistributedAppLogReporter::CLogInfo> LogInfos;

		auto LogGenerators = co_await (co_await LogsResults.f_GetResults() | g_Unwrap);
		{
			for (auto &LogGenerator : LogGenerators)
			{
				for (auto iLogs = co_await fg_Move(LogGenerator).f_GetIterator(); iLogs; co_await ++iLogs)
				{
					for (auto &LogInfo : *iLogs)
						LogInfos[LogInfo.f_Key()] = LogInfo;
				}
			}
		}

		TCVector<CDistributedAppLogReporter::CLogInfo> ToYield;

		for (auto &LogInfo : LogInfos)
			ToYield.f_Insert(fg_Move(LogInfo));

		co_yield fg_Move(ToYield);

		co_return {};
	}

	TCAsyncGenerator<TCVector<CDistributedAppLogReader_LogKeyAndEntry>> CCloudClientAppActor::fp_CommandLine_CloudManager_GetAggregatedLogEntries
		(
			TCSharedPointer<TCMap<CHostInfo, TCDistributedActorInterfaceWithID<CDistributedAppLogReader>>> const &_pLogReaders
			, CDistributedAppLogReader_LogEntryFilter const &_Filter
		)
	{
		TCActorResultVector<TCAsyncGenerator<TCVector<CDistributedAppLogReader_LogKeyAndEntry>>> EntriesResults;

		for (auto &Reader : *_pLogReaders)
			Reader.f_CallActor(&CDistributedAppLogReader::f_GetLogEntries)(CDistributedAppLogReader::CGetLogEntries{.m_Filters = {_Filter}}) > EntriesResults.f_AddResult();

		auto LogGenerators = co_await (co_await EntriesResults.f_GetResults() | g_Unwrap);
		if (LogGenerators.f_IsEmpty())
			co_return {};
		if (LogGenerators.f_GetLen() == 1)
		{
			for (auto iEntries = co_await fg_Move(LogGenerators.f_GetFirst()).f_GetIterator(); iEntries; co_await ++iEntries)
				co_yield fg_Move(*iEntries);
			co_return {};
		}

		TCActorResultVector<TCAsyncGenerator<TCVector<CDistributedAppLogReader_LogKeyAndEntry>>::CIterator> IteratorResults;

		for (auto &Generator : LogGenerators)
			fg_Move(Generator).f_GetIterator() > IteratorResults.f_AddResult();

		TCVector<TCAsyncGenerator<CDistributedAppLogReader_LogKeyAndEntry>::CIterator> Iterators;

		for (auto &iEntries : co_await (co_await IteratorResults.f_GetResults() | g_Unwrap))
		{
			Iterators.f_Insert
				(
					co_await
					(
						fg_CallSafe
						(
							[iEntries = fg_Move(iEntries)]() mutable -> TCAsyncGenerator<CDistributedAppLogReader_LogKeyAndEntry>
							{
								for (; iEntries; co_await ++iEntries)
								{
									for (auto &Entry : *iEntries)
										co_yield fg_Move(Entry);
								}

								co_return {};
							}
						)
					).f_GetIterator()
				)
			;
		}

		auto fIsLess = [bNewestFirst = !!(_Filter.m_Flags & CDistributedAppLogReader_LogEntryFilter::ELogEntriesFlag_ReportNewestFirst)]
			(CDistributedAppLogReader_LogKeyAndEntry const &_Left, CDistributedAppLogReader_LogKeyAndEntry const &_Right)
			{
				if (bNewestFirst)
				{
					return fg_TupleReferences(_Right.m_Entry.m_Timestamp, _Right.m_Entry.m_UniqueSequence)
						< fg_TupleReferences(_Left.m_Entry.m_Timestamp, _Left.m_Entry.m_UniqueSequence)
					;
				}
				else
				{
					return fg_TupleReferences(_Left.m_Entry.m_Timestamp, _Left.m_Entry.m_UniqueSequence)
						< fg_TupleReferences(_Right.m_Entry.m_Timestamp, _Right.m_Entry.m_UniqueSequence)
					;
				}
			}
		;

		bool bAtEnd = false;
		CDistributedAppLogReader_LogKeyAndEntry LastEntry;

		auto fGetNextEntry = [&]() -> TCFuture<CDistributedAppLogReader_LogKeyAndEntry>
			{
				co_await ECoroutineFlag_AllowReferences;

				TCAsyncGenerator<CDistributedAppLogReader_LogKeyAndEntry>::CIterator *pBestEntry = nullptr;

				while (true)
				{
					for (auto &iEntries : Iterators)
					{
						if (!iEntries)
							continue;

						if (!pBestEntry)
						{
							pBestEntry = &iEntries;
							continue;
						}

						if (fIsLess(*iEntries, **pBestEntry))
							pBestEntry = &iEntries;
					}

					if (pBestEntry)
					{
						auto ToReturn = fg_Move(**pBestEntry);
						co_await ++*pBestEntry;

						if (ToReturn.m_LogInfoKey == LastEntry.m_LogInfoKey && ToReturn.m_Entry.m_UniqueSequence == LastEntry.m_Entry.m_UniqueSequence)
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

		TCVector<CDistributedAppLogReader_LogKeyAndEntry> ToYield;

		while (true)
		{
			auto NextEntry = co_await fGetNextEntry();
			if (bAtEnd)
				break;

			ToYield.f_Insert(fg_Move(NextEntry));
			if (ToYield.f_GetLen() >= 1024)
				co_yield fg_Move(ToYield);
		}

		if (!ToYield.f_IsEmpty())
			co_yield fg_Move(ToYield);

		co_return {};
	}
}
