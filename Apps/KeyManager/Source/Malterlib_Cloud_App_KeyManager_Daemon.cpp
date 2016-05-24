// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Cloud/KeyManagerServer>
#include <Mib/Cloud/KeyManagerDatabases/EncryptedFile>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/DistributedActorTrustManager>
#include <Mib/Concurrency/DistributedActorTrustManagerDatabases/JSONDirectory>

#include "Malterlib_Cloud_App_KeyManager.h"
#include "Malterlib_Cloud_App_KeyManager_Daemon.h"

namespace NMib
{
	namespace NCloud
	{
		namespace NKeyManager
		{
			extern ch8 const *g_pLocalListenAddress;
			CKeyManagerDaemonActor::CKeyManagerDaemonActor()
				: mp_StateDatabase(CFile::fs_GetProgramDirectory() + "/ProgramState.json")
				, mp_ConfigDatabase(CFile::fs_GetProgramDirectory() + "/Config.json")
			{
				fg_GetSys()->f_SetDefaultLogFileName("KeyManager.log");
				fg_GetSys()->f_SetDefaultLogFileDirectory(CFile::fs_GetProgramDirectory() + "/Log");
			}
			
			CKeyManagerDaemonActor::~CKeyManagerDaemonActor()
			{
			}

			TCContinuation<void> CKeyManagerDaemonActor::fp_SetupListen()
			{
				DMibLogWithCategory(Mib/Cloud/KeyManager/Daemon, Info, "Setting up listen config");
				TCContinuation<void> Continuation;
				
				TCSet<CDistributedActorTrustManager_Address> WantedListens;
				
				auto const *pListen = mp_ConfigDatabase.m_Data.f_GetMember("Listen", EJSONType_Array);
				if (!pListen)
					return DMibErrorInstance("Missing 'Listen' array section in Config.json");
				
				CDistributedActorTrustManager_Address LocalListen;
				LocalListen.m_URL = g_pLocalListenAddress; 
				
				bool bFirst = true; 
				for (auto &Object : pListen->f_Array())
				{
					if (!Object.f_IsObject())
						return DMibErrorInstance("Invalid listen entry. Entry need to be an object with 'Address' and optionally 'PreferType' specified");
						
					CDistributedActorTrustManager_Address Address;
					if (auto pAddress = Object.f_GetMember("Address"))
					{
						if (pAddress->f_Type() != EJSONType_String)
							return DMibErrorInstance("Listen 'Address' has wrong type, only string is supported");
						if (!Address.m_URL.f_Decode(pAddress->f_String()))
							return DMibErrorInstance(fg_Format("Invalid listen URL '{}'", pAddress->f_String()));
						if (Address.m_URL.f_GetScheme() != "wss")
							return DMibErrorInstance(fg_Format("Invalid scheme in URL '{}': Only wss is currently supported", pAddress->f_String()));
					}
					else
						return DMibErrorInstance("Missing 'Address' for listen entry");

					if (auto pPreferType = Object.f_GetMember("PreferType"))
					{
						if (pPreferType->f_Type() != EJSONType_String)
							return DMibErrorInstance("Listen 'PreferType' has wrong type, only string is supported");
						if (pPreferType->f_String() == "None")
							Address.m_PreferType = ENetAddressType_None;
						else if (pPreferType->f_String() == "TCPv4")
							Address.m_PreferType = ENetAddressType_TCPv4;
						else if (pPreferType->f_String() == "TCPv6")
							Address.m_PreferType = ENetAddressType_TCPv6;
						else
							return DMibErrorInstance("Listen 'PreferType' is invalid, only None, TCPV4 or TCPv6 are supported");
					}
					
					if (bFirst)
						mp_PrimaryListen = Address;
					
					if (Address == LocalListen)
						return DMibErrorInstance(fg_Format("'{}' cannot be used as listen address, it is reserved for local command line communication", g_pLocalListenAddress));
					WantedListens[Address];
				}
				
				if (WantedListens.f_IsEmpty())
					return DMibErrorInstance("At least one Listen entry needs to be in Config.json");
				
				WantedListens[LocalListen];
				
				mp_TrustManager(&CDistributedActorTrustManager::f_EnumListens) 
					> Continuation % "Failed to enum current listen" / [this, Continuation, WantedListens](NContainer::TCSet<CDistributedActorTrustManager_Address> &&_Listens)
					{
						TCActorResultVector<void> ChangesResults;
						bool bChanged = false;
						for (auto const &CurrentListen : _Listens)
						{
							if (WantedListens.f_FindEqual(CurrentListen))
								continue;
							mp_TrustManager(&CDistributedActorTrustManager::f_RemoveListen, CurrentListen) > ChangesResults.f_AddResult();
							DMibLogWithCategory(Mib/Cloud/KeyManager/Daemon, Info, "Removing listen config {}", CurrentListen.m_URL.f_Encode());
							bChanged = true;
						}
						for (auto const &WantedListen : WantedListens)
						{
							if (_Listens.f_FindEqual(WantedListen))
								continue;
							mp_TrustManager(&CDistributedActorTrustManager::f_AddListen, WantedListen) > ChangesResults.f_AddResult();
							DMibLogWithCategory(Mib/Cloud/KeyManager/Daemon, Info, "Adding listen config {}", WantedListen.m_URL.f_Encode());
							bChanged = true;
						}
						if (!bChanged)
						{
							DMibLogWithCategory(Mib/Cloud/KeyManager/Daemon, Info, "No listen config changes needed");
							Continuation.f_SetResult();
							return;
						}
						ChangesResults.f_GetResults() > [this, Continuation](TCAsyncResult<TCVector<TCAsyncResult<void>>> &&_Results)
							{
								if (!fg_CombineResults(Continuation, fg_Move(_Results)))
									return;
								
								DMibLogWithCategory(Mib/Cloud/KeyManager/Daemon, Info, "Finished changing listen config");
								Continuation.f_SetResult();
							}
						;
					}
				;
				
				return Continuation;
			}
			

