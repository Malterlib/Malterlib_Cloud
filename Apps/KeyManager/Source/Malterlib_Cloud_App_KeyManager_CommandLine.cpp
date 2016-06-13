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
#include <Mib/Process/StdIn>
#include <Mib/Encoding/JSONShortcuts>

#include "Malterlib_Cloud_App_KeyManager.h"

namespace NMib
{
	namespace NCloud
	{
		namespace NKeyManager
		{
			void CKeyManagerDaemonActor::fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine)
			{
				CDistributedAppActor::fp_BuildCommandLine(o_CommandLine);
				
				auto DefaultSection = o_CommandLine.f_GetDefaultSection();

				DefaultSection.f_RegisterDirectCommand
					(
						{
							"Names"_= {"--provide-password"}
							, "Description"_= "Provide a password for the key database to be able to start the key manager."
						}
						, [this](const NEncoding::CEJSON &_Parameters, CDistributedAppCommandLineClient &_CommandLineClient) -> uint32
						{
							CBlockingStdInReader StdInReader;
							CBlockingStdInReader::CPromptParams Params;
							Params.m_bPassword = true;
							Params.m_Prompt = "Type password for key database: ";
							NStr::CStr Password;
							if (!StdInReader.f_ReadPrompt(Params, Password))
								return 1;
							
							_CommandLineClient.f_RunCommand("--provide-password-direct", {"Password"_= Password});
							return 0;
						}
					)
				;
				DefaultSection.f_RegisterCommand
					(
						{
							"Names"_= {"--provide-password-direct"}
							, "Description"_= 
								"Provide a password for the key database to be able to start the key manager. [INTERNAL]\n"
								"Ideally use --provide-password instead so password is not sent on the command line."
							, "Parameters"_=
							{
								"Password"_=
								{
									"Description"_= "The password."
									, "Type"_= ""
								}
							}
						}
						, [this](const NEncoding::CEJSON &_Parameters)
						{
							return f_ProvidePassword(CStrSecure{_Parameters["Password"].f_String()});
						}
					)
				;
			}
			
			TCContinuation<CDistributedAppCommandLineResults> CKeyManagerDaemonActor::f_ProvidePassword(NStr::CStrSecure const &_Password)
			{
				TCContinuation<CDistributedAppCommandLineResults> Continuation;
				fg_Dispatch
					(
						[this, _Password] () -> TCContinuation<void>
						{
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
						{
							DMibLogWithCategory(Mib/Cloud/KeyManager/Daemon, Error, "Provide password attempt failed: {}", _Result.f_GetExceptionStr());
							Continuation.f_SetException(_Result);
						}
						else						
							Continuation.f_SetResult(CDistributedAppCommandLineResults());
					}
				;
				return Continuation;
			}
		}		
	}
}
