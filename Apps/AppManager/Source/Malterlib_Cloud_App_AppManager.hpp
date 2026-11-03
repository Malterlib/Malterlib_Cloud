// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

namespace NMib::NCloud::NAppManager
{
	template <typename tf_CStr>
	void CAppManagerActor::CRemoteApplicationKey::f_Format(tf_CStr &o_Str) const
	{
		o_Str += typename tf_CStr::CFormat("{}/{}") << m_Group << m_VersionManagerApplication;
	}

	template <typename tf_CStr>
	void CAppManagerActor::CRemoteApplicationWithTypeKey::f_Format(tf_CStr &o_Str) const
	{
		o_Str += typename tf_CStr::CFormat("{}/{}/{}") << m_Group << m_VersionManagerApplication << m_UpdateType;
	}

	inline auto &CAppManagerActor::CRemoteAppManager::CCompareActor::operator () (CRemoteAppManager const &_Node) const
	{
		return _Node.m_Actor;
	}

	template <typename tf_CType>
	bool CAppManagerActor::CUpdateApplicationState::f_CheckAbort(TCPromise<tf_CType> const &o_Error)
	{
		if (auto pError = f_CheckAbort())
		{
			if (!o_Error.f_IsSet())
				o_Error.f_SetException(pError);
			return true;
		}
		return false;
	}
}
