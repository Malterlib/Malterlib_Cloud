// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

namespace NMib::NCloud
{
	template <typename tf_CStream>
	void ICNetworkTunnels::CNetworkTunnel::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Metadata;
	}

	template <typename tf_CStream>
	void ICNetworkTunnels::CSubscribeTunnels::f_Stream(tf_CStream &_Stream)
	{
		_Stream % fg_Move(m_fOnTunnelChange);
	}

	template <typename tf_CStream>
	void ICNetworkTunnels::CTunnelChange_Initial::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Tunnels;
	}

	template <typename tf_CStream>
	void ICNetworkTunnels::CTunnelChange_Add::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_TunnelName;
		_Stream % m_Tunnel;
	}

	template <typename tf_CStream>
	void ICNetworkTunnels::CTunnelChange_Remove::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_TunnelName;
	}

	template <typename tf_CStream>
	void ICNetworkTunnels::COpenConnection::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Name;
		_Stream % fg_Move(m_fOnReceive);

		if (_Stream.f_GetVersion() >= EProtocolVersion_SupportConnectionID)
			_Stream % m_ConnectionID;
	}
}
