// Copyright © 2025 Unbroken AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Concurrency/AsyncDestroy>
#include <Mib/Web/AWS/EC2>
#include <Mib/Web/AWS/SSM>
#include <Mib/Web/Curl>

#include "Malterlib_Cloud_Bootstrap_Aws.h"
#include "Malterlib_Cloud_Bootstrap_Prompts.h"

namespace NMib::NCloud::NBootstrap
{
	TCFuture<NWeb::CAwsCredentials> fg_PromptForAwsCredentials
		(
			TCSharedPointer<CCommandLineControl> _pCommandLine
			, CStr _AccessKeyId
			, CStr _SecretAccessKey
			, CStr _DefaultRegion
		)
	{
		NWeb::CAwsCredentials Credentials;

		if (_AccessKeyId.f_IsEmpty())
		{
			*_pCommandLine += "AWS credentials not found in environment.\n";
			_AccessKeyId = co_await _pCommandLine->f_ReadPrompt({"AWS Access Key ID: ", false});
			_AccessKeyId = _AccessKeyId.f_Trim();
		}

		CStrSecure SecretKey;
		if (_SecretAccessKey.f_IsEmpty())
		{
			SecretKey = co_await _pCommandLine->f_ReadPrompt({"AWS Secret Access Key: ", true});
			SecretKey = SecretKey.f_Trim();
		}
		else
		{
			SecretKey = _SecretAccessKey;
		}

		if (_AccessKeyId.f_IsEmpty() || SecretKey.f_IsEmpty())
		{
			*_pCommandLine %= "Error: AWS credentials are required.\n";
			co_return Credentials;
		}

		Credentials.m_AccessKeyID = _AccessKeyId;
		Credentials.m_SecretKey = SecretKey;
		Credentials.m_Region = _DefaultRegion;

		co_return Credentials;
	}

	TCFuture<TCOptional<CStr>> fg_SelectAwsRegion
		(
			TCSharedPointer<CCommandLineControl> _pCommandLine
			, NWeb::CAwsCredentials _Credentials
			, CStr _Default
			, TCSharedPointer<CRegionCache> _pRegionCache
		)
	{
		TCActor<NWeb::CCurlActor> CurlActor{fg_Construct(), "Curl"};
		auto DestroyCurl = co_await fg_AsyncDestroy(CurlActor);

		TCActor<NWeb::CAwsEc2Actor> Ec2Actor{fg_Construct(CurlActor, _Credentials)};
		auto DestroyEc2 = co_await fg_AsyncDestroy(Ec2Actor);

		TCActor<NWeb::CAwsSsmActor> SsmActor{fg_Construct(CurlActor, _Credentials)};
		auto DestroySsm = co_await fg_AsyncDestroy(SsmActor);

		if (!_pRegionCache->m_Regions)
		{
			*_pCommandLine += "Fetching available AWS regions...\n";

			_pRegionCache->m_Regions = co_await Ec2Actor(&NWeb::CAwsEc2Actor::f_DescribeRegions);
		}

		if (!_pRegionCache->m_RegionNames)
		{
			TCVector<CStr> RegionCodes;
			for (auto &RegionInfo : *_pRegionCache->m_Regions)
				RegionCodes.f_InsertLast(RegionInfo.m_RegionName);

			_pRegionCache->m_RegionNames = co_await SsmActor(&NWeb::CAwsSsmActor::f_GetRegionLongNames, fg_Move(RegionCodes));
		}

		TCVector<TCVector<CStr>> RegionItems;
		for (auto &RegionInfo : *_pRegionCache->m_Regions)
		{
			auto pName = _pRegionCache->m_RegionNames->f_FindEqual(RegionInfo.m_RegionName);
			CStr LongName = pName ? *pName : "Unknown";

			RegionItems.f_Insert({RegionInfo.m_RegionName, LongName});
		}

		RegionItems.f_Sort();

		TCOptional<CStr> Region = co_await fg_SelectFromListWithFilter(_pCommandLine, RegionItems, {"Region", "Name"}, "Select AWS Region", _Default);

		co_return Region;
	}
}
