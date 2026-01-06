
#include "Malterlib_Cloud_App_BackupManager_Internal.h"
#include "Malterlib_Cloud_App_BackupManager.h"

#include <Mib/Web/WebSocket>
#include <Mib/Network/SSL>
#include <Mib/Network/Sockets/SSL>

namespace NMib::NCloud::NBackupManager
{
	void CBackupManagerApp::fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine)
	{
		CDistributedAppActor::fp_BuildCommandLine(o_CommandLine);

		o_CommandLine.f_SetProgramDescription
			(
				"Malterlib Cloud Backup Manager"
				, "Manages backups for malterlib cloud applications."
			)
		;
	}
}
