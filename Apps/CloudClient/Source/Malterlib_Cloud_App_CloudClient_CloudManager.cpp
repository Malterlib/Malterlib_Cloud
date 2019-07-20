// Copyright © 2019 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/ActorSubscription>
#include <Mib/Encoding/JSONShortcuts>
#include <Mib/CommandLine/TableRenderer>
#include <Mib/CommandLine/AnsiEncoding>

#include "Malterlib_Cloud_App_CloudClient.h"

namespace NMib::NCloud::NCloudClient
{
	void CCloudClientAppActor::fp_CloudManager_RegisterCommands(CDistributedAppCommandLineSpecification::CSection _Section)
	{
		auto OptionalHost = "Host?"_=
			{
				"Names"_= {"--host"}
				, "Default"_= ""
				, "Description"_= "Limit cloud managers to only specified host ID."
			}
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
						OptionalHost
						, QuietOption
						, IncludeCloudManagerOption
						, CTableRenderHelper::fs_OutputTypeOption()
					}
				}
				, [this](CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine) -> TCFuture<uint32>
				{
					auto ReportFor = ECloudManagerStatusFlag_Applications | ECloudManagerStatusFlag_AppManagers;
					return self(&CCloudClientAppActor::fp_CommandLine_CloudManager_Status, _Params, _pCommandLine, ReportFor);
				}
				, CDistributedAppCommandLineSpecification::ECommandFlag_WaitForRemotes
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
						OptionalHost
						, QuietOption
						, IncludeCloudManagerOption
						, CTableRenderHelper::fs_OutputTypeOption()
					}
				}
				, [this](CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine) -> TCFuture<uint32>
				{
					auto ReportFor = ECloudManagerStatusFlag_AppManagers;
					return self(&CCloudClientAppActor::fp_CommandLine_CloudManager_Status, _Params, _pCommandLine, ReportFor);
				}
				, CDistributedAppCommandLineSpecification::ECommandFlag_WaitForRemotes
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
						OptionalHost
						, QuietOption
						, CTableRenderHelper::fs_OutputTypeOption()
					}
				}
				, [this](CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine) -> TCFuture<uint32>
				{
					auto ReportFor = ECloudManagerStatusFlag_Applications;
					return self(&CCloudClientAppActor::fp_CommandLine_CloudManager_Status, _Params, _pCommandLine, ReportFor);
				}
				, CDistributedAppCommandLineSpecification::ECommandFlag_WaitForRemotes
			)
		;
	}
	
	TCFuture<void> CCloudClientAppActor::fp_CloudManager_SubscribeToServers()
	{
		if (!mp_CloudManagers.f_IsEmpty())
			co_return {};
		
		DMibLogWithCategory(Malterlib/Cloud/CloudClient, Info, "Subscribing to cloud managers");
		
		TCPromise<void> Promise;
		
		auto Subscription = co_await mp_State.m_TrustManager
			(
				&CDistributedActorTrustManager::f_SubscribeTrustedActors<NCloud::CCloudManager>
				, "com.malterlib/Cloud/CloudManager"
				, fg_ThisActor(this)
			)
			.f_Wrap()
		;

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
						<< _Info.m_LastConnectionErrorTime
						<< _AnsiEncoding.f_Default()
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

		CTableRenderHelper TableRenderer = _pCommandLine->f_TableRenderer();
		CAnsiEncoding AnsiEncoding = _pCommandLine->f_AnsiEncoding();

		TableRenderer.f_AddDescription("App Managers");
		TableRenderer.f_AddHeadings("Cloud Manager", "Environment", "Hostname", "Location", "Platform", "Version", "Date", "ID", "Last Seen", "Status");
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

			TableRenderer.f_AddRow
				(
					HostInfo.f_GetDescColored(_pCommandLine->m_AnsiFlags)
					, AppManagerInfo.m_Environment
					, HostName
					, AppManagerInfo.m_ProgramDirectory
				 	, AppManagerInfo.m_PlatformFamily
				 	, AppManagerInfo.m_Version
				 	, "{td}"_f << AppManagerInfo.m_VersionDate
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

		CTableRenderHelper TableRenderer = _pCommandLine->f_TableRenderer();
		CAnsiEncoding AnsiEncoding = _pCommandLine->f_AnsiEncoding();

		TableRenderer.f_AddDescription("Applications");
		TableRenderer.f_AddHeadings("Environment", "App Manager", "Name", "Application", "Auto Update Tags", "Version", "Date", "Status");
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
			if (ApplicationInfo.m_AutoUpdateTags.f_IsEmpty())
				AutoUpdate = "{}Manual Update{}"_f << AnsiEncoding.f_StatusWarning() << AnsiEncoding.f_Default();
			else
				AutoUpdate = "{vs,vb}"_f << ApplicationInfo.m_AutoUpdateTags;

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

			TableRenderer.f_AddRow
				(
					AppManagerInfo.m_Environment
				 	, "{}{}{}:{}"_f << AnsiEncoding.f_Foreground256(75) << AppManagerInfo.m_HostName << AnsiEncoding.f_Default() << AppManagerInfo.m_ProgramDirectory
					, ApplicationKey.m_Name
					, ApplicationInfo.m_VersionManagerApplication
				 	, AutoUpdate
					, ApplicationInfo.m_Version.m_VersionID
				 	, "{td}"_f << ApplicationInfo.m_VersionInfo.m_Time
				 	, Status
				)
			;
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
		TCPromise<uint32> Promise;
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
}
