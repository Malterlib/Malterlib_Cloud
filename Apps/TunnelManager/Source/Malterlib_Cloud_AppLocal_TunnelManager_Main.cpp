
#include <Mib/Core/Core>
#include <Mib/Core/Application>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedDaemon>

#include "Malterlib_Cloud_App_TunnelManager.h"

using namespace NMib;
using namespace NMib::NCloud::NTunnelManager;

class CTunnelManagerApplication : public CApplication
{
	aint f_Main()
	{
		NConcurrency::CDistributedDaemon Daemon
			{
				"MalterlibCloudTunnelManager"
				, "Malterlib Tunnel Manager"
				, "Manages network tunnels."
				, []
				{
					return fg_ConstructActor<CTunnelManagerApp>();
				}
			}
		;
		return Daemon.f_Run();
	}	
};

DAppImplement(CTunnelManagerApplication);
