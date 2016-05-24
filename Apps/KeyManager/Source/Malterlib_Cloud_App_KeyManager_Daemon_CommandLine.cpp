// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Cloud/KeyManagerServer>
#include <Mib/Cloud/KeyManagerDatabases/EncryptedFile>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/DistributedActorTrustManager>
#include <Mib/Concurrency/DistributedActorTrustManagerDatabases/JSONDirectory>
#include <Mib/Concurrency/Actor/Timer>

#include "Malterlib_Cloud_App_KeyManager.h"
#include "Malterlib_Cloud_App_KeyManager_Daemon.h"

namespace NMib
{
	namespace NCloud
	{
		namespace NKeyManager
		{
			ch8 const *g_pLocalListenAddress = "wss://localhost:11392/";
			CKeyManagerDaemonActor::CCommandLine::CCommandLine(TCWeakActor<CKeyManagerDaemonActor> const &_Actor)
				: mp_Actor(_Actor)
			{
			}
			
			TCContinuation<void> CKeyManagerDaemonActor::CCommandLine::f_ProvidePassword(NStr::CStrSecure const &_Password)
			{
				return mp_Actor(&CKeyManagerDaemonActor::f_ProvidePassword, _Password, fg_GetCallingHostID());
			}
			
			TCContinuation<CStr> CKeyManagerDaemonActor::CCommandLine::f_GenerateTrustTicket()
			{
				return mp_Actor(&CKeyManagerDaemonActor::f_GenerateTrustTicket, fg_GetCallingHostID());
			}
			
			TCContinuation<void> CKeyManagerDaemonActor::fp_CreateCommandLineTrust()
			{
				NStr::CStr CommandLineTrustPath = CFile::fs_GetProgramDirectory() + "/CommandLineTrustDatabase";
				
				TCContinuation<void> Continuation;
				
				fg_Dispatch
					(
						mp_FileOperationsActor
						, [this, Continuation, CommandLineTrustPath]
						{
							if (!CFile::fs_FileExists(CommandLineTrustPath))
								return false;
							return true;
						}
					)
					> Continuation / [this, Continuation, CommandLineTrustPath](bool _bAlreadySetUp)
					{
						if (_bAlreadySetUp && mp_StateDatabase.m_Data.f_GetMember("CommandLineHostID", EJSONType_String))
						{
							// Already setup
							Continuation.f_SetResult();
							return;
						}

						// Remove trust database if anything went wrong during setup
						auto pCleanup = fg_OnScopeExitShared
							(
								[CommandLineTrustPath, FileOperationsActor = mp_FileOperationsActor]
								{
									fg_Dispatch
										(
											FileOperationsActor
											, [CommandLineTrustPath]
											{
												if (CFile::fs_FileExists(CommandLineTrustPath))
													CFile::fs_DeleteDirectoryRecursive(CommandLineTrustPath);
											}
										)
										> fg_DiscardResult()
									;
								}
							)
						;
						
						{
							struct CState
							{
								TCActor<ICDistributedActorTrustManagerDatabase> m_TrustManagerDatabase;
								TCActor<CDistributedActorTrustManager> m_TrustManager;
							};
							
							TCSharedPointer<CState> pState = fg_Construct();
							auto &State = *pState;
							State.m_TrustManagerDatabase = fg_ConstructActor<CDistributedActorTrustManagerDatabase_JSONDirectory>(CommandLineTrustPath);
							State.m_TrustManager = fg_ConstructActor<CDistributedActorTrustManager>
								(
									State.m_TrustManagerDatabase
									, [](NStr::CStr const &_HostID)
									{
										return fg_ConstructActor<NConcurrency::CActorDistributionManager>(_HostID);
									}
								)
							;
							State.m_TrustManager(&CDistributedActorTrustManager::f_Initialize) > Continuation / [this, Continuation, pState, pCleanup](NStr::CStr const &_HostID)
								{
									mp_TrustManager(&CDistributedActorTrustManager::f_HasClient, _HostID) > Continuation / [this, Continuation, pState, _HostID, pCleanup](bool _bHasClient)
										{
											auto fContinue = [this, Continuation, pState, _HostID, pCleanup]
												{
													CDistributedActorTrustManager_Address LocalListenAddress;
													LocalListenAddress.m_URL = g_pLocalListenAddress;
											
													mp_TrustManager(&CDistributedActorTrustManager::f_GenerateConnectionTicket, LocalListenAddress) 
														> Continuation / [this, Continuation, pState, _HostID, pCleanup](CDistributedActorTrustManager::CTrustTicket &&_TrustTicket)
														{
															auto &State = *pState;
															State.m_TrustManager(&CDistributedActorTrustManager::f_AddClientConnection, _TrustTicket, 60.0) 
																> Continuation / [this, Continuation, _HostID, pCleanup, pState]
																{
																	pCleanup->f_Clear();
																	auto &Setting = mp_StateDatabase.m_Data["CommandLineHostID"];
																	if (!Setting.f_IsString() || Setting.f_String() != _HostID)
																	{
																		mp_StateDatabase.m_Data["CommandLineHostID"] = _HostID;
																		mp_StateDatabase.f_Save() > Continuation % "Failed to save state database"; 
																	}
																	else
																		Continuation.f_SetResult();
																}
															;
														}
													;
												}
											;
											
											if (!_bHasClient)
												fContinue();
											else
											{
												mp_TrustManager(&CDistributedActorTrustManager::f_RemoveClient, _HostID) > Continuation / [fContinue = fg_Move(fContinue)]()
													{
														fContinue();
													}
												;
											}
										}
									;
								}
							;
						}
					}
				;				
				return Continuation;
			}

