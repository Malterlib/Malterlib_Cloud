// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>

#include "Malterlib_Cloud_KeyManagerServer.h"

namespace NMib::NCloud
{
	class CKeyManagerServerDatabase_EncryptedFile : public ICKeyManagerServerDatabase
	{
	public:
		using CActorHolder = NConcurrency::CSeparateThreadActorHolder;
		
		CKeyManagerServerDatabase_EncryptedFile(NStr::CStr const &_Path, NStr::CStrSecure const &_Password, NContainer::CSecureByteVector const &_Salt);
		
		~CKeyManagerServerDatabase_EncryptedFile();
		
		NConcurrency::TCFuture<void> f_Initialize() override;
		NConcurrency::TCFuture<void> f_WriteDatabase(CDatabase const &_Database) override;
		NConcurrency::TCFuture<CDatabase> f_ReadDatabase() override;
	
	private:
		struct CInternal;
		NStorage::TCUniquePointer<CInternal> mp_pInternal;
	};
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NCloud;
#endif
