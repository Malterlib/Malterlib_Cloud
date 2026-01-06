// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

namespace NMib::NCloud
{
	template <typename tf_CStr>
	void CCloudVersion::f_Format(tf_CStr &o_String) const
	{
		o_String += typename tf_CStr::CFormat("{}/{}.{}.{}") << m_Branch << m_Major << m_Minor << m_Revision;
	}

	template <typename tf_CStream>
	void CCloudVersion::f_Stream(tf_CStream &_Stream)
	{
		uint32 Version = 0x101;
		_Stream % Version;
		DMibBinaryStreamVersion(_Stream, Version);

		_Stream % m_Branch;
		_Stream % m_Major;
		_Stream % m_Minor;
		_Stream % m_Revision;
	}

	template <typename tf_CStream>
	void CCloudVersionInfo::f_Stream(tf_CStream &_Stream)
	{
		uint32 Version = 0x101;
		_Stream % Version;
		DMibBinaryStreamVersion(_Stream, Version);

		_Stream % m_Application;
		_Stream % m_Version;
		_Stream % m_Platform;
		_Stream % m_Configuration;
		_Stream % m_ExtraInfo;
	}
}
