// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

namespace NMib::NCloud
{
	template <typename tf_CStream>
	void CKeyManagerServerSync::CReadDatabase::CClientKey::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_KeyID;
		_Stream % m_Key;
		_Stream % m_VerifiedOnServers;
	}

	template <typename tf_CStream>
	void CKeyManagerServerSync::CReadDatabase::CClient::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_HostID;
		_Stream % m_Keys;
	}

	template <typename tf_CStream>
	void CKeyManagerServerSync::CReadDatabase::CAvailableKeys::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_KeySize;
		_Stream % m_Keys;
	}

	template <typename tf_CStream>
	void CKeyManagerServerSync::CReadDatabase::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Clients;
		_Stream % m_AvailableKeys;
	}

	template <typename tf_CStream>
	void CKeyManagerServerSync::CHostKeyID::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_HostID;
		_Stream % m_KeyID;
	}

	template <typename tf_CStream>
	void CKeyManagerServerSync::CUseAvailableKeyResult::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_bCanUse;
		_Stream % fg_Move(m_Subscription);
	}
}
