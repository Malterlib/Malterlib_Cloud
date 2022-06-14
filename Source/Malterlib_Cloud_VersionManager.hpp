// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

namespace NMib::NCloud
{
	template <typename tf_CStr>
	void CVersionManager::CVersionID::f_Format(tf_CStr &o_Str) const
	{
		o_Str += typename tf_CStr::CFormat("{}/{}.{}.{}")
			<< m_Branch
			<< m_Major
			<< m_Minor
			<< m_Revision
		;
	}

	template <typename tf_CStr>
	void CVersionManager::CVersionIDAndPlatform::f_Format(tf_CStr &o_Str) const
	{
		o_Str += typename tf_CStr::CFormat("{} {}")
			<< m_VersionID
			<< m_Platform
		;
	}

	template <typename tf_CStream>
	void CVersionManager::CVersionID::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Branch;
		_Stream % m_Major;
		_Stream % m_Minor;
		_Stream % m_Revision;
	}

	template <typename tf_CStream>
	void CVersionManager::CVersionIDAndPlatform::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_VersionID;
		_Stream % m_Platform;
	}

	template <typename tf_CStream>
	void CVersionManager::CVersionInformation::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Time;
		_Stream % m_Configuration;
		_Stream % m_Tags;
		_Stream % m_ExtraInfo;
		_Stream % m_nFiles;
		_Stream % m_nBytes;
		if (_Stream.f_GetVersion() >= EProtocolVersion_SupportIncreaseRetrySequence)
			_Stream % m_RetrySequence;
	}
}
