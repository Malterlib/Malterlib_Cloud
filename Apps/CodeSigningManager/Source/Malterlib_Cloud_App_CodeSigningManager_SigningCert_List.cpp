// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "Malterlib_Cloud_App_CodeSigningManager.h"

#include <Mib/Encoding/JsonShortcuts>
#include <Mib/CommandLine/TableRenderer>

namespace NMib::NCloud::NCodeSigningManager
{
	TCFuture<uint32> CCodeSigningManagerActor::fp_CommandLine_SigningCertList(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		bool bVerbose = _Params["Verbose"].f_Boolean();
		CStr SigningCertName = _Params["SigningCert"].f_String();
		CStr AuthorityName = _Params["Authority"].f_String();

		auto AnsiEncoding = _pCommandLine->f_AnsiEncoding();
		CTableRenderHelper TableRenderer = _pCommandLine->f_TableRenderer();

		TCVector<CStr> Headings;
		TCSet<mint> VerboseHeadings;

		auto fAddHeading = [&](CStr const &_Name, bool _bVerbose = true)
			{
				if (_bVerbose)
					VerboseHeadings[Headings.f_GetLen()];

				Headings.f_Insert(_Name);
			}
		;

		fAddHeading("Authority", false);
		fAddHeading("SigningCert", false);
		fAddHeading("Key Type");
		fAddHeading("Secret Managers");
		mint iMissingOnManagersHeading = Headings.f_GetLen();
		fAddHeading("{}{}Missing on Managers{}"_f << AnsiEncoding.f_StatusWarning() << AnsiEncoding.f_Bold() << AnsiEncoding.f_Default());
		fAddHeading("Status", false);

		TableRenderer.f_AddHeadingsVector(Headings);
		TableRenderer.f_SetOptions(CTableRenderHelper::EOption_Rounded | CTableRenderHelper::EOption_AvoidRowSeparators);
		bool bHasMissing = false;

		for (auto &SigningCert : mp_SigningCerts)
		{
			auto &SigningCertKey = SigningCert.f_GetKey();
			if (!SigningCertName.f_IsEmpty() && SigningCertKey.m_Name != SigningCertName)
				continue;
			if (!AuthorityName.f_IsEmpty() && SigningCertKey.m_Authority != AuthorityName)
				continue;

			CStr StatusDescription;
			switch (SigningCert.m_Status.m_Severity)
			{
			case CDistributedAppSensorReporter::EStatusSeverity_Info:
				StatusDescription = SigningCert.m_Status.m_Description;
				break;
			case CDistributedAppSensorReporter::EStatusSeverity_Ok:
				StatusDescription = "{}{}{}"_f << AnsiEncoding.f_StatusNormal() << SigningCert.m_Status.m_Description << AnsiEncoding.f_Default();
				break;
			case CDistributedAppSensorReporter::EStatusSeverity_Warning:
				StatusDescription = "{}{}{}"_f << AnsiEncoding.f_StatusWarning() << SigningCert.m_Status.m_Description << AnsiEncoding.f_Default();
				break;
			case CDistributedAppSensorReporter::EStatusSeverity_Error:
				StatusDescription = "{}{}{}"_f << AnsiEncoding.f_StatusError() << SigningCert.m_Status.m_Description << AnsiEncoding.f_Default();
				break;
			}

			TCVector<CStr> SecretManagers;

			for (auto &ModifiedTime : SigningCert.m_SecretsManagers)
			{
				auto &WeakManager = SigningCert.m_SecretsManagers.fs_GetKey(ModifiedTime);
				auto Manager = WeakManager.f_Lock();
				if (!Manager)
					continue;

				auto *pSecretManager = mp_SecretsManagerSubscription.m_Actors.f_FindEqual(Manager);
				DMibCheck(pSecretManager);

				if (!pSecretManager)
					continue;

				SecretManagers.f_Insert(pSecretManager->m_TrustInfo.m_HostInfo.f_GetDescColored(AnsiEncoding.f_Flags()));
			}

			TCVector<CStr> MissingSecretManagers;

			for (auto &Manager : mp_SecretsManagerSubscription.m_Actors)
			{
				auto WeakManager = Manager.m_Actor.f_Weak();

				if (SigningCert.m_SecretsManagers.f_FindEqual(WeakManager))
					continue;

				bHasMissing = true;

				MissingSecretManagers.f_Insert(Manager.m_TrustInfo.m_HostInfo.f_GetDescColored(AnsiEncoding.f_Flags()));
			}

			TableRenderer.f_AddRow
				(
					SigningCertKey.m_Authority
					, SigningCertKey.m_Name
					, fsp_PublicKeySettingToStr(SigningCert.m_PublicKeySetting)
					, CStr::fs_Join(SecretManagers, "\n")
					, CStr::fs_Join(MissingSecretManagers, "\n")
					, StatusDescription
				)
			;
		}

		if (!bVerbose)
		{
			while (auto pLargest = VerboseHeadings.f_FindLargest())
			{
				TableRenderer.f_RemoveColumn(*pLargest);
				VerboseHeadings.f_Remove(pLargest);
			}
		}
		else
		{
			if (!bHasMissing)
				TableRenderer.f_RemoveColumn(iMissingOnManagersHeading);
		}

		TableRenderer.f_Output(_Params);

		co_return 0;
	}
}