			TCContinuation<void> CKeyManagerDaemonActor::fp_SetupCommandLineListen()
			{
				TCContinuation<void> Continuation;
				
				CDistributedActorTrustManager_Address LocalListenAddress;
				LocalListenAddress.m_URL = g_pLocalListenAddress;
				
				mp_TrustManager(&CDistributedActorTrustManager::f_HasListen, LocalListenAddress) > Continuation / [this, Continuation, LocalListenAddress](bool _bHasListen)
					{
						if (_bHasListen)
						{
							Continuation.f_SetResult();
							return;
						}
						mp_TrustManager(&CDistributedActorTrustManager::f_AddListen, LocalListenAddress) > Continuation / [this, Continuation]()
							{
								Continuation.f_SetResult();
							}
						;
					}
				;
				return Continuation;
			}
			
			TCContinuation<void> CKeyManagerDaemonActor::fp_SetupCommandLineTrust()
			{
				TCContinuation<void> Continuation;
				DMibLogWithCategory(Mib/Cloud/KeyManager/Daemon, Info, "Setting up command line trust");
				
				fg_ThisActor(this)(&CKeyManagerDaemonActor::fp_SetupCommandLineListen) > Continuation / [this, Continuation]()
					{
						fg_ThisActor(this)(&CKeyManagerDaemonActor::fp_CreateCommandLineTrust) > Continuation / [this, Continuation]()
							{
								mp_CommandLine = fg_ConstructDistributedActor<CCommandLine>(fg_ThisActor(this));
								DMibLogWithCategory(Mib/Cloud/KeyManager/Daemon, Info, "Finished setting up command line trust, publishing command line actor");
								
								fg_GetDistributionManager()
									(
										&CActorDistributionManager::f_PublishActor
										, mp_CommandLine
										, "Malterlib/Concurrency/Commandline"
										, CDistributedActorInheritanceHeirarchyPublish::fs_GetHierarchy<ICCommandLine>()
									)
									> Continuation / [this, Continuation](CDistributedActorPublication &&_Publication)
									{
										DMibLogWithCategory(Mib/Cloud/KeyManager/Daemon, Info, "Command line published");
										mp_CommandLinePublication = fg_Move(_Publication);
										Continuation.f_SetResult();
									}
								;
							}
						;
					}
				;
				
				return Continuation;
			}

