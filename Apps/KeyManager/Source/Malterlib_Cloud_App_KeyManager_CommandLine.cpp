// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Process/StdIn>
#include <Mib/Concurrency/DistributedActor>

#include "Malterlib_Cloud_App_KeyManager.h"
#include "Malterlib_Cloud_App_KeyManager_CommandLineClient.h"

namespace NMib
{
	namespace NCloud
	{
		namespace NKeyManager
		{
			void fg_SetupCommandLine()
			{
				fg_GetSys()->f_RemoveTraceLogger();
				fg_GetSys()->f_SetDefaultLogFileName("KeyManager.log");
				fg_GetSys()->f_SetDefaultLogFileDirectory(CFile::fs_GetProgramDirectory() + "/LogCommand");
			}
			
			aint fg_ProvidePassword()
			{
				fg_SetupCommandLine();
				
				CCommandLineClient CommandLineClient;

				CBlockingStdInReader StdInReader;
				CBlockingStdInReader::CPromptParams Params;
				Params.m_bPassword = true;
				Params.m_Prompt = "Type password for key database: ";
				NStr::CStr Password;
				if (!StdInReader.f_ReadPrompt(Params, Password))
					return 1;
				
				DCallActor(CommandLineClient.f_GetClient(), ICCommandLine::f_ProvidePassword, Password).f_CallSync(60.0);
				
				return 0;
			}
			
			aint fg_GenerateTrustTicket()
			{
				fg_SetupCommandLine();
				
				CCommandLineClient CommandLineClient;
				auto TrustTicket = DCallActor(CommandLineClient.f_GetClient(), ICCommandLine::f_GenerateTrustTicket).f_CallSync(60.0);
				DMibConOut("{}{\n}", TrustTicket);
				return 0;
			}
		}		
	}
}
