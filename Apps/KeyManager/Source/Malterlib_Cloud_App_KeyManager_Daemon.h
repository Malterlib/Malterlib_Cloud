// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Cloud/KeyManagerServer>
#include <Mib/Cloud/KeyManagerDatabases/EncryptedFile>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/DistributedActorTrustManager>
#include <Mib/Concurrency/ActorCallOnce>

#include "Malterlib_Cloud_App_KeyManager.h"
#include "Malterlib_Cloud_App_KeyManager_JSONDatabase.h"

namespace NMib
{
	namespace NCloud
	{
		namespace NKeyManager
		{
			
			struct CKeyManagerDaemonActor : public CActor
			{
				struct CCommandLine : public ICCommandLine
				{
					CCommandLine(TCWeakActor<CKeyManagerDaemonActor> const &_Actor);
					
					TCContinuation<void> f_ProvidePassword(NStr::CStrSecure const &_Password) override;
					TCContinuation<CStr> f_GenerateTrustTicket() override;
					
				private:
					TCWeakActor<CKeyManagerDaemonActor> mp_Actor; 
				};
				
				CKeyManagerDaemonActor();
				~CKeyManagerDaemonActor();
				
				void f_Construct();

				TCContinuation<CStr> f_GenerateTrustTicket(NStr::CStr const &_FromHostID);
				TCContinuation<void> f_ProvidePassword(NStr::CStrSecure const &_Password, NStr::CStr const &_FromHostID);
				TCContinuation<void> f_Destroy();
				
			private:
				
				TCContinuation<void> fp_Initialize();
				TCContinuation<void> fp_SetupListen();
				
				TCContinuation<void> fp_CreateCommandLineTrust();
				TCContinuation<void> fp_SetupCommandLineListen();
				TCContinuation<void> fp_SetupCommandLineTrust();
				
				bool fp_HasCommandLineAccess(NStr::CStr const &_HostID);

				void fp_DatabaseDecrypted();

				TCActor<ICDistributedActorTrustManagerDatabase> mp_TrustManagerDatabase;
				TCActor<CDistributedActorTrustManager> mp_TrustManager;
				TCActor<CKeyManagerServer> mp_ServerActor;
				TCActor<CKeyManagerServerDatabase_EncryptedFile> mp_DatabaseActor;
				TCActor<CSeparateThreadActor> mp_FileOperationsActor;
				TCUniquePointer<TCActorCallOnce<void, NStr::CStr>> mp_pProvidePasswordOnce;
				TCDistributedActor<CCommandLine> mp_CommandLine;
				CDistributedActorPublication mp_CommandLinePublication;
				CJSONDatabase mp_StateDatabase;
				CJSONDatabase mp_ConfigDatabase;
				CDistributedActorTrustManager_Address mp_PrimaryListen;
				bool mp_bDatabaseDecrypted = false;
			};
		}		
	}
}
