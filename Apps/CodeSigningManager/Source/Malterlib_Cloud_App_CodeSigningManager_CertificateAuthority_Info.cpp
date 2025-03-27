// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "Malterlib_Cloud_App_CodeSigningManager.h"

#include <Mib/Encoding/JsonShortcuts>

namespace NMib::NCloud::NCodeSigningManager
{
	TCFuture<uint32> CCodeSigningManagerActor::fp_CommandLine_AuthorityInfo(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		CStr AuthorityName = _Params["Name"].f_String();

		if (AuthorityName.f_IsEmpty())
			co_return DMibErrorInstance("Authority name must be specified with --name");

		auto *pAuthority = mp_Authorities.f_FindEqual(AuthorityName);
		if (!pAuthority)
			co_return DMibErrorInstance("Authority '{}' not found", AuthorityName);

		CEJsonSorted Result;
		Result["Name"] = AuthorityName;
		Result["KeyType"] = fsp_PublicKeySettingToStr(pAuthority->m_PublicKeySetting);
		Result["Serial"] = pAuthority->m_Serial;
		Result["Created"] = "{}"_f << pAuthority->m_Created;
		Result["LastModified"] = "{}"_f << pAuthority->m_LastModified;
		Result["Certificate"] = pAuthority->m_Certificate;

		co_await _pCommandLine->f_StdOut(Result.f_ToString());

		co_return 0;
	}
}
