// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedApp>
#include <Mib/Daemon/Daemon>
#include <Mib/Cloud/KeyManagerServer>
#include <Mib/Cloud/KeyManagerDatabases/EncryptedFile>

namespace NMib
{
	namespace NCloud
	{
		namespace NKeyManager
		{
			struct CKeyManagerDaemonActor : public CDistributedAppActor
			{
				CKeyManagerDaemonActor();
				~CKeyManagerDaemonActor();
				
				TCContinuation<CDistributedAppCommandLineResults> f_ProvidePassword(NStr::CStrSecure const &_Password);
				TCContinuation<CDistributedAppCommandLineResults> f_PrecreateKeys(uint32 _KeySize, uint32 _nKeys);
				
			private:
				TCContinuation<void> fp_StartApp() override;
				TCContinuation<void> fp_StopApp() override;
				void fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine) override; 
				
				TCContinuation<void> fp_Initialize();

				void fp_DatabaseDecrypted();

				TCActor<CKeyManagerServer> mp_ServerActor;
				TCActor<CKeyManagerServerDatabase_EncryptedFile> mp_DatabaseActor;
				TCUniquePointer<TCActorCallOnce<void, NStr::CStr>> mp_pProvidePasswordOnce;
				bool mp_bDatabaseDecrypted = false;
			};
		}		
	}
}
