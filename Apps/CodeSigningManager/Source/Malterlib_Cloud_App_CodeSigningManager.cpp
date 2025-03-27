// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Platform>

#include <Mib/Encoding/JsonShortcuts>
#include <Mib/Encoding/Base64>
#include <Mib/Concurrency/LogError>
#include <Mib/File/File>
#include "Malterlib_Cloud_App_CodeSigningManager.h"

namespace NMib::NCloud::NCodeSigningManager
{
	CCodeSigningManagerActor::CCodeSigningManagerActor()
		: CDistributedAppActor(CDistributedAppActor_Settings("CodeSigningManager").f_AuditCategory("Malterlib/Cloud/CodeSigningManager"))
	{
	}

	CCodeSigningManagerActor::~CCodeSigningManagerActor() = default;

	TCFuture<void> CCodeSigningManagerActor::fp_StartApp(CEJsonSorted const _Params)
	{
		auto OnResume = co_await fg_OnResume
			(
				[&]() -> NException::CExceptionPointer
				{
					if (mp_State.m_bStoppingApp || f_IsDestroyed())
						return DMibErrorInstance("Startup aborted");
					return {};
				}
			)
		;

		{
			auto CaptureExceptions = co_await (g_CaptureExceptions % "Failed to parse timestamp configuration");

			if (auto *pServers = mp_State.m_ConfigDatabase.m_Data.f_GetMember("TimestampServers", EJsonType_Array))
				mp_TimestampOptions.m_TimestampURLs = pServers->f_StringArray();
			else
				mp_TimestampOptions.m_TimestampURLs = {gc_Str<"http://timestamp.digicert.com">.m_Str, gc_Str<"http://timestamp.sectigo.com">.m_Str};

			if (auto *pCerts = mp_State.m_ConfigDatabase.m_Data.f_GetMember("TimestampTrustedCACertificates", EJsonType_Array))
			{
				mp_TimestampOptions.m_TimestampTrustedCACertificates.f_Clear();
				for (auto const &Cert : pCerts->f_Array())
				{
					if (Cert.f_IsBinary())
						mp_TimestampOptions.m_TimestampTrustedCACertificates.f_InsertLast(Cert.f_Binary());
					else if (Cert.f_IsString())
					{
						auto CaptureExceptions = co_await (g_CaptureExceptions % "Failed to parse base64 for trusted timestamp CA certificate");
						CByteVector Decoded;
						fg_Base64Decode(Cert.f_String(), Decoded);
						mp_TimestampOptions.m_TimestampTrustedCACertificates.f_InsertLast(Decoded);
					}
				}
			}
		}

		mp_SecretsManagerSubscription = co_await mp_State.m_TrustManager->f_SubscribeTrustedActors<CSecretsManager>(CSecretsManager::EProtocolVersion_SupportMapSecrets);

		{
			auto Result = co_await mp_SecretsManagerSubscription.f_OnActor
				(
					g_ActorFunctor / [this](TCDistributedActor<CSecretsManager> _SecretsManager, CTrustedActorInfo _ActorInfo) -> TCFuture<void>
					{
						co_await fp_SecretsManagerAddedWithRetry(_SecretsManager, _ActorInfo);

						co_return {};
					}
					, g_ActorFunctor / [this](TCWeakDistributedActor<CActor> _SecretsManager, CTrustedActorInfo _ActorInfo) -> TCFuture<void>
					{
						fp_SecretsManagerRemoved(_SecretsManager, _ActorInfo)
							> fg_LogError("Mib/Cloud/CodeSigningManager", "Failed to handle secrets manager removed")
						;

						co_return {};
					}
				)
				.f_Wrap()
			;

			if (!Result)
				DMibLog(Error, "Failed when subscripbing to secrets manager: {}", Result.f_GetExceptionStr());
		}


		mp_SensorUpdateTimerSubscription = co_await fg_RegisterTimer
			(
				24.0 * 60.0 * 60.0 // 24 h
				, [this]() -> TCFuture<void>
				{
					co_await (fp_Authority_UpdateSensors() + fp_SigningCert_UpdateSensors());

					co_return {};
				}
			)
		;

		co_await fp_SigningCert_UpdateSensors();
		co_await fp_Authority_UpdateSensors();
		co_await fp_Publish();

		co_return {};
	}

