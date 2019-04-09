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
		struct CState : public TCSharedPointerIntrusiveBase<ESharedPointerOption_SupportWeakPointer>
		{
			virtual ~CState() = default;
			virtual TCFuture<void> f_Abort() = 0;
		};
		
		struct CDownloadState : public CState 
		{
			
			TCActor<CFileTransferReceive> m_DownloadVersionReceive;
			CActorSubscription m_DownloadVersionSubscription;
			
			TCFuture<void> f_Abort()
			{
				m_DownloadVersionReceive.f_Clear();
				m_DownloadVersionSubscription.f_Clear();
				if (m_DownloadVersionReceive)
					return m_DownloadVersionReceive->f_Destroy();
				return fg_Explicit();
			}
		};

		struct CUploadState : public CState
		{
			TCActor<CFileTransferSend> m_UploadVersionSend;
			CActorSubscription m_UploadVersionSubscription;
			
			TCFuture<void> f_Abort()
			{
				m_UploadVersionSend.f_Clear();
				m_UploadVersionSubscription.f_Clear();
				if (m_UploadVersionSend)
					return m_UploadVersionSend->f_Destroy();
				return fg_Explicit();
			}
		};

		struct CProcessLaunchState : public CState
		{
			TCActor<CProcessLaunchActor> m_Launch;
			
			TCFuture<void> f_Abort()
			{
				if (m_Launch)
					return m_Launch->f_Destroy();
				return fg_Explicit();
			}
		};
	}

	struct CVersionManagerHelperInternal
	{
		CVersionManagerHelperInternal(TCActor<CSeparateThreadActor> const &_FileActor, uint64 _QueueSize, fp64 _Timeout, CStr const &_RootDirectory)
			: m_FileActor(_FileActor)
			, m_QueueSize(_QueueSize)
			, m_Timeout(_Timeout)
			, m_RootDirectory(_RootDirectory)
		{
		}

		TCActor<CSeparateThreadActor> f_GetFileActor()
		{
			if (!m_FileActor)
				m_FileActor = fg_ConstructActor<CSeparateThreadActor>(fg_Construct("File Actor (version manager helper)"));
			return m_FileActor;
		}

		TCActor<CSeparateThreadActor> m_FileActor;
		CStr m_RootDirectory;
		TCMap<CStr, TCWeakPointer<CState>> m_States;
		fp64 m_Timeout = 0.0;
		uint64 m_QueueSize = 0;
	};
	
	CVersionManagerHelper::CVersionManagerHelper
		(
		 	CStr const &_RootDirectory
 			, TCActor<CSeparateThreadActor> const &_FileActor
			, uint64 _QueueSize
			, fp64 _Timeout
		)
		: mp_pInternal(fg_Construct(_FileActor, _QueueSize, _Timeout, _RootDirectory))
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
		
		TCActorResultVector<void> Destroys;
		
		for (auto &pState : Internal.m_States)
		{
			auto pLockedState = pState.f_Lock();
			if (pLockedState)
				pLockedState->f_Abort() > Destroys.f_AddResult();
		}
		
		TCPromise<void> Promise;
		Destroys.f_GetResults() > Promise.f_ReceiveAny();
		
		return Promise.f_MoveFuture();
	}
	
	TCFuture<CVersionManagerHelper::CUploadResult> CVersionManagerHelper::f_Upload
		(
			TCDistributedActor<CVersionManager> const &_VersionManager
			, CStr const &_Application
			, CVersionManager::CVersionIDAndPlatform const &_VersionID
			, CVersionManager::CVersionInformation const &_VersionInfo
			, NStr::CStr const &_SourceTGZFile
			, CVersionManager::CStartUploadVersion::EFlag _Flags
			, uint64 _QueueSize
		) const
	{
		auto &Internal = *mp_pInternal;
		
		TCPromise<CUploadResult> Promise;
		
		TCSharedPointer<CUploadState, CSupportWeakTag> pState = fg_Construct();
		
		pState->m_UploadVersionSend = fg_ConstructActor<CFileTransferSend>(_SourceTGZFile);
		
		CVersionManager::CStartUploadVersion StartUpload;
		StartUpload.m_Application = _Application;
		StartUpload.m_VersionIDAndPlatform = _VersionID;
		StartUpload.m_VersionInfo = _VersionInfo;
		StartUpload.m_QueueSize = _QueueSize ? _QueueSize : Internal.m_QueueSize;
		StartUpload.m_Flags = _Flags;
		
		StartUpload.m_DispatchActor = fg_CurrentActor();
		StartUpload.m_fStartTransfer = [pState](CVersionManager::CStartUploadTransfer &&_Params) 
			-> TCFuture<CVersionManager::CStartUploadTransfer::CResult>
			{
				TCPromise<CVersionManager::CStartUploadTransfer::CResult> StartTransferPromise;
				pState->m_UploadVersionSend(&CFileTransferSend::f_SendFiles, fg_Move(_Params.m_TransferContext)) 
					> StartTransferPromise / [pState, StartTransferPromise](CActorSubscription &&_Subscription)
					{
						CVersionManager::CStartUploadTransfer::CResult Result;
						Result.m_Subscription = fg_Move(_Subscription);
						StartTransferPromise.f_SetResult(fg_Move(Result));
					}
				;
				return StartTransferPromise.f_MoveFuture();
			}
		;
		
		auto pCleanupAfterTimeout = g_OnScopeExitActor > [pState]
			{
				(void)pState->f_Abort();
			}
		;

		CStr StateID = fg_RandomID();
		Internal.m_States[StateID] = pState;
		
		auto pStateCleanup = g_OnScopeExitActor > [StateID, pInternal = mp_pInternal]
			{
				auto &Internal = *pInternal;
				Internal.m_States.f_Remove(StateID);
			}
		;
		
		DMibCallActor
			(
				_VersionManager
				, CVersionManager::f_UploadVersion
				, fg_Move(StartUpload)
			)
			.f_Timeout(Internal.m_Timeout, "Timed out waiting for version manager to reply")
			> Promise % "Failed to start upload on remote server" 
			/ [pCleanupAfterTimeout, pState, Promise, pStateCleanup]
			(CVersionManager::CStartUploadVersion::CResult &&_Result)
			{
				pCleanupAfterTimeout->f_Clear();
				
				pState->m_UploadVersionSubscription = fg_Move(_Result.m_Subscription);
				pState->m_UploadVersionSend(&CFileTransferSend::f_GetResult) 
					> Promise / [pState, Promise, DeniedTags = _Result.m_DeniedTags, pStateCleanup](CFileTransferResult &&_Result) mutable
					{
						pState->m_UploadVersionSubscription.f_Clear();
						
						CUploadResult Result;
						Result.m_TransferResult = fg_Move(_Result);
						Result.m_DeniedTags = fg_Move(DeniedTags);
						
						Promise.f_SetResult(fg_Move(Result));
					}
				;
			}
		;
		
		return Promise.f_MoveFuture();
	}
	
	TCFuture<CFileTransferResult> CVersionManagerHelper::f_Download
		(
			TCDistributedActor<CVersionManager> const &_VersionManager
			, CStr const &_Application
			, CVersionManager::CVersionIDAndPlatform const &_VersionID
			, CStr const &_DestinationDirectory
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
			 	, Internal.f_GetFileActor()
			)
		;

		CStr StateID = fg_RandomID();
		Internal.m_States[StateID] = pState;
		
		auto pStateCleanup = g_OnScopeExitActor > [StateID, pInternal = mp_pInternal]
			{
				auto &Internal = *pInternal;
				Internal.m_States.f_Remove(StateID);
			}
		;
		
		TCPromise<CFileTransferResult> Promise;

		pState->m_DownloadVersionReceive(&CFileTransferReceive::f_ReceiveFiles, _QueueSize ? _QueueSize : Internal.m_QueueSize, _ReceiveFlags) 
			> Promise % "Failed to initialize file transfer context" 
			/ [_VersionManager, _Application, _VersionID, pInternal = mp_pInternal, Promise, pState, pStateCleanup]
			(CFileTransferContext &&_TransferContext)
			{
				auto &Internal = *pInternal;
				
				CVersionManager::CStartDownloadVersion StartDownload;
				StartDownload.m_Application = _Application;
				StartDownload.m_VersionIDAndPlatform = _VersionID;
				StartDownload.m_TransferContext = fg_Move(_TransferContext);

				auto pCleanupAfterTimeout = g_OnScopeExitActor > [pState]
					{
						(void)pState->f_Abort();
					}
				;

				DMibCallActor
					(
						_VersionManager
						, CVersionManager::f_DownloadVersion
						, fg_Move(StartDownload)
					)
					.f_Timeout(Internal.m_Timeout, "Timed out waiting for version manager to reply")
					> Promise % "Failed to start download on remote server" 
					/ [pState, pCleanupAfterTimeout, Promise, pStateCleanup](CVersionManager::CStartDownloadVersion::CResult &&_Result)
					{
						pCleanupAfterTimeout->f_Clear();
						pState->m_DownloadVersionSubscription = fg_Move(_Result.m_Subscription);

						pState->m_DownloadVersionReceive(&CFileTransferReceive::f_GetResult) > [pState, Promise](TCAsyncResult<CFileTransferResult> &&_Results)
							{
								pState->m_DownloadVersionSubscription.f_Clear();
								Promise.f_SetResult(fg_Move(_Results));
							}
						;
					}
				;
			}
		;
		
		return Promise.f_MoveFuture();
	}
	
	TCActor<CSeparateThreadActor> CVersionManagerHelper::f_GetFileActor() const
	{
		auto &Internal = *mp_pInternal;
		return Internal.f_GetFileActor();
	}
	
	TCFuture<CVersionManagerHelper::CPackageInfo> CVersionManagerHelper::f_CreatePackage(NStr::CStr const &_SourceDirectory, NStr::CStr const &_DestinationFileName) const
	{
		auto &Internal = *mp_pInternal;
		TCSharedPointer<CProcessLaunchState, CSupportWeakTag> pState = fg_Construct();
		pState->m_Launch = fg_ConstructActor<CProcessLaunchActor>();

		CStr StateID = fg_RandomID();
		Internal.m_States[StateID] = pState;
		
		auto pStateCleanup = g_OnScopeExitActor > [StateID, pInternal = mp_pInternal]
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
					"--disable-copyfile"
					, "-czf"
					, _DestinationFileName
					, "."
				}
				, _SourceDirectory
				, CProcessLaunchActor::ESimpleLaunchFlag_GenerateExceptionOnNonZeroExitCode
			}
		;
	
		TCPromise<CPackageInfo> Promise;
		pState->m_Launch(&CProcessLaunchActor::f_LaunchSimple, fg_Move(Launch))
			+ fg_Dispatch
			(
				Internal.f_GetFileActor()
				, [=]() -> CPackageInfo
				{
					auto Files = CFile::fs_FindFiles(_SourceDirectory + "/*VersionInfo.json", EFileAttrib_File, false);
					if (Files.f_IsEmpty())
						DMibError("No VersionInfo.json file found in package source directory");
					if (Files.f_GetLen() > 1)
						DMibError("Several VersionInfo.json file found in package source directory");
					
					CStr VersionInfoFile = Files[0];
					CEJSON VersionInfoJSON = CEJSON::fs_FromString(CFile::fs_ReadStringFromFile(VersionInfoFile, true), VersionInfoFile);
					
					CPackageInfo PackageInfo;
					
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
						PackageInfo.m_VersionInfo.m_Time = Newest;
					}
					
					{
						CStr Version;
						if (auto *pValue = VersionInfoJSON.f_GetMember("Version", EJSONType_String))
							Version = pValue->f_String();
						
						CStr Error; 
						if (!CVersionManager::fs_IsValidVersionIdentifier(Version, Error, &PackageInfo.m_VersionID.m_VersionID))
							DMibError(fg_Format("Version identifier format is invalid: {}", Error));
					}

					{
						CStr Platform;
						if (auto *pValue = VersionInfoJSON.f_GetMember("Platform", EJSONType_String))
							Platform = pValue->f_String();
						
						if (!CVersionManager::fs_IsValidPlatform(Platform))
							DMibError("Invalid version platform format");
						
						PackageInfo.m_VersionID.m_Platform = Platform;
					}
					
					if (auto *pValue = VersionInfoJSON.f_GetMember("Configuration", EJSONType_String))
						PackageInfo.m_VersionInfo.m_Configuration = pValue->f_String();
					
					if (auto *pValue = VersionInfoJSON.f_GetMember("ExtraInfo", EJSONType_Object))
						PackageInfo.m_VersionInfo.m_ExtraInfo = pValue->f_Object();
					
					return PackageInfo;
				}
			)
			> Promise 
			/ [pState, pStateCleanup, Promise](CProcessLaunchActor::CSimpleLaunchResult &&_LaunchResult, CPackageInfo &&_PackageInfo)
			{
				Promise.f_SetResult(fg_Move(_PackageInfo));
			}
		;
		
		return Promise.f_MoveFuture();
	}
}
