// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

namespace NMib::NCloud::NHostMonitor
{
	template <typename tf_CStream>
	void CConfigFileContents_GeneralText::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Parsed;
	}

	template <typename tf_CStream>
	void CConfigFileContents_GeneralBinary::f_Stream(tf_CStream &_Stream)
	{
	}

	template <typename tf_CStream>
	void CConfigFileContents_Registry::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Parsed;
	}

	template <typename tf_CStream>
	void CConfigFileContents_Json::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Parsed;
	}

	template <typename tf_CStream>
	void CConfigFileContents::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Raw;
		_Stream % m_Parsed;
	}

	template <typename tf_CStream>
	void CConfigFileUniqueProperties::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_ConfigType;
		_Stream % m_Digest;
		_Stream % m_ParseError;
		_Stream % m_Owner;
		_Stream % m_Group;
		_Stream % m_Size;
		_Stream % m_Attributes;
		_Stream % m_bExists;
	}

	template <typename tf_CStream>
	void CConfigFileProperties::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_UniqueProperties;
		_Stream % m_Timestamp;
	}
}
