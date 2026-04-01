// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

namespace NMib::NCloud
{
	template <typename tf_CStream>
	void ICKeyManagerServerDatabase::CDatabase::CClientKey::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Key;

		if (_Stream.f_SupportsVersion(EVersion::mc_ServerSyncSupport))
			_Stream % m_VerifiedOnServers;
	}

	template <typename tf_CStream>
	void ICKeyManagerServerDatabase::CDatabase::CClientStore::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Keys;
	}

	template <typename tf_CStream>
	void ICKeyManagerServerDatabase::CDatabase::f_Stream(tf_CStream &_Stream)
	{
		auto Version = _Stream.f_StreamVersion(EVersion::mc_Current);

		_Stream % m_Clients;
		_Stream % m_AvailableKeys;
	}
}
