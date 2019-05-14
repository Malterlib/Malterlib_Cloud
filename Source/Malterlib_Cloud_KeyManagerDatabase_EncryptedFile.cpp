// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Cryptography/SymmetricCrypto>

#include "Malterlib_Cloud_KeyManagerDatabase_EncryptedFile.h"

namespace NMib::NCloud
{
	struct CKeyManagerServerDatabase_EncryptedFile::CInternal : public NConcurrency::CActorInternal
	{
		CInternal(NStr::CStr const &_Path, NStr::CStrSecure const &_Password, NContainer::CSecureByteVector const &_Salt)
			: m_AESContext
			(
			 	NCryptography::CEncryptKeyIV::fs_GenerateKeyIV
			 	(
				 	_Password
				 	, _Salt
				 	, NCryptography::CKeyDerivationSettings_PKCS5_Deprecated{NCryptography::EDigestType_SHA256}
				 	, NCryptography::ECryptoType_AES_256_CBC
				)
			)
			, m_Path(_Path)
		{
			
		}
		
		~CInternal()
		{
			
		}
		
		NConcurrency::TCFuture<void> f_Initialize()
		{
			return NConcurrency::TCFuture<void>::fs_RunProtected<NException::CException>() / [&]
				{
					if (!NFile::CFile::fs_FileExists(m_Path))
					{
						CDatabase Database;
						fp_WriteDatabase(Database);
					}
					else
					{
						NContainer::CByteVector FileData;
						NFile::CFile File;
						File.f_Open(m_Path, NFile::EFileOpen_Read|NFile::EFileOpen_ShareAll);
						FileData.f_SetLen(32);
						File.f_Read(FileData.f_GetArray(), FileData.f_GetLen());
						
						NContainer::CSecureByteVector PlainTextCheck;
						m_AESContext.f_Decrypt(FileData.f_GetArray(), FileData.f_GetLen(), PlainTextCheck.f_GetArray(32));
						
						if (NMemory::fg_MemCmp(PlainTextCheck.f_GetArray(), (uint8 const *)mc_pPlainTextCheck, 32) != 0)
							DMibError("Password mismatch");
					}
				}
			;
		}
		
		NConcurrency::TCFuture<void> f_WriteDatabase(CDatabase const &_Database)
		{
			return NConcurrency::TCFuture<void>::fs_RunProtected<NException::CException>() / [&]
				{
					fp_WriteDatabase(_Database);
				}
			;
		}
		
		NConcurrency::TCFuture<CDatabase> f_ReadDatabase()
		{
			return NConcurrency::TCFuture<CDatabase>::fs_RunProtected<NException::CException>() / [&]
				{
					NContainer::CByteVector EncryptedDatabase = NFile::CFile::fs_ReadFile(m_Path);
					NContainer::CSecureByteVector RawDatabase;
					m_AESContext.f_Decrypt(EncryptedDatabase.f_GetArray(), EncryptedDatabase.f_GetLen(), RawDatabase.f_GetArray(EncryptedDatabase.f_GetLen()));
					
					ch8 PlainTextCheck[32];
					CDatabase Database;
					NStream::CBinaryStreamMemoryPtr<> Stream;
					Stream.f_OpenRead(RawDatabase.f_GetArray(), RawDatabase.f_GetLen());
					Stream.f_ConsumeBytes(PlainTextCheck, 32);
					
					if (NMemory::fg_MemCmp((uint8 const *)PlainTextCheck, (uint8 const *)mc_pPlainTextCheck, 32) != 0)
						DMibError("Password mismatch");
					
					Stream >> Database;
					return Database;
				}
			;
		}
		
		NCryptography::CEncryptAES m_AESContext;
		NStr::CStr m_Path;
		static constexpr ch8 const *mc_pPlainTextCheck = "ABCDEFGHIJKLMNOPQRSTUVWXYZABCDEF";
		
	private:
		
		void fp_WriteDatabase(CDatabase const &_Database)
		{
			NStream::CBinaryStreamMemory<NStream::CBinaryStreamDefault, NContainer::CSecureByteVector> Stream;
			Stream.f_FeedBytes(mc_pPlainTextCheck, 32);
			Stream << _Database;
			NStream::fg_PadAlignStream(Stream, 32);
			
			NContainer::CSecureByteVector RawDatabase = Stream.f_MoveVector();
			NContainer::CByteVector EncryptedDatabase;
			m_AESContext.f_Encrypt(RawDatabase.f_GetArray(), RawDatabase.f_GetLen(), EncryptedDatabase.f_GetArray(RawDatabase.f_GetLen()));
			
			NFile::CFile::fs_CreateDirectory(NFile::CFile::fs_GetPath(m_Path));

			{
				NFile::CFile OutFile;
				auto Attributes = NFile::EFileAttrib_UserWrite | NFile::EFileAttrib_UserRead | NFile::CFile::fs_GetValidAttributes();
				OutFile.f_Open(m_Path + ".tmp", NFile::EFileOpen_Write | NFile::EFileOpen_ShareAll, Attributes);
				OutFile.f_Write(EncryptedDatabase.f_GetArray(), EncryptedDatabase.f_GetLen());
			}
			if (NFile::CFile::fs_FileExists(m_Path))
				NFile::CFile::fs_AtomicReplaceFile(m_Path + ".tmp", m_Path);
			else
				NFile::CFile::fs_RenameFile(m_Path + ".tmp", m_Path);
		}
	};
	
	CKeyManagerServerDatabase_EncryptedFile::CKeyManagerServerDatabase_EncryptedFile
		(
			NStr::CStr const &_Path
			, NStr::CStrSecure const &_Password
			, NContainer::CSecureByteVector const &_Salt
		)
		: mp_pInternal(fg_Construct(_Path, _Password, _Salt))
	{
		
	}
	
	CKeyManagerServerDatabase_EncryptedFile::~CKeyManagerServerDatabase_EncryptedFile()
	{
		
	}
	
	NConcurrency::TCFuture<void> CKeyManagerServerDatabase_EncryptedFile::f_Initialize()
	{
		return mp_pInternal->f_Initialize();
	}
		
	NConcurrency::TCFuture<void> CKeyManagerServerDatabase_EncryptedFile::f_WriteDatabase(CDatabase const &_Database)
	{
		return mp_pInternal->f_WriteDatabase(_Database);
	}
	
	NConcurrency::TCFuture<ICKeyManagerServerDatabase::CDatabase> CKeyManagerServerDatabase_EncryptedFile::f_ReadDatabase()
	{
		return mp_pInternal->f_ReadDatabase();
	}
}
