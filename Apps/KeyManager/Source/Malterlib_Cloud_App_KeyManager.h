// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

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

		TCFuture<uint32> f_ProvidePassword(NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> f_ChangePassword(NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> f_PreCreateKeys(uint32 _KeySize, uint32 _nKeys, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> f_ListPreCreatedKeys(CEJsonSorted const _Parameters, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> f_RemovePreCreatedKeys(CEJsonSorted const _Parameters, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> f_RemoveVerifiedHosts(TCSet<CStr> _HostIDs, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> f_ListVerifiedHosts(CEJsonSorted const _Parameters, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> f_ListKeys(CEJsonSorted const _Parameters, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> f_CopyKey(CEJsonSorted const _Parameters, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);

	private:
		TCFuture<void> fp_StartApp(NEncoding::CEJsonSorted const _Params) override;
		TCFuture<void> fp_StopApp() override;
		void fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine) override;

		TCFuture<void> fp_DatabaseDecrypted();
		TCFuture<void> fp_SetPasswordStatus(CDistributedAppSensorReporter::CStatus _Status);

		TCActor<CKeyManagerServer> mp_ServerActor;
		TCActor<CKeyManagerServerDatabase_EncryptedFile> mp_DatabaseActor;
		TCUniquePointer<TCActorCallOnce<void, NStr::CStrSecure>> mp_pProvidePasswordOnce;
		TCOptional<CDistributedAppSensorReporter::CSensorReporter> mp_PasswordStatusReporter;
		bool mp_bDatabaseDecrypted = false;
	};
}
