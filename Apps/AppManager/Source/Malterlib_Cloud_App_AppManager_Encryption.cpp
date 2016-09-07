// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib
{
	namespace NCloud
	{
		namespace NAppManager
		{
#ifdef DPlatformFamily_Linux
			ch8 const *g_pSetupEncryptionScript =
#				include "Malterlib_Cloud_App_AppManager_Encryption_SetupLinux.sh"
			;
			ch8 const *g_pOpenEncryptionScript =
#				include "Malterlib_Cloud_App_AppManager_Encryption_OpenLinux.sh"
			;
			ch8 const *g_pCloseEncryptionScript =
#				include "Malterlib_Cloud_App_AppManager_Encryption_CloseLinux.sh"
			;
#endif
			TCContinuation<void> CAppManagerActor::fp_ChangeEncryption(TCSharedPointer<CApplication> const &_pApplication, EEncryptOperation _Operation, bool _bForceOverwrite)
			{
				if (_pApplication->m_EncryptionStorage.f_IsEmpty())
					return fg_Explicit();
				
#if !defined(DPlatformFamily_Linux)
				return DMibErrorInstance("Encryption is not supported on this platform");
#else
				if (_Operation == EEncryptOperation_Open)
				{
					if (_pApplication->m_bEncryptionOpened)
						return fg_Explicit(); 
				}
				
				if (mp_KeyManagerSubscription.m_Actors.f_IsEmpty())
					return DMibErrorInstance("No key managers are connected, so key cannot be generated");
				
				TCContinuation<void> Continuation;
				
				auto fLaunchScript = [this, Continuation, _pApplication, _Operation, _bForceOverwrite](CSymmetricKey &&_Key)
					{
						TCMap<CStr, CStr> Environment;
						Environment["MibCloudApp_EncryptionStorage"] = _pApplication->m_EncryptionStorage;
						Environment["MibCloudApp_DeviceName"] = "enc_" + _pApplication->m_Name.f_LowerCase();
						Environment["MibCloudApp_ZPoolName"] = "zpool_" + _pApplication->m_Name.f_LowerCase();
						Environment["MibCloudApp_MountPoint"] = _pApplication->f_GetDirectory();
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
						
						fg_ThisActor(this)
							(
								&CAppManagerActor::fp_RunBashScript
								, pScript
								, pDesc
								, ""
								, ""
								, fg_Move(Environment)
								, [_Key](NMib::NStr::CStr const &_Output, TCActor<CProcessLaunchActor> const &_LaunchActor)
								{
									if (_Output == "PROVIDE KEY")
										_LaunchActor(&CProcessLaunchActor::f_SendStdInBinary, _Key) > fg_DiscardResult();
								}
							)
							> Continuation % "Failed to change encryption" / [Continuation, _pApplication, _Operation](CBashScriptOutput &&_Output)
							{
								if (_Operation == EEncryptOperation_Open || _Operation == EEncryptOperation_Setup)
									_pApplication->m_bEncryptionOpened = true;
								else if (_Operation == EEncryptOperation_Close)
									_pApplication->m_bEncryptionOpened = false;
								Continuation.f_SetResult();
							}
						;
					}
				;
				
				if (_Operation == EEncryptOperation_Close)
				{
					fLaunchScript(fg_Default());
					return Continuation;
				}
				
				auto &KeyManagerInfo = *mp_KeyManagerSubscription.m_Actors.f_FindAny();
				auto KeyManager = mp_KeyManagerSubscription.m_Actors.fs_GetKey(KeyManagerInfo);
				static const mint c_KeyBits = 512; 
				DCallActor(KeyManager, CKeyManager::f_RequestKey, _pApplication->m_Name, c_KeyBits / 8)
					> Continuation / [this, Continuation, _pApplication, _bForceOverwrite, fLaunchScript](CSymmetricKey &&_Key)
					{
						fLaunchScript(fg_Move(_Key));
					}
				;
				return Continuation;
#endif
			}
		}
	}
}