	TCFuture<void> CCodeSigningManagerActor::fp_Destroy()
	{
		co_await fg_Move(mp_InitSensorReporterSequencer).f_Destroy().f_Wrap() > fg_LogError("Mib/Cloud/CodeSigningManager", "Failed to destroy sequencer");

		{
			TCFutureVector<void> DestroyTasks;

			for (auto &Entry : mp_SignSessions)
			{
				if (Entry.m_FileTransferReceive)
					fg_Move(Entry.m_FileTransferReceive).f_Destroy() > DestroyTasks;

				auto Path = Entry.m_WorkDirectory;
				if (!Path.f_IsEmpty())
				{
					auto BlockingActor = NConcurrency::fg_BlockingActor();
					NConcurrency::g_Dispatch(BlockingActor) /
						[Path]() -> NConcurrency::TCFuture<void>
						{
							auto Capture = co_await (NConcurrency::g_CaptureExceptions % "Failed to cleanup signing directory");
							if (NFile::CFile::fs_FileExists(Path))
								NFile::CFile::fs_DeleteDirectoryRecursive(Path);
							co_return {};
						}
						> DestroyTasks
					;
				}
			}

			co_await fg_AllDoneWrapped(DestroyTasks);
			mp_SignSessions.f_Clear();
		}

		co_await mp_CodeSigningInterface.f_Destroy().f_Wrap() > fg_LogError("Mib/Cloud/CodeSigningManager", "Failed to destroy signing interface");

		co_await CDistributedAppActor::fp_Destroy();

		co_return {};
	}

	TCFuture<void> CCodeSigningManagerActor::fp_StopApp()
	{
		TCFutureVector<void> Destroys;
		mp_SecretsManagerSubscription.f_Destroy() > Destroys;

		co_await fg_AllDoneWrapped(Destroys);

		co_return {};
	}

	EPublicKeyType CCodeSigningManagerActor::fsp_EllipticCurveTypeFromStr(CStr const &_String)
	{
		if (_String == "secp256r1")
			return EPublicKeyType::mc_EC_secp256r1;
		else if (_String == "secp384r1")
			return EPublicKeyType::mc_EC_secp384r1;
		else if (_String == "secp521r1")
			return EPublicKeyType::mc_EC_secp521r1;
		else if (_String == "X25519")
			return EPublicKeyType::mc_EC_X25519;
		else
			DMibError("Unknown elliptic key type: {}"_f << _String);
	}

	CPublicKeySetting CCodeSigningManagerActor::fsp_PublicKeySettingFromStr(CStr const &_String)
	{
		if (_String.f_StartsWith("RSA-"))
			return _String.f_RemovePrefix("RSA-").f_ToInt(uint32(4096));
		else
			return fsp_EllipticCurveTypeToKeySettings(fsp_EllipticCurveTypeFromStr(_String));
	}

	CStr CCodeSigningManagerActor::fsp_PublicKeySettingToStr(CPublicKeySetting const &_PublicKeySetting)
	{
		switch (_PublicKeySetting.f_GetTypeID())
		{
		case EPublicKeyType::mc_RSA: return "RSA-{}"_f << _PublicKeySetting.f_Get<EPublicKeyType::mc_RSA>().m_KeyLength;
		case EPublicKeyType::mc_EC_secp256r1: return "secp256r1";
		case EPublicKeyType::mc_EC_secp384r1: return "secp384r1";
		case EPublicKeyType::mc_EC_secp521r1: return "secp521r1";
		case EPublicKeyType::mc_EC_X25519: return "X25519";
		}
		return "Unknown";
	}

	CStr CCodeSigningManagerActor::fsp_EllipticCurveTypeToStr(EPublicKeyType _Type)
	{
		switch (_Type)
		{
		case EPublicKeyType::mc_EC_secp256r1: return "secp256r1";
		case EPublicKeyType::mc_EC_secp384r1: return "secp384r1";
		case EPublicKeyType::mc_EC_secp521r1: return "secp521r1";
		case EPublicKeyType::mc_EC_X25519: return "X25519";
		default: break;
		}
		return "Unknown";
	}

	CPublicKeySetting CCodeSigningManagerActor::fsp_EllipticCurveTypeToKeySettings(EPublicKeyType _Type)
	{
		switch (_Type)
		{
		case EPublicKeyType::mc_EC_secp256r1: return CPublicKeySettings_EC_secp256r1{};
		case EPublicKeyType::mc_EC_secp384r1: return CPublicKeySettings_EC_secp384r1{};
		case EPublicKeyType::mc_EC_secp521r1: return CPublicKeySettings_EC_secp521r1{};
		case EPublicKeyType::mc_EC_X25519: return CPublicKeySettings_EC_X25519{};
		default: break;
		}
		return CPublicKeySettings_EC_secp521r1{};
	}
}

namespace NMib::NCloud
{
	TCActor<CDistributedAppActor> fg_ConstructApp_CodeSigningManager()
	{
		return fg_Construct<NCodeSigningManager::CCodeSigningManagerActor>();
	}
}
