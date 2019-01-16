// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Network/SSL>
#include <Mib/Cryptography/EncryptedStream>

#include "Malterlib_Cloud_App_SecretsManager_Database.h"

namespace NMib::NCloud::NSecretsManager
{
	CSecretsManagerServerDatabase::CSecretsManagerServerDatabase(CStr const &_Path, CSecureByteVector const &_Key)
		: mp_Path{_Path}
		, mp_Key{_Key}
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

	CSecureByteVector CSecretsManagerServerDatabase::fsp_ComputeSalt(CSecretsDatabaseIV const &_Salt)
	{
		CBinaryStreamMemory<CBinaryStreamDefault, CSecureByteVector> Stream;
		Stream << _Salt.m_InternalSalt;
		Stream << _Salt.m_ExternalSalt;
		return Stream.f_MoveVector();
	}

	TCContinuation<void> CSecretsManagerServerDatabase::f_Initialize()
	{
		return TCContinuation<void>::fs_RunProtected<CException>() / [&]
			{
				if (!CFile::fs_FileExists(mp_Path))
				{
					DMibLogWithCategory(Mib/Cloud/SecretsManager, Info, "No current database found. Creating new");
					CSecretsDatabase Database;
					fp_WriteDatabase(Database);
				}
				else
				{
					fp_ReadDatabase(nullptr);
				}
			}
		;
	}
	
	TCContinuation<void> CSecretsManagerServerDatabase::f_WriteDatabase(CSecretsDatabase &&_Database)
	{
		if (!mp_pPendingWrite)
		{
			g_Dispatch / [this]
				{
					auto WriteResult = TCContinuation<void>::fs_RunProtected<CException>() / [&]
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
	
	TCContinuation<CSecretsDatabase> CSecretsManagerServerDatabase::f_ReadDatabase()
	{
		return TCContinuation<CSecretsDatabase>::fs_RunProtected<CException>() / [&]
			{
				CSecretsDatabase Database;
				fp_ReadDatabase(&Database);
				return Database;
			}
		;
	}

	void CSecretsManagerServerDatabase::fp_WriteDatabase(CSecretsDatabase const &_Database)
	{
		{
			auto Attributes = EFileAttrib_UserWrite | EFileAttrib_UserRead | CFile::fs_GetValidAttributes();
			TCBinaryStreamFile<> BaseStream;
			CFile::fs_CreateDirectory(CFile::fs_GetPath(mp_Path));
			BaseStream.f_Open(mp_Path + ".tmp", NFile::EFileOpen_Write, Attributes);
			BaseStream << mp_IVSalt;
			auto BasePosition = BaseStream.f_GetPosition();

			CBinaryStreamSubStream<> SubStream;
			SubStream.f_Open(&BaseStream, BasePosition);

			CSecureByteVector Salt{fsp_ComputeSalt(mp_IVSalt)};
			CKeyExpansion KeyExpansion{mp_Key, Salt};

			TCBinaryStream_Encrypted<CBinaryStream *> EncryptedStream{KeyExpansion.f_GetKeyIV(), ESSLDigest_SHA512, KeyExpansion.f_GetHMACKey(ESSLDigest_SHA512)};

			auto CheckBuffer = KeyExpansion.f_GetKey("SecretsManagerCheck", 32);

			EncryptedStream.f_Open(&SubStream, NFile::EFileOpen_Write);
			EncryptedStream.f_FeedBytes(CheckBuffer.f_GetArray(), 32u);
			EncryptedStream << _Database;
		}

		if (CFile::fs_FileExists(mp_Path))
			CFile::fs_AtomicReplaceFile(mp_Path + ".tmp", mp_Path);
		else
			CFile::fs_RenameFile(mp_Path + ".tmp", mp_Path);

		++mp_IVSalt.m_InternalSalt;
	}

	void CSecretsManagerServerDatabase::fp_ReadDatabase(CSecretsDatabase *_pDatabase)
	{
		TCBinaryStreamFile<> BaseStream;
		BaseStream.f_Open(mp_Path, EFileOpen_Read|EFileOpen_ShareAll);
		BaseStream >> mp_IVSalt;
		auto BasePosition = BaseStream.f_GetPosition();

		CBinaryStreamSubStream<> SubStream;
		SubStream.f_Open(&BaseStream, BasePosition, BaseStream.f_GetLength() - BasePosition);

		CSecureByteVector Salt{fsp_ComputeSalt(mp_IVSalt)};
		CKeyExpansion KeyExpansion{mp_Key, Salt};

		TCBinaryStream_Encrypted<CBinaryStream *> EncryptedStream{KeyExpansion.f_GetKeyIV(), ESSLDigest_SHA512, KeyExpansion.f_GetHMACKey(ESSLDigest_SHA512)};
		EncryptedStream.f_Open(&SubStream, NFile::EFileOpen_Read);

		auto CheckBuffer = KeyExpansion.f_GetKey("SecretsManagerCheck", 32);

		uint8 PlainTextCheck[32];
		EncryptedStream.f_ConsumeBytes(PlainTextCheck, 32);
		if (fg_MemCmp(PlainTextCheck, CheckBuffer.f_GetArray(), 32) != 0)
			DMibError("Invalid database. Key mismatch.");
		if (_pDatabase)
			EncryptedStream >> *_pDatabase;
		EncryptedStream.f_Close();

		++mp_IVSalt.m_ExternalSalt;
		mp_IVSalt.m_InternalSalt = 0;
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
