// Copyright © 2020 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Concurrency/LogError>

#include "Malterlib_Cloud_HostMonitor.h"
#include "Malterlib_Cloud_HostMonitor_Internal.h"

namespace NMib::NCloud
{
	auto CHostMonitor::CMonitorPathOptions::f_Tuple() const
	{
		return fg_TupleReferences(m_Path, m_WarnFree, m_CriticalFree, m_WarnFreePercent, m_CriticalFreePercent);
	}

	bool CHostMonitor::CMonitorPathOptions::operator == (CMonitorPathOptions const &_Other) const
	{
		return f_Tuple() == _Other.f_Tuple();
	}

	TCFuture<CActorSubscription> CHostMonitor::f_MonitorPath(CMonitorPathOptions const &_Options)
	{
		auto OnResume = g_OnResume / [&]
			{
				if (f_IsDestroyed())
					DMibError("Shutting down");
			}
		;

		auto &Internal = *mp_pInternal;

		auto &MonitoredPath = Internal.m_MonitoredPaths[_Options.m_Path];

		bool bUpdateReporter = false;
		++MonitoredPath.m_RefCount;

		if (MonitoredPath.m_RefCount == 1 || MonitoredPath.m_Options != _Options)
		{
			MonitoredPath.m_Options = _Options;
			bUpdateReporter = true;
		}

		if (bUpdateReporter)
		{
			auto fGetSensorInfo = [&](CStr const &_Identifier, CStr const &_Name, CInternal::ESensorType _Type)
				{
					CDistributedAppSensorReporter::CSensorInfo SensorInfo;
					SensorInfo.m_Identifier = "org.malterlib.diskspace.{}"_f << _Identifier;
					SensorInfo.m_IdentifierScope = _Options.m_Path;
					SensorInfo.m_Name = "{} ({})"_f << _Name << _Options.m_Path;
					if (Internal.m_HostMonitorInterval != 0.0)
						SensorInfo.m_ExpectedReportInterval = Internal.m_HostMonitorInterval;

					switch (_Type)
					{
					case CInternal::ESensorType_Free:
					case CInternal::ESensorType_Total:
						SensorInfo.m_Type = CDistributedAppSensorReporter::ESensorDataType_Integer;
						SensorInfo.m_UnitDivisors = CDistributedAppSensorReporter::fs_DiskSpaceDivisors();
						break;
					case CInternal::ESensorType_FreePercent:
						SensorInfo.m_Type = CDistributedAppSensorReporter::ESensorDataType_Float;
						{
							auto &UnitDivisor = SensorInfo.m_UnitDivisors[NConcurrency::CDistributedAppSensorReporter::CSensorData(fp64(0.0))];
							UnitDivisor.m_nDecimals = 1;
							UnitDivisor.m_UnitFormatter = "{fn1} %";
						}
						break;
					}

					constexpr auto c_LessThan = CDistributedAppSensorReporter::EValueComparisonOperator_LessThan;

					switch (_Type)
					{
					case CInternal::ESensorType_Free:
						if (_Options.m_WarnFree != TCLimitsInt<uint64>::mc_Max)
							SensorInfo.m_WarnValue = CDistributedAppSensorReporter::CValueComparison{_Options.m_WarnFree, c_LessThan};

						if (_Options.m_CriticalFree != TCLimitsInt<uint64>::mc_Max)
							SensorInfo.m_CriticalValue = CDistributedAppSensorReporter::CValueComparison{_Options.m_CriticalFree, c_LessThan};
						break;
					case CInternal::ESensorType_FreePercent:
						if (_Options.m_WarnFreePercent != fp32::fs_Inf())
							SensorInfo.m_WarnValue = CDistributedAppSensorReporter::CValueComparison{_Options.m_WarnFreePercent, c_LessThan};

						if (_Options.m_CriticalFreePercent != fp32::fs_Inf())
							SensorInfo.m_CriticalValue = CDistributedAppSensorReporter::CValueComparison{_Options.m_CriticalFreePercent, c_LessThan};
						break;
					case CInternal::ESensorType_Total:
						break;
					}

					return SensorInfo;
				}
			;

			{
				auto SensorInfo = fGetSensorInfo("free", "Free Disk Space", CInternal::ESensorType_Free);
				auto Reporter = co_await Internal.m_SensorStore(&CDistributedAppSensorStoreLocal::f_OpenSensorReporter, fg_Move(SensorInfo));

				if (MonitoredPath.m_FreeReporter)
					fg_Move(MonitoredPath.m_FreeReporter->m_fReportReadings).f_Destroy() > fg_LogError("Malterlib/Cloud/HostMonitor", "Failed to destroy previous reporter");

				MonitoredPath.m_FreeReporter = fg_Move(Reporter);
			}
			{
				auto SensorInfo = fGetSensorInfo("freepercent", "Free Disk Space %", CInternal::ESensorType_FreePercent);
				auto Reporter = co_await Internal.m_SensorStore(&CDistributedAppSensorStoreLocal::f_OpenSensorReporter, fg_Move(SensorInfo));

				if (MonitoredPath.m_FreePercentReporter)
					fg_Move(MonitoredPath.m_FreePercentReporter->m_fReportReadings).f_Destroy() > fg_LogError("Malterlib/Cloud/HostMonitor", "Failed to destroy previous reporter");

				MonitoredPath.m_FreePercentReporter = fg_Move(Reporter);
			}
			{
				auto SensorInfo = fGetSensorInfo("total", "Total Disk Space", CInternal::ESensorType_Total);
				auto Reporter = co_await Internal.m_SensorStore(&CDistributedAppSensorStoreLocal::f_OpenSensorReporter, fg_Move(SensorInfo));

				if (MonitoredPath.m_TotalReporter)
					fg_Move(MonitoredPath.m_TotalReporter->m_fReportReadings).f_Destroy() > fg_LogError("Malterlib/Cloud/HostMonitor", "Failed to destroy previous reporter");

				MonitoredPath.m_TotalReporter = fg_Move(Reporter);
			}
		}

		co_await fg_CallSafe(Internal, &CInternal::f_PeriodicUpdate_Diskspace, false);

		co_return g_ActorSubscription / [this, Path = _Options.m_Path]() -> TCFuture<void>
			{
				auto &Internal = *mp_pInternal;

				auto *pMonitoredPath = Internal.m_MonitoredPaths.f_FindEqual(Path);
				if (!pMonitoredPath)
					co_return {};

				auto &MonitoredPath = *pMonitoredPath;

				if (--MonitoredPath.m_RefCount > 0)
					co_return {};

				auto DestroyResult = co_await MonitoredPath.f_Destroy().f_Wrap();
				if (!DestroyResult)
					DMibLogWithCategory(Malterlib/Cloud/HostMonitor, Error, "Failed to destroy reporters for '{}': {}", Path, DestroyResult.f_GetExceptionStr());

				Internal.m_MonitoredPaths.f_Remove(pMonitoredPath);

				co_return {};
			}
		;
	}

