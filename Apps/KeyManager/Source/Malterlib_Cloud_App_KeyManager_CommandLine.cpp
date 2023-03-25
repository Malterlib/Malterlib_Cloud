// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Cloud/KeyManagerServer>
#include <Mib/Cloud/KeyManagerDatabases/EncryptedFile>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/DistributedActorTrustManager>
#include <Mib/Concurrency/DistributedActorTrustManagerDatabases/JSONDirectory>
#include <Mib/Process/StdIn>
#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Concurrency/ActorFunctor>

#include "Malterlib_Cloud_App_KeyManager.h"

namespace NMib::NCloud::NKeyManager
{
	void CKeyManagerDaemonActor::fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine)
	{
		CDistributedAppActor::fp_BuildCommandLine(o_CommandLine);
		o_CommandLine.f_SetProgramDescription
			(
				"Malterlib Cloud Key Manager"
				, "Manages encryption keys for Malterlib cloud apps."
			)
		;

		auto DefaultSection = o_CommandLine.f_GetDefaultSection();

		DefaultSection.f_RegisterCommand
			(
				{
					"Names"_= {"--provide-password"}
					, "Description"_= "Provide a password for the key database to be able to start the key manager."
				}
				, [this](CEJSON const &_Parameters, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return g_Future <<= self(&CKeyManagerDaemonActor::f_ProvidePassword, _pCommandLine);
				}
			)
		;
		DefaultSection.f_RegisterCommand
			(
				{
					"Names"_= {"--precreate-keys"}
					, "Description"_= "Precreate keys of a certain size. Useful to allow backup of future keys not yet sent to a client."
					, "Options"_=
					{
						"KeySize?"_=
						{
							"Names"_= {"--key-size"}
							, "Description"_= "Set size in number of bits for the created keys."
							, "Default"_= 512
						}
						, "NumberOfKeys?"_=
						{
							"Names"_= {"--number-of-keys"}
							, "Description"_= "Precreate this number of keys."
							, "Default"_= 128
						}
					}
				}
				, [this](CEJSON const &_Parameters, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return g_Future <<= self(&CKeyManagerDaemonActor::f_PrecreateKeys, _Parameters["KeySize"].f_Integer(), _Parameters["NumberOfKeys"].f_Integer(), _pCommandLine);
				}
			)
		;
	}

	TCFuture<uint32> CKeyManagerDaemonActor::f_PrecreateKeys(uint32 _KeySize, uint32 _nKeys, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		if (!mp_ServerActor)
			co_return DErrorInstance("The key database has not yet been decrypted. Use --provide-key to decrypt it.");

		co_await mp_ServerActor(&CKeyManagerServer::f_PreCreateKeys, _KeySize, _nKeys);

		*_pCommandLine += "The server now has at least {} ({} bit) keys{\n}"_f << _nKeys << _KeySize;
		co_return 0;
	}

	TCFuture<uint32> CKeyManagerDaemonActor::f_ProvidePassword(NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		CStdInReaderPromptParams PasswordPrompt;
		PasswordPrompt.m_bPassword = true;
		PasswordPrompt.m_Prompt = "Type password for key database: ";

		CStrSecure Password = co_await _pCommandLine->f_ReadPrompt(PasswordPrompt);
		
		auto Result = co_await
			(
				g_Dispatch / [this, Password = fg_Move(Password)]() mutable -> TCFuture<void>
				{
					if (mp_bDatabaseDecrypted)
						co_return DErrorInstance("A correct password has already been provided");

					if (!mp_pProvidePasswordOnce)
					{
						mp_pProvidePasswordOnce = fg_Construct
							(
								g_ActorFunctor / [this](NStr::CStrSecure &&_Password) -> TCFuture<void>
								{
									TCPromise<void> Promise;

									CSecureByteVector Salt{(uint8 const *)"MiBKeyMa", 8};
									TCActor<CKeyManagerServerDatabase_EncryptedFile> DatabaseActor = fg_ConstructActor<CKeyManagerServerDatabase_EncryptedFile>
										(
											fg_Construct("Encrypted Key Manager Database Actor")
											, mp_State.m_RootDirectory + "/KeyDatabase.encrypted"
											, _Password
											, Salt
										)
									;

									CClock Clock;
									Clock.f_Start();

									DatabaseActor(&CKeyManagerServerDatabase_EncryptedFile::f_Initialize)
										> [this, Promise, DatabaseActor, Clock](TCAsyncResult<void> &&_Result)
										{
											if (!_Result)
											{
												// Delay reply to be same response time every time
												fg_Timeout(fg_Max(fp64(0.5) - Clock.f_GetTime(), fp64(0.01)))
													> [Promise, Result = fg_Move(_Result)]
													{
														Promise.f_SetException
															(
																DMibErrorInstance(fg_Format("Failed to initialize database: {}", Result.f_GetExceptionStr()))
															)
														;
													}
												;
												return;
											}
											mp_bDatabaseDecrypted = true;
											mp_DatabaseActor = DatabaseActor;

											fp_DatabaseDecrypted();
											Promise.f_SetResult();
										}
									;

									return Promise.f_MoveFuture();
								}
								, true
								, "Already processing a password. Try again later."
							)
						;
					}

					co_await (*mp_pProvidePasswordOnce)(fg_Move(Password));

					co_return {};
				}
			)
			.f_Wrap()
		;

		if (!Result)
		{
			DMibLogWithCategory(Mib/Cloud/KeyManager/Daemon, Error, "Provide password attempt failed: {}", Result.f_GetExceptionStr());
			co_return Result.f_GetException();
		}

		co_return 0;
	}
}
