// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Cloud/VersionManager>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Encoding/JSONShortcuts>
#include <Mib/CommandLine/TableRenderer>
#include <Mib/CommandLine/AnsiEncoding>

#include "Malterlib_Cloud_App_CloudClient.h"

#ifdef DPlatformFamily_Windows
#include <Mib/Core/PlatformSpecific/WindowsFilePath>
#endif

namespace NMib::NCloud::NCloudClient
{
	void CCloudClientAppActor::fp_NetworkTunnel_RegisterCommands(CDistributedAppCommandLineSpecification::CSection _Section)
	{
		_Section.f_RegisterCommand
			(
				{
					"Names"_o= {"--network-tunnel-enum"}
					, "Description"_o= "List network tunnels available on remotes."
					, "Options"_o=
					{
						"Hosts?"_o=
						{
							"Names"_o= {"--hosts"}
							, "Default"_o= _[_]
							, "Description"_o= "The hosts to list tunnels for. If empty all hosts are included.\n"
							, "Type"_o= {""}
						}
						, CTableRenderHelper::fs_OutputTypeOption()
					}
					, "Parameters"_o=
					{
						"TunnelName...?"_o=
						{
							"Default"_o= _[_]
							, "Type"_o= {""}
							, "Description"_o= "A list of wildcards for tunnel names to open tunnels to.\n"
						}
					}
				}
				, [this](CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return g_Future <<= self(&CCloudClientAppActor::fp_CommandLine_NetworkTunnel_EnumTunnels, _Params, _pCommandLine);
				}
				, EDistributedAppCommandFlag_WaitForRemotes
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_o= {"--network-tunnel-open"}
					, "Description"_o= "Open network tunnels."
					, "Options"_o=
					{
						"Hosts?"_o=
						{
							"Names"_o= {"--hosts"}
							, "Default"_o= _[_]
							, "Description"_o= "The hosts to open tunnels for. If empty all hosts are included.\n"
							, "Type"_o= {""}
						}
						, "ListenHost?"_o=
						{
							"Names"_o= {"--listen-host", "-l"}
							, "Default"_o= ""
							, "Description"_o= "The hostname to listen on. For unix sockets prefix path with UNIX:\n"
						}
						, "Verbose?"_o=
						{
							"Names"_o= {"--verbose", "-v"}
							, "Default"_o= false
							, "Description"_o= "Log every connection.\n"
						}
						, CTableRenderHelper::fs_OutputTypeOption()
					}
					, "Parameters"_o=
					{
						"TunnelName...?"_o=
						{
							"Default"_o= _[_]
							, "Type"_o= {""}
							, "Description"_o= "A list of wildcards for tunnel names to open tunnels to.\n"
						}
					}
				}
				, [this](CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return g_Future <<= self(&CCloudClientAppActor::fp_CommandLine_NetworkTunnel_OpenTunnels, _Params, _pCommandLine);
				}
				, EDistributedAppCommandFlag_WaitForRemotes
			)
		;
	}

	TCFuture<void> CCloudClientAppActor::fp_NetworkTunnel_Init()
	{
		if (!mp_TunnelsClient)
		{
			mp_TunnelsClient = fg_Construct(mp_State.m_DistributionManager, mp_State.m_TrustManager);
			co_await mp_TunnelsClient(&CNetworkTunnelsClient::f_Start);
		}
		co_return {};
	}

	TCFuture<TCMap<CStr, TCMap<ICNetworkTunnels::CNetworkTunnelName, ICNetworkTunnels::CNetworkTunnel>>> CCloudClientAppActor::fp_NetworkTunnel_Filter(CEJSONSorted const &_Params)
	{
		co_await self(&CCloudClientAppActor::fp_NetworkTunnel_Init);

		TCSet<CStr> Wildcards;
		for (auto &Wildcard : _Params["TunnelName"].f_Array())
			Wildcards[Wildcard.f_String()];

		TCSet<CStr> Hosts;
		for (auto &Host : _Params["Hosts"].f_Array())
			Hosts[Host.f_String()];

		auto TunnelsPerHost = co_await mp_TunnelsClient(&CNetworkTunnelsClient::f_EnumTunnels);

		TCMap<CStr, TCMap<ICNetworkTunnels::CNetworkTunnelName, ICNetworkTunnels::CNetworkTunnel>> Return;
		for (auto &Tunnels : TunnelsPerHost)
		{
			auto &HostID = TunnelsPerHost.fs_GetKey(Tunnels);
			if (!Hosts.f_IsEmpty() && !Hosts.f_Exists(HostID))
				continue;
			for (auto &Tunnel : Tunnels)
			{
				auto &TunnelName = Tunnels.fs_GetKey(Tunnel);
				if (!Wildcards.f_IsEmpty() && !fg_StrMatchesAnyWildcardInMap(TunnelName, Wildcards))
					continue;
				Return[HostID][TunnelName] = fg_Move(Tunnel);
			}
		}
		co_return fg_Move(Return);
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_NetworkTunnel_EnumTunnels(CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		auto TunnelsPerHost = co_await self(&CCloudClientAppActor::fp_NetworkTunnel_Filter, _Params);

		CTableRenderHelper TableRenderer = _pCommandLine->f_TableRenderer();
		TableRenderer.f_AddHeadings("HostID", "Tunnel Name", "Meta Data");

		for (auto &Tunnels : TunnelsPerHost)
		{
			auto &HostID = TunnelsPerHost.fs_GetKey(Tunnels);
			for (auto &Tunnel : Tunnels)
			{
				auto &TunnelName = Tunnels.fs_GetKey(Tunnel);
				TableRenderer.f_AddRow(HostID, TunnelName, Tunnel.m_MetaData.f_ToStringColored(_pCommandLine->m_AnsiFlags, "\t", EJSONDialectFlag_AllowUndefined));
			}
		}

		TableRenderer.f_Output(_Params);

		co_return 0;
	}

	struct CTunnelKey
	{
		CStr m_HostID;
		CStr m_TunnelName;

		auto operator <=> (CTunnelKey const &_Right) const = default;
	};

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_NetworkTunnel_OpenTunnels(CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		auto TunnelsPerHost = co_await self(&CCloudClientAppActor::fp_NetworkTunnel_Filter, _Params);

		if (TunnelsPerHost.f_IsEmpty())
			co_return DMibErrorInstance("No matching tunnels found");

		bool bVerbose = _Params["Verbose"].f_Boolean();
		CStr ListenHost = _Params["ListenHost"].f_String();

		TCActorResultMap<CTunnelKey, CNetworkTunnelsClient::CTunnel> OpenedTunnelResults;
		TCMap<CTunnelKey, CStr> URLTemplates;
		for (auto &Tunnels : TunnelsPerHost)
		{
			auto &HostID = TunnelsPerHost.fs_GetKey(Tunnels);
			for (auto &Tunnel : Tunnels)
			{
				auto &TunnelName = Tunnels.fs_GetKey(Tunnel);

				CTunnelKey TunnelKey{HostID, TunnelName};

				auto &URLTemplate = URLTemplates[TunnelKey];
				if (auto *pValue = Tunnel.m_MetaData.f_GetMember("URLTemplate", EJSONType_String))
					URLTemplate = pValue->f_String();
				else
					URLTemplate = "{Host}:{Port}";

				mp_TunnelsClient
					(
						&CNetworkTunnelsClient::f_OpenTunnel
						, HostID
						, TunnelName
						, g_ActorFunctor / [=, LoggedAddresses = TCSet<CStr>()](CNetAddress const &_Address) mutable -> TCFuture<void>
						{
							if (!bVerbose && !LoggedAddresses(_Address.f_GetString()).f_WasCreated())
								co_return {};

							CStr ActionString;
							auto AnsiEncoding = _pCommandLine->f_AnsiEncoding();
							ActionString = "{}Connected{}"_f << AnsiEncoding.f_StatusNormal() << AnsiEncoding.f_Default();

							*_pCommandLine += "{} '{}': {}:{}\n"_f << ActionString << TunnelName << _Address.f_GetString() << _Address.f_GetPort();

							co_return {};
						}
						, g_ActorFunctor / [=, LoggedAddresses = TCSet<CStr>()](CNetAddress const &_Address, NStr::CStr const &_Message) mutable -> TCFuture<void>
						{
							if (!bVerbose && !LoggedAddresses(_Address.f_GetString()).f_WasCreated())
								co_return {};

							CStr ActionString;
							auto AnsiEncoding = _pCommandLine->f_AnsiEncoding();
							ActionString = "{}Closed   {}"_f << AnsiEncoding.f_StatusWarning() << AnsiEncoding.f_Default();

							*_pCommandLine += "{} '{}': {}:{} {}\n"_f << ActionString << TunnelName << _Address.f_GetString() << _Address.f_GetPort() << _Message;

							co_return {};
						}
						, g_ActorFunctor / [=](CNetAddress const &_Address, CStr const &_Error) -> TCFuture<void>
						{
							CStr ActionString;
							auto AnsiEncoding = _pCommandLine->f_AnsiEncoding();
							ActionString = "{}Error    {}"_f << AnsiEncoding.f_StatusError() << AnsiEncoding.f_Default();

							*_pCommandLine += "{} '{}': {}:{} {}\n"_f << ActionString << TunnelName << _Address.f_GetString() << _Address.f_GetPort() << _Error;
							co_return {};
						}
						, ListenHost
					)
					> OpenedTunnelResults.f_AddResult(TunnelKey)
				;
			}
		}

		auto OpenedTunnels = co_await (co_await OpenedTunnelResults.f_GetResults() | g_Unwrap);

		CTableRenderHelper TableRenderer = _pCommandLine->f_TableRenderer();
		TableRenderer.f_AddHeadings("HostID", "Tunnel Name", "URL");

		for (auto &Tunnel : OpenedTunnels)
		{
			auto &TunnelKey = OpenedTunnels.fs_GetKey(Tunnel);

			CStr HostString = Tunnel.m_ListenAddress.f_GetString(true);

			CStr TunnelURL = URLTemplates[TunnelKey];

			TunnelURL = TunnelURL.f_Replace("{Host}", HostString);
			CStr PortString;
			if (auto Port = Tunnel.m_ListenAddress.f_GetPort(); Port != 0)
				PortString = "{}"_f << Port;
			TunnelURL = TunnelURL.f_Replace("{Port}", PortString);

			TableRenderer.f_AddRow(TunnelKey.m_HostID, TunnelKey.m_TunnelName, TunnelURL);
		}

		TableRenderer.f_Output(_Params);

		co_await mp_AppStopPromises.f_Insert().f_Future();

		co_return 0;
	}
}
