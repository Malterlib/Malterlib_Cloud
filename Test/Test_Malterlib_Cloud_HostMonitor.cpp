// Copyright © 2024 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Cloud/HostMonitor>

using namespace NMib;
using namespace NMib::NCloud;
using namespace NMib::NTime;
using namespace NMib::NStr;

class CHostMonitor_Tests : public NMib::NTest::CTest
{
public:
	static CTime fs_Time(CStr const &_Time)
	{
		uint32 Year;
		uint32 Month;
		uint32 DayOfMonth;
		uint32 Hour;
		uint32 Minute;
		uint32 Second = 0;

		aint nParsed = 0;
		(CStr::CParse("{}-{}-{} {}:{}:{}") >> Year >> Month >> DayOfMonth >> Hour >> Minute).f_Parse(_Time, nParsed);
		DMibCheck(nParsed >= 5)(nParsed)(_Time);

		return CTimeConvert::fs_CreateTime(Year, Month, DayOfMonth, Hour, Minute, Second);
	}

	CTime f_Next(CHostMonitorRebootSchedule const &_Schedule, CTime const &_StartTime)
	{
		if (m_bTestingLocal)
			return _Schedule.f_GetNextRebootTime(_StartTime.f_ToUTC(), m_bTestingLocal).f_ToLocal();
		else
			return _Schedule.f_GetNextRebootTime(_StartTime, m_bTestingLocal);
	}

