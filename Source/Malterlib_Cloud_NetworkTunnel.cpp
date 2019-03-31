// Copyright © 2019 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

#include "Malterlib_Cloud_NetworkTunnel.h"

namespace NMib::NCloud
{
	using namespace NStr;
	
	ICNetworkTunnel::ICNetworkTunnel()
	{
		DMibPublishActorFunction(ICNetworkTunnel::f_EnumerateTunnels);
		DMibPublishActorFunction(ICNetworkTunnel::f_OpenConnection);
	}

	ICNetworkTunnel::~ICNetworkTunnel() = default;
}
