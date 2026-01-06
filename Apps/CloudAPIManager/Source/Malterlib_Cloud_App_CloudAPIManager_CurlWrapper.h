// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Encoding/JsonShortcuts>

#include "Malterlib_Cloud_App_CloudAPIManager.h"
#include "Malterlib_Cloud_App_CloudAPIManager_Server.h"

namespace NMib::NCloud::NCloudAPIManager
{
	enum ECurlMethod
	{
		ECurlMethod_GET
		, ECurlMethod_POST
		, ECurlMethod_PUT
		, ECurlMethod_DELETE
	};

	struct CState
	{
		CByteVector m_Headers;
		CByteVector m_Body;
	};

	struct CCurlResult
	{
		CCurlResult(CState const &_State);

		uint32 m_StatusCode;
		CStr m_StatusMessage;
		TCMap<CStr, CStr> m_Headers;
		CStr m_Body;
	};

	CCurlResult fg_Curl(ECurlMethod _Method, CStr const &_URL, TCMap<CStr, CStr> const &_Headers, CStr const &_Data);
}
