// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Cryptography/RandomID>
#include <Mib/Process/ProcessLaunchActor>
#include <Mib/File/File>

#ifdef DPlatformFamily_Windows
#include <Mib/Core/PlatformSpecific/WindowsFilePath>
#endif

#include "Malterlib_Cloud_VersionManager.h"

namespace NMib::NCloud
{
	using namespace NConcurrency;
	using namespace NContainer;
	using namespace NCryptography;
	using namespace NEncoding;
	using namespace NFile;
	using namespace NProcess;
	using namespace NStorage;
	using namespace NStr;
	using namespace NTime;

	namespace
	{
		struct CState
		{
			virtual ~CState() = default;
			virtual TCFuture<void> f_Abort() = 0;

			CIntrusiveRefCountWithWeak m_RefCount;
		};

		struct CDownloadState : public CState
		{

			TCActor<CFileTransferReceive> m_DownloadVersionReceive;
			CActorSubscription m_DownloadVersionSubscription;

			TCFuture<void> f_Abort()
			{
				auto This = co_await fg_MoveThis(*this);

				if (This.m_DownloadVersionSubscription)
					co_await fg_DestroySubscription(This.m_DownloadVersionSubscription);
				if (This.m_DownloadVersionReceive)
					co_await fg_Move(This.m_DownloadVersionReceive).f_Destroy();

				co_return {};
			}
		};

		struct CUploadState : public CState
		{
			TCActor<CFileTransferSend> m_UploadVersionSend;
			TCActorFunctor<NConcurrency::TCFuture<void> ()> m_fFinish;

			TCFuture<void> f_Abort()
			{
				auto This = co_await fg_MoveThis(*this);

				co_await fg_Move(This.m_fFinish).f_Destroy();
				if (This.m_UploadVersionSend)
					co_await fg_Move(This.m_UploadVersionSend).f_Destroy();

				co_return {};
			}
		};

		struct CProcessLaunchState : public CState
		{
			TCActor<CProcessLaunchActor> m_Launch;
			TCActor<CProcessLaunchActor> m_Launch2;

			TCFuture<void> f_Abort()
			{
				auto This = co_await fg_MoveThis(*this);

				if (This.m_Launch)
					co_await fg_Move(This.m_Launch).f_Destroy();

				if (This.m_Launch2)
					co_await fg_Move(This.m_Launch2).f_Destroy();

				co_return {};
			}
		};
	}

	struct CVersionManagerHelperInternal
	{
		CVersionManagerHelperInternal(uint64 _QueueSize, fp64 _Timeout, CStr const &_RootDirectory)
			: m_QueueSize(_QueueSize)
			, m_Timeout(_Timeout)
			, m_RootDirectory(_RootDirectory)
		{
		}

		CStr m_RootDirectory;
		TCMap<CStr, TCWeakPointer<CState>> m_States;
		fp64 m_Timeout = 0.0;
		uint64 m_QueueSize = 0;
	};

	CVersionManagerHelper::CVersionManagerHelper
		(
			CStr const &_RootDirectory
			, uint64 _QueueSize
			, fp64 _Timeout
		)
		: mp_pInternal(fg_Construct(_QueueSize, _Timeout, _RootDirectory))
	{
	}

	CVersionManagerHelper::~CVersionManagerHelper() = default;
	CVersionManagerHelper::CVersionManagerHelper(CVersionManagerHelper const &) = default;
	CVersionManagerHelper::CVersionManagerHelper(CVersionManagerHelper &&) = default;
	CVersionManagerHelper &CVersionManagerHelper::operator = (CVersionManagerHelper const &) = default;
	CVersionManagerHelper &CVersionManagerHelper::operator = (CVersionManagerHelper &&) = default;

	TCFuture<void> CVersionManagerHelper::f_AbortAll() const
	{
		auto &Internal = *mp_pInternal;

		TCFutureVector<void> Destroys;

		for (auto &pState : Internal.m_States)
		{
			auto pLockedState = pState.f_Lock();
			if (pLockedState)
				pLockedState->f_Abort() > Destroys;
		}

		TCPromiseFuturePair<void> Promise;

		fg_AllDoneWrapped(Destroys) > fg_Move(Promise.m_Promise).f_ReceiveAny();

		return fg_Move(Promise.m_Future);
	}