			TCContinuation<void> CKeyManagerDaemonActor::fp_Initialize()
			{
				TCContinuation<void> Continuation;
				DMibLogWithCategory(Mib/Cloud/KeyManager/Daemon, Info, "Loading config file and state");
				
				mp_StateDatabase.f_Load()
					+ mp_ConfigDatabase.f_Load()
					> Continuation / [this, Continuation]()
					{
						DMibLogWithCategory(Mib/Cloud/KeyManager/Daemon, Info, "Initializing trust manager");
						mp_TrustManager = fg_ConstructActor<CDistributedActorTrustManager>(mp_TrustManagerDatabase);
						mp_TrustManager(&CDistributedActorTrustManager::f_Initialize)
							> Continuation % "Failed to initialize trust manager" / [this, Continuation]()
							{
								fg_ThisActor(this)(&CKeyManagerDaemonActor::fp_SetupListen) 
									> Continuation % "Failed to setup listen config" / [this, Continuation]()
									{
										fg_ThisActor(this)(&CKeyManagerDaemonActor::fp_SetupCommandLineTrust) 
											> Continuation % "Failed to setup commmand line trust" / [this, Continuation]()
											{
												Continuation.f_SetResult();
											}
										;
									}
								;
								
							}
						;
					}
				;
				
				return Continuation;				
			}

			void CKeyManagerDaemonActor::f_Construct()
			{
				mp_FileOperationsActor = fg_ConstructActor<CSeparateThreadActor>(fg_Construct("Key manager daemon file access"));
				mp_TrustManagerDatabase = fg_ConstructActor<CDistributedActorTrustManagerDatabase_JSONDirectory>
					(
						NFile::CFile::fs_GetProgramDirectory() + "/TrustDatabase"
					)
				;
				
				fg_ThisActor(this)(&CKeyManagerDaemonActor::fp_Initialize) > [this](TCAsyncResult<void> &&_Result)
					{
						if (!_Result)
							DMibLogWithCategory(Mib/Cloud/KeyManager/Daemon, Error, "Failed to initialize: {}", _Result.f_GetExceptionStr());
						else
							DMibLogWithCategory(Mib/Cloud/KeyManager/Daemon, Info, "Intitialization finished, now waiting for database password to be provided");
					}
				;
				
			}
			
			void CKeyManagerDaemonActor::fp_DatabaseDecrypted()
			{
				DMibLogWithCategory(Mib/Cloud/KeyManager/Daemon, Info, "Password provided, starting up key manager");
				CKeyManagerServerConfig Config;
				Config.m_DatabaseActor = mp_DatabaseActor;
				mp_ServerActor = fg_ConstructActor<CKeyManagerServer>(Config);
			}
				
			TCContinuation<void> CKeyManagerDaemonActor::f_Destroy()
			{
				TCSharedPointer<CCanDestroyTracker> pCanDestroy = fg_Construct();
				
				DMibLogWithCategory(Mib/Cloud/KeyManager/Daemon, Info, "Shutting down");
				
				if (mp_ServerActor)
				{
					DMibLogWithCategory(Mib/Cloud/KeyManager/Daemon, Info, "Shutting down key server");
					mp_ServerActor->f_Destroy
						(
							[this, pCanDestroy](TCAsyncResult<void> &&_Result)
							{
								DMibLogWithCategory(Mib/Cloud/KeyManager/Daemon, Info, "Key server shut down");
								if (mp_DatabaseActor)
								{
									DMibLogWithCategory(Mib/Cloud/KeyManager/Daemon, Info, "Shutting down key server database");
									mp_DatabaseActor->f_Destroy
										(
											[this, pCanDestroy](TCAsyncResult<void> &&_Result)
											{
												DMibLogWithCategory(Mib/Cloud/KeyManager/Daemon, Info, "Key server database shut down");
											}
										)
									;
									mp_DatabaseActor = nullptr;
								}
							}
						)
					;
					mp_ServerActor = nullptr;
				}
				else if (mp_DatabaseActor)
				{
					DMibLogWithCategory(Mib/Cloud/KeyManager/Daemon, Info, "Shutting down key server database");
					mp_DatabaseActor->f_Destroy
						(
							[this, pCanDestroy](TCAsyncResult<void> &&_Result)
							{
							}
						)
					;
					mp_DatabaseActor = nullptr;
				}
				
				if (mp_CommandLine)
				{
					mp_CommandLine->f_Destroy
						(
							[this, pCanDestroy](TCAsyncResult<void> &&_Result)
							{
							}
						)
					;
					mp_CommandLine = nullptr;
				}

				return pCanDestroy->m_Continuation;
			}

			struct CKeyManagerDaemon : public NService::CServiceImp
			{
				CKeyManagerDaemon()
				{
					m_Actor = fg_ConstructActor<CKeyManagerDaemonActor>();
				}
				~CKeyManagerDaemon()
				{
					if (m_Actor)
						m_Actor->f_BlockDestroy();
				}
			
				TCActor<CKeyManagerDaemonActor> m_Actor;
			};
			
			NPtr::TCUniquePointer<NService::CServiceImp> fg_CreateDaemon()
			{
				return fg_Construct<CKeyManagerDaemon>();
			}
		}		
	}
}
