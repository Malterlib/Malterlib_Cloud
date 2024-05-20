// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

namespace NMib::NCloud
{
	struct CNetworkTunnelManager : public CActor
	{
		CNetworkTunnelManager();

		TCFuture<void> f_Subscribe(CStr const &_DomainWildcard);

	private:
	};
}
