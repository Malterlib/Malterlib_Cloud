// Copyright © 2023 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Concurrency/LogError>
#include <Mib/Process/ProcessLaunchActor>

#include "Malterlib_Cloud_HostMonitor.h"
#include "Malterlib_Cloud_HostMonitor_Internal.h"

namespace NMib::NCloud
{
	using namespace NHostMonitorDatabase;
	using namespace NProcess;
	using namespace NMemory;

	namespace
	{
		CDistributedAppSensorReporter::EValueComparisonOperator fg_ToSensorReporterComparisonOperator(EMemoryStatisticComparisonOperator _Operator)
		{
			switch (_Operator)
			{
			case EMemoryStatisticComparisonOperator::mc_None: break;
			case EMemoryStatisticComparisonOperator::mc_LessThan: return CDistributedAppSensorReporter::EValueComparisonOperator_LessThan;
			case EMemoryStatisticComparisonOperator::mc_GreaterThan: return CDistributedAppSensorReporter::EValueComparisonOperator_GreaterThan;
			case EMemoryStatisticComparisonOperator::mc_Equal: return CDistributedAppSensorReporter::EValueComparisonOperator_Equal;
			}

			return CDistributedAppSensorReporter::EValueComparisonOperator_LessThan;
		}
	}

	TCFuture<void> CHostMonitor::CInternal::f_PeriodicUpdate_Memory(bool _bCanSkip)
	{
		if (!m_Config.m_MemoryInterval)
			co_return {};

		auto OnResume = co_await m_pThis->f_CheckDestroyedOnResume();

		auto SequenceSubscription = co_await m_UpdatePeriodicMemory.f_Sequence();

		bool bIsOverTime = m_MemoryClock && (*m_MemoryClock).f_GetTime() >= m_Config.m_MemoryInterval;

		if (!m_MemoryClock)
			m_MemoryClock = CClock{true};
		else if (_bCanSkip && !bIsOverTime)
			co_return {};

		if (bIsOverTime)
			(*m_MemoryClock).f_AddOffset(m_Config.m_MemoryInterval);

		CLogError LogError("Malterlib/Cloud/HostMonitor");

		auto CaptureScope = co_await (g_CaptureExceptions % "Failed to get memory statistics");

		auto MemoryStats = NMemory::NPlatform::fg_Memory_GetStatistics(EMemoryStatisticsDetailLevel::mc_Basic);

		TCActorResultVector<void> ReportResults;

		for (auto &StatisticEntry : MemoryStats.m_Statistics.f_Entries())
		{
			auto &StatisticName = StatisticEntry.f_Key();
			auto &Statistic = StatisticEntry.f_Value();
			auto &Reporter = m_MemoryReporters[StatisticName];
			if (!Reporter.m_SensorReporter || Reporter.m_Characteristics != Statistic.m_Characteristics)
			{
				auto &Characteristics = Statistic.m_Characteristics;

				CDistributedAppSensorReporter::CSensorInfo SensorInfo;
				SensorInfo.m_Identifier = "org.malterlib.os.memory.{}"_f << StatisticName;
				SensorInfo.m_Name = "Memory ({})"_f << StatisticName;
				SensorInfo.m_Type = CDistributedAppSensorReporter::ESensorDataType_Integer;
				SensorInfo.m_MetaData = m_SensorMetaData;
				if (m_Config.m_MemoryInterval != 0.0)
					SensorInfo.m_ExpectedReportInterval = m_Config.m_MemoryInterval;

				if (Characteristics.m_Unit == EMemoryStatisticUnit::mc_Bytes)
					SensorInfo.m_UnitDivisors = CDistributedAppSensorReporter::fs_BytesDivisors();

				if (Characteristics.m_ValueComparisonOperator != EMemoryStatisticComparisonOperator::mc_None)
				{
					auto ReporterOperator = fg_ToSensorReporterComparisonOperator(Characteristics.m_ValueComparisonOperator);
					SensorInfo.m_WarnValue = CDistributedAppSensorReporter::CValueComparison{.m_CompareToValue = Characteristics.m_WarningValue, .m_Operator = ReporterOperator};
					SensorInfo.m_CriticalValue = CDistributedAppSensorReporter::CValueComparison{.m_CompareToValue = Characteristics.m_ErrorValue, .m_Operator = ReporterOperator};
				}

				Reporter.m_SensorReporter = co_await m_SensorStore(&CDistributedAppSensorStoreLocal::f_OpenSensorReporter, fg_Move(SensorInfo));
				Reporter.m_Characteristics = Statistic.m_Characteristics;
			}

			CDistributedAppSensorReporter::CSensorReading Reading;
			Reading.m_Data = Statistic.m_Value;

			Reporter.m_SensorReporter->m_fReportReadings(TCVector<CDistributedAppSensorReporter::CSensorReading>{fg_Move(Reading)})
				% ("Failed to report memory readings for '{}'"_f << StatisticName)
				> ReportResults.f_AddResult()
			;
		}

		co_await ReportResults.f_GetUnwrappedResults();

		co_return {};
	}
}
