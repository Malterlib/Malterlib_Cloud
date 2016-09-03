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
							"Names"_= {"--application-add"}
							, "Description"_= 
								"Adds an application.\n"
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
							"Names"_= {"--application-list"}
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
							"Names"_= {"--application-remove"}
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
							"Names"_= {"--application-update"}
							, "Description"_= "Update the application package"
							, "Options"_= 
							{
								"Name"_= 
								{
									"Names"_= {"--name"}
									,"Type"_= ""
									, "Description"_= "Unique name of the application to update."
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
							"Names"_= {"--application-stop"}
							, "Description"_= "Stop the application, keeping any encryption loaded"
							, "Parameters"_=
							{
								"Name"_= 
								{
									"Type"_= ""
									, "Description"_= "Unique name of the application to stop."
								}
							}
						}
						, [this](CEJSON const &_Params)
						{
							return fp_CommandLine_StopApplication(_Params);
						}
					)
				;
				DefaultSection.f_RegisterCommand
					(
						{
							"Names"_= {"--application-restart"}
							, "Description"_= "Restart the application, keeping any encryption loaded"
							, "Parameters"_=
							{
								"Name"_= 
								{
									"Type"_= ""
									, "Description"_= "Unique name of the application to restart."
								}
							}
						}
						, [this](CEJSON const &_Params)
						{
							return fp_CommandLine_RestartApplication(_Params);
						}
					)
				;
				DefaultSection.f_RegisterCommand
					(
						{
							"Names"_= {"--application-start"}
							, "Description"_= "Start the application"
							, "Parameters"_=
							{
								"Name"_= 
								{
									"Type"_= ""
									, "Description"_= "Unique name of the application to start."
								}
							}
						}
						, [this](CEJSON const &_Params)
						{
							return fp_CommandLine_StartApplication(_Params);
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
		}
	}
}
