// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

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

	// CVersionID

	template <typename tf_CJson>
	tf_CJson CVersionManager::CVersionID::f_ToJson() const
	{
		tf_CJson Ret;
		Ret["Branch"] = m_Branch;
		Ret["Major"] = m_Major;
		Ret["Minor"] = m_Minor;
		Ret["Revision"] = m_Revision;
		return Ret;
	}

	template <typename tf_CJson>
	auto CVersionManager::CVersionID::fs_FromJson(tf_CJson const &_Json) -> CVersionID
	{
		CVersionID Ret;
		Ret.m_Branch = _Json["Branch"].f_String();
		Ret.m_Major = _Json["Major"].f_Integer();
		Ret.m_Minor = _Json["Minor"].f_Integer();
		Ret.m_Revision = _Json["Revision"].f_Integer();
		return Ret;
	}

	// CVersionIDAndPlatform

	template <typename tf_CJson>
	tf_CJson CVersionManager::CVersionIDAndPlatform::f_ToJson() const
	{
		tf_CJson Ret;
		Ret["VersionID"] = m_VersionID.f_ToJson<tf_CJson>();
		Ret["Platform"] = m_Platform;
		return Ret;
	}

	template <typename tf_CJson>
	auto CVersionManager::CVersionIDAndPlatform::fs_FromJson(tf_CJson const &_Json) -> CVersionIDAndPlatform
	{
		CVersionIDAndPlatform Ret;
		Ret.m_VersionID = CVersionID::fs_FromJson(_Json["VersionID"]);
		Ret.m_Platform = _Json["Platform"].f_String();
		return Ret;
	}

	// CVersionInformation

	template <typename tf_CJson>
	tf_CJson CVersionManager::CVersionInformation::f_ToJson() const
	{
		NEncoding::CEJsonSorted Ret;
		Ret["Time"] = m_Time;
		Ret["Configuration"] = m_Configuration;
		auto &TagArray = Ret["Tags"].f_Array();
		for (auto &Tag : m_Tags)
			TagArray.f_Insert(Tag);
		if (m_ExtraInfo.f_IsObject())
			Ret["ExtraInfo"] = m_ExtraInfo;
		Ret["NumFiles"] = m_nFiles;
		Ret["NumBytes"] = m_nBytes;
		Ret["RetrySequence"] = m_RetrySequence;

		if constexpr (NTraits::cIsSame<tf_CJson, NEncoding::CEJsonSorted>)
			return Ret;
		else
			return tf_CJson::fs_FromCompatible(fg_Move(Ret));
	}

	template <typename tf_CJson>
	auto CVersionManager::CVersionInformation::fs_FromJson(tf_CJson const &_Json) -> CVersionInformation
	{
		CVersionInformation Ret;
		Ret.m_Time = _Json["Time"].f_Date();
		Ret.m_Configuration = _Json["Configuration"].f_String();
		if (auto *pValue = _Json.f_GetMember("Tags", NEncoding::EJsonType_Array))
		{
			for (auto &Tag : pValue->f_Array())
				Ret.m_Tags[Tag.f_String()];
		}
		if (auto *pValue = _Json.f_GetMember("ExtraInfo", NEncoding::EJsonType_Object))
			Ret.m_ExtraInfo = NEncoding::CEJsonSorted::fs_FromCompatible(*pValue);
		Ret.m_nFiles = _Json["NumFiles"].f_Integer();
		Ret.m_nBytes = _Json["NumBytes"].f_Integer();
		if (auto *pValue = _Json.f_GetMember("RetrySequence", NEncoding::EJsonType_Integer))
			Ret.m_RetrySequence = pValue->f_Integer();
		return Ret;
	}
}