	void f_TestTimeZone(bool _bLocal)
	{
		m_bTestingLocal = _bLocal;

		// None
		DMibExpect(f_Next({}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-07 11:22"));
		DMibExpect(f_Next({}, fs_Time("2024-10-07 11:22:20")), ==, fs_Time("2024-10-07 11:22"));

		// Minute
		DMibExpect(f_Next({.m_Minute = uint8(0)},  fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-07 12:00"));
		DMibExpect(f_Next({.m_Minute = uint8(22)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-07 11:22"));
		DMibExpect(f_Next({.m_Minute = uint8(40)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-07 11:40"));

		// Hour
		DMibExpect(f_Next({.m_Hour = uint8(10)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-8 10:00"));
		DMibExpect(f_Next({.m_Hour = uint8(11)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-7 11:22"));
		DMibExpect(f_Next({.m_Hour = uint8(20)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-7 20:00"));

		// DayOfMonth
		DMibExpect(f_Next({.m_DayOfMonth = uint8(6)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-11-06 00:00"));
		DMibExpect(f_Next({.m_DayOfMonth = uint8(7)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-07 11:22"));
		DMibExpect(f_Next({.m_DayOfMonth = uint8(8)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-08 00:00"));
		DMibExpect(f_Next({.m_DayOfMonth = uint8(31)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-31 00:00"));
		DMibExpect(f_Next({.m_DayOfMonth = uint8(31)}, fs_Time("2024-11-07 11:22")), ==, fs_Time("2024-12-31 00:00"));

		// DayOfMonth + DayOfWeek
		DMibExpect(f_Next({.m_DayOfMonth = uint8(6), .m_DayOfWeek = uint8(6)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-11-06 00:00"));
		DMibExpect(f_Next({.m_DayOfMonth = uint8(7), .m_DayOfWeek = uint8(0)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-07 11:22"));
		DMibExpect(f_Next({.m_DayOfMonth = uint8(8), .m_DayOfWeek = uint8(1)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-08 00:00"));

		// DayOfWeek
		DMibExpect(f_Next({.m_DayOfWeek = uint8(6)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-13 00:00"));
		DMibExpect(f_Next({.m_DayOfWeek = uint8(0)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-07 11:22"));
		DMibExpect(f_Next({.m_DayOfWeek = uint8(1)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-08 00:00"));

		// Month
		DMibExpect(f_Next({.m_Month = uint8(9)},  fs_Time("2024-10-07 11:22")), ==, fs_Time("2025-09-01 00:00"));
		DMibExpect(f_Next({.m_Month = uint8(10)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-07 11:22"));
		DMibExpect(f_Next({.m_Month = uint8(11)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-11-01 00:00"));

		// Month Hour
		DMibExpect(f_Next({.m_Month = uint8(9),  .m_Hour = uint8(10)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2025-09-01 10:00"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_Hour = uint8(10)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-08 10:00"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_Hour = uint8(11)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-07 11:22"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_Hour = uint8(12)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-07 12:00"));
		DMibExpect(f_Next({.m_Month = uint8(11), .m_Hour = uint8(10)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-11-01 10:00"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_Hour = uint8(10)}, fs_Time("2024-10-31 11:22")), ==, fs_Time("2025-10-01 10:00"));

		// Month Minute
		DMibExpect(f_Next({.m_Month = uint8(9), .m_Minute = uint8(30)},  fs_Time("2024-10-07 11:22")), ==, fs_Time("2025-09-01 00:30"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_Minute = uint8(22)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-07 11:22"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_Minute = uint8(30)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-07 11:30"));
		DMibExpect(f_Next({.m_Month = uint8(11), .m_Minute = uint8(30)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-11-01 00:30"));

		// Month DayOfMonth
		DMibExpect(f_Next({.m_Month = uint8(9), .m_DayOfMonth = uint8(22)},  fs_Time("2024-10-07 11:22")), ==, fs_Time("2025-09-22 00:00"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_DayOfMonth = uint8(7)},  fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-07 11:22"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_DayOfMonth = uint8(22)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-22 00:00"));
		DMibExpect(f_Next({.m_Month = uint8(11), .m_DayOfMonth = uint8(22)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-11-22 00:00"));
		DMibExpect(f_Next({.m_Month = uint8(2), .m_DayOfMonth = uint8(29)},  fs_Time("2024-10-07 11:22")), ==, fs_Time("2028-02-29 00:00"));

		// Month DayOfWeek
		DMibExpect(f_Next({.m_Month = uint8(9), .m_DayOfWeek = uint8(5)},  fs_Time("2024-10-07 11:22")), ==, fs_Time("2025-09-06 00:00"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_DayOfWeek = uint8(0)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-07 11:22"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_DayOfWeek = uint8(1)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-08 00:00"));
		DMibExpect(f_Next({.m_Month = uint8(11), .m_DayOfWeek = uint8(0)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-11-04 00:00"));

		// DayOfMonth Hour
		DMibExpect(f_Next({.m_DayOfMonth = uint8(6), .m_Hour = uint8(10)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-11-06 10:00"));
		DMibExpect(f_Next({.m_DayOfMonth = uint8(7), .m_Hour = uint8(10)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-11-07 10:00"));
		DMibExpect(f_Next({.m_DayOfMonth = uint8(7), .m_Hour = uint8(11)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-07 11:22"));
		DMibExpect(f_Next({.m_DayOfMonth = uint8(7), .m_Hour = uint8(12)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-07 12:00"));
		DMibExpect(f_Next({.m_DayOfMonth = uint8(8), .m_Hour = uint8(10)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-08 10:00"));

		// DayOfWeek Hour
		DMibExpect(f_Next({.m_DayOfWeek = uint8(6), .m_Hour = uint8(10)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-13 10:00"));
		DMibExpect(f_Next({.m_DayOfWeek = uint8(0), .m_Hour = uint8(10)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-14 10:00"));
		DMibExpect(f_Next({.m_DayOfWeek = uint8(0), .m_Hour = uint8(11)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-07 11:22"));
		DMibExpect(f_Next({.m_DayOfWeek = uint8(0), .m_Hour = uint8(12)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-07 12:00"));
		DMibExpect(f_Next({.m_DayOfWeek = uint8(1), .m_Hour = uint8(10)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-08 10:00"));

		// DayOfMonth Minute
		DMibExpect(f_Next({.m_DayOfMonth = uint8(6), .m_Minute = uint8(20)},  fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-11-06 00:20"));
		DMibExpect(f_Next({.m_DayOfMonth = uint8(7), .m_Minute = uint8(20)},  fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-07 12:20"));
		DMibExpect(f_Next({.m_DayOfMonth = uint8(7), .m_Minute = uint8(22)},  fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-07 11:22"));
		DMibExpect(f_Next({.m_DayOfMonth = uint8(7), .m_Minute = uint8(24)},  fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-07 11:24"));
		DMibExpect(f_Next({.m_DayOfMonth = uint8(7), .m_Minute = uint8(20)},  fs_Time("2024-10-07 23:22")), ==, fs_Time("2024-11-07 00:20"));
		DMibExpect(f_Next({.m_DayOfMonth = uint8(8), .m_Minute = uint8(20)},  fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-08 00:20"));
		DMibExpect(f_Next({.m_DayOfMonth = uint8(31), .m_Minute = uint8(20)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-31 00:20"));
		DMibExpect(f_Next({.m_DayOfMonth = uint8(31), .m_Minute = uint8(20)}, fs_Time("2024-11-07 11:22")), ==, fs_Time("2024-12-31 00:20"));

		// DayOfWeek Minute
		DMibExpect(f_Next({.m_DayOfWeek = uint8(6), .m_Minute = uint8(20)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-13 00:20"));
		DMibExpect(f_Next({.m_DayOfWeek = uint8(0), .m_Minute = uint8(20)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-07 12:20"));
		DMibExpect(f_Next({.m_DayOfWeek = uint8(0), .m_Minute = uint8(20)}, fs_Time("2024-10-07 23:50")), ==, fs_Time("2024-10-14 00:20"));
		DMibExpect(f_Next({.m_DayOfWeek = uint8(1), .m_Minute = uint8(20)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-08 00:20"));

		// Hour Minute
		DMibExpect(f_Next({.m_Hour = uint8(10), .m_Minute = uint8(40)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-08 10:40"));
		DMibExpect(f_Next({.m_Hour = uint8(11), .m_Minute = uint8(40)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-07 11:40"));
		DMibExpect(f_Next({.m_Hour = uint8(11), .m_Minute = uint8(22)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-07 11:22"));
		DMibExpect(f_Next({.m_Hour = uint8(10), .m_Minute = uint8(22)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-08 10:22"));
		DMibExpect(f_Next({.m_Hour = uint8(20), .m_Minute = uint8(40)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-07 20:40"));
		DMibExpect(f_Next({.m_Hour = uint8(11), .m_Minute = uint8(30)}, fs_Time("2024-10-07 11:59")), ==, fs_Time("2024-10-08 11:30"));

		// Month Hour Minute
		DMibExpect(f_Next({.m_Month = uint8(9),  .m_Hour = uint8(10), .m_Minute = uint8(40)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2025-09-01 10:40"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_Hour = uint8(10), .m_Minute = uint8(40)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-08 10:40"));
		DMibExpect(f_Next({.m_Month = uint8(11), .m_Hour = uint8(10), .m_Minute = uint8(40)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-11-01 10:40"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_Hour = uint8(10), .m_Minute = uint8(40)}, fs_Time("2024-10-31 11:22")), ==, fs_Time("2025-10-01 10:40"));

		// Month DayOfMonth Hour
		DMibExpect(f_Next({.m_Month = uint8(9), .m_DayOfMonth = uint8(22), .m_Hour = uint8(12)},  fs_Time("2024-10-07 11:22")), ==, fs_Time("2025-09-22 12:00"));
		DMibExpect(f_Next({.m_Month = uint8(9), .m_DayOfMonth = uint8(22), .m_Hour = uint8(10)},  fs_Time("2024-10-07 11:22")), ==, fs_Time("2025-09-22 10:00"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_DayOfMonth = uint8(7), .m_Hour = uint8(11)},  fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-07 11:22"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_DayOfMonth = uint8(7), .m_Hour = uint8(12)},  fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-07 12:00"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_DayOfMonth = uint8(7), .m_Hour = uint8(10)},  fs_Time("2024-10-07 11:22")), ==, fs_Time("2025-10-07 10:00"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_DayOfMonth = uint8(22), .m_Hour = uint8(12)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-22 12:00"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_DayOfMonth = uint8(22), .m_Hour = uint8(10)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-22 10:00"));
		DMibExpect(f_Next({.m_Month = uint8(11), .m_DayOfMonth = uint8(22), .m_Hour = uint8(12)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-11-22 12:00"));
		DMibExpect(f_Next({.m_Month = uint8(11), .m_DayOfMonth = uint8(22), .m_Hour = uint8(10)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-11-22 10:00"));
		DMibExpect(f_Next({.m_Month = uint8(2), .m_DayOfMonth = uint8(29), .m_Hour = uint8(12)},  fs_Time("2024-10-07 11:22")), ==, fs_Time("2028-02-29 12:00"));
		DMibExpect(f_Next({.m_Month = uint8(2), .m_DayOfMonth = uint8(29), .m_Hour = uint8(10)},  fs_Time("2024-10-07 11:22")), ==, fs_Time("2028-02-29 10:00"));

		// Month DayOfWeek Hour
		DMibExpect(f_Next({.m_Month = uint8(9), .m_DayOfWeek = uint8(5), .m_Hour = uint32(12)},  fs_Time("2024-10-07 11:22")), ==, fs_Time("2025-09-06 12:00"));
		DMibExpect(f_Next({.m_Month = uint8(9), .m_DayOfWeek = uint8(5), .m_Hour = uint32(10)},  fs_Time("2024-10-07 11:22")), ==, fs_Time("2025-09-06 10:00"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_DayOfWeek = uint8(0), .m_Hour = uint32(11)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-07 11:22"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_DayOfWeek = uint8(0), .m_Hour = uint32(12)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-07 12:00"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_DayOfWeek = uint8(0), .m_Hour = uint32(10)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-14 10:00"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_DayOfWeek = uint8(1), .m_Hour = uint32(12)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-08 12:00"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_DayOfWeek = uint8(1), .m_Hour = uint32(10)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-08 10:00"));
		DMibExpect(f_Next({.m_Month = uint8(11), .m_DayOfWeek = uint8(0), .m_Hour = uint32(12)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-11-04 12:00"));
		DMibExpect(f_Next({.m_Month = uint8(11), .m_DayOfWeek = uint8(0), .m_Hour = uint32(10)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-11-04 10:00"));

		// Month DayOfMonth Minute
		DMibExpect(f_Next({.m_Month = uint8(9), .m_DayOfMonth = uint8(22), .m_Minute = uint8(23)},  fs_Time("2024-10-07 11:22")), ==, fs_Time("2025-09-22 00:23"));
		DMibExpect(f_Next({.m_Month = uint8(9), .m_DayOfMonth = uint8(22), .m_Minute = uint8(21)},  fs_Time("2024-10-07 11:22")), ==, fs_Time("2025-09-22 00:21"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_DayOfMonth = uint8(7), .m_Minute = uint8(22)},  fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-07 11:22"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_DayOfMonth = uint8(7), .m_Minute = uint8(23)},  fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-07 11:23"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_DayOfMonth = uint8(7), .m_Minute = uint8(21)},  fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-07 12:21"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_DayOfMonth = uint8(7), .m_Minute = uint8(21)},  fs_Time("2024-10-07 23:22")), ==, fs_Time("2025-10-07 00:21"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_DayOfMonth = uint8(22), .m_Minute = uint8(23)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-22 00:23"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_DayOfMonth = uint8(22), .m_Minute = uint8(21)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-22 00:21"));
		DMibExpect(f_Next({.m_Month = uint8(11), .m_DayOfMonth = uint8(22), .m_Minute = uint8(23)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-11-22 00:23"));
		DMibExpect(f_Next({.m_Month = uint8(11), .m_DayOfMonth = uint8(22), .m_Minute = uint8(21)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-11-22 00:21"));
		DMibExpect(f_Next({.m_Month = uint8(2), .m_DayOfMonth = uint8(29), .m_Minute = uint8(23)},  fs_Time("2024-10-07 11:22")), ==, fs_Time("2028-02-29 00:23"));
		DMibExpect(f_Next({.m_Month = uint8(2), .m_DayOfMonth = uint8(29), .m_Minute = uint8(21)},  fs_Time("2024-10-07 11:22")), ==, fs_Time("2028-02-29 00:21"));

		// Month DayOfWeek Minute
		DMibExpect(f_Next({.m_Month = uint8(9), .m_DayOfWeek = uint8(5), .m_Minute = uint32(23)},  fs_Time("2024-10-07 11:22")), ==, fs_Time("2025-09-06 00:23"));
		DMibExpect(f_Next({.m_Month = uint8(9), .m_DayOfWeek = uint8(5), .m_Minute = uint32(21)},  fs_Time("2024-10-07 11:22")), ==, fs_Time("2025-09-06 00:21"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_DayOfWeek = uint8(0), .m_Minute = uint32(22)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-07 11:22"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_DayOfWeek = uint8(0), .m_Minute = uint32(23)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-07 11:23"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_DayOfWeek = uint8(0), .m_Minute = uint32(21)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-07 12:21"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_DayOfWeek = uint8(0), .m_Minute = uint32(21)}, fs_Time("2024-10-07 23:22")), ==, fs_Time("2024-10-14 00:21"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_DayOfWeek = uint8(1), .m_Minute = uint32(23)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-08 00:23"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_DayOfWeek = uint8(1), .m_Minute = uint32(21)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-08 00:21"));
		DMibExpect(f_Next({.m_Month = uint8(11), .m_DayOfWeek = uint8(0), .m_Minute = uint32(23)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-11-04 00:23"));
		DMibExpect(f_Next({.m_Month = uint8(11), .m_DayOfWeek = uint8(0), .m_Minute = uint32(21)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-11-04 00:21"));

		// DayOfWeek Hour Minute
		DMibExpect(f_Next({.m_DayOfWeek = uint8(6), .m_Hour = uint8(10), .m_Minute = uint32(21)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-13 10:21"));
		DMibExpect(f_Next({.m_DayOfWeek = uint8(6), .m_Hour = uint8(10), .m_Minute = uint32(23)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-13 10:23"));
		DMibExpect(f_Next({.m_DayOfWeek = uint8(0), .m_Hour = uint8(10), .m_Minute = uint32(21)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-14 10:21"));
		DMibExpect(f_Next({.m_DayOfWeek = uint8(0), .m_Hour = uint8(10), .m_Minute = uint32(23)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-14 10:23"));
		DMibExpect(f_Next({.m_DayOfWeek = uint8(0), .m_Hour = uint8(11), .m_Minute = uint32(22)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-07 11:22"));
		DMibExpect(f_Next({.m_DayOfWeek = uint8(0), .m_Hour = uint8(11), .m_Minute = uint32(21)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-14 11:21"));
		DMibExpect(f_Next({.m_DayOfWeek = uint8(0), .m_Hour = uint8(11), .m_Minute = uint32(23)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-07 11:23"));
		DMibExpect(f_Next({.m_DayOfWeek = uint8(0), .m_Hour = uint8(12), .m_Minute = uint32(21)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-07 12:21"));
		DMibExpect(f_Next({.m_DayOfWeek = uint8(0), .m_Hour = uint8(12), .m_Minute = uint32(23)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-07 12:23"));
		DMibExpect(f_Next({.m_DayOfWeek = uint8(1), .m_Hour = uint8(10), .m_Minute = uint32(21)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-08 10:21"));
		DMibExpect(f_Next({.m_DayOfWeek = uint8(1), .m_Hour = uint8(10), .m_Minute = uint32(23)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-08 10:23"));

		// DayOfMonth Hour Minute
		DMibExpect(f_Next({.m_DayOfMonth = uint8(6), .m_Hour = uint8(10), .m_Minute = uint32(21)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-11-06 10:21"));
		DMibExpect(f_Next({.m_DayOfMonth = uint8(6), .m_Hour = uint8(10), .m_Minute = uint32(23)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-11-06 10:23"));
		DMibExpect(f_Next({.m_DayOfMonth = uint8(7), .m_Hour = uint8(10), .m_Minute = uint32(21)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-11-07 10:21"));
		DMibExpect(f_Next({.m_DayOfMonth = uint8(7), .m_Hour = uint8(10), .m_Minute = uint32(23)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-11-07 10:23"));
		DMibExpect(f_Next({.m_DayOfMonth = uint8(7), .m_Hour = uint8(11), .m_Minute = uint32(21)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-11-07 11:21"));
		DMibExpect(f_Next({.m_DayOfMonth = uint8(7), .m_Hour = uint8(11), .m_Minute = uint32(22)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-07 11:22"));
		DMibExpect(f_Next({.m_DayOfMonth = uint8(7), .m_Hour = uint8(11), .m_Minute = uint32(23)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-07 11:23"));
		DMibExpect(f_Next({.m_DayOfMonth = uint8(7), .m_Hour = uint8(12), .m_Minute = uint32(21)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-07 12:21"));
		DMibExpect(f_Next({.m_DayOfMonth = uint8(7), .m_Hour = uint8(12), .m_Minute = uint32(23)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-07 12:23"));
		DMibExpect(f_Next({.m_DayOfMonth = uint8(8), .m_Hour = uint8(10), .m_Minute = uint32(21)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-08 10:21"));
		DMibExpect(f_Next({.m_DayOfMonth = uint8(8), .m_Hour = uint8(10), .m_Minute = uint32(23)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-08 10:23"));

		// Month DayOfWeek Hour Minute
		DMibExpect(f_Next({.m_Month = uint8(9), .m_DayOfWeek = uint8(5), .m_Hour = uint32(12), .m_Minute = uint32(21)},  fs_Time("2024-10-07 11:22")), ==, fs_Time("2025-09-06 12:21"));
		DMibExpect(f_Next({.m_Month = uint8(9), .m_DayOfWeek = uint8(5), .m_Hour = uint32(12), .m_Minute = uint32(23)},  fs_Time("2024-10-07 11:22")), ==, fs_Time("2025-09-06 12:23"));
		DMibExpect(f_Next({.m_Month = uint8(9), .m_DayOfWeek = uint8(5), .m_Hour = uint32(10), .m_Minute = uint32(21)},  fs_Time("2024-10-07 11:22")), ==, fs_Time("2025-09-06 10:21"));
		DMibExpect(f_Next({.m_Month = uint8(9), .m_DayOfWeek = uint8(5), .m_Hour = uint32(10), .m_Minute = uint32(23)},  fs_Time("2024-10-07 11:22")), ==, fs_Time("2025-09-06 10:23"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_DayOfWeek = uint8(0), .m_Hour = uint32(11), .m_Minute = uint32(21)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-14 11:21"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_DayOfWeek = uint8(0), .m_Hour = uint32(11), .m_Minute = uint32(22)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-07 11:22"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_DayOfWeek = uint8(0), .m_Hour = uint32(11), .m_Minute = uint32(23)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-07 11:23"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_DayOfWeek = uint8(0), .m_Hour = uint32(12), .m_Minute = uint32(21)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-07 12:21"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_DayOfWeek = uint8(0), .m_Hour = uint32(12), .m_Minute = uint32(23)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-07 12:23"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_DayOfWeek = uint8(0), .m_Hour = uint32(10), .m_Minute = uint32(21)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-14 10:21"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_DayOfWeek = uint8(0), .m_Hour = uint32(10), .m_Minute = uint32(23)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-14 10:23"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_DayOfWeek = uint8(1), .m_Hour = uint32(12), .m_Minute = uint32(21)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-08 12:21"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_DayOfWeek = uint8(1), .m_Hour = uint32(12), .m_Minute = uint32(23)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-08 12:23"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_DayOfWeek = uint8(1), .m_Hour = uint32(10), .m_Minute = uint32(21)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-08 10:21"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_DayOfWeek = uint8(1), .m_Hour = uint32(10), .m_Minute = uint32(23)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-08 10:23"));
		DMibExpect(f_Next({.m_Month = uint8(11), .m_DayOfWeek = uint8(0), .m_Hour = uint32(12), .m_Minute = uint32(21)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-11-04 12:21"));
		DMibExpect(f_Next({.m_Month = uint8(11), .m_DayOfWeek = uint8(0), .m_Hour = uint32(12), .m_Minute = uint32(23)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-11-04 12:23"));
		DMibExpect(f_Next({.m_Month = uint8(11), .m_DayOfWeek = uint8(0), .m_Hour = uint32(10), .m_Minute = uint32(21)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-11-04 10:21"));
		DMibExpect(f_Next({.m_Month = uint8(11), .m_DayOfWeek = uint8(0), .m_Hour = uint32(10), .m_Minute = uint32(23)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-11-04 10:23"));


		// Month DayOfMonth Hour Minute
		DMibExpect(f_Next({.m_Month = uint8(9), .m_DayOfMonth = uint8(22), .m_Hour = uint8(12), .m_Minute = uint32(21)},  fs_Time("2024-10-07 11:22")), ==, fs_Time("2025-09-22 12:21"));
		DMibExpect(f_Next({.m_Month = uint8(9), .m_DayOfMonth = uint8(22), .m_Hour = uint8(12), .m_Minute = uint32(23)},  fs_Time("2024-10-07 11:22")), ==, fs_Time("2025-09-22 12:23"));
		DMibExpect(f_Next({.m_Month = uint8(9), .m_DayOfMonth = uint8(22), .m_Hour = uint8(10), .m_Minute = uint32(21)},  fs_Time("2024-10-07 11:22")), ==, fs_Time("2025-09-22 10:21"));
		DMibExpect(f_Next({.m_Month = uint8(9), .m_DayOfMonth = uint8(22), .m_Hour = uint8(10), .m_Minute = uint32(23)},  fs_Time("2024-10-07 11:22")), ==, fs_Time("2025-09-22 10:23"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_DayOfMonth = uint8(7), .m_Hour = uint8(11), .m_Minute = uint32(21)},  fs_Time("2024-10-07 11:22")), ==, fs_Time("2025-10-07 11:21"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_DayOfMonth = uint8(7), .m_Hour = uint8(11), .m_Minute = uint32(22)},  fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-07 11:22"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_DayOfMonth = uint8(7), .m_Hour = uint8(11), .m_Minute = uint32(23)},  fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-07 11:23"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_DayOfMonth = uint8(7), .m_Hour = uint8(12), .m_Minute = uint32(21)},  fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-07 12:21"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_DayOfMonth = uint8(7), .m_Hour = uint8(12), .m_Minute = uint32(23)},  fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-07 12:23"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_DayOfMonth = uint8(7), .m_Hour = uint8(10), .m_Minute = uint32(21)},  fs_Time("2024-10-07 11:22")), ==, fs_Time("2025-10-07 10:21"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_DayOfMonth = uint8(7), .m_Hour = uint8(10), .m_Minute = uint32(23)},  fs_Time("2024-10-07 11:22")), ==, fs_Time("2025-10-07 10:23"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_DayOfMonth = uint8(22), .m_Hour = uint8(12), .m_Minute = uint32(21)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-22 12:21"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_DayOfMonth = uint8(22), .m_Hour = uint8(12), .m_Minute = uint32(23)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-22 12:23"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_DayOfMonth = uint8(22), .m_Hour = uint8(10), .m_Minute = uint32(21)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-22 10:21"));
		DMibExpect(f_Next({.m_Month = uint8(10), .m_DayOfMonth = uint8(22), .m_Hour = uint8(10), .m_Minute = uint32(23)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-10-22 10:23"));
		DMibExpect(f_Next({.m_Month = uint8(11), .m_DayOfMonth = uint8(22), .m_Hour = uint8(12), .m_Minute = uint32(21)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-11-22 12:21"));
		DMibExpect(f_Next({.m_Month = uint8(11), .m_DayOfMonth = uint8(22), .m_Hour = uint8(12), .m_Minute = uint32(23)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-11-22 12:23"));
		DMibExpect(f_Next({.m_Month = uint8(11), .m_DayOfMonth = uint8(22), .m_Hour = uint8(10), .m_Minute = uint32(21)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-11-22 10:21"));
		DMibExpect(f_Next({.m_Month = uint8(11), .m_DayOfMonth = uint8(22), .m_Hour = uint8(10), .m_Minute = uint32(23)}, fs_Time("2024-10-07 11:22")), ==, fs_Time("2024-11-22 10:23"));
		DMibExpect(f_Next({.m_Month = uint8(2), .m_DayOfMonth = uint8(29), .m_Hour = uint8(12), .m_Minute = uint32(21)},  fs_Time("2024-10-07 11:22")), ==, fs_Time("2028-02-29 12:21"));
		DMibExpect(f_Next({.m_Month = uint8(2), .m_DayOfMonth = uint8(29), .m_Hour = uint8(12), .m_Minute = uint32(23)},  fs_Time("2024-10-07 11:22")), ==, fs_Time("2028-02-29 12:23"));
		DMibExpect(f_Next({.m_Month = uint8(2), .m_DayOfMonth = uint8(29), .m_Hour = uint8(10), .m_Minute = uint32(21)},  fs_Time("2024-10-07 11:22")), ==, fs_Time("2028-02-29 10:21"));
		DMibExpect(f_Next({.m_Month = uint8(2), .m_DayOfMonth = uint8(29), .m_Hour = uint8(10), .m_Minute = uint32(23)},  fs_Time("2024-10-07 11:22")), ==, fs_Time("2028-02-29 10:23"));
	}

	void f_DoTests()
	{
		DMibTestCategory("RebootSchedule")
		{
			DMibTestSuite("UTC")
			{
				f_TestTimeZone(false);
			};

			DMibTestSuite("Local")
			{
				for
				(
					CTimeSpan TimeZoneOffset = CTimeSpanConvert::fs_CreateHourSpan(-12)
					; TimeZoneOffset <= CTimeSpanConvert::fs_CreateHourSpan(14)
					; TimeZoneOffset += CTimeSpanConvert::fs_CreateMinuteSpan(15)
				)
				{
					CTimeSpanConvert TimeZone(TimeZoneOffset);
					DMibTestPath("{}{sj2,sf0}:{sj2,sf0}"_f << (TimeZone.f_GetSeconds() < 0 ? "-" : "") << fg_Abs(TimeZone.f_GetHours()) << TimeZone.f_GetMinuteOfHour());
					NMib::NTime::CSystem_Time::fs_SetTimeSpeed(1.0, nullptr, &TimeZoneOffset);

					auto Cleanup = g_OnScopeExit / []
						{
							CSystem_Time::fs_DisableTimeSpeed();
						}
					;

					f_TestTimeZone(true);
				}
			};
		};
	}

	bool m_bTestingLocal = false;
};

DMibTestRegister(CHostMonitor_Tests, Malterlib::Cloud);
