// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Network/SSL>

#include "Malterlib_Cloud_KeyManagerDatabase_EncryptedFile.h"

namespace NMib::NCloud
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
		
		NConcurrency::TCContinuation<void> f_Initialize()
		{
			return NConcurrency::TCContinuation<void>::fs_RunProtected<NException::CException>() > [&]
				{
					if (!NFile::CFile::fs_FileExists(m_Path))
					{
						CDatabase Database;
						fp_WriteDatabase(Database);
					}
					else
					{
						NContainer::TCVector<uint8> FileData;
						NFile::CFile File;
						File.f_Open(m_Path, NFile::EFileOpen_Read|NFile::EFileOpen_ShareAll);
						FileData.f_SetLen(32);
						File.f_Read(FileData.f_GetArray(), FileData.f_GetLen());
						
						NContainer::TCVector<uint8, NMem::CAllocator_HeapSecure> PlainTextCheck;
						m_AESContext.f_Decrypt(FileData.f_GetArray(), FileData.f_GetLen(), PlainTextCheck.f_GetArray(32));
						
						if (NMem::fg_MemCmp(PlainTextCheck.f_GetArray(), (uint8 const *)mc_pPlainTextCheck, 32) != 0)
							DMibError("Password mismatch");
					}
				}
			;
		}
		
		NConcurrency::TCContinuation<void> f_WriteDatabase(CDatabase const &_Database)
		{
			return NConcurrency::TCContinuation<void>::fs_RunProtected<NException::CException>() > [&]
				{
					fp_WriteDatabase(_Database);
				}
			;
		}
		
		NConcurrency::TCContinuation<CDatabase> f_ReadDatabase()
		{
			return NConcurrency::TCContinuation<CDatabase>::fs_RunProtected<NException::CException>() > [&]
				{
					NContainer::TCVector<uint8> EncryptedDatabase = NFile::CFile::fs_ReadFile(m_Path);
					NContainer::TCVector<uint8, NMem::CAllocator_HeapSecure> RawDatabase;
					m_AESContext.f_Decrypt(EncryptedDatabase.f_GetArray(), EncryptedDatabase.f_GetLen(), RawDatabase.f_GetArray(EncryptedDatabase.f_GetLen()));
					
					ch8 PlainTextCheck[32];
					CDatabase Database;
					NStream::CBinaryStreamMemoryPtr<> Stream;
					Stream.f_OpenRead(RawDatabase.f_GetArray(), RawDatabase.f_GetLen());
					Stream.f_ConsumeBytes(PlainTextCheck, 32);
					
					if (NMem::fg_MemCmp((uint8 const *)PlainTextCheck, (uint8 const *)mc_pPlainTextCheck, 32) != 0)
						DMibError("Password mismatch");
					
					Stream >> Database;
					return Database;
				}
			;
		}
		
		NNet::CEncryptAES m_AESContext;
		NStr::CStr m_Path;
		static constexpr ch8 const *mc_pPlainTextCheck = "ABCDEFGHIJKLMNOPQRSTUVWXYZABCDEF";
		
	private:
		
		void fp_WriteDatabase(CDatabase const &_Database)
		{
			NStream::CBinaryStreamMemory<NStream::CBinaryStreamDefault, NContainer::TCVector<uint8, NMem::CAllocator_HeapSecure>> Stream;
			Stream.f_FeedBytes(mc_pPlainTextCheck, 32);
			Stream << _Database;
			NStream::fg_PadAlignStream(Stream, 32);
			
			NContainer::TCVector<uint8, NMem::CAllocator_HeapSecure> RawDatabase = Stream.f_MoveVector();
			NContainer::TCVector<uint8> EncryptedDatabase;
			m_AESContext.f_Encrypt(RawDatabase.f_GetArray(), RawDatabase.f_GetLen(), EncryptedDatabase.f_GetArray(RawDatabase.f_GetLen()));
			
			NFile::CFile::fs_CreateDirectory(NFile::CFile::fs_GetPath(m_Path));
			NFile::CFile::fs_WriteFile(EncryptedDatabase, m_Path);
		}
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
	
	NConcurrency::TCContinuation<void> CKeyManagerServerDatabase_EncryptedFile::f_Initialize()
	{
		return mp_pInternal->f_Initialize();
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
