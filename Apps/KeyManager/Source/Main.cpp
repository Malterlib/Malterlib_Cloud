// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Core/Application>
#include <Mib/Daemon/Daemon>

#include "Malterlib_Cloud_App_KeyManager.h"

using namespace NMib;
using namespace NMib::NCloud::NKeyManager;

class CKeyManager : public CApplication
{
	aint f_Main()
	{
		NContainer::TCVector<NMib::NStr::CStr> CommandLineArgs;
		NSys::fg_Process_GetCommandLineArgs(CommandLineArgs);
		
		try
		{
			for (auto iCommand = CommandLineArgs.f_GetIterator(); iCommand; ++iCommand)
			{
				if (*iCommand == "--provide-password")
					return fg_ProvidePassword();
				else if (*iCommand == "--generate-trust-ticket")
					return fg_GenerateTrustTicket();
			}
		}
		catch (NException::CException const &_Exception)
		{
			DConErrOut("{}{\n}", _Exception.f_GetErrorStr());
			return 1;
		}
		
		NService::fg_RunDaemon
			(
				"MalterlibCloudKeyManager"
				, "Malterlib Cloud Key Manager"
				, ""
				, &fg_CreateDaemon
			)
		;
		return 0;
	}	
};

DAppImplement(CKeyManager);
