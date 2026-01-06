// Copyright © 2019 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Encoding/EJson>

namespace NMib::NCloud
{
	struct ICNetworkTunnels : public NConcurrency::CActor
	{
		static constexpr ch8 const *mc_pDefaultNamespace = "com.malterlib/Cloud/NetworkTunnel";

		ICNetworkTunnels();
		~ICNetworkTunnels();

		enum : uint32
		{
			EProtocolVersion_Min = 0x101

			, EProtocolVersion_SupportSubscribeTunnels = 0x102
			, EProtocolVersion_SupportConnectionID = 0x103

			, EProtocolVersion_Current = 0x103
		};

		using CNetworkTunnelName = NStr::CStr;

		struct CNetworkTunnel
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NEncoding::CEJsonSorted m_Metadata;
		};

		enum ETunnelChange
		{
			ETunnelChange_Initial
			, ETunnelChange_Add
			, ETunnelChange_Remove
		};

		struct CTunnelChange_Initial
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NContainer::TCMap<CNetworkTunnelName, CNetworkTunnel> m_Tunnels;
		};

		struct CTunnelChange_Add
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			CNetworkTunnelName m_TunnelName;
			CNetworkTunnel m_Tunnel;
		};

		struct CTunnelChange_Remove
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			CNetworkTunnelName m_TunnelName;
		};

		using CTunnelChange = NStorage::TCStreamableVariant
			<
				ETunnelChange
				, NStorage::TCMember<CTunnelChange_Initial, ETunnelChange_Initial>
				, NStorage::TCMember<CTunnelChange_Add, ETunnelChange_Add>
				, NStorage::TCMember<CTunnelChange_Remove, ETunnelChange_Remove>
			>
		;

		struct CSubscribeTunnels
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NConcurrency::TCActorFunctorWithID<NConcurrency::TCFuture<void> (CTunnelChange _TunnelChange)> m_fOnTunnelChange;
		};

		using FSendBytes = NConcurrency::TCActorFunctorWithID<NConcurrency::TCFuture<void> (NContainer::CIOByteVector _Data)>;

		struct COpenConnection
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			CNetworkTunnelName m_Name;
			FSendBytes m_fOnReceive;
			NStr::CStr m_ConnectionID;
		};

		virtual NConcurrency::TCFuture<NContainer::TCMap<CNetworkTunnelName, CNetworkTunnel>> f_EnumerateTunnels() = 0;
		virtual NConcurrency::TCFuture<FSendBytes> f_OpenConnection(COpenConnection _OpenConnection) = 0;
		virtual NConcurrency::TCFuture<NConcurrency::TCActorSubscriptionWithID<>> f_SubscribeTunnels(CSubscribeTunnels _Subscribe) = 0;
	};
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NCloud;
#endif

#include "Malterlib_Cloud_NetworkTunnels.hpp"
