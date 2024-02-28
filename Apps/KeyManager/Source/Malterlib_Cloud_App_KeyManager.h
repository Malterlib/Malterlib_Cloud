// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedApp>
#include <Mib/Daemon/Daemon>
#include <Mib/Cloud/KeyManagerServer>
#include <Mib/Cloud/KeyManagerDatabases/EncryptedFile>

namespace NMib::NCloud::NKeyManager
{
	struct CKeyManagerDaemonActor : public CDistributedAppActor
	{
		CKeyManagerDaemonActor();
		~CKeyManagerDaemonActor();

		TCFuture<uint32> f_ProvidePassword(NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCFuture<uint32> f_PreCreateKeys(uint32 _KeySize, uint32 _nKeys, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCFuture<uint32> f_RemoveVerifiedHosts(TCSet<CStr> &&_HostIDs, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCFuture<uint32> f_ListVerifiedHosts(CEJSONSorted const &_Parameters, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCFuture<uint32> f_ListKeys(CEJSONSorted const &_Parameters, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCFuture<uint32> f_CopyKey(CEJSONSorted const &_Parameters, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);

	private:
		TCFuture<void> fp_StartApp(NEncoding::CEJSONSorted const &_Params) override;
		TCFuture<void> fp_StopApp() override;
		void fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine) override;

		TCFuture<void> fp_DatabaseDecrypted();

		TCActor<CKeyManagerServer> mp_ServerActor;
		TCActor<CKeyManagerServerDatabase_EncryptedFile> mp_DatabaseActor;
		TCUniquePointer<TCActorCallOnce<void, NStr::CStrSecure &&>> mp_pProvidePasswordOnce;
		bool mp_bDatabaseDecrypted = false;
	};
}
