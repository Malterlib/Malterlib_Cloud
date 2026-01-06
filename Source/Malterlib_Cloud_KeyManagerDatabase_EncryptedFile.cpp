// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Concurrency/ActorSequencerActor>
#include <Mib/Concurrency/LogError>
#include <Mib/Cryptography/SymmetricCrypto>

#include "Malterlib_Cloud_KeyManagerDatabase_EncryptedFile.h"

namespace NMib::NCloud
{
	struct CKeyManagerServerDatabase_EncryptedFile::CInternal : public NConcurrency::CActorInternal
	{
		CInternal(NStr::CStr const &_Path, NStr::CStrSecure const &_Password, NContainer::CSecureByteVector const &_Salt)
			: m_pAESContext
			(
				fg_Construct
				(
					NCryptography::CEncryptKeyIV::fs_GenerateKeyIV
					(
						_Password
						, _Salt
						, NCryptography::CKeyDerivationSettings_PKCS5_Deprecated{NCryptography::EDigestType_SHA256}
						, NCryptography::ECryptoType_AES_256_CBC
					)
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
			auto SequenceSubscription = co_await m_Sequencer.f_Sequence();
			auto BlockingActorCheckout = NConcurrency::fg_BlockingActor();
			co_return co_await
				(
					NConcurrency::g_Dispatch(BlockingActorCheckout) / [pAESContext = m_pAESContext, Path = m_Path]
					{
						if (!NFile::CFile::fs_FileExists(Path))
						{
							CDatabase Database;
							fsp_WriteDatabase(pAESContext, Database, Path);
						}
						else
						{
							NContainer::CByteVector FileData;
							NFile::CFile File;
							File.f_Open(Path, NFile::EFileOpen_Read|NFile::EFileOpen_ShareAll);
							FileData.f_SetLen(32);
							File.f_Read(FileData.f_GetArray(), FileData.f_GetLen());

							NContainer::CSecureByteVector PlainTextCheck;
							pAESContext->f_Decrypt(FileData.f_GetArray(), FileData.f_GetLen(), PlainTextCheck.f_GetArray(32));

							if (NMemory::fg_MemCmp(PlainTextCheck.f_GetArray(), (uint8 const *)mc_pPlainTextCheck, 32) != 0)
								DMibError("Password mismatch");
						}
					}
				)
			;
		}

		NConcurrency::TCFuture<void> f_ChangePassword(NStr::CStrSecure _Password, NContainer::CSecureByteVector _Salt)
		{
			m_pAESContext = fg_Construct
				(
					NCryptography::CEncryptKeyIV::fs_GenerateKeyIV
					(
						_Password
						, _Salt
						, NCryptography::CKeyDerivationSettings_PKCS5_Deprecated{NCryptography::EDigestType_SHA256}
						, NCryptography::ECryptoType_AES_256_CBC
					)
				)
			;

			co_return {};
		}

		NConcurrency::TCFuture<void> f_WriteDatabase(CDatabase _Database)
		{
			auto SequenceSubscription = co_await m_Sequencer.f_Sequence();
			auto BlockingActorCheckout = NConcurrency::fg_BlockingActor();
			co_return co_await
				(
					NConcurrency::g_Dispatch(BlockingActorCheckout) / [pAESContext = m_pAESContext, Database = fg_Move(_Database), Path = m_Path]
					{
						fsp_WriteDatabase(pAESContext, Database, Path);
					}
				)
			;
		}

		NConcurrency::TCFuture<CDatabase> f_ReadDatabase()
		{
			auto SequenceSubscription = co_await m_Sequencer.f_Sequence();
			auto BlockingActorCheckout = NConcurrency::fg_BlockingActor();
			co_return co_await
				(
					NConcurrency::g_Dispatch(BlockingActorCheckout) / [pAESContext = m_pAESContext, Path = m_Path]
					{
						NContainer::CByteVector EncryptedDatabase = NFile::CFile::fs_ReadFile(Path);
						NContainer::CSecureByteVector RawDatabase;
						pAESContext->f_Decrypt(EncryptedDatabase.f_GetArray(), EncryptedDatabase.f_GetLen(), RawDatabase.f_GetArray(EncryptedDatabase.f_GetLen()));

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
				)
			;
		}

		NStorage::TCSharedPointer<NCryptography::CEncryptAES> m_pAESContext;
		NStr::CStr const m_Path;
		static constexpr ch8 const *mc_pPlainTextCheck = "ABCDEFGHIJKLMNOPQRSTUVWXYZABCDEF";
		NConcurrency::CSequencer m_Sequencer{"Key manager database"};

	private:
		static void fsp_WriteDatabase(NStorage::TCSharedPointer<NCryptography::CEncryptAES> const &_pAESContext, CDatabase const &_Database, NStr::CStr const &_Path)
		{
			NStream::CBinaryStreamMemory<NStream::CBinaryStreamDefault, NContainer::CSecureByteVector> Stream;
			Stream.f_FeedBytes(mc_pPlainTextCheck, 32);
			Stream << _Database;
			NStream::fg_PadAlignStream(Stream, 32);

			NContainer::CSecureByteVector RawDatabase = Stream.f_MoveVector();
			NContainer::CByteVector EncryptedDatabase;
			_pAESContext->f_Encrypt(RawDatabase.f_GetArray(), RawDatabase.f_GetLen(), EncryptedDatabase.f_GetArray(RawDatabase.f_GetLen()));

			NFile::CFile::fs_CreateDirectory(NFile::CFile::fs_GetPath(_Path));

			{
				NFile::CFile OutFile;
				auto Attributes = NFile::EFileAttrib_UserWrite | NFile::EFileAttrib_UserRead | NFile::CFile::fs_GetValidAttributes();
				OutFile.f_Open(_Path + ".tmp", NFile::EFileOpen_Write | NFile::EFileOpen_ShareAll, Attributes);
				OutFile.f_Write(EncryptedDatabase.f_GetArray(), EncryptedDatabase.f_GetLen());
			}

			NFile::CFile::fs_AtomicReplaceFile(_Path + ".tmp", _Path);
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

	NConcurrency::TCFuture<void> CKeyManagerServerDatabase_EncryptedFile::fp_Destroy()
	{
		auto &Internal = *mp_pInternal;

		co_await fg_Move(Internal.m_Sequencer).f_Destroy().f_Wrap() > NConcurrency::fg_LogError("KeyManagerServerDatabase", "Failed to destroy sequencer");

		co_return {};
	}

	NConcurrency::TCFuture<void> CKeyManagerServerDatabase_EncryptedFile::f_Initialize()
	{
		return mp_pInternal->f_Initialize();
	}

	NConcurrency::TCFuture<void> CKeyManagerServerDatabase_EncryptedFile::f_ChangePassword(NStr::CStrSecure _Password, NContainer::CSecureByteVector _Salt)
	{
		return mp_pInternal->f_ChangePassword(fg_Move(_Password), fg_Move(_Salt));
	}

	NConcurrency::TCFuture<void> CKeyManagerServerDatabase_EncryptedFile::f_WriteDatabase(CDatabase _Database)
	{
		return mp_pInternal->f_WriteDatabase(fg_Move(_Database));
	}

	NConcurrency::TCFuture<ICKeyManagerServerDatabase::CDatabase> CKeyManagerServerDatabase_EncryptedFile::f_ReadDatabase()
	{
		return mp_pInternal->f_ReadDatabase();
	}
}
