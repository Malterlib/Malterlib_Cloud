// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/CommandLine/TableRenderer>
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
					"Names"_o= {"--provide-password"}
					, "Description"_o= "Provide a password for the key database to be able to start the key manager."
				}
				, [this](CEJSONSorted const &_Parameters, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return g_Future <<= self(&CKeyManagerDaemonActor::f_ProvidePassword, _pCommandLine);
				}
			)
		;
		DefaultSection.f_RegisterCommand
			(
				{
					"Names"_o= {"--precreate-keys"}
					, "Description"_o= "Pre-create keys of a certain size. Useful to allow backup of future keys not yet sent to a client."
					, "Options"_o=
					{
						"KeySize?"_o=
						{
							"Names"_o= {"--key-size"}
							, "Description"_o= "Set size in number of bits for the created keys."
							, "Default"_o= 512
						}
						, "NumberOfKeys?"_o=
						{
							"Names"_o= {"--number-of-keys"}
							, "Description"_o= "Pre-create this number of keys."
							, "Default"_o= 128
						}
					}
				}
				, [this](CEJSONSorted const &_Parameters, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return g_Future <<= self(&CKeyManagerDaemonActor::f_PreCreateKeys, _Parameters["KeySize"].f_Integer(), _Parameters["NumberOfKeys"].f_Integer(), _pCommandLine);
				}
			)
		;
		DefaultSection.f_RegisterCommand
			(
				{
					"Names"_o= {"--verified-host-remove"}
					, "Description"_o= "Remove hosts that should no longer be part of the server cluster."
					, "Parameters"_o=
					{
						"HostIDs"_o=
						{
							"Type"_o= CEJSONOrdered({""})
							, "Description"_o= "The host IDs that should be removed."
						}
					}
				}
				, [this](CEJSONSorted const &_Parameters, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return g_Future <<= self(&CKeyManagerDaemonActor::f_RemoveVerifiedHosts, TCSet<CStr>::fs_FromContainer(_Parameters["HostIDs"].f_StringArray()), _pCommandLine);
				}
			)
		;
		DefaultSection.f_RegisterCommand
			(
				{
					"Names"_o= {"--verified-host-list"}
					, "Description"_o= "Display all verified hosts in database."
					, "Options"_o=
					{
						CTableRenderHelper::fs_OutputTypeOption()
					}
				}
				, [this](CEJSONSorted const &_Parameters, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return g_Future <<= self(&CKeyManagerDaemonActor::f_ListVerifiedHosts, _Parameters, _pCommandLine);
				}
			)
		;
		DefaultSection.f_RegisterCommand
			(
				{
					"Names"_o= {"--key-list"}
					, "Description"_o= "List all keys."
					, "Options"_o=
					{
						CTableRenderHelper::fs_OutputTypeOption()
					}
				}
				, [this](CEJSONSorted const &_Parameters, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return g_Future <<= self(&CKeyManagerDaemonActor::f_ListKeys, _Parameters, _pCommandLine);
				}
			)
		;
		DefaultSection.f_RegisterCommand
			(
				{
					"Names"_o= {"--key-copy"}
					, "Description"_o= "Copy key from one ID to another ID."
					, "Options"_o=
					{
						"FromHostID"_o=
						{
							"Names"_o= {"--from-host-id"}
							, "Type"_o= ""
							, "Description"_o= "The host ID to copy from."
						}
						, "FromKeyID"_o=
						{
							"Names"_o= {"--from-key-id"}
							, "Type"_o= ""
							, "Description"_o= "The key ID to copy from."
						}
						, "ToHostID"_o=
						{
							"Names"_o= {"--to-host-id"}
							, "Type"_o= ""
							, "Description"_o= "The host ID to copy to."
						}
						, "ToKeyID"_o=
						{
							"Names"_o= {"--to-key-id"}
							, "Type"_o= ""
							, "Description"_o= "The key ID to copy to."
						}
					}
				}
				, [this](CEJSONSorted const &_Parameters, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return g_Future <<= self(&CKeyManagerDaemonActor::f_CopyKey, _Parameters, _pCommandLine);
				}
			)
		;
	}

	TCFuture<uint32> CKeyManagerDaemonActor::f_PreCreateKeys(uint32 _KeySize, uint32 _nKeys, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		if (!mp_ServerActor)
			co_return DErrorInstance("The key database has not yet been decrypted. Use --provide-key to decrypt it.");

		co_await mp_ServerActor(&CKeyManagerServer::f_PreCreateKeys, _KeySize, _nKeys);

		*_pCommandLine += "The server now has at least {} ({} bit) keys{\n}"_f << _nKeys << _KeySize;
		co_return 0;
	}

	TCFuture<uint32> CKeyManagerDaemonActor::f_RemoveVerifiedHosts(NContainer::TCSet<CStr> &&_HostIDs, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		if (!mp_ServerActor)
			co_return DErrorInstance("The key database has not yet been decrypted. Use --provide-key to decrypt it.");

		auto RemovedHostIDs = co_await mp_ServerActor(&CKeyManagerServer::f_RemoveVerifiedHosts, _HostIDs);

		*_pCommandLine %= "Removed {} hosts: {vs}\n"_f  << RemovedHostIDs.f_GetLen() << RemovedHostIDs;

		co_return 0;
	}

	TCFuture<uint32> CKeyManagerDaemonActor::f_ListVerifiedHosts(CEJSONSorted const &_Parameters, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		if (!mp_ServerActor)
			co_return DErrorInstance("The key database has not yet been decrypted. Use --provide-key to decrypt it.");

		auto Hosts = co_await mp_ServerActor(&CKeyManagerServer::f_GetAllVerifiedHosts);

		auto AnsiEncoding = _pCommandLine->f_AnsiEncoding();
		CTableRenderHelper TableRenderer = _pCommandLine->f_TableRenderer();
		CTableRenderHelper::CColumnHelper Columns(0);
		Columns.f_AddHeading("Host ID", 0);
		Columns.f_AddHeading("Notes", 0);

		auto ThisHostID = co_await mp_State.m_TrustManager(&CDistributedActorTrustManager::f_GetHostID);

		TCActorResultMap<CStr, CStr> FriendlyNamesResults;
		for (auto &HostID : Hosts)
			mp_State.m_TrustManager(&CDistributedActorTrustManager::f_GetHostFriendlyName, HostID) > FriendlyNamesResults.f_AddResult(HostID);

		auto FriendlyNames = co_await FriendlyNamesResults.f_GetUnwrappedResults();

		TableRenderer.f_AddHeadings(&Columns);

		for (auto &HostID : Hosts)
		{
			CHostInfo HostInfo(HostID, FriendlyNames[HostID]);

			CStr Notes;
			if (HostID == ThisHostID)
				Notes = "Host ID for this server";

			TableRenderer.f_AddRow(HostInfo.f_GetDescColored(AnsiEncoding.f_Flags()), Notes);
		}

		TableRenderer.f_Output(_Parameters);

		co_return 0;
	}

	TCFuture<uint32> CKeyManagerDaemonActor::f_ListKeys(CEJSONSorted const &_Parameters, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		if (!mp_ServerActor)
			co_return DErrorInstance("The key database has not yet been decrypted. Use --provide-key to decrypt it.");

		auto Keys = co_await mp_ServerActor(&CKeyManagerServer::f_GetKeys);

		TCSet<CStr> Hosts;
		for (auto &Key : Keys)
		{
			Hosts[Key.m_Key.m_HostID];
			Hosts += Key.m_VerifiedOnServers;
		}

		auto AnsiEncoding = _pCommandLine->f_AnsiEncoding();
		CTableRenderHelper TableRenderer = _pCommandLine->f_TableRenderer();
		CTableRenderHelper::CColumnHelper Columns(0);
		Columns.f_AddHeading("Host ID", 0);
		Columns.f_AddHeading("Key ID", 0);
		Columns.f_AddHeading("Verified on servers", 0);

		TCActorResultMap<CStr, CStr> FriendlyNamesResults;
		for (auto &HostID : Hosts)
			mp_State.m_TrustManager(&CDistributedActorTrustManager::f_GetHostFriendlyName, HostID) > FriendlyNamesResults.f_AddResult(HostID);

		auto FriendlyNames = co_await FriendlyNamesResults.f_GetUnwrappedResults();

		TableRenderer.f_AddHeadings(&Columns);

		for (auto &Key : Keys)
		{
			CHostInfo HostInfo(Key.m_Key.m_HostID, FriendlyNames[Key.m_Key.m_HostID]);

			CStr VerifiedOn;

			for (auto &HostID : Key.m_VerifiedOnServers)
				fg_AddStrSep(VerifiedOn, CHostInfo(HostID, FriendlyNames[HostID]).f_GetDescColored(AnsiEncoding.f_Flags()), "\n");

			TableRenderer.f_AddRow(HostInfo.f_GetDescColored(AnsiEncoding.f_Flags()), Key.m_Key.m_KeyID, VerifiedOn);
		}

		TableRenderer.f_Output(_Parameters);

		co_return 0;
	}

	TCFuture<uint32> CKeyManagerDaemonActor::f_CopyKey(CEJSONSorted const &_Parameters, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		if (!mp_ServerActor)
			co_return DErrorInstance("The key database has not yet been decrypted. Use --provide-key to decrypt it.");

		CKeyManagerServer::CKeyManagerKeyID FromID{.m_HostID = _Parameters["FromHostID"].f_String(), .m_KeyID = _Parameters["FromKeyID"].f_String()};
		CKeyManagerServer::CKeyManagerKeyID ToID{.m_HostID = _Parameters["ToHostID"].f_String(), .m_KeyID = _Parameters["ToKeyID"].f_String()};

		co_await mp_ServerActor(&CKeyManagerServer::f_CopyKey, FromID, ToID);

		co_return {};
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

									auto Result = co_await DatabaseActor(&CKeyManagerServerDatabase_EncryptedFile::f_Initialize).f_Wrap();

									if (!Result)
									{
										// Delay reply to be same response time every time
										co_await fg_Timeout(fg_Max(fp64(0.5) - Clock.f_GetTime(), fp64(0.01)));
										co_return DMibErrorInstance("Failed to initialize database: {}"_f << Result.f_GetExceptionStr());
									}
									mp_bDatabaseDecrypted = true;
									mp_DatabaseActor = fg_Move(DatabaseActor);

									co_await fp_DatabaseDecrypted();

									co_return {};
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
