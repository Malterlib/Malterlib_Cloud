// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Cryptography/RandomID>
#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib
{
	namespace NCloud
	{
		namespace NAppManager
		{
			void CAppManagerActor::fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine)
			{
				CDistributedAppActor::fp_BuildCommandLine(o_CommandLine);
				o_CommandLine.f_SetProgramDescription
					(
						"Malterlib Cloud App Manager"
						, "Manages malterlib cloud applications by providing services such as encryption at rest and automatic updates." 
					)
				;
				
				auto DefaultSection = o_CommandLine.f_GetDefaultSection();
				
				auto PackageParameter = "Package"_=
					{
						"Type"_= ""
						, "Description"_= "The files needed to run the application.\n"
						"Can be a directory, or a tar.gz file."
					}
				;
				
				DefaultSection.f_RegisterCommand
					(
						{
							"Names"_= {"--add-application"}
							, "Description"_= 
								"Adds an application."
								"By default the application will run as root."
							, "Options"_= 
							{
								"Name"_= 
								{
									"Names"_= {"--name"}
									,"Type"_= ""
									, "Description"_= "Uniquely name the application."
								}
								, "EncryptionStorage?"_= 
								{
									"Names"_= {"--encryption-storage"}
									, "Default"_= ""
									, "Description"_= "Select the file or device that should be the storage for encryption."
								}
								, "Executable"_= 
								{
									"Names"_= {"--executable"}
									,"Type"_= ""
									, "Description"_= "Start this executable contained in the package."
								}
								, "ExecutableParameters?"_= 
								{
									"Names"_= {"--executable-parameters"}
									, "Default"_= {"--daemon-run"}
									, "Type"_= _[_]
									, "Description"_= "Start this executable contained in the package."
								}
								, "RunAsUser?"_=
								{
									"Names"_= {"--run-as-user"}
									, "Default"_= ""
									, "Description"_= "Run the application as this user."
								}
								, "RunAsGroup?"_= 
								{
									"Names"_= {"--run-as-group"}
									, "Default"_= ""
									, "Description"_= "Run the application as this group."
								}
								, "ForceOverwrite?"_= 
								{
									"Names"_= {"--force-overwrite"}
									,"Default"_= false 
									, "Description"_= "Force zfs to overwrite storage"
								}
							}
							, "Parameters"_=
							{
								PackageParameter
							}
						}
						, [this](CEJSON const &_Params)
						{
							return fp_CommandLine_AddApplication(_Params);
						}
					)
				;
				DefaultSection.f_RegisterCommand
					(
						{
							"Names"_= {"--enum-applications"}
							, "Description"_= "List applications"
							, "Options"_=
							{
								"Verbose?"_= 
								{
									"Names"_= {"--verbose", "-v"}
									, "Default"_= false
									, "Description"_= "Display more extensive information about the applications." 
								}
							}
						}
						, [this](CEJSON const &_Params)
						{
							return fp_CommandLine_EnumApplications(_Params);
						}
					)
				;
				DefaultSection.f_RegisterCommand
					(
						{
							"Names"_= {"--remove-application"}
							, "Description"_= "Remove the application"
							, "Parameters"_=
							{
								"Name"_= 
								{
									"Type"_= ""
									, "Description"_= "The name of the application to remove"
								}
							}
						}
						, [this](CEJSON const &_Params)
						{
							return fp_CommandLine_RemoveApplication(_Params);
						}
					)
				;
				DefaultSection.f_RegisterCommand
					(
						{
							"Names"_= {"--update-application"}
							, "Description"_= "Update the application package"
							, "Options"_= 
							{
								"Name"_= 
								{
									"Names"_= {"--name"}
									,"Type"_= ""
									, "Description"_= "Uniquely name the application to update."
								}
							}
							, "Parameters"_=
							{
								PackageParameter
							}
						}
						, [this](CEJSON const &_Params)
						{
							return fp_CommandLine_UpdateApplication(_Params);
						}
					)
				;
				DefaultSection.f_RegisterCommand
					(
						{
							"Names"_= {"--add-allowed-key-manager"}
							, "Description"_= "Add an allowed key manager server"
							, "Parameters"_=
							{
								"HostID"_= 
								{
									"Type"_= ""
									, "Description"_= "The host ID of the key manager to allow. This host ID is returned when you add a connection to the key manager."
								}
							}
						}
						, [this](CEJSON const &_Params)
						{
							return fp_CommandLine_AddAllowedKeyManager(_Params);
						}
					)
				;
				DefaultSection.f_RegisterCommand
					(
						{
							"Names"_= {"--remove-allowed-key-manager"}
							, "Description"_= "Remove an allowed key manager server"
							, "Parameters"_=
							{
								"HostID"_= 
								{
									"Type"_= ""
									, "Description"_= "The host ID of the key manager to dis-allow."
								}
							}
						}
						, [this](CEJSON const &_Params)
						{
							return fp_CommandLine_RemoveAllowedKeyManager(_Params);
						}
					)
				;
			}
			
			TCContinuation<CDistributedAppCommandLineResults> CAppManagerActor::fp_CommandLine_EnumApplications(CEJSON const &_Params)
			{
				bool bVerbose = _Params["Verbose"].f_Boolean();
				CDistributedAppCommandLineResults Results;
				for (auto &pApplication : mp_Applications)
				{
					auto &Application = *pApplication;
					Results.f_AddStdOut(fg_Format("{}{\n}", Application.m_Name));
					if (bVerbose)
					{
						Results.f_AddStdOut(fg_Format("            Executable: {}{\n}", Application.m_Executable));
						Results.f_AddStdOut(fg_Format("            Parameters: {vs}{\n}", Application.m_ExecutableParameters));
						Results.f_AddStdOut(fg_Format("           Run as user: {}{\n}", Application.m_RunAsUser));
						Results.f_AddStdOut(fg_Format("          Run as group: {}{\n}", Application.m_RunAsGroup));
						Results.f_AddStdOut(fg_Format("    Encryption storage: {}{\n}", Application.m_EncryptionStorage));
						Results.f_AddStdOut(fg_Format("                Status: {}{\n}", Application.m_LaunchState));
					}
				}
				return fg_Explicit(fg_Move(Results));
			}
			
			TCContinuation<CDistributedAppCommandLineResults> CAppManagerActor::fp_CommandLine_AddAllowedKeyManager(CEJSON const &_Params)
			{
				CStr HostID = _Params["HostID"].f_String();
				if (mp_AllowedKeyManagers.f_FindEqual(HostID))
					return DMibErrorInstance("Host ID already allowed");

				auto &Managers = mp_StateDatabase.m_Data["AllowedKeyManagers"].f_Array();
				if (Managers.f_Contains(CEJSON(HostID)) < 0)
					Managers.f_Insert(HostID);
				TCContinuation<CDistributedAppCommandLineResults> Continuation;
				mp_StateDatabase.f_Save() > Continuation % "Failed to save state" / [Continuation, HostID, this]
					{
						mp_AllowedKeyManagers[HostID];

						if (auto *pKeyManager = mp_HostToKeyManager.f_FindEqual(HostID))
						{
							mp_KeyManagers[*pKeyManager];
							fp_KeyManagerAvailable();
						}
						
						Continuation.f_SetResult("Success" DMibNewLine);
					}
				;
				
				return Continuation;
			}
			
			TCContinuation<CDistributedAppCommandLineResults> CAppManagerActor::fp_CommandLine_RemoveAllowedKeyManager(CEJSON const &_Params)
			{
				CStr HostID = _Params["HostID"].f_String();
				if (!mp_AllowedKeyManagers.f_FindEqual(HostID))
					return DMibErrorInstance("Host ID is already disallowed");
				
				auto &AllowedKeyManagers = mp_StateDatabase.m_Data["AllowedKeyManagers"].f_Array();
				auto iAllowed = AllowedKeyManagers.f_GetIterator();
				for (; iAllowed; ++iAllowed)
				{
					if (iAllowed->f_String() == HostID)
						break;
				}
				if (iAllowed)
					AllowedKeyManagers.f_Remove(&*iAllowed - AllowedKeyManagers.f_GetArray());

				mp_AllowedKeyManagers.f_Remove(HostID);
				
				TCContinuation<CDistributedAppCommandLineResults> Continuation;
				mp_StateDatabase.f_Save() > Continuation % "Failed to save state" / [this, Continuation, HostID]
					{
						Continuation.f_SetResult("Success" DMibNewLine);
						if (auto *pKeyManager = mp_HostToKeyManager.f_FindEqual(HostID))
							mp_KeyManagers.f_Remove(*pKeyManager);
					}
				;
				
				return Continuation;
			}
		}
	}
}
