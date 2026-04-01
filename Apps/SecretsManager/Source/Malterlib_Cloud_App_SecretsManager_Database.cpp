// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Cryptography/EncryptedStream>
#include <Mib/Concurrency/LogError>

#include "Malterlib_Cloud_App_SecretsManager_Database.h"

namespace NMib::NCloud::NSecretsManager
{
	CSecretsManagerServerDatabase::CSecretsManagerServerDatabase(CStr const &_Path, CSecureByteVector const &_Key)
		: mp_Path{_Path}
		, mp_EncryptionState{.m_Key = _Key}
	{
	}

	CSecretsManagerServerDatabase::~CSecretsManagerServerDatabase()
	{
	}

	TCFuture<void> CSecretsManagerServerDatabase::fp_Destroy()
	{
		CLogError LogError("Mib/Cloud/SecretsManager");

		if (mp_pPendingWrite)
			co_await mp_PendingWritePromises.f_Insert().f_Future().f_Wrap() > LogError.f_Warning("Failed to destroy secret manager database write promises");

		co_await fg_Move(mp_Sequencer).f_Destroy().f_Wrap() > LogError.f_Warning("Failed to destroy secret manager database sequencer");;

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
		auto SequenceSubscription = co_await mp_Sequencer.f_Sequence();
		auto BlockingActorCheckout = fg_BlockingActor();

		mp_EncryptionState.m_IVSalt = co_await
			(
				g_Dispatch(BlockingActorCheckout) / [Path = mp_Path, State = mp_EncryptionState]
				{
					CSecretsDatabaseIV NewSaltIV;
					if (!CFile::fs_FileExists(Path))
					{
						DMibLogWithCategory(Mib/Cloud/SecretsManager, Info, "No current database found. Creating new");
						CSecretsDatabase Database;
						NewSaltIV = fsp_WriteDatabase(Database, Path, State);
					}
					else
						NewSaltIV = fsp_ReadDatabase(nullptr, Path, State);

					return NewSaltIV;
				}
			)
		;

		co_return {};
	}

	TCFuture<void> CSecretsManagerServerDatabase::f_WriteDatabase(CSecretsDatabase _Database)
	{
		if (!mp_pPendingWrite)
		{
			g_Dispatch / [this]() -> TCFuture<void>
				{
					auto SequenceSubscription = co_await mp_Sequencer.f_Sequence();
					auto Promises = fg_Move(mp_PendingWritePromises);

					TCAsyncResult<CSecretsDatabaseIV> WriteResult;
					{
						auto BlockingActorCheckout = fg_BlockingActor();
						WriteResult = co_await
							(
								g_Dispatch(BlockingActorCheckout) / [pPendingWrite = fg_Move(mp_pPendingWrite), Path = mp_Path, EncryptionState = mp_EncryptionState]
								{
									return fsp_WriteDatabase(*pPendingWrite, Path, EncryptionState);
								}
							).f_Wrap();
						;

					}

					if (WriteResult)
					{
						mp_EncryptionState.m_IVSalt = *WriteResult;
						for (auto &Promise : Promises)
							Promise.f_SetResult();
					}
					else
					{
						for (auto &Promise : Promises)
							Promise.f_SetException(WriteResult);
					}

					co_return {};
				}
				> fg_LogError("Mib/Cloud/SecretsManager", "Failed to write database");
			;

			mp_pPendingWrite = fg_Construct(fg_Move(_Database));
		}
		else
			*mp_pPendingWrite = fg_Move(_Database);

		co_return co_await mp_PendingWritePromises.f_Insert().f_Future();
	}

	TCFuture<CSecretsDatabase> CSecretsManagerServerDatabase::f_ReadDatabase()
	{
		auto SequenceSubscription = co_await mp_Sequencer.f_Sequence();
		auto BlockingActorCheckout = fg_BlockingActor();
		auto [Database, NewSaltIV] = co_await
			(
				g_Dispatch(BlockingActorCheckout) / [Path = mp_Path, EncryptionState = mp_EncryptionState]
				{
					CSecretsDatabase Database;
					auto NewSaltIV = fsp_ReadDatabase(&Database, Path, EncryptionState);
					return fg_Tuple(fg_Move(Database), NewSaltIV);
				}
			)
		;

		mp_EncryptionState.m_IVSalt = fg_Move(NewSaltIV);

		co_return fg_Move(Database);
	}

	CSecretsDatabaseIV CSecretsManagerServerDatabase::fsp_WriteDatabase(CSecretsDatabase const &_Database, CStr const &_Path, CEncryptionState const &_State)
	{
		{
			auto Attributes = EFileAttrib_UserWrite | EFileAttrib_UserRead | CFile::fs_GetValidAttributes();
			TCBinaryStreamFile<> BaseStream;
			CFile::fs_CreateDirectory(CFile::fs_GetPath(_Path));
			BaseStream.f_Open(_Path + ".tmp", NFile::EFileOpen_Write, Attributes);
			BaseStream << _State.m_IVSalt;
			auto BasePosition = BaseStream.f_GetPosition();

			CBinaryStreamSubStream<> SubStream;
			SubStream.f_Open(&BaseStream, BasePosition);

			CSecureByteVector Salt{fsp_ComputeSalt(_State.m_IVSalt)};
			CKeyExpansion KeyExpansion{_State.m_Key, Salt};

			TCBinaryStream_Encrypted<CBinaryStream *> EncryptedStream{KeyExpansion.f_GetKeyIV(), EDigestType_SHA512, KeyExpansion.f_GetHMACKey(EDigestType_SHA512)};

			auto CheckBuffer = KeyExpansion.f_GetKey("SecretsManagerCheck", 32);

			EncryptedStream.f_Open(&SubStream, NFile::EFileOpen_Write);
			EncryptedStream.f_FeedBytes(CheckBuffer.f_GetArray(), 32u);
			EncryptedStream << _Database;
		}

		CFile::fs_AtomicReplaceFile(_Path + ".tmp", _Path);

		auto NewSaltIV = _State.m_IVSalt;

		++NewSaltIV.m_InternalSalt;

		return NewSaltIV;
	}

	CSecretsDatabaseIV CSecretsManagerServerDatabase::fsp_ReadDatabase(CSecretsDatabase *_pDatabase, CStr const &_Path, CEncryptionState const &_State)
	{
		CSecretsDatabaseIV SaltIV;

		TCBinaryStreamFile<> BaseStream;
		BaseStream.f_Open(_Path, EFileOpen_Read|EFileOpen_ShareAll);
		BaseStream >> SaltIV;
		auto BasePosition = BaseStream.f_GetPosition();

		CBinaryStreamSubStream<> SubStream;
		SubStream.f_Open(&BaseStream, BasePosition, BaseStream.f_GetLength() - BasePosition);

		CSecureByteVector Salt{fsp_ComputeSalt(SaltIV)};
		CKeyExpansion KeyExpansion{_State.m_Key, Salt};

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

		++SaltIV.m_ExternalSalt;
		SaltIV.m_InternalSalt = 0;

		return SaltIV;
	}

	void NSecretsManager::CSecretPropertiesInternal::f_Format(CStr &o_Str) const
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
