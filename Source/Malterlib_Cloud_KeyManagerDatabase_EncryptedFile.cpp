// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Network/SSL>

#include "Malterlib_Cloud_KeyManagerDatabase_EncryptedFile.h"

namespace NMib
{
	namespace NCloud
	{
		struct CKeyManagerServerDatabase_EncryptedFile::CInternal
		{
			CInternal(NStr::CStr const &_Path, NStr::CStrSecure const &_Password, NNet::CEncryptAES::CSalt const *_pSalt)
				: m_AESContext(_Password, _pSalt)
				, m_Path(_Path)
			{
				
			}
			
			~CInternal()
			{
				
			}
			
			NConcurrency::TCContinuation<void> f_WriteDatabase(CDatabase const &_Database)
			{
				return NConcurrency::TCContinuation<void>::fs_RunProtected<NException::CException>() > [&]
					{
						NStream::CBinaryStreamMemory<NStream::CBinaryStreamDefault, NContainer::TCVector<uint8, NMem::CAllocator_HeapSecure>> Stream;
						Stream << _Database;
						NStream::fg_PadAlignStream(Stream, 32);
						
						NContainer::TCVector<uint8, NMem::CAllocator_HeapSecure> RawDatabase = Stream.f_MoveVector();
						NContainer::TCVector<uint8> EncryptedDatabase;
						m_AESContext.f_Encrypt(RawDatabase.f_GetArray(), RawDatabase.f_GetLen(), EncryptedDatabase.f_GetArray(RawDatabase.f_GetLen()));
						
						NFile::CFile::fs_WriteFile(EncryptedDatabase, m_Path);
					}
				;
			}
			
			NConcurrency::TCContinuation<CDatabase> f_ReadDatabase()
			{
				return NConcurrency::TCContinuation<CDatabase>::fs_RunProtected<NException::CException>() > [&]
					{
						CDatabase Database;
						if (!NFile::CFile::fs_FileExists(m_Path))
							return Database;
						
						NContainer::TCVector<uint8> EncryptedDatabase = NFile::CFile::fs_ReadFile(m_Path);
						NContainer::TCVector<uint8, NMem::CAllocator_HeapSecure> RawDatabase;
						m_AESContext.f_Decrypt(EncryptedDatabase.f_GetArray(), EncryptedDatabase.f_GetLen(), RawDatabase.f_GetArray(EncryptedDatabase.f_GetLen()));
						
						NStream::CBinaryStreamMemoryPtr<> Stream;
						Stream.f_OpenRead(RawDatabase.f_GetArray(), RawDatabase.f_GetLen());
						Stream >> Database;
						return Database;
					}
				;
			}
			
			NNet::CEncryptAES m_AESContext;
			NStr::CStr m_Path;
		};
		
		CKeyManagerServerDatabase_EncryptedFile::CKeyManagerServerDatabase_EncryptedFile
			(
				NStr::CStr const &_Path
				, NStr::CStrSecure const &_Password
				, NNet::CEncryptAES::CSalt const *_pSalt
			)
			: mp_pInternal(fg_Construct(_Path, _Password, _pSalt))
		{
			
		}
		
		CKeyManagerServerDatabase_EncryptedFile::~CKeyManagerServerDatabase_EncryptedFile()
		{
			
		}
			
		NConcurrency::TCContinuation<void> CKeyManagerServerDatabase_EncryptedFile::f_WriteDatabase(CDatabase const &_Database)
		{
			return mp_pInternal->f_WriteDatabase(_Database);
		}
		
		NConcurrency::TCContinuation<ICKeyManagerServerDatabase::CDatabase> CKeyManagerServerDatabase_EncryptedFile::f_ReadDatabase()
		{
			return mp_pInternal->f_ReadDatabase();
		}
	}
}