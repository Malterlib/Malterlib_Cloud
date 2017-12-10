// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Network/SSL>

#include "Malterlib_Cloud_App_SecretsManager_Database.h"

namespace NMib::NCloud::NSecretsManager
{
	CSecretsManagerServerDatabase::CSecretsManagerServerDatabase
		(
			CStr const &_Path
			, CSecureByteVector const &_Key
			, CEncryptAES::CSalt const *_pSalt
		)
		: mp_Path(_Path)
		, mp_AESContext(_Key, _pSalt)
	{
	}
	
	CSecretsManagerServerDatabase::~CSecretsManagerServerDatabase()
	{
	}

	TCContinuation<void> CSecretsManagerServerDatabase::fp_Destroy()
	{
		if (!mp_pPendingWrite)
			return fg_Explicit();

		auto WriteContinuation = mp_PendingWriteContinuations.f_Insert();

		TCContinuation<void> Continuation;
		WriteContinuation > [Continuation](TCAsyncResult<void> &&_Result)
			{
				Continuation.f_SetResult();
			}
		;
		return Continuation;
	}

	TCContinuation<void> CSecretsManagerServerDatabase::f_Initialize()
	{
		return TCContinuation<void>::fs_RunProtected<CException>() > [&]
			{
				if (!CFile::fs_FileExists(mp_Path))
				{
					DMibLogWithCategory(Mib/Cloud/SecretsManager, Info, "No current database found. Creating new");
					CSecretsManagerServerDatabase::CDatabase Database;
					fp_WriteDatabase(Database);
				}
				else
				{
					TCVector<uint8> FileData;
					CFile File;
					File.f_Open(mp_Path, EFileOpen_Read|EFileOpen_ShareAll);
					FileData.f_SetLen(32);
					File.f_Read(FileData.f_GetArray(), FileData.f_GetLen());
					
					CSecureByteVector PlainTextCheck;
					mp_AESContext.f_Decrypt(FileData.f_GetArray(), FileData.f_GetLen(), PlainTextCheck.f_GetArray(32));
					
					if (fg_MemCmp(PlainTextCheck.f_GetArray(), (uint8 const *)mc_pPlainTextCheck, 32) != 0)
						DMibError("Password mismatch");
				}
			}
		;
	}
	
	TCContinuation<void> CSecretsManagerServerDatabase::f_WriteDatabase(CSecretsManagerServerDatabase::CDatabase &&_Database)
	{
		if (!mp_pPendingWrite)
		{
			g_Dispatch > [this]
				{
					auto WriteResult = TCContinuation<void>::fs_RunProtected<CException>() > [&]
						{
							fp_WriteDatabase(*mp_pPendingWrite);
						}
					;

					mp_pPendingWrite.f_Clear();

					for (auto &Continuation : mp_PendingWriteContinuations)
						Continuation.f_SetResult(WriteResult.m_pData->m_Result);

					mp_PendingWriteContinuations.f_Clear();
				}
				> fg_DiscardResult();
			;

			mp_pPendingWrite = fg_Construct(fg_Move(_Database));
		}
		else
			*mp_pPendingWrite = fg_Move(_Database);

		return mp_PendingWriteContinuations.f_Insert();
	}
	
	TCContinuation<CSecretsManagerServerDatabase::CDatabase> CSecretsManagerServerDatabase::f_ReadDatabase()
	{
		return TCContinuation<CDatabase>::fs_RunProtected<CException>() > [&]
			{
				TCVector<uint8> EncryptedDatabase = CFile::fs_ReadFile(mp_Path);
				CSecureByteVector RawDatabase;
				mp_AESContext.f_Decrypt(EncryptedDatabase.f_GetArray(), EncryptedDatabase.f_GetLen(), RawDatabase.f_GetArray(EncryptedDatabase.f_GetLen()));

				ch8 PlainTextCheck[32];
				CDatabase Database;
				CBinaryStreamMemoryPtr<> Stream;
				Stream.f_OpenRead(RawDatabase.f_GetArray(), RawDatabase.f_GetLen());
				Stream.f_ConsumeBytes(PlainTextCheck, 32);

				if (fg_MemCmp((uint8 const *)PlainTextCheck, (uint8 const *)mc_pPlainTextCheck, 32) != 0)
					DMibError("Password mismatch");

				Stream >> Database;
				return Database;
			}
		;
	}

	void CSecretsManagerServerDatabase::fp_WriteDatabase(CDatabase const &_Database)
	{
		CBinaryStreamMemory<CBinaryStreamDefault, CSecureByteVector> Stream;
		Stream.f_FeedBytes(mc_pPlainTextCheck, 32);
		Stream << _Database;
		fg_PadAlignStream(Stream, 32);
		
		CSecureByteVector RawDatabase = Stream.f_MoveVector();
		TCVector<uint8> EncryptedDatabase;
		mp_AESContext.f_Encrypt(RawDatabase.f_GetArray(), RawDatabase.f_GetLen(), EncryptedDatabase.f_GetArray(RawDatabase.f_GetLen()));
		
		CFile::fs_CreateDirectory(CFile::fs_GetPath(mp_Path));

		{
			CFile OutFile;
			auto Attributes = EFileAttrib_UserWrite | EFileAttrib_UserRead | CFile::fs_GetValidAttributes();
			OutFile.f_Open(mp_Path + ".tmp", EFileOpen_Write | EFileOpen_ShareAll, Attributes);
			OutFile.f_Write(EncryptedDatabase.f_GetArray(), EncryptedDatabase.f_GetLen());
		}

		if (CFile::fs_FileExists(mp_Path))
			CFile::fs_AtomicReplaceFile(mp_Path + ".tmp", mp_Path);
		else
			CFile::fs_RenameFile(mp_Path + ".tmp", mp_Path);
	}

	void NSecretsManager::CSecretPropertiesInternal::f_Format(CStrAggregate &o_Str) const
	{
		o_Str += "Username: {} URL: {} Expires: {} Notes: {} Metadata: {} Created: {} Modified: {} SemanticID: {} Tags: {}"_f
			<< m_UserName
			<< m_URL
			<< m_Expires
			<< m_Notes
			<< m_Metadata
			<< m_Created
			<< m_Modified
			<< m_SemanticID
			<< m_Tags
		;
	}
}
