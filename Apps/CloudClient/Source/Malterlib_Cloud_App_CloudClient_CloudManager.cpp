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
						, "Quiet?"_=
						{
							"Names"_= {"--quiet", "-q"}
							, "Default"_= false
							, "Description"_= "Don't output information on std out, just return the status"
						}
						, "IncludeCloudManager?"_=
						{
							"Names"_= {"--include-cloud-manager", "-c"}
							, "Default"_= false
							, "Description"_= "Include the cloud manager column"
						}
						, CTableRenderHelper::fs_OutputTypeOption()
					}
				}
				, [this](CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return self(&CCloudClientAppActor::fp_CommandLine_CloudManager_Status, _Params, _pCommandLine);
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
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_CloudManager_Status(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		TCPromise<uint32> Promise;
		CStr Host = _Params["Host"].f_String();

		bool bQuiet = _Params["Quiet"].f_Boolean();
		bool bIncludeCloudManager = _Params["IncludeCloudManager"].f_Boolean();

		co_await fp_CloudManager_SubscribeToServers().f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for cloud managers");

		TCActorResultMap<CHostInfo, TCMap<CStr, CCloudManager::CAppManagerDynamicInfo>> AppManagersResults;

		for (auto &TrustedCloudManager : mp_CloudManagers.m_Actors)
		{
			if (!Host.f_IsEmpty() && TrustedCloudManager.m_TrustInfo.m_HostInfo.m_HostID != Host)
				continue;
			auto &CloudManager = TrustedCloudManager.m_Actor;
			CloudManager.f_CallActor(&CCloudManager::f_EnumAppManagers)()
				.f_Timeout(mp_Timeout, "Timed out waiting for cloud manager to reply")
				> AppManagersResults.f_AddResult(TrustedCloudManager.m_TrustInfo.m_HostInfo)
			;
		}

		auto AppManagers = co_await AppManagersResults.f_GetResults();

		CTableRenderHelper TableRenderer = _pCommandLine->f_TableRenderer();
		CAnsiEncoding AnsiEncoding = _pCommandLine->f_AnsiEncoding();

		TableRenderer.f_AddDescription("App Managers");
		TableRenderer.f_AddHeadings("Cloud Manager", "Environment", "Hostname", "Location", "Platform", "Version", "ID", "Last Seen", "Status");
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
			CStr m_Error;
			CStr m_AppManagerID;
			CCloudManager::CAppManagerDynamicInfo m_AppManagerInfo;
		};

		TCVector<CRow> Rows;

		for (auto &AppManagersForHost : AppManagers)
		{
			auto &HostInfo = AppManagers.fs_GetKey(AppManagersForHost);

			if (!AppManagersForHost)
			{
				Rows.f_Insert({HostInfo, AppManagersForHost.f_GetExceptionStr()});
				Return = 1;
				continue;
			}

			for (auto &AppManagerInfo : *AppManagersForHost)
			{
				auto &AppManagerID = (*AppManagersForHost).fs_GetKey(AppManagerInfo);
				Rows.f_Insert({HostInfo, "", AppManagerID, AppManagerInfo});
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

		for (auto &Row : Rows)
		{
			auto &HostInfo = Row.m_HostInfo;

			if (Row.m_Error)
			{
				Return = 1;
				CStr ErrorString = "{}Error:{} {}"_f << AnsiEncoding.f_StatusError() << AnsiEncoding.f_Default() << Row.m_Error;

				TableRenderer.f_AddRow(HostInfo.f_GetDesc(), "", "", "", "", "", ErrorString);
				continue;
			}

			auto &AppManagerID = Row.m_AppManagerID;
			auto &AppManagerInfo = Row.m_AppManagerInfo;

			CStr Status;
			if (AppManagerInfo.m_bActive)
				Status = "{}OK{}"_f << AnsiEncoding.f_StatusNormal() << AnsiEncoding.f_Default();
			else
			{
				if (!AppManagerInfo.m_LastConnectionError.f_IsEmpty())
				{
					Status = "{}{tc6}{} {}"_f
						<< AnsiEncoding.f_StatusError()
						<< AppManagerInfo.m_LastConnectionErrorTime
						<< AnsiEncoding.f_Default()
						<< AppManagerInfo.m_LastConnectionError
					;
				}
				else
					Status = "{}Missing{}"_f << AnsiEncoding.f_StatusError() << AnsiEncoding.f_Default();

				Return = 1;
			}

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
}
