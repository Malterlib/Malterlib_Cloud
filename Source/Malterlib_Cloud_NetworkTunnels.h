// Copyright © 2019 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Encoding/EJSON>

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
			, EProtocolVersion_Current = 0x101
		};

		using CNetworkTunnelName = NStr::CStr;

		struct CNetworkTunnel
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NEncoding::CEJSONSorted m_MetaData;
		};

		using FSendBytes = NConcurrency::TCActorFunctorWithID<NConcurrency::TCFuture<void> (NContainer::CSecureByteVector &&_Data)>;

		virtual NConcurrency::TCFuture<NContainer::TCMap<CNetworkTunnelName, CNetworkTunnel>> f_EnumerateTunnels() = 0;
		virtual NConcurrency::TCFuture<FSendBytes> f_OpenConnection(CNetworkTunnelName const &_Name, FSendBytes &&_fOnReceive) = 0;
	};
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NCloud;
#endif

#include "Malterlib_Cloud_NetworkTunnels.hpp"