	TCFuture<void> CHostMonitor::CInternal::f_PeriodicUpdate_Diskspace_UpdateMounts()
	{
		auto OnResume = g_OnResume / [&]
			{
				if (m_pThis->f_IsDestroyed())
					DMibError("Shutting down");
			}
		;

		auto Mounts = co_await
			(
				g_Dispatch(m_FileActor) / []
				{
					return CFile::fs_GetMounts(EFileMountType_Block | EFileMountType_Local | EFileMountType_Remote);
				}
			)
		;

		auto MountsToDelete = m_AutomaticMounts.f_KeySet();
		for (auto &Mount : Mounts)
		{
			MountsToDelete.f_Remove(Mount);

			auto Mapping = m_AutomaticMounts(Mount);
			if (!Mapping.f_WasCreated())
				continue;

			auto &AutomaticMount = *Mapping;

			CMonitorPathOptions MonitorOptions;
			MonitorOptions.m_Path = Mount;
			AutomaticMount.m_MonitorPathSubscription = co_await m_pThis->self(&CHostMonitor::f_MonitorPath, MonitorOptions);
		}

		for (auto &MountToDelete : MountsToDelete)
		{
			auto pAutomaticMount = m_AutomaticMounts.f_FindEqual(MountToDelete);
			DMibCheck(pAutomaticMount);
			if (!pAutomaticMount)
				continue;

			if (pAutomaticMount->m_MonitorPathSubscription)
				co_await fg_Exchange(pAutomaticMount->m_MonitorPathSubscription, nullptr)->f_Destroy();

			m_AutomaticMounts.f_Remove(pAutomaticMount);
		}

		co_return {};
	}