			bool CKeyManagerDaemonActor::fp_HasCommandLineAccess(NStr::CStr const &_HostID)
			{
				if (auto pCommandLineHost = mp_StateDatabase.m_Data.f_GetMember("CommandLineHostID", EJSONType_String))
					return pCommandLineHost->f_String() == _HostID;
				return false;
			}

			TCContinuation<CStr> CKeyManagerDaemonActor::f_GenerateTrustTicket(NStr::CStr const &_FromHostID)
			{
				if (!fp_HasCommandLineAccess(_FromHostID))
					return DErrorInstance("Access denied");
				
				TCContinuation<CStr> Continuation;
				mp_TrustManager(&CDistributedActorTrustManager::f_GenerateConnectionTicket, mp_PrimaryListen) 
					> Continuation / [Continuation, _FromHostID](CDistributedActorTrustManager::CTrustTicket &&_Ticket)
					{
						DMibLogWithCategory(Mib/Cloud/KeyManager/Daemon, Info, "Generated trust ticket for host '{}' with address: {}", _FromHostID, _Ticket.m_ServerAddress.m_URL.f_Encode());
						Continuation.f_SetResult(_Ticket.f_ToStringTicket());
					}
				;
				return Continuation;
			}
			
			TCContinuation<void> CKeyManagerDaemonActor::f_ProvidePassword(NStr::CStrSecure const &_Password, NStr::CStr const &_FromHostID)
			{
				TCContinuation<void> Continuation;
				fg_Dispatch
					(
						[this, _Password, _FromHostID] () -> TCContinuation<void>
						{
							if (!fp_HasCommandLineAccess(_FromHostID))
								return DErrorInstance("Access denied");

							if (mp_bDatabaseDecrypted)
								return DErrorInstance("A correct password has already been provided");
							
							if (!mp_pProvidePasswordOnce)
							{
								mp_pProvidePasswordOnce = fg_Construct
									(
										fg_ThisActor(this)
										, [this](NStr::CStr const &_Password)
										{
											TCContinuation<void> Continuation;
											
											NNet::CEncryptAES::CSalt Salt{'M', 'i', 'B', 'K', 'e', 'y', 'M', 'a'};
											TCActor<CKeyManagerServerDatabase_EncryptedFile> DatabaseActor = fg_ConstructActor<CKeyManagerServerDatabase_EncryptedFile>
												(
													fg_Construct("Encrypted Key Manager Database Actor")
													, CFile::fs_GetProgramDirectory() + "/KeyDatabase.encrypted"
													, _Password
													, &Salt
												)
											;
											
											CClock Clock;
											Clock.f_Start();
											
											DatabaseActor(&CKeyManagerServerDatabase_EncryptedFile::f_Initialize) 
												> [this, Continuation, DatabaseActor, Clock](TCAsyncResult<void> &&_Result)
												{
													if (!_Result)
													{
														// Delay reply to be same response time every time
														fg_Timeout(fg_Max(fp64(0.5) - Clock.f_GetTime(), fp64(0.01))) 
															> [Continuation, Result = fg_Move(_Result)](TCAsyncResult<void> &&_Result)
															{
																Continuation.f_SetException
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
													Continuation.f_SetResult();
												}
											;
											
											return Continuation;
										}
										, "Already processing a password. Try again later."
									)
								;
							}
							
							return (*mp_pProvidePasswordOnce)(_Password);
						}
					)
					> [Continuation](TCAsyncResult<void> &&_Result)
					{
						if (!_Result)
							DMibLogWithCategory(Mib/Cloud/KeyManager/Daemon, Error, "Provide password attempt failed: {}", _Result.f_GetExceptionStr());
						Continuation.f_SetResult(_Result);
					}
				;
				return Continuation;
			}
		}		
	}
}
