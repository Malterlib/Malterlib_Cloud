// Copyright © 2025 Unbroken AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedApp>
#include <Mib/String/String>
#include <Mib/Web/AWS/EC2>

namespace NMib::NCloud::NBootstrap
{
	TCFuture<NWeb::CAwsCredentials> fg_PromptForAwsCredentials
		(
			TCSharedPointer<CCommandLineControl> _pCommandLine
			, CStr _AccessKeyId
			, CStr _SecretAccessKey
			, CStr _DefaultRegion
		)
	;

	struct CRegionCache
	{
		TCOptional<TCVector<CAwsEc2Actor::CRegionInfo>> m_Regions;
		TCOptional<TCMap<CStr, CStr>> m_RegionNames;
	};

	TCFuture<TCOptional<CStr>> fg_SelectAwsRegion
		(
			TCSharedPointer<CCommandLineControl> _pCommandLine
			, NWeb::CAwsCredentials _Credentials
			, CStr _Default
			, TCSharedPointer<CRegionCache> _pRegionCache
		)
	;
}
