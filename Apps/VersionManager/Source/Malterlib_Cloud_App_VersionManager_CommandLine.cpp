// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/DistributedActorTrustManager>
#include <Mib/Concurrency/DistributedActorTrustManagerDatabases/JsonDirectory>
#include <Mib/Process/StdIn>
#include <Mib/Encoding/JsonShortcuts>

#include "Malterlib_Cloud_App_VersionManager.h"
#include "Malterlib_Cloud_App_VersionManager_Server.h"

namespace NMib::NCloud::NVersionManager
{
	void CVersionManagerDaemonActor::fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine)
	{
		CDistributedAppActor::fp_BuildCommandLine(o_CommandLine);

		o_CommandLine.f_SetProgramDescription
			(
				"Malterlib Cloud Version Manager"
				, "Manages updates for Malterlib cloud apps."
			)
		;

		auto DefaultSection = o_CommandLine.f_GetDefaultSection();

		DefaultSection.f_RegisterCommand
			(
				{
					"Names"_o= _o["--refresh-versions"]
					, "Description"_o= "Rescan disk and refresh version database. Removes entries for versions no longer on disk, adds new versions, and updates changed metadata."
				}
				, [this](CEJsonSorted const, TCSharedPointer<CCommandLineControl> _pCommandLine) -> TCFuture<uint32>
				{
					using namespace NStr;
					auto Result = co_await mp_pServer(&CServer::f_RefreshDatabaseFromDisk);
					co_await _pCommandLine->f_StdOut("Refreshed versions: {} added, {} updated, {} removed\n"_f << Result.m_nAdded << Result.m_nUpdated << Result.m_nRemoved);
					co_return 0;
				}
				, EDistributedAppCommandFlag_WaitForRemotes
			)
		;
	}
}
