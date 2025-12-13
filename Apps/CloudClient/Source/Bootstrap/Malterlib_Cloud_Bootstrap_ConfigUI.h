// Copyright © 2025 Unbroken AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedApp>
#include <Mib/Time/Time>
#include <Mib/Web/AWS/EC2>

#include "Malterlib_Cloud_Bootstrap_Config.h"

namespace NMib::NCloud::NBootstrap
{
	// Pricing data fetched from AWS Pricing API
	struct CAwsPricingData
	{
		template <typename tf_CStream>
		void f_Stream(tf_CStream &_Stream)
		{
			_Stream % m_InstancePrices;
			_Stream % m_ReservedPrices;
			_Stream % m_InstanceInfo;
			_Stream % m_EBSPricePerGBMonth;
			_Stream % m_GP3IOPSPricePerMonth;
			_Stream % m_GP3ThroughputPricePerMonth;
			_Stream % m_SnapshotPricePerGBMonth;
			_Stream % m_PublicIPPricePerHour;
			_Stream % m_NATGatewayPricePerHour;
			_Stream % m_RegionalNATGatewayPricePerHour;
			_Stream % m_NATGatewayDataProcessedPerGB;
			_Stream % m_RegionalNATGatewayDataProcessedPerGB;
		}

		NContainer::TCMap<NStr::CStr, fp64> m_InstancePrices;	// Instance type -> hourly price
		// Reserved prices: Key = "{InstanceType}|{Term}|{Class}|{Payment}"
		NContainer::TCMap<NStr::CStr, fp64> m_ReservedPrices;
		// Instance metadata (vCPUs, memory)
		NContainer::TCMap<NStr::CStr, CInstanceInfo> m_InstanceInfo;
		fp64 m_EBSPricePerGBMonth = 0;
		fp64 m_GP3IOPSPricePerMonth = 0;					// GP3 IOPS cost per IOPS/month (above 3000 baseline)
		fp64 m_GP3ThroughputPricePerMonth = 0;				// GP3 throughput cost per MB/s/month (above 125 baseline)
		fp64 m_SnapshotPricePerGBMonth = 0;
		fp64 m_PublicIPPricePerHour = 0;
		fp64 m_NATGatewayPricePerHour = 0;					// Per-AZ NAT Gateway hourly
		fp64 m_RegionalNATGatewayPricePerHour = 0;			// Regional NAT Gateway hourly (per active AZ)
		fp64 m_NATGatewayDataProcessedPerGB = 0;			// Per-AZ NAT Gateway data processing
		fp64 m_RegionalNATGatewayDataProcessedPerGB = 0;	// Regional NAT Gateway data processing
	};

	// Cached pricing data with timestamp for 24-hour expiration
	struct CAwsPricingDataCached
	{
		enum : uint32
		{
			EVersion_Initial = 1
			, EVersion_InstanceInfo = 2			// Added instance metadata (vCPUs, memory)
			, EVersion_Architecture = 3			// Added CPU architecture
			, EVersion_CurrentGeneration = 4	// Added current generation flag
			, EVersion_GP3Pricing = 5			// Added GP3 IOPS and throughput pricing
			, EVersion_Current = EVersion_GP3Pricing
		};

		template <typename tf_CStream>
		void f_Stream(tf_CStream &_Stream)
		{
			uint32 Version = EVersion_Current;
			_Stream % Version;

			if (Version > EVersion_Current)
				DMibErrorInstance("Unsupported cache version");

			_Stream % m_CacheTime;
			_Stream % m_Pricing;
		}

		NTime::CTime m_CacheTime;
		CAwsPricingData m_Pricing;
	};

	// Main function to configure the bootstrap deployment
	// Shows an interactive UI with buttons for configuration options
	// and a table showing the deployment layout and costs
	TCFuture<CBootstrapConfig> fg_ConfigureBootstrapDeployment
		(
			TCSharedPointer<CCommandLineControl> _pCommandLine
			, NWeb::CAwsCredentials _Credentials
			, CStr _Region
			, CStr _RootDirectory
		)
	;

	// Fetch pricing information from AWS Pricing API (with caching)
	TCFuture<CAwsPricingData> fg_FetchAwsPricing
		(
			TCSharedPointer<CCommandLineControl> _pCommandLine
			, NWeb::CAwsCredentials _Credentials
			, CStr _Region
			, CStr _RootDirectory
		)
	;
}
