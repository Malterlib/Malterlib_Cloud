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
					"Names"_= {"--network-tunnel-enum"}
					, "Description"_= "List network tunnels available on remotes."
					, "Options"_=
					{
						"Hosts?"_=
						{
							"Names"_= {"--hosts"}
							, "Default"_= _[_]
							, "Description"_= "The hosts to list tunnels for. If empty all hosts are included.\n"
							, "Type"_= {""}
						}
					}
					, "Parameters"_=
					{
						"TunnelName...?"_=
						{
							"Default"_= _[_]
							, "Type"_= {""}
							, "Description"_= "A list of wildcards for tunnel names to open tunnels to.\n"
						}
					}
				}
				, [this](CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return self(&CCloudClientAppActor::fp_CommandLine_NetworkTunnel_EnumTunnels, _Params, _pCommandLine);
				}
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_= {"--network-tunnel-open"}
					, "Description"_= "Open network tunnels."
					, "Options"_=
					{
						"Hosts?"_=
						{
							"Names"_= {"--hosts"}
							, "Default"_= _[_]
							, "Description"_= "The hosts to open tunnels for. If empty all hosts are included.\n"
							, "Type"_= {""}
						}
					}
					, "Parameters"_=
					{
						"TunnelName...?"_=
						{
							"Default"_= _[_]
							, "Type"_= {""}
							, "Description"_= "A list of wildcards for tunnel names to open tunnels to.\n"
						}
					}
				}
				, [this](CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return self(&CCloudClientAppActor::fp_CommandLine_NetworkTunnel_OpenTunnels, _Params, _pCommandLine);
				}
			)
		;
	}

	TCFuture<void> CCloudClientAppActor::fp_NetworkTunnel_Init()
	{
		if (!mp_TunnelClient)
		{
			mp_TunnelClient = fg_Construct(mp_State.m_DistributionManager, mp_State.m_TrustManager);
			co_await mp_TunnelClient(&CNetworkTunnelClient::f_Start);
		}
		co_return {};
	}

	TCFuture<TCMap<CStr, TCMap<ICNetworkTunnel::CNetworkTunnelName, ICNetworkTunnel::CNetworkTunnel>>> CCloudClientAppActor::fp_NetworkTunnel_Filter(CEJSON const &_Params)
	{
		co_await self(&CCloudClientAppActor::fp_NetworkTunnel_Init);

		TCSet<CStr> Wildcards;
		for (auto &Wildcard : _Params["TunnelName"].f_Array())
			Wildcards[Wildcard.f_String()];

		TCSet<CStr> Hosts;
		for (auto &Host : _Params["Hosts"].f_Array())
			Hosts[Host.f_String()];

		auto TunnelsPerHost = co_await mp_TunnelClient(&CNetworkTunnelClient::f_EnumTunnels);

		TCMap<CStr, TCMap<ICNetworkTunnel::CNetworkTunnelName, ICNetworkTunnel::CNetworkTunnel>> Return;
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

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_NetworkTunnel_EnumTunnels(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		auto TunnelsPerHost = co_await self(&CCloudClientAppActor::fp_NetworkTunnel_Filter, _Params);

		CTableRenderHelper TableRenderer
			(
				[&](NStr::CStr const &_Output)
			 	{
					*_pCommandLine += _Output;
				}
			 	, (_pCommandLine->m_bColorEnabled ? CTableRenderHelper::EOption_Color : CTableRenderHelper::EOption_None) | CTableRenderHelper::EOption_Rounded
		 	)
		;

		TableRenderer.f_AddHeadings("HostID", "Tunnel Name", "Meta Data");

		for (auto &Tunnels : TunnelsPerHost)
		{
			auto &HostID = TunnelsPerHost.fs_GetKey(Tunnels);
			for (auto &Tunnel : Tunnels)
			{
				auto &TunnelName = Tunnels.fs_GetKey(Tunnel);
				TableRenderer.f_AddRow(HostID, TunnelName, Tunnel.m_MetaData.f_ToStringColored("\t", _pCommandLine->m_bColorEnabled, true));
			}
		}

		TableRenderer.f_Output();

		co_return 0;
	}

	struct CTunnelKey
	{
		CStr m_HostID;
		CStr m_TunnelName;

		bool operator < (CTunnelKey const &_Right) const
		{
			return fg_TupleReferences(m_HostID, m_TunnelName) < fg_TupleReferences(_Right.m_HostID, _Right.m_TunnelName);
		}
	};

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_NetworkTunnel_OpenTunnels(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		auto TunnelsPerHost = co_await self(&CCloudClientAppActor::fp_NetworkTunnel_Filter, _Params);

		if (TunnelsPerHost.f_IsEmpty())
			co_return DMibErrorInstance("No matching tunnels found");

		TCActorResultMap<CTunnelKey, CNetworkTunnelClient::CTunnel> OpenedTunnelResults;
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

				mp_TunnelClient
					(
						&CNetworkTunnelClient::f_OpenTunnel
						, HostID
						, TunnelName
						, g_ActorFunctor / [=](CNetAddress const &_Address) -> TCFuture<void>
						{
							CStr ActionString;
							if (_pCommandLine->m_bColorEnabled)
								ActionString = "{}Connected{}"_f << CAnsiEncoding::ms_StatusNormal << CAnsiEncoding::ms_Default;
							else
								ActionString = "Connected";

							*_pCommandLine += "{} '{}': {}:{}\n"_f << ActionString<< TunnelName << _Address.f_GetString() << _Address.f_GetPort();
							return fg_Explicit();
						}
						, g_ActorFunctor / [=](CNetAddress const &_Address, NStr::CStr const &_Message) -> TCFuture<void>
						{
							CStr ActionString;
							if (_pCommandLine->m_bColorEnabled)
								ActionString = "{}Closed   {}"_f << CAnsiEncoding::ms_StatusWarning << CAnsiEncoding::ms_Default;
							else
								ActionString = "Closed   ";

							*_pCommandLine += "{} '{}': {}:{} {}\n"_f << ActionString << TunnelName << _Address.f_GetString() << _Address.f_GetPort() << _Message;
							return fg_Explicit();
						}
						, g_ActorFunctor / [=](CNetAddress const &_Address, CStr const &_Error) -> TCFuture<void>
					 	{
							CStr ActionString;
							if (_pCommandLine->m_bColorEnabled)
								ActionString = "{}Error    {}"_f << CAnsiEncoding::ms_StatusError << CAnsiEncoding::ms_Default;
							else
								ActionString = "Error    ";

							*_pCommandLine += "{} '{}': {}:{} {}\n"_f << ActionString << TunnelName << _Address.f_GetString() << _Address.f_GetPort() << _Error;
							return fg_Explicit();
						}
					)
					> OpenedTunnelResults.f_AddResult(TunnelKey)
				;
			}
		}

		auto OpenedTunnels = co_await OpenedTunnelResults.f_GetResults() | g_Unwrap;

		CTableRenderHelper TableRenderer
			(
				[&](NStr::CStr const &_Output)
			 	{
					*_pCommandLine += _Output;
				}
			 	, (_pCommandLine->m_bColorEnabled ? CTableRenderHelper::EOption_Color : CTableRenderHelper::EOption_None) | CTableRenderHelper::EOption_Rounded
		 	)
		;

		TableRenderer.f_AddHeadings("HostID", "Tunnel Name", "URL");

		for (auto &Tunnel : OpenedTunnels)
		{
			auto &TunnelKey = OpenedTunnels.fs_GetKey(Tunnel);

			auto &IP = Tunnel.m_ListenAddress.f_GetIP().m_IP;

			CStr HostString = CStr("{}.{}.{}.{}"_f << IP[0] << IP[1] << IP[2] << IP[3]);
			if (HostString == "127.0.0.1")
				HostString = "localhost";

			CStr TunnelURL = URLTemplates[TunnelKey];

			TunnelURL = TunnelURL.f_Replace("{Host}", HostString);
			TunnelURL = TunnelURL.f_Replace("{Port}", CStr("{}"_f << Tunnel.m_ListenAddress.m_Port));

			TableRenderer.f_AddRow(TunnelKey.m_HostID, TunnelKey.m_TunnelName, TunnelURL);
		}

		TableRenderer.f_Output();

		co_await mp_AppStopPromises.f_Insert().f_Future();

		co_return 0;
	}
}
