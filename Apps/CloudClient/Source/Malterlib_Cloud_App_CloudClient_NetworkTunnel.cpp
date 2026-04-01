// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>
#include <Mib/Cloud/VersionManager>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Encoding/JsonShortcuts>
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
					"Names"_o= _o["--network-tunnel-enum"]
					, "Description"_o= "List network tunnels available on remotes."
					, "Options"_o=
					{
						"Hosts?"_o=
						{
							"Names"_o= _o["--hosts"]
							, "Default"_o= _o[]
							, "Description"_o= "The hosts to list tunnels for. If empty all hosts are included.\n"
							, "Type"_o= _o[""]
						}
						, CTableRenderHelper::fs_OutputTypeOption()
					}
					, "Parameters"_o=
					{
						"TunnelName...?"_o=
						{
							"Default"_o= _o[]
							, "Type"_o= _o[""]
							, "Description"_o= "A list of wildcards for tunnel names to open tunnels to.\n"
						}
					}
				}
				, [this](CEJsonSorted &&_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_NetworkTunnel_EnumTunnels(fg_Move(_Params), fg_Move(_pCommandLine));
				}
				, EDistributedAppCommandFlag_WaitForRemotes
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_o= _o["--network-tunnel-open"]
					, "Description"_o= "Open network tunnels."
					, "Options"_o=
					{
						"Hosts?"_o=
						{
							"Names"_o= _o["--hosts"]
							, "Default"_o= _o[]
							, "Description"_o= "The hosts to open tunnels for. If empty all hosts are included.\n"
							, "Type"_o= _o[""]
						}
						, "ListenHost?"_o=
						{
							"Names"_o= _o["--listen-host", "-l"]
							, "Default"_o= ""
							, "Description"_o= "The hostname to listen on. For unix sockets prefix path with UNIX:\n"
						}
						, "Verbose?"_o=
						{
							"Names"_o= _o["--verbose", "-v"]
							, "Default"_o= false
							, "Description"_o= "Log every connection.\n"
						}
						, CTableRenderHelper::fs_OutputTypeOption()
					}
					, "Parameters"_o=
					{
						"TunnelName...?"_o=
						{
							"Default"_o= _o[]
							, "Type"_o= _o[""]
							, "Description"_o= "A list of wildcards for tunnel names to open tunnels to.\n"
						}
					}
				}
				, [this](CEJsonSorted &&_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_NetworkTunnel_OpenTunnels(fg_Move(_Params), fg_Move(_pCommandLine));
				}
				, EDistributedAppCommandFlag_WaitForRemotes
			)
		;
	}

	TCFuture<void> CCloudClientAppActor::fp_NetworkTunnel_Init()
	{
		auto CheckDestroy = co_await fp_CheckStoppedOrDestroyedOnResume();

		if (!mp_TunnelsClient)
		{
			mp_TunnelsClient = fg_Construct(mp_State.m_DistributionManager, mp_State.m_TrustManager);
			co_await mp_TunnelsClient(&CNetworkTunnelsClient::f_Start);
		}
		co_return {};
	}

	TCFuture<TCMap<CStr, TCMap<ICNetworkTunnels::CNetworkTunnelName, ICNetworkTunnels::CNetworkTunnel>>> CCloudClientAppActor::fp_NetworkTunnel_Filter(CEJsonSorted const _Params)
	{
		auto CheckDestroy = co_await fp_CheckStoppedOrDestroyedOnResume();

		co_await fp_NetworkTunnel_Init();

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
				if (!Wildcards.f_IsEmpty() && !fg_StrMatchesAnyWildcardInContainerKeys(TunnelName, Wildcards))
					continue;
				Return[HostID][TunnelName] = fg_Move(Tunnel);
			}
		}
		co_return fg_Move(Return);
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_NetworkTunnel_EnumTunnels(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		auto CheckDestroy = co_await fp_CheckStoppedOrDestroyedOnResume();

		auto TunnelsPerHost = co_await fp_NetworkTunnel_Filter(_Params);

		CTableRenderHelper TableRenderer = _pCommandLine->f_TableRenderer();
		TableRenderer.f_AddHeadings("HostID", "Tunnel Name", "Metadata");

		for (auto &Tunnels : TunnelsPerHost)
		{
			auto &HostID = TunnelsPerHost.fs_GetKey(Tunnels);
			for (auto &Tunnel : Tunnels)
			{
				auto &TunnelName = Tunnels.fs_GetKey(Tunnel);
				TableRenderer.f_AddRow(HostID, TunnelName, Tunnel.m_Metadata.f_ToStringColored(_pCommandLine->m_AnsiFlags, "\t", EJsonDialectFlag_AllowUndefined));
			}
		}

		TableRenderer.f_Output(_Params);

		co_return 0;
	}

	struct CTunnelKey
	{
		CStr m_HostID;
		CStr m_TunnelName;

		auto operator <=> (CTunnelKey const &_Right) const noexcept = default;
	};

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_NetworkTunnel_OpenTunnels(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		auto CheckDestroy = co_await fp_CheckStoppedOrDestroyedOnResume();

		auto TunnelsPerHost = co_await fp_NetworkTunnel_Filter(_Params);

		if (TunnelsPerHost.f_IsEmpty())
			co_return DMibErrorInstance("No matching tunnels found");

		bool bVerbose = _Params["Verbose"].f_Boolean();
		CStr ListenHost = _Params["ListenHost"].f_String();

		TCFutureMap<CTunnelKey, CNetworkTunnelsClient::CTunnel> OpenedTunnelResults;
		TCMap<CTunnelKey, CStr> URLTemplates;
		for (auto &Tunnels : TunnelsPerHost)
		{
			auto &HostID = TunnelsPerHost.fs_GetKey(Tunnels);
			for (auto &Tunnel : Tunnels)
			{
				auto &TunnelName = Tunnels.fs_GetKey(Tunnel);

				CTunnelKey TunnelKey{HostID, TunnelName};

				auto &URLTemplate = URLTemplates[TunnelKey];
				if (auto *pValue = Tunnel.m_Metadata.f_GetMember("URLTemplate", EJsonType_String))
					URLTemplate = pValue->f_String();
				else
					URLTemplate = "{Host}:{Port}";

				mp_TunnelsClient
					(
						&CNetworkTunnelsClient::f_OpenTunnel
						, CNetworkTunnelsClient::COpenTunnel
						{
							.m_HostID = HostID
							, .m_TunnelName = TunnelName
							, .m_fOnConnection = g_ActorFunctor / [=, LoggedAddresses = TCSet<CStr>()](CNetworkTunnelsClient::CCallbackInfo _CallbackInfo) mutable -> TCFuture<void>
							{
								if (!bVerbose && !LoggedAddresses(_CallbackInfo.m_Address.f_GetString(ENetAddressStringFlag_None)).f_WasCreated())
									co_return {};

								CStr ActionString;
								auto AnsiEncoding = _pCommandLine->f_AnsiEncoding();
								ActionString = "{}Connected{}"_f << AnsiEncoding.f_StatusNormal() << AnsiEncoding.f_Default();

								*_pCommandLine += "<{}> ({}) {{{}} {} {}\n"_f
									<< _CallbackInfo.m_RemoteHostID
									<< _CallbackInfo.m_ConnectionID
									<< TunnelName
									<< _CallbackInfo.m_Address.f_GetString(ENetAddressStringFlag_IncludePort)
									<< ActionString
								;

								co_return {};
							}
							, .m_fOnClose = g_ActorFunctor / [=, LoggedAddresses = TCSet<CStr>()]
							(CNetworkTunnelsClient::CCallbackInfo _CallbackInfo, NStr::CStr _Message) mutable -> TCFuture<void>
							{
								if (!bVerbose && !LoggedAddresses(_CallbackInfo.m_Address.f_GetString(ENetAddressStringFlag_None)).f_WasCreated())
									co_return {};

								CStr ActionString;
								auto AnsiEncoding = _pCommandLine->f_AnsiEncoding();
								ActionString = "{}Closed   {}"_f << AnsiEncoding.f_StatusWarning() << AnsiEncoding.f_Default();

								*_pCommandLine += "<{}> ({}) {{{}} {} {} {}\n"_f
									<< _CallbackInfo.m_RemoteHostID
									<< _CallbackInfo.m_ConnectionID
									<< TunnelName
									<< _CallbackInfo.m_Address.f_GetString(ENetAddressStringFlag_IncludePort)
									<< ActionString
									<< _Message
								;

								co_return {};
							}
							, .m_fOnError = g_ActorFunctor / [=](CNetworkTunnelsClient::CCallbackInfo _CallbackInfo, CStr _Error) -> TCFuture<void>
							{
								CStr ActionString;
								auto AnsiEncoding = _pCommandLine->f_AnsiEncoding();
								ActionString = "{}Error    {}"_f << AnsiEncoding.f_StatusError() << AnsiEncoding.f_Default();

								*_pCommandLine += "<{}> ({}) {{{}} {} {} {}\n"_f
									<< _CallbackInfo.m_RemoteHostID
									<< _CallbackInfo.m_ConnectionID
									<< TunnelName
									<< _CallbackInfo.m_Address.f_GetString(ENetAddressStringFlag_IncludePort)
									<< ActionString
									<< _Error
								;
								co_return {};
							}
							, .m_ListenHost = ListenHost
						}
					)
					> OpenedTunnelResults[TunnelKey]
				;
			}
		}

		auto OpenedTunnels = co_await fg_AllDone(OpenedTunnelResults);

		CTableRenderHelper TableRenderer = _pCommandLine->f_TableRenderer();
		TableRenderer.f_AddHeadings("HostID", "Tunnel Name", "URL");

		for (auto &Tunnel : OpenedTunnels)
		{
			auto &TunnelKey = OpenedTunnels.fs_GetKey(Tunnel);

			CStr HostString = Tunnel.m_ListenAddress.f_GetString(ENetAddressStringFlag_IncludeType);

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
