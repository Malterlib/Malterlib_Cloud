// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

namespace NMib::NCloud
{
	template <typename tf_CContainer>
	void CBackupManagerClient::CInternal::fs_ValidateWildcards(tf_CContainer const &_Container, bool _bIsFileSearch)
	{
		for (auto iWildcard = _Container.f_GetIterator(); iWildcard; ++iWildcard)
			fs_ValidateWildcard(iWildcard.f_GetKey(), _bIsFileSearch);
	}

	template <typename tf_CContainer>
	bool CBackupManagerClient::CInternal::fs_MatchesAnyWildcard(CStr const &_String, tf_CContainer const &_Container)
	{
		for (auto iWildcard = _Container.f_GetIterator(); iWildcard; ++iWildcard)
		{
			if (fg_StrMatchWildcard(_String.f_GetStr(), iWildcard.f_GetKey().f_GetStr()) == EMatchWildcardResult_WholeStringMatchedAndPatternExhausted)
				return true;
		}
		
		return false;
	}
}