	TCUnsafeFuture<CVersionManagerHelper::CUploadResult> CVersionManagerHelper::f_Upload
		(
			TCDistributedActor<CVersionManager> _VersionManager
			, CStr _Application
			, CVersionManager::CVersionIDAndPlatform _VersionID
			, CVersionManager::CVersionInformation _VersionInfo
			, NStr::CStr _SourceTGZFile
			, CVersionManager::CStartUploadVersion::EFlag _Flags
			, uint64 _QueueSize
		) const
	{
		auto &Internal = *mp_pInternal;

		TCSharedPointer<CUploadState, CSupportWeakTag> pState = fg_Construct();

		pState->m_UploadVersionSend = fg_ConstructActor<CFileTransferSend>(_SourceTGZFile);

		CVersionManager::CStartUploadVersion StartUpload;
		StartUpload.m_Application = _Application;
		StartUpload.m_VersionIDAndPlatform = _VersionID;
		StartUpload.m_VersionInfo = _VersionInfo;
		StartUpload.m_QueueSize = _QueueSize ? _QueueSize : Internal.m_QueueSize;
		StartUpload.m_Flags = _Flags;

		StartUpload.m_fStartTransfer = g_ActorFunctor / [pState](CVersionManager::CStartUploadTransfer _Params) -> TCFuture<CVersionManager::CStartUploadTransfer::CResult>
			{
				CVersionManager::CStartUploadTransfer::CResult Result;

				Result.m_Subscription = co_await pState->m_UploadVersionSend.f_Bind<&CFileTransferSend::f_SendFiles>(fg_Move(_Params.m_TransferContext));

				co_return fg_Move(Result);
			}
		;

		auto pCleanupAfterTimeout = g_OnScopeExitActor / [pState]
			{
				(void)pState->f_Abort();
			}
		;

		CStr StateID = fg_RandomID(Internal.m_States);
		Internal.m_States[StateID] = pState;

		auto pStateCleanup = g_OnScopeExitActor / [StateID, pInternal = mp_pInternal]
			{
				auto &Internal = *pInternal;
				Internal.m_States.f_Remove(StateID);
			}
		;

		auto Result = co_await
			(
				_VersionManager.f_CallActor(&CVersionManager::f_UploadVersion)(fg_Move(StartUpload))
				.f_Timeout(Internal.m_Timeout, "Timed out waiting for version manager to reply")
				%"Failed to start upload on remote server"
			)
		;

		pCleanupAfterTimeout->f_Clear();

		pState->m_fFinish = fg_Move(Result.m_fFinish);

		auto TransferResult = co_await pState->m_UploadVersionSend(&CFileTransferSend::f_GetResult);

		CUploadResult ReturnResult;
		ReturnResult.m_TransferResult = fg_Move(TransferResult);
		ReturnResult.m_DeniedTags = fg_Move(Result.m_DeniedTags);

		if (pState->m_fFinish.f_GetFunctor())
			co_await pState->m_fFinish();

		pState->m_fFinish.f_Clear();

		co_return fg_Move(ReturnResult);
	}

	TCUnsafeFuture<CFileTransferResult> CVersionManagerHelper::f_Download
		(
			TCDistributedActor<CVersionManager> _VersionManager
			, CStr _Application
			, CVersionManager::CVersionIDAndPlatform _VersionID
			, CStr _DestinationDirectory
			, CFileTransferReceive::EReceiveFlag _ReceiveFlags
			, uint64 _QueueSize
		) const
	{
		auto &Internal = *mp_pInternal;
		TCSharedPointer<CDownloadState, CSupportWeakTag> pState = fg_Construct();

		pState->m_DownloadVersionReceive = fg_ConstructActor<CFileTransferReceive>
			(
				_DestinationDirectory
				, EFileAttrib_UserRead | EFileAttrib_UserWrite | EFileAttrib_UserExecute | EFileAttrib_UnixAttributesValid
				, EFileAttrib_UserRead | EFileAttrib_UserWrite | EFileAttrib_UnixAttributesValid
			)
		;

		CStr StateID = fg_RandomID(Internal.m_States);
		Internal.m_States[StateID] = pState;

		auto pStateCleanup = g_OnScopeExitActor / [StateID, pInternal = mp_pInternal]
			{
				auto &Internal = *pInternal;
				Internal.m_States.f_Remove(StateID);
			}
		;

		auto TransferContext = co_await
			(
				pState->m_DownloadVersionReceive(&CFileTransferReceive::f_ReceiveFiles, _QueueSize ? _QueueSize : Internal.m_QueueSize, _ReceiveFlags)
				% "Failed to initialize file transfer context"
			)
		;

		CVersionManager::CStartDownloadVersion StartDownload;
		StartDownload.m_Application = _Application;
		StartDownload.m_VersionIDAndPlatform = _VersionID;
		StartDownload.m_TransferContext = fg_Move(TransferContext);

		if (_VersionManager->f_InterfaceVersion() >= CVersionManager::EProtocolVersion_RefactorToActorFunctorsUploadDownload)
			StartDownload.m_Subscription = co_await pState->m_DownloadVersionReceive.f_Bind<&CFileTransferReceive::f_GetAbortSubscription>();

		auto pCleanupAfterTimeout = g_OnScopeExitActor / [pState]
			{
				(void)pState->f_Abort();
			}
		;

		auto Result = co_await
			(
				_VersionManager.f_CallActor(&CVersionManager::f_DownloadVersion)(fg_Move(StartDownload))
				.f_Timeout(Internal.m_Timeout, "Timed out waiting for version manager to reply")
				% "Failed to start download on remote server"
			)
		;

		pCleanupAfterTimeout->f_Clear();
		pState->m_DownloadVersionSubscription = fg_Move(Result.m_Subscription);

		auto Results = co_await pState->m_DownloadVersionReceive(&CFileTransferReceive::f_GetResult);
		pState->m_DownloadVersionSubscription.f_Clear();

		co_return fg_Move(Results);
	}

