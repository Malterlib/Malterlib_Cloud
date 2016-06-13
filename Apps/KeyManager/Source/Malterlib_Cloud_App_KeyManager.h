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
				TCContinuation<void> f_Destroy() override;
				
			private:
				TCContinuation<void> fp_DestroyLocal();
				TCContinuation<void> fp_StartApp() override;
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
