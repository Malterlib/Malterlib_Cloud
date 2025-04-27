// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud::NAppManager
{
#ifdef DPlatformFamily_Linux
	ch8 const *g_pSetupEncryptionScript =
#		include "Malterlib_Cloud_App_AppManager_Encryption_Common.sh"
#		include "Malterlib_Cloud_App_AppManager_Encryption_SetupLinux.sh"
	;
	ch8 const *g_pOpenEncryptionScript =
#		include "Malterlib_Cloud_App_AppManager_Encryption_Common.sh"
#		include "Malterlib_Cloud_App_AppManager_Encryption_OpenLinux.sh"
	;
	ch8 const *g_pCloseEncryptionScript =
#		include "Malterlib_Cloud_App_AppManager_Encryption_Common.sh"
#		include "Malterlib_Cloud_App_AppManager_Encryption_CloseLinux.sh"
	;
#endif

	void CAppManagerActor::fp_AppEncryptionStateChanged(TCSharedPointer<CApplication> const &_pApplication, bool _bEncrypted)
	{
		_pApplication->m_bEncryptionOpened = _bEncrypted;
		fp_UpdateApplicationDependencies();
	}

	TCFuture<void> CAppManagerActor::fp_ChangeEncryption(TCSharedPointer<CApplication> _pApplication, EEncryptOperation _Operation, bool _bForceOverwrite)
	{
		auto pEncryptionApplication = _pApplication;
		if (pEncryptionApplication->f_IsChildApp())
		{
			if (!pEncryptionApplication->m_pParentApplication)
				co_return DMibErrorInstance("Cannot change encryption because parent application is missing");
			if (_Operation == EEncryptOperation_Setup)
				co_return {}; // Parent application already set up
			else if (_Operation == EEncryptOperation_Close)
				co_return {}; // Don't close until main application closes

			pEncryptionApplication = fg_Explicit(pEncryptionApplication->m_pParentApplication);
		}

		if (pEncryptionApplication->m_Settings.m_EncryptionStorage.f_IsEmpty())
			co_return {};

#if !defined(DPlatformFamily_Linux)
		co_return DMibErrorInstance("Encryption is not supported on this platform");
#else
		if (_Operation == EEncryptOperation_Open)
		{
			if (pEncryptionApplication->m_bEncryptionOpened)
				co_return {};
		}

		CSymmetricKey Key;
		if (_Operation == EEncryptOperation_Close)
		{
			if (pEncryptionApplication->m_DirectoryMonitorSubscription)
				co_await fg_Exchange(pEncryptionApplication->m_DirectoryMonitorSubscription, nullptr)->f_Destroy();
		}
		else
		{
			if (mp_KeyManagerSubscription.m_Actors.f_IsEmpty())
				co_return DMibErrorInstance("No key managers are connected, so key cannot be generated");

			auto &KeyManagerInfo = *mp_KeyManagerSubscription.m_Actors.f_FindAny();
			static const mint c_KeyBits = 512;
			Key = co_await KeyManagerInfo.m_Actor.f_CallActor(&CKeyManager::f_RequestKey)(pEncryptionApplication->m_Name, c_KeyBits / 8);
		}

		CStr UniqueName = CHash_SHA256::fs_DigestFromData(mp_State.m_RootDirectory.f_GetStr(), mp_State.m_RootDirectory.f_GetLen()).f_GetString().f_Left(8);

		CStr DeviceName;
		if (mp_State.m_ConfigDatabase.m_Data.f_GetMemberValue("UniqueEncriptionDeviceName", true).f_Boolean())
			DeviceName = "enc_{}_{}"_f << UniqueName << pEncryptionApplication->m_Name.f_LowerCase();
		else
			DeviceName = "enc_{}"_f << pEncryptionApplication->m_Name.f_LowerCase();

		CStr ZPoolName;
		if (mp_State.m_ConfigDatabase.m_Data.f_GetMemberValue("UniqueZPoolName", false).f_Boolean())
			ZPoolName = "zpool_{}_{}"_f << UniqueName << pEncryptionApplication->m_Name.f_LowerCase();
		else
			ZPoolName = "zpool_{}"_f << pEncryptionApplication->m_Name.f_LowerCase();

		TCMap<CStr, CStr> Environment;
		Environment["MibCloudApp_EncryptionStorage"] = pEncryptionApplication->m_Settings.m_EncryptionStorage;
		Environment["MibCloudApp_EncryptionFileSystem"] = pEncryptionApplication->m_Settings.m_EncryptionFileSystem;
		Environment["MibCloudApp_DeviceName"] = DeviceName;
		Environment["MibCloudApp_ZPoolName"] = ZPoolName;
		Environment["MibCloudApp_MountPoint"] = pEncryptionApplication->f_GetDirectory();
		if (_Operation == EEncryptOperation_Setup && _bForceOverwrite)
			Environment["MibCloudApp_ForceOverwrite"] = "1";

		ch8 const *pScript = nullptr;
		ch8 const *pDesc = nullptr;

		switch (_Operation)
		{
		case EEncryptOperation_Setup:
			pScript = g_pSetupEncryptionScript;
			pDesc = "SetupEncryption";
			break;
		case EEncryptOperation_Open:
			pScript = g_pOpenEncryptionScript;
			pDesc = "OpenEncryption";
			break;
		case EEncryptOperation_Close:
			pScript = g_pCloseEncryptionScript;
			pDesc = "CloseEncryption";
			break;
		default:
			DNeverGetHere;
			break;
		}

		co_await
			(
				fp_RunBashScript
				(
					pScript
					, pDesc
					, fg_Move(Environment)
					, [Key](NMib::NStr::CStr const &_Output, TCActor<CProcessLaunchActor> const &_LaunchActor)
					{
						if (_Output == "PROVIDE KEY")
							_LaunchActor(&CProcessLaunchActor::f_SendStdInBinary, CIOByteVector::fs_AllowInsecureConversion(Key)).f_DiscardResult();
					}
					, TCLimitsInt<uint32>::mc_Max
				)
				% "Failed to change encryption"
			)
		;

		if (_Operation == EEncryptOperation_Open || _Operation == EEncryptOperation_Setup)
		{
			CHostMonitor::CMonitorPathOptions PathOptions;
			PathOptions.m_Path = pEncryptionApplication->f_GetDirectory();
			pEncryptionApplication->m_DirectoryMonitorSubscription = co_await mp_HostMonitor(&CHostMonitor::f_MonitorPath, PathOptions);

			fp_AppEncryptionStateChanged(pEncryptionApplication, true);
		}
		else if (_Operation == EEncryptOperation_Close)
			fp_AppEncryptionStateChanged(pEncryptionApplication, false);

		co_return {};
#endif
	}
}