	TCFuture<void> CHostMonitor::CInternal::f_PeriodicUpdate_Diskspace(bool _bCanSkip)
	{
		auto OnResume = g_OnResume / [&]
			{
				if (m_pThis->f_IsDestroyed())
					DMibError("Shutting down");
			}
		;

		if (_bCanSkip && m_UpdatePeriodicDiskSpaceSequencer.f_NumWaiting() > 0)
			co_return {}; // Already waiting for update

		auto SequenceSubscription = co_await m_UpdatePeriodicDiskSpaceSequencer.f_Sequence();

		if (m_Flags & EInitFlag_MonitorAllMounts)
		{
			auto Result = co_await fg_CallSafe(*this, &CInternal::f_PeriodicUpdate_Diskspace_UpdateMounts).f_Wrap();
			if (!Result)
				DMibLogWithCategory(Malterlib/Cloud/HostMonitor, Error, "Failed update disk space monitoring for mounts: {}", Result.f_GetExceptionStr());
		}

		struct CPathInfo
		{
			uint64 m_FreeSpace = 0;
			uint64 m_TotalSpace = 0;
		};

		auto FreeDiskSpace = co_await
			(
				(
					g_Dispatch(m_FileActor) / [Paths = m_MonitoredPaths.f_KeySet()]() -> TCMap<CStr, TCAsyncResult<CPathInfo>>
					{
						TCMap<CStr, TCAsyncResult<CPathInfo>> Return;

						for (auto &Path : Paths)
						{
							try
							{
								uint64 FreeSpace = CFile::fs_GetFreeSpace(Path);
								uint64 TotalSpace = CFile::fs_GetTotalSpace(Path);

								Return[Path].f_SetResult(CPathInfo{FreeSpace, TotalSpace});
							}
							catch (CException const &_Exception)
							{
								Return[Path].f_SetException(_Exception.f_ExceptionPointer());
							}
						}

						return Return;
					}
				).f_Timeout(60.0, "Timed out getting disk space").f_Wrap()
			)
		;

		if (!FreeDiskSpace)
		{
			if (FreeDiskSpace.f_HasExceptionType<CExceptionAsyncTimeout>())
			{
				fg_Move(m_FileActor).f_Destroy() > fg_DiscardResult(); // Hope that the operation will finish eventually
				m_FileActor = fg_Construct(fg_Construct(), "Host monitor file actor {}"_f << m_FileActorSequence++);
			}

			co_return FreeDiskSpace.f_GetException();
		}

		TCActorResultVector<void> ReportResults;

		for (auto &PathInfoResult : *FreeDiskSpace)
		{
			auto &Path = FreeDiskSpace->fs_GetKey(PathInfoResult);

			if (!PathInfoResult)
			{
				DMibLogWithCategory(Malterlib/Cloud/HostMonitor, Error, "Failed to get free disk space for path '{}': {}", Path, PathInfoResult.f_GetExceptionStr());
				continue;
			}

			auto &PathInfo = *PathInfoResult;

			auto *pMonitoredPath = m_MonitoredPaths.f_FindEqual(Path);
			if (!pMonitoredPath)
				continue;

			if (pMonitoredPath->m_FreeReporter)
			{
				CDistributedAppSensorReporter::CSensorReading Reading;
				Reading.m_Data = PathInfo.m_FreeSpace;

				pMonitoredPath->m_FreeReporter->m_fReportReadings(TCVector<CDistributedAppSensorReporter::CSensorReading>{fg_Move(Reading)})
					% ("Failed to report readings (free) for path '{}'"_f << Path)
					> ReportResults.f_AddResult()
				;
			}

			if (pMonitoredPath->m_TotalReporter)
			{
				CDistributedAppSensorReporter::CSensorReading Reading;
				Reading.m_Data = PathInfo.m_TotalSpace;

				pMonitoredPath->m_TotalReporter->m_fReportReadings(TCVector<CDistributedAppSensorReporter::CSensorReading>{fg_Move(Reading)})
					% ("Failed to report readings (total) for path '{}'"_f << Path)
					> ReportResults.f_AddResult()
				;
			}

			if (pMonitoredPath->m_FreePercentReporter)
			{
				CDistributedAppSensorReporter::CSensorReading Reading;
				Reading.m_Data = fp64(PathInfo.m_FreeSpace) / fp64(PathInfo.m_TotalSpace) * fp64(100.0);

				pMonitoredPath->m_FreePercentReporter->m_fReportReadings(TCVector<CDistributedAppSensorReporter::CSensorReading>{fg_Move(Reading)})
					% ("Failed to report readings (free %) for path '{}'"_f << Path)
					> ReportResults.f_AddResult()
				;
			}
		}

		co_await ReportResults.f_GetResults() | g_Unwrap;

		co_return {};
	}
}
