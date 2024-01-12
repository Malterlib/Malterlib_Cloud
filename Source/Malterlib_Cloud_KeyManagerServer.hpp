// Copyright © 2024 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

namespace NMib::NCloud
{
	template <typename tf_CStream>
	void ICKeyManagerServerDatabase::CDatabase::CClientStore::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Keys;
	}

	template <typename tf_CStream>
	void ICKeyManagerServerDatabase::CDatabase::f_Stream(tf_CStream &_Stream)
	{
		EVersion Version = EVersion::mc_Current;
		_Stream % Version;
		DMibBinaryStreamVersion(_Stream, Version);

		_Stream % m_Clients;
		_Stream % m_AvailableKeys;
	}
}
