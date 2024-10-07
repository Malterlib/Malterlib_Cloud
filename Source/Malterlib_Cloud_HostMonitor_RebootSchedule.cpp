// Copyright © 2024 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_HostMonitor.h"

namespace NMib::NCloud
{
	using namespace NTime;

	namespace
	{
		CTime fg_TimeFromDateTime(CTimeConvert::CDateTime const &_DateTime)
		{
			return CTimeConvert::fs_CreateTime(_DateTime.m_Year, _DateTime.m_Month, _DateTime.m_DayOfMonth, _DateTime.m_Hour, _DateTime.m_Minute);
		}

		struct CDateTimeState
		{
			CTime m_Time;
			CTimeConvert::CDateTime m_DateTime;
		};

		struct CDateTimeEdit
		{
			CDateTimeEdit(CDateTimeState &_State)
				: mp_State(_State)
			{
			}

			~CDateTimeEdit()
			{
				mp_State.m_Time = fg_TimeFromDateTime(mp_State.m_DateTime);
				mp_State.m_DateTime = CTimeConvert(mp_State.m_Time).f_ExtractDateTime();
			}

			CTimeConvert::CDateTime *operator ->()
			{
				return &mp_State.m_DateTime;
			}

		private:
			CDateTimeState &mp_State;
		};

		struct CTimeEdit
		{
			CTimeEdit(CDateTimeState &_State)
				: mp_State(_State)
			{
			}

			~CTimeEdit()
			{
				mp_State.m_DateTime = CTimeConvert(mp_State.m_Time).f_ExtractDateTime();
			}

			CTime &operator *()
			{
				return mp_State.m_Time;
			}

		private:
			CDateTimeState &mp_State;
		};
	}

	CTime CHostMonitorRebootSchedule::f_GetNextRebootTime(CTime const &_EarliestRebootTime, bool _bLocalTime) const
	{
		CDateTimeState RebootTime;
		RebootTime.m_DateTime = CTimeConvert(_bLocalTime ? _EarliestRebootTime.f_ToLocal() : _EarliestRebootTime).f_ExtractDateTime();
		RebootTime.m_Time = fg_TimeFromDateTime(RebootTime.m_DateTime);

		auto Now = RebootTime.m_Time;

		auto fEditTime = [&]
			{
				return CTimeEdit(RebootTime);
			}
		;

		auto fEditDateTime = [&]
			{
				return CDateTimeEdit(RebootTime);
			}
		;

		if (m_Hour)
		{
			{
				auto EditDateTime = fEditDateTime();
				if (m_Minute)
					EditDateTime->m_Minute = *m_Minute;
				else if (RebootTime.m_DateTime.m_Hour != *m_Hour)
					EditDateTime->m_Minute = 0;

				EditDateTime->m_Hour = *m_Hour;
			}

			if (RebootTime.m_Time < Now)
				*fEditTime() += CTimeSpanConvert::fs_CreateDaySpan(1);
		}
		else if (m_Minute)
		{
			fEditDateTime()->m_Minute = *m_Minute;

			if (RebootTime.m_Time < Now)
				*fEditTime() += CTimeSpanConvert::fs_CreateHourSpan(1);
		}

		auto fResetHourMinute = [&](auto &&_EditDateTime)
			{
				if (!m_Hour)
					_EditDateTime->m_Hour = 0;

				if (!m_Minute)
					_EditDateTime->m_Minute = 0;
			}
		;

		for (bool bDidSomething = true; fg_Exchange(bDidSomething, false);)
		{
			if (m_DayOfMonth)
			{
				if (RebootTime.m_DateTime.m_DayOfMonth != *m_DayOfMonth)
				{
					bDidSomething = true;

					auto EditDateTime = fEditDateTime();

					if (EditDateTime->m_DayOfMonth > *m_DayOfMonth)
					{
						++EditDateTime->m_Month;
						if (EditDateTime->m_Month > 12)
						{
							EditDateTime->m_Month = 1;
							++EditDateTime->m_Year;
						}
					}

					while (*m_DayOfMonth > CTimeConvert::fs_GetDaysInMonth(EditDateTime->m_Year, EditDateTime->m_Month - 1))
					{
						if (m_Month)
						{
							if (EditDateTime->m_Month != *m_Month)
							{
								if (EditDateTime->m_Month > *m_Month)
									++EditDateTime->m_Year;
								EditDateTime->m_Month = *m_Month;
							}
							else
								++EditDateTime->m_Year;
						}
						else
						{
							++EditDateTime->m_Month;
							if (EditDateTime->m_Month > 12)
							{
								EditDateTime->m_Month = 1;
								++EditDateTime->m_Year;
							}
						}
					}

					EditDateTime->m_DayOfMonth = *m_DayOfMonth;

					fResetHourMinute(EditDateTime);
				}
			}
			else if (m_DayOfWeek)
			{
				if (RebootTime.m_DateTime.m_DayOfWeek != *m_DayOfWeek)
				{
					bDidSomething = true;

					if (RebootTime.m_DateTime.m_DayOfWeek > *m_DayOfWeek)
						*fEditTime() += CTimeSpanConvert::fs_CreateDaySpan(7 - (RebootTime.m_DateTime.m_DayOfWeek - *m_DayOfWeek));
					else
						*fEditTime() += CTimeSpanConvert::fs_CreateDaySpan(*m_DayOfWeek - RebootTime.m_DateTime.m_DayOfWeek);

					fResetHourMinute(fEditDateTime());
				}
			}

			if (m_Month && RebootTime.m_DateTime.m_Month != *m_Month)
			{
				bDidSomething = true;

				auto EditDateTime = fEditDateTime();

				if (EditDateTime->m_Month > *m_Month)
					++EditDateTime->m_Year;

				EditDateTime->m_DayOfMonth = 1;
				
				fResetHourMinute(EditDateTime);

				EditDateTime->m_Month = *m_Month;
			}
		}

		if (_bLocalTime)
			return RebootTime.m_Time.f_ToUTC();
		else
			return RebootTime.m_Time;
	}
}
