// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include "Malterlib_Cloud_App_CloudClient.h"

namespace NMib::NCloud::NCloudClient
{
	struct CCloudClientAppLocalActor : public CCloudClientAppActor
	{
	private:
		
		struct CSelfUpdateVersion
		{
			TCDistributedActor<CVersionManager> m_Actor;
			CVersionManager::CVersionID m_VersionID;
		};
		
		void fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine) override;
		TCFuture<void> fp_PreRunCommandLine(CStr const &_Command, NEncoding::CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine) override;
		
		TCFuture<CSelfUpdateVersion> fp_GetSelfUpdateVersion();
		TCFuture<uint32> fp_CommandLine_SelfUpdate(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
	};
}
