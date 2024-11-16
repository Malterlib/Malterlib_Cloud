// Copyright © 2023 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_App_CloudManager.h"
#include "Malterlib_Cloud_App_CloudManager_Internal.h"
#include "Malterlib_Cloud_App_CloudManager_Database.h"

#include <Mib/Concurrency/LogError>
#include <Mib/Process/Platform>

namespace NMib::NCloud::NCloudManager
{
	using namespace NCloudManagerDatabase;

	CNotifications::CNotifications(CCloudManagerServer &_This)
		: mp_This(_This)
	{
	}

	TCFuture<void> CNotifications::f_Init()
	{
		auto OnResume = co_await mp_This.f_CheckDestroyedOnResume();

		{
			auto CaptureScope = co_await g_CaptureExceptions;

			if (auto *pValue = fg_Const(mp_This.mp_AppState.m_ConfigDatabase.m_Data).f_GetMember("SlackChannels"))
			{
				for (auto &Channel : pValue->f_Object())
				{
					auto &SlackChannel = mp_SlackChannels[Channel.f_Name()];
					SlackChannel.m_Token = Channel.f_Value()["Token"].f_String();
					SlackChannel.m_AlertDestination = Channel.f_Value().f_GetMemberValue("AlertDestination", SlackChannel.m_AlertDestination).f_String();
					SlackChannel.m_IconEmoji = Channel.f_Value().f_GetMemberValue("IconEmoji", SlackChannel.m_IconEmoji).f_String();

					if (Channel.f_Value().f_GetMemberValue("ReportStartup", true).f_Boolean())
						SlackChannel.m_ReportFlags |= EType_Startup;

					if (Channel.f_Value().f_GetMemberValue("ReportUpdates", true).f_Boolean())
						SlackChannel.m_ReportFlags |= EType_Update;

					if (Channel.f_Value().f_GetMemberValue("ReportSensors", true).f_Boolean())
						SlackChannel.m_ReportFlags |= EType_Sensor;

					if (Channel.f_Value().f_GetMemberValue("ReportLogs", true).f_Boolean())
						SlackChannel.m_ReportFlags |= EType_Log;
				}
			}
		}

		if (!mp_SlackChannels.f_IsEmpty())
		{
			if (!mp_CurlActor)
				mp_CurlActor = fg_Construct(fg_Construct(), "Curl actor");

			if (!mp_SlackActor)
				mp_SlackActor = fg_Construct(mp_CurlActor);
		}

		TCFutureVector<void> InitResults;
		mp_This.mp_UpdateNotifications.f_Init() % "Failed to init update notifications" > InitResults;
		mp_This.mp_SensorNotifications.f_Init() % "Failed to init sensor notifications" > InitResults;
		mp_This.mp_LogNotifications.f_Init() % "Failed to init log notifications" > InitResults;

		auto InitResult = co_await fg_AllDone(InitResults).f_Wrap();

		CStr Error;
		if (!InitResult)
		{
			Error = InitResult.f_GetExceptionStr();
			InitResult > fg_LogError("Malterlib/Cloud/CloudManager", "Failed to init notifications");
		}

		co_await fp_SendStartupMessage(Error);

		co_return {};
	}

	TCFuture<void> CNotifications::f_Destroy()
	{
		if (mp_SlackActor)
			co_await fg_Move(mp_SlackActor).f_Destroy();

		if (mp_CurlActor)
			co_await fg_Move(mp_CurlActor).f_Destroy();

		co_return {};
	}

