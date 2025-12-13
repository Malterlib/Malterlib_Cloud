// Copyright © 2025 Unbroken AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedApp>
#include <Mib/Cloud/VersionManager>

#include "Malterlib_Cloud_Bootstrap_Config.h"

namespace NMib::NCloud::NBootstrap
{
	// Runtime cache for VersionManager data (not serialized)
	struct CVersionManagerCache
	{
		bool m_bQueried = false;
		// App name -> (VersionIDAndPlatform -> VersionInformation)
		NContainer::TCMap<NStr::CStr, NContainer::TCMap<CVersionManager::CVersionIDAndPlatform, CVersionManager::CVersionInformation>> m_Applications;
		NContainer::TCSet<NStr::CStr> m_AvailablePlatforms;
		NTime::CTime m_QueryTime;
	};

	// Main package configuration function
	// Shows UI for selecting package source and packages to download
	// Returns modified config on success, empty optional if cancelled
	TCFuture<TCOptional<CBootstrapConfig>> fg_ConfigurePackages
		(
			TCSharedPointer<CCommandLineControl> _pCommandLine
			, TCActor<NConcurrency::CDistributedActorTrustManager> _TrustManager
			, NStr::CStr _RootDirectory
			, CBootstrapConfig _Config
		)
	;

	// Validate local package directory structure
	// Returns: App name -> list of platforms found
	TCFuture<NContainer::TCMap<NStr::CStr, NContainer::TCVector<NStr::CStr>>> fg_ValidateLocalPackages(NStr::CStr _PackageDirectory);
}
