// Copyright © 2023 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

namespace NMib::NCloud::NCloudManager
{
	struct CCloudManagerServer;

	struct CNotifications : public CAllowUnsafeThis
	{
		enum EType : uint32
		{
			EType_None = 0

			, EType_Startup = DMibBit(0)
			, EType_Update = DMibBit(1)
			, EType_Sensor = DMibBit(2)
			, EType_Log = DMibBit(3)

			, EType_Alert = DMibBitTyped(31, uint32)
		};

		CNotifications(CCloudManagerServer &_This);

		TCFuture<void> f_Init();
		TCFuture<void> f_Destroy();
		TCFuture<TCMap<CStr, CStr>> f_PostSlackMessage(EType _TypeFlags, CSlackActor::CMessage _Message, TCMap<CStr, CStr> _Timestamps);

		static void fs_LimitMessage(CSlackActor::CMessage &o_Message, CStr const &_ErrorType);
		static CStr fs_ReformatHostForSlack(CStr const &_FriendlyName);

	private:
		struct CSlackNotificationChannel
		{
			NStr::CStr m_Token;
			CStr m_AlertDestination = "!channel";
			CStr m_IconEmoji = ":malterlib:";
			EType m_ReportFlags = EType_None;
		};

		TCFuture<void> fp_SendStartupMessage(CStr _Error);

		CCloudManagerServer &mp_This;

		TCMap<CStr, CSlackNotificationChannel> mp_SlackChannels;
		TCActor<CHttpClientActor> mp_HttpClientActor;
		TCActor<CSlackActor> mp_SlackActor;
	};
}

