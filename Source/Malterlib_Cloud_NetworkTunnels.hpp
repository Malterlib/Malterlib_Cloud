// Copyright © 2019 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

namespace NMib::NCloud
{
	template <typename tf_CStream>
	void ICNetworkTunnels::CNetworkTunnel::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_MetaData;
	}
}


