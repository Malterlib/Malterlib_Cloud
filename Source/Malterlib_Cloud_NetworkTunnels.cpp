// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

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