	namespace
	{
		CVersionManagerHelper::CPackageInfo fg_ExtractPackageInfo(CEJSONSorted const &_VersionInfoJSON)
		{
			CVersionManagerHelper::CPackageInfo PackageInfo;

			{
				CStr Version;
				if (auto *pValue = _VersionInfoJSON.f_GetMember("Version", EJSONType_String))
					Version = pValue->f_String();

				CStr Error;
				if (!CVersionManager::fs_IsValidVersionIdentifier(Version, Error, &PackageInfo.m_VersionID.m_VersionID))
					DMibError(fg_Format("Version identifier format is invalid: {}", Error));
			}

			{
				CStr Platform;
				if (auto *pValue = _VersionInfoJSON.f_GetMember("Platform", EJSONType_String))
					Platform = pValue->f_String();

				if (!CVersionManager::fs_IsValidPlatform(Platform))
					DMibError("Invalid version platform format");

				PackageInfo.m_VersionID.m_Platform = Platform;
			}

			if (auto *pValue = _VersionInfoJSON.f_GetMember("Configuration", EJSONType_String))
				PackageInfo.m_VersionInfo.m_Configuration = pValue->f_String();

			if (auto *pValue = _VersionInfoJSON.f_GetMember("ExtraInfo", EJSONType_Object))
				PackageInfo.m_VersionInfo.m_ExtraInfo = pValue->f_Object();

			return PackageInfo;
		}
	}

