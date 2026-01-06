// Copyright © 2019 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

#include "Malterlib_Cloud_NetworkTunnels.h"

namespace NMib::NCloud
{
	using namespace NStr;

	ICNetworkTunnels::ICNetworkTunnels()
	{
		DMibPublishActorFunction(ICNetworkTunnels::f_EnumerateTunnels);
		DMibPublishActorFunction(ICNetworkTunnels::f_OpenConnection);
		DMibPublishActorFunction(ICNetworkTunnels::f_SubscribeTunnels);
	}

	ICNetworkTunnels::~ICNetworkTunnels() = default;
}
