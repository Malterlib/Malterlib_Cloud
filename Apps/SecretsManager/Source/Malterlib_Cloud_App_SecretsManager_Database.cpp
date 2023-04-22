// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Cryptography/EncryptedStream>
#include <Mib/Concurrency/LogError>

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

	TCFuture<void> CSecretsManagerServerDatabase::fp_Destroy()
	{
		CLogError LogError("Mib/Cloud/SecretsManager");

		if (!mp_pPendingWrite)
			co_return {};

		co_await mp_PendingWritePromises.f_Insert().f_Future().f_Wrap() > LogError.f_Warning("Failed to destroy secret manager database write promises");

		co_return {};
	}

	CSecureByteVector CSecretsManagerServerDatabase::fsp_ComputeSalt(CSecretsDatabaseIV const &_Salt)
	{
		CBinaryStreamMemory<CBinaryStreamDefault, CSecureByteVector> Stream;
		Stream << _Salt.m_InternalSalt;
		Stream << _Salt.m_ExternalSalt;
		return Stream.f_MoveVector();
	}

	TCFuture<void> CSecretsManagerServerDatabase::f_Initialize()
	{
		return TCFuture<void>::fs_RunProtected<CException>() / [&]
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
	
	TCFuture<void> CSecretsManagerServerDatabase::f_WriteDatabase(CSecretsDatabase &&_Database)
	{
		if (!mp_pPendingWrite)
		{
			g_Dispatch / [this]
				{
					auto WriteResult = TCFuture<void>::fs_RunProtectedAsyncResult<CException>() / [&]
						{
							fp_WriteDatabase(*mp_pPendingWrite);
						}
					;

					mp_pPendingWrite.f_Clear();

					for (auto &Promise : mp_PendingWritePromises)
						Promise.f_SetResult(WriteResult);

					mp_PendingWritePromises.f_Clear();
				}
				> fg_DiscardResult();
			;

			mp_pPendingWrite = fg_Construct(fg_Move(_Database));
		}
		else
			*mp_pPendingWrite = fg_Move(_Database);

		return mp_PendingWritePromises.f_Insert().f_Future();
	}
	
	TCFuture<CSecretsDatabase> CSecretsManagerServerDatabase::f_ReadDatabase()
	{
		return TCFuture<CSecretsDatabase>::fs_RunProtected<CException>() / [&]
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

			TCBinaryStream_Encrypted<CBinaryStream *> EncryptedStream{KeyExpansion.f_GetKeyIV(), EDigestType_SHA512, KeyExpansion.f_GetHMACKey(EDigestType_SHA512)};

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

		TCBinaryStream_Encrypted<CBinaryStream *> EncryptedStream{KeyExpansion.f_GetKeyIV(), EDigestType_SHA512, KeyExpansion.f_GetHMACKey(EDigestType_SHA512)};
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
		o_Str += "Username: {} URL: {} Expires: {} Notes: {} Metadata: {} Created: {} Modified: {} SemanticID: {} Tags: {} Immutable: {}"_f
			<< m_UserName
			<< m_URL
			<< m_Expires
			<< m_Notes
			<< m_Metadata
			<< m_Created
			<< m_Modified
			<< m_SemanticID
			<< m_Tags
			<< m_bImmutable
		;
	}
}