	TCFuture<TCMap<CStr, CStr>> CNotifications::f_PostSlackMessage
		(
			EType _TypeFlags
			, CSlackActor::CMessage _Message
			, TCMap<CStr, CStr> _Timestamps
		)
	{
		if (mp_SlackChannels.f_IsEmpty())
			co_return {};

		TCMap<CStr, CStr> OutTimestamps;

		for (auto &Channel : mp_SlackChannels)
		{
			if (!(Channel.m_ReportFlags & _TypeFlags))
				continue;

			auto &ChannelID = mp_SlackChannels.fs_GetKey(Channel);

			CSlackActor::CMessage Message = _Message;
			Message.m_Channel = ChannelID;
			Message.m_UserName = "Cloud Manager ({})"_f << NProcess::NPlatform::fg_Process_GetComputerName();
			if (!Message.m_IconEmoji && Channel.m_IconEmoji)
				Message.m_IconEmoji = Channel.m_IconEmoji;

			if (_TypeFlags & EType_Alert)
				Message.m_Text = "<{}>"_f << Channel.m_AlertDestination;

			TCAsyncResult<CStr> Result;
			if (auto *pTimestamp = _Timestamps.f_FindEqual(ChannelID))
			{
				if (Message.m_bReplyBroadcast && *Message.m_bReplyBroadcast)
				{
					Message.m_ThreadTimestamp = *pTimestamp;
					Result = co_await mp_SlackActor(&CSlackActor::f_PostMessage, Channel.m_Token, Message).f_Wrap();
				}
				else
					Result = co_await mp_SlackActor(&CSlackActor::f_UpdateMessage, Channel.m_Token, *pTimestamp, Message).f_Wrap();
			}
			else
			{
				if (Message.m_bReplyBroadcast)
					Message.m_bReplyBroadcast.f_Clear();

				Result = co_await mp_SlackActor(&CSlackActor::f_PostMessage, Channel.m_Token, Message).f_Wrap();
			}

			if (!Result)
				Result > fg_LogError("Malterlib/Cloud/CloudManager", "Failed to post to Slack API:\n{}"_f << ("{}"_f << _Message).f_GetStr().f_Indent("    "));
			else
				OutTimestamps[ChannelID] = *Result;
		}

		co_return fg_Move(OutTimestamps);
	}

	TCFuture<void> CNotifications::fp_SendStartupMessage(CStr _Error)
	{
		CSlackActor::CMessage Message;

		auto &SlackAttachment = Message.m_Attachments.f_Insert();
		SlackAttachment.m_bFieldsMarkdown = true;
		if (_Error)
		{
			SlackAttachment.m_Text = "Cloud Manager started with error:\n```{}```"_f << CSlackActor::CMessage::fs_EscapeString(_Error);
			SlackAttachment.m_Color = CSlackActor::EPredefinedColor_Danger;
		}
		else
		{
			SlackAttachment.m_Text = "Cloud Manager started";
			SlackAttachment.m_Color = CSlackActor::EPredefinedColor_Good;
		}

		SlackAttachment.m_Fields.f_Insert
			(
				{
					{
						"Host"
						, NProcess::NPlatform::fg_Process_GetComputerName()
						, true
					}
					,
					{
						"Path"
						, CFile::fs_GetProgramDirectory()
						, true
					}
				}
			)
		;

		try
		{
			NProcess::CVersionInfo VersionInfo;
			NProcess::NPlatform::fg_Process_GetVersionInfo(CFile::fs_GetProgramPathForExecutabelContents(), VersionInfo);
			SlackAttachment.m_Fields.f_Insert
				(
					{
						{
							"Version"
							, "{}.{}.{}.{}"_f
							<< VersionInfo.m_Major
							<< VersionInfo.m_Minor
							<< VersionInfo.m_Revision
							<< VersionInfo.m_MinorRevision
							, true
						}
						,
						{
							"Version Time"
							, "{tc5}"_f << VersionInfo.m_BuildTime
							, true
						}
					}
				)
			;
		}
		catch (NException::CException const &_Exception)
		{
			SlackAttachment.m_Fields.f_Insert
				(
					{
						{
							"Error getting version"
							, "{}"_f << _Exception
							, false
						}
					}
				)
			;
		}

		co_await f_PostSlackMessage(EType_Startup, Message, fg_Default());
		
		co_return {};
	}

	void CNotifications::fs_LimitMessage(CSlackActor::CMessage &o_Message, CStr const &_ErrorType)
	{
		mint nAttachments = o_Message.m_Attachments.f_GetLen();
		constexpr mint c_MaxAttachments = 10;
		if (nAttachments > c_MaxAttachments)
		{
			o_Message.m_Attachments.f_SetLen(c_MaxAttachments);
			auto &Attachment = o_Message.m_Attachments.f_Insert();
			Attachment.m_Text = "Warning: {} other {}, but were cut off to limit size of this message"_f << (nAttachments - c_MaxAttachments) << _ErrorType;
		}
	}

	CStr CNotifications::fs_ReformatHostForSlack(CStr const &_FriendlyName)
	{
		CStr UserName;
		CStr HostName;
		CStr Application;
		aint nParsed;
		(CStr::CParse("{}@{}/{}") >> UserName >> HostName >> Application).f_Parse(_FriendlyName, nParsed);

		CStr Return;
		if (nParsed == 3)
		{
			return "`{}` *{}* (_{}_)"_f
				<< CSlackActor::CMessage::fs_EscapeString(HostName)
				<< CSlackActor::CMessage::fs_EscapeString(Application)
				<< CSlackActor::CMessage::fs_EscapeString(UserName)
			;
		}
		else
			return _FriendlyName;
	}
}
