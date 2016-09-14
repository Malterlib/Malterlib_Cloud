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
								, "Version?"_= 
								{
									"Names"_= {"--version"}
									,"Default"_= "" 
									, "Description"_= "The version to install from version manager.\n"
										"Defaults to the latest version available.\n"
								}
								, "Executable"_= 
								{
									"Names"_= {"--executable"}
									,"Default"_= ""
									, "Description"_= "Start this executable contained in the package."
								}
								, "ExecutableParameters?"_= 
								{
									"Names"_= {"--executable-parameters"}
									, "Default"_= {"--daemon-run"}
									, "Type"_= _[_]
									, "Description"_= "Executable parameters to run the application with."
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
								"Package"_=  
								{
									"Type"_= ""
									, "Description"_= "The files needed to run the application.\n"
									"Can be a version manager application name, a directory, or a tar.gz file. Will look for version manager applications"
									"first. If not found it will assume that the name is a file and look for it on disk."
								}
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
							"Names"_= {"--application-list-versions"}
							, "Description"_= "List versions available to update to."
							, "Options"_=
							{
								"Verbose?"_= 
								{
									"Names"_= {"--verbose", "-v"}
									, "Default"_= false
									, "Description"_= "Display more extensive information about the versions." 
								}
								, "Application?"_= 
								{
									"Names"_= {"--application"}
									, "Default"_= ""
									, "Description"_= "The application to list versions for.\n"
										"Leave empty to list all applications.\n" 
								}
							}
						}
						, [this](CEJSON const &_Params)
						{
							return fp_CommandLine_ListAvailableVersions(_Params);
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
							"Names"_= {"--application-update-from-file"}
							, "Description"_= "Update the application package from file"
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
								"Package"_=  
								{
									"Type"_= ""
									, "Description"_= "The files needed to run the application.\n"
									"Can be a directory, or a tar.gz file."
								}
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
							"Names"_= {"--application-update"}
							, "Description"_= "Update the application package from version manager"
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
								"Version?"_=  
								{
									"Default"_= ""
									, "Description"_= "The version to update to.\n"
									"Defaults to the latest version in the same branch.\n"
								}
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
						Results.f_AddStdOut(fg_Format("      Application name: {}{\n}", Application.m_VersionManagerApplication));
						Results.f_AddStdOut(fg_Format("               Version: {}{\n}", Application.m_LastInstalledVersion));
						Results.f_AddStdOut(fg_Format("          Version time: {}{\n}", Application.m_LastInstalledVersionInfo.m_Time));
						Results.f_AddStdOut(fg_Format("        Version config: {}{\n}", Application.m_LastInstalledVersionInfo.m_Configuration));
						Results.f_AddStdOut(fg_Format("          Version size: {}{\n}", Application.m_LastInstalledVersionInfo.m_nBytes));
						Results.f_AddStdOut(fg_Format("         Version files: {}{\n}", Application.m_LastInstalledVersionInfo.m_nFiles));
						CStr InfoString = Application.m_LastInstalledVersionInfo.m_ExtraInfo.f_ToString("    ");
						CStr FirstLine = fg_GetStrLineSep(InfoString);
						Results.f_AddStdOut(fg_Format("         Version extra: {}{\n}", FirstLine));
						while (!InfoString.f_IsEmpty())
							Results.f_AddStdOut(fg_Format("                        {}{\n}", fg_GetStrLineSep(InfoString)));
					}
				}
				return fg_Explicit(fg_Move(Results));
			}
			
			TCContinuation<CDistributedAppCommandLineResults> CAppManagerActor::fp_CommandLine_ListAvailableVersions(CEJSON const &_Params)
			{
				bool bVerbose = _Params["Verbose"].f_Boolean();
				CStr ApplicationName = _Params["Application"].f_String();
				CDistributedAppCommandLineResults CommandLineResults;

				mint LongestApplication = fg_StrLen("Application");
				mint LongestVersion = fg_StrLen("Version");
				mint LongestConfig = fg_StrLen("Config");
				mint LongestTime = fg_StrLen("Time");
				mint LongestSize = fg_StrLen("Size");
				mint LongestFiles = fg_StrLen("Files");
				for (auto &Application : mp_VersionManagerApplications)
				{
					LongestApplication = fg_Max(LongestApplication, Application.f_GetApplicationName().f_GetLen());
					for (auto &Version : Application.m_VersionsByTime)
					{
						LongestVersion = fg_Max(LongestVersion, fg_Format("{}", Version.f_GetVersionID()).f_GetLen());
						LongestConfig = fg_Max(LongestConfig, Version.m_VersionInfo.m_Configuration.f_GetLen());
						LongestTime = fg_Max(LongestTime, fg_Format("{tc6}", Version.m_VersionInfo.m_Time.f_ToLocal()).f_GetLen());
						LongestSize = fg_Max(LongestSize, fg_Format("{ns }", Version.m_VersionInfo.m_nBytes).f_GetLen());
						LongestFiles = fg_Max(LongestFiles, fg_Format("{}", Version.m_VersionInfo.m_nFiles).f_GetLen());
					}
				}
				
				auto fOutputLine = [&]
					(
						auto const &_Application
						, auto const &_Version
						, auto const &_Config
						, auto const &_Time
						, auto const &_Size
						, auto const &_Files
					)
					{
						CommandLineResults.f_AddStdOut
							(
								fg_Format
								(
									"{sj*,a-}   {sj*,a-}   {sj*,a-}   {sj*,a-}   {sj*}   {sj*}\n"
									, _Application
									, LongestApplication
									, _Version
									, LongestVersion
									, _Config
									, LongestConfig
									, _Time
									, LongestTime
									, _Size
									, LongestSize
									, _Files
									, LongestFiles
								)
							)
						;
					}
				;
				
				fOutputLine("Application", "Version", "Config", "Time", "Size", "Files");
				
				for (auto &Application : mp_VersionManagerApplications)
				{
					if (!ApplicationName.f_IsEmpty() && Application.f_GetApplicationName() != ApplicationName)
						continue;
					for (auto &Version : Application.m_VersionsByTime)
					{
						fOutputLine
							(
								Application.f_GetApplicationName()
								, Version.f_GetVersionID()
								, Version.m_VersionInfo.m_Configuration
								, fg_Format("{tc6}", Version.m_VersionInfo.m_Time.f_ToLocal())
								, fg_Format("{ns }", Version.m_VersionInfo.m_nBytes)
								, fg_Format("{}", Version.m_VersionInfo.m_nFiles)
							)
						;
						if (bVerbose && Version.m_VersionInfo.m_ExtraInfo.f_IsObject() && Version.m_VersionInfo.m_ExtraInfo.f_Object().f_OrderedIterator())
						{
							CStr JSONString = Version.m_VersionInfo.m_ExtraInfo.f_ToString("    ");
							while (!JSONString.f_IsEmpty())
							{
								CStr Line = fg_GetStrLineSep(JSONString); 
								CommandLineResults.f_AddStdOut(fg_Format("{}\n", Line));
							}
						}
					}
				}
				
				return fg_Explicit(fg_Move(CommandLineResults));
			}
		}
	}
}
