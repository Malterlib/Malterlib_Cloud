// Copyright © 2025 Unbroken AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedApp>

namespace NMib::NCloud::NBootstrap
{
	TCFuture<uint32> fg_Bootstrap_MalterlibCloud
		(
			NEncoding::CEJsonSorted _Params
			, TCSharedPointer<CCommandLineControl> _pCommandLine
			, NStr::CStr _RootDirectory
			, TCActor<NConcurrency::CDistributedActorTrustManager> _TrustManager
		)
	;
}