	NConcurrency::TCFuture<CVersionManagerHelper::CPackageInfo> CVersionManagerHelper::f_GetPackageInfo(NStr::CStr const &_PackageFile) const
	{
		return g_DirectDispatch / [pInternal = mp_pInternal, _PackageFile]() -> TCFuture<CVersionManagerHelper::CPackageInfo>
			{
				auto &Internal = *pInternal;

				TCSharedPointer<CProcessLaunchState, CSupportWeakTag> pState = fg_Construct();
				pState->m_Launch = fg_ConstructActor<CProcessLaunchActor>();
				pState->m_Launch2 = fg_ConstructActor<CProcessLaunchActor>();

				CStr StateID = fg_RandomID(Internal.m_States);
				Internal.m_States[StateID] = pState;

				auto pStateCleanup = g_OnScopeExitActor / [StateID, pInternal]
					{
						auto &Internal = *pInternal;
						Internal.m_States.f_Remove(StateID);
					}
				;

				CProcessLaunchActor::CSimpleLaunch Launch
					{
						Internal.m_RootDirectory / "bin/bsdtar"
						,
						{
							"-xqOf"
							, _PackageFile
							, "*VersionInfo.json"
						}
						, CFile::fs_GetPath(_PackageFile)
						, CProcessLaunchActor::ESimpleLaunchFlag_GenerateExceptionOnNonZeroExitCode
					}
				;

				auto VersionInfoStr = (co_await pState->m_Launch(&CProcessLaunchActor::f_LaunchSimple, fg_Move(Launch))).f_GetStdOut();

				CEJSONSorted VersionInfo = CEJSONSorted::fs_FromString(VersionInfoStr);

				CVersionManagerHelper::CPackageInfo PackageInfo = fg_ExtractPackageInfo(VersionInfo);

				CProcessLaunchActor::CSimpleLaunch LaunchList
					{
						Internal.m_RootDirectory / "bin/bsdtar"
						,
						{
							"-tvvf"
							, _PackageFile
						}
						, CFile::fs_GetPath(_PackageFile)
						, CProcessLaunchActor::ESimpleLaunchFlag_GenerateExceptionOnNonZeroExitCode
					}
				;

				auto ListStr = (co_await pState->m_Launch2(&CProcessLaunchActor::f_LaunchSimple, fg_Move(LaunchList))).f_GetStdOut();

				CTime Newest;
				for (auto &Line : ListStr.f_SplitLine<true>())
				{
					ch8 const *pParse = Line.f_GetStr();

					auto fParseField = [&]() -> CStr
						{
							ch8 const *pStart = pParse;
							fg_ParseNonWhiteSpaceAndSeparators(pParse, "");
							CStr Field(pStart, pParse - pStart);
							fg_ParseWhiteSpace(pParse);
							return Field;
						}
					;

					fParseField(); // Permissions
					fParseField(); // ?
					fParseField(); // User
					fParseField(); // Group
					fParseField(); // Size

					CStr UnixSeconds = fParseField();
					CStr NanoSeconds = fParseField();

					auto FileTime = CTimeConvert::fs_FromUnixSeconds(UnixSeconds.f_ToInt(int64(0)));
					FileTime.f_SetFraction(fp64(NanoSeconds.f_ToInt(int32(0))) / fp64(1'000'000'000.0));

					if (!Newest.f_IsValid() || FileTime > Newest)
						Newest = FileTime;
				}

				PackageInfo.m_VersionInfo.m_Time = CTimeConvert::fs_FromUnixMilliseconds(CTimeConvert(Newest).f_UnixMilliseconds());

				co_return PackageInfo;
			}
		;
	}

	TCUnsafeFuture<CVersionManagerHelper::CPackageInfo> CVersionManagerHelper::f_CreatePackage(NStr::CStr _SourceDirectory, NStr::CStr _DestinationFileName, uint32 _CompressionLevel) const
	{
		auto pInternal = mp_pInternal;
		auto &Internal = *pInternal;
		TCSharedPointer<CProcessLaunchState, CSupportWeakTag> pState = fg_Construct();
		pState->m_Launch = fg_ConstructActor<CProcessLaunchActor>();

		CStr StateID = fg_RandomID(Internal.m_States);
		Internal.m_States[StateID] = pState;

		auto pStateCleanup = g_OnScopeExitActor / [StateID, pInternal = mp_pInternal]
			{
				auto &Internal = *pInternal;
				Internal.m_States.f_Remove(StateID);
			}
		;

		TCVector<CStr> Params =
			{
				"--disable-copyfile"
			}
		;

		if (_CompressionLevel > 0)
		{
			Params.f_Insert
				(
					{
						"--options"
						, "gzip:compression-level={}"_f << _CompressionLevel
						, "-czf"
					}
				)
			;
		}
		else
			Params.f_Insert({"-cf"});

		Params.f_Insert
			(
				{
					_DestinationFileName
					, "."
				}
			)
		;

		CProcessLaunchActor::CSimpleLaunch Launch
			{
				Internal.m_RootDirectory / "bin/bsdtar"
				, Params
				, _SourceDirectory
				, CProcessLaunchActor::ESimpleLaunchFlag_GenerateExceptionOnNonZeroExitCode
			}
		;

		auto BlockingActorCheckout = fg_BlockingActor();
		auto [LaunchResult, PackageInfo] = co_await
			(
				pState->m_Launch(&CProcessLaunchActor::f_LaunchSimple, fg_Move(Launch))
				+ g_Dispatch(BlockingActorCheckout) / [=]() -> CPackageInfo
				{
					auto Files = CFile::fs_FindFiles(_SourceDirectory + "/*VersionInfo.json", EFileAttrib_File, false);
					if (Files.f_IsEmpty())
						DMibError("No VersionInfo.json file found in package source directory");
					if (Files.f_GetLen() > 1)
						DMibError("Several VersionInfo.json file found in package source directory");

					CStr VersionInfoFile = Files[0];
					CEJSONSorted VersionInfoJSON = CEJSONSorted::fs_FromString(CFile::fs_ReadStringFromFile(VersionInfoFile, true), VersionInfoFile);

					CPackageInfo PackageInfo = fg_ExtractPackageInfo(VersionInfoJSON);

					{
						CFile::CFindFilesOptions FindOptions{_SourceDirectory + "/*", true};
						FindOptions.m_AttribMask = EFileAttrib_File;
						CTime Newest;
						for (auto &FoundFile : CFile::fs_FindFiles(FindOptions))
						{
							auto FoundTime = CFile::fs_GetWriteTime(FoundFile.m_Path);
							if (!Newest.f_IsValid() || FoundTime > Newest)
								Newest = FoundTime;
						}
						PackageInfo.m_VersionInfo.m_Time = CTimeConvert::fs_FromUnixMilliseconds(CTimeConvert(Newest).f_UnixMilliseconds());
					}
					return PackageInfo;
				}
			)
		;

		co_return fg_Move(PackageInfo);
	}
}
