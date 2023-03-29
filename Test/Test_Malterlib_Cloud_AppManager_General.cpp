// Copyright © 2020 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Test_Malterlib_Cloud_AppManager.h"

#ifdef DPlatformFamily_Windows
#include <Windows.h>
#endif

static fp64 g_Timeout = 120.0 * NMib::NTest::gc_TimeoutMultiplier;

class CAppManager_General_Tests : public NMib::NTest::CTest
{
public:
	struct CApplicationKey
	{
		auto operator <=> (CApplicationKey const &_Right) const = default;

		template <typename tf_CStr>
		void f_Format(tf_CStr &o_Str) const
		{
			o_Str += typename tf_CStr::CFormat("{}:{}") << m_AppName << m_iAppManager;
		}

		CStr m_AppName;
		mint m_iAppManager;
	};

	struct CUpdateNotificationsApplicationState
	{
		TCSet<CApplicationKey> m_InProgress;
		mint m_nMaxInProgress = 0;

		TCMap<CApplicationKey, CAppManagerInterface::EUpdateStage> m_LastInStage;
		TCMap<CApplicationKey, CAppManagerInterface::EUpdateStage> m_LastInStageCoordination;
		TCMap<CAppManagerInterface::EUpdateStage, TCSet<CApplicationKey>> m_InStage;
		TCMap<CAppManagerInterface::EUpdateStage, TCSet<CApplicationKey>> m_InStageCoordination;

		TCMap<CAppManagerInterface::EUpdateStage, zmint> m_MaxInStage;
		TCMap<CAppManagerInterface::EUpdateStage, zmint> m_MaxInStageCoordination;
		TCAtomic<mint> m_nSuccess = 0;
		TCAtomic<mint> m_nFinished = 0;

		template <typename tf_CStr>
		void f_Format(tf_CStr &o_Str) const
		{
			o_Str += typename tf_CStr::CFormat("InProgress: {}\n") << m_InProgress;
			o_Str += typename tf_CStr::CFormat("LastInStage: {}\n") << m_LastInStage;
			o_Str += typename tf_CStr::CFormat("LastInStageCoordination: {}\n") << m_LastInStageCoordination;
			o_Str += typename tf_CStr::CFormat("InStage: {}\n") << m_InStage;
			o_Str += typename tf_CStr::CFormat("InStageCoordination: {}\n") << m_InStageCoordination;
			o_Str += typename tf_CStr::CFormat("MaxInStage: {}\n") << m_MaxInStage;
			o_Str += typename tf_CStr::CFormat("MaxInStageCoordination: {}\n") << m_MaxInStageCoordination;
			o_Str += typename tf_CStr::CFormat("nSuccess: {}\n") << m_nSuccess.f_Load();
			o_Str += typename tf_CStr::CFormat("nFinished: {}\n") << m_nFinished.f_Load();
		}

		void f_Clear()
		{
			m_InProgress.f_Clear();
			m_LastInStage.f_Clear();
			m_InStage.f_Clear();
			m_MaxInStage.f_Clear();
			m_nMaxInProgress = 0;
			m_nSuccess = 0;
			m_nFinished = 0;
		}
	};

	struct CUpdateNotificationsState : CAllowUnsafeThis
	{
		CUpdateNotificationsState()
		{
		}

		void f_Destroy()
		{
			TCActorResultVector<void> Destroys;
			for (auto &Subscription : m_Subscriptions)
				Subscription->f_Destroy() > Destroys.f_AddResult();;
			Destroys.f_GetResults() > fg_DiscardResult();
			f_Clear();
		}

		void f_Clear()
		{
			for (auto &Application : m_Applications)
				Application.f_Clear();

			for (auto &AppManager : m_ApplicationsPerAppmanager)
			{
				for (auto &Application : AppManager)
					Application.f_Clear();
			}

			m_AllApplications.f_Clear();
			m_AppsInProgressPerAppManager.f_Clear();
			m_MaxAppsInProgressPerAppManager.f_Clear();
			m_nAppsInProgress = 0;
			m_nMaxAppsInProgress = 0;
			m_nMaxAppsInProgressPerAppManager = 0;
		}

		void f_Signal()
		{
			m_bSignalCompletion = true;
			auto ToWake = fg_Move(m_WaitingSignals);
			for (auto &Promise : ToWake)
				Promise.f_SetResult();
		}

		TCFuture<bool> f_Wait()
		{
			auto Future = m_WaitingSignals.f_Insert().f_Future();
			auto Result = co_await fg_Move(Future).f_Timeout(g_Timeout, "Timed out waiting for notification state").f_Wrap();
			co_return !Result;
		}

		bool m_bSignalCompletion = false;
		TCVector<CActorSubscription> m_Subscriptions;
		TCMap<CStr, CUpdateNotificationsApplicationState> m_Applications;
		TCMap<mint, TCMap<CStr, CUpdateNotificationsApplicationState>> m_ApplicationsPerAppmanager;
		CUpdateNotificationsApplicationState m_AllApplications;
		mint m_nAppsInProgress = 0;
		mint m_nMaxAppsInProgress = 0;
		TCMap<mint, zmint> m_AppsInProgressPerAppManager;
		TCMap<mint, zmint> m_MaxAppsInProgressPerAppManager;
		mint m_nMaxAppsInProgressPerAppManager = 0;
		TCVector<TCPromise<void>> m_WaitingSignals;
	};

	void f_DoTests()
	{
		DMibTestSuite("Upgrades") -> TCFuture<void>
		{
			CAppManagerTestHelper::EOption Options = CAppManagerTestHelper::EOption_EnableVersionManager;

			if (fg_TestReportFlags() & ETestReportFlag_EnableLogs)
				Options |= CAppManagerTestHelper::EOption_EnableOtherOutput;

			CAppManagerTestHelper AppManagerTestHelper("AppManagerTests", Options, g_Timeout);

			auto &PackageInfo = AppManagerTestHelper.m_PackageInfo;
			auto &TestAppArchive = AppManagerTestHelper.m_TestAppArchive;
			auto &VersionManager = AppManagerTestHelper.m_VersionManager;
			auto &AppManagers = AppManagerTestHelper.m_AppManagerInfos;

			// Copy AppManagers to their directories
			mint nAppManagers = 10;
#if DMibPPtrBits <= 32
			nAppManagers = 2;
#elif defined(DMibSanitizerEnabled_Address) || defined(DMibSanitizerEnabled_Thread)
			nAppManagers = 2;
#endif
			co_await AppManagerTestHelper.f_Setup(nAppManagers);

			// Update Application
			auto fUpdateTestApp = [&](TCSet<CStr> _Tags) -> CUnsafeFuture
				{
					++PackageInfo.m_VersionID.m_VersionID.m_Revision;
					PackageInfo.m_VersionInfo.m_Tags = _Tags;
					co_await
						(
							g_Dispatch /
							[=, VersionManagerHelper = AppManagerTestHelper.m_VersionManagerHelper]() -> TCFuture<void>
							{
								co_await VersionManagerHelper.f_Upload(VersionManager, "TestApp", PackageInfo.m_VersionID, PackageInfo.m_VersionInfo, TestAppArchive);
								co_return {};
							}
						)
						.f_Timeout(g_Timeout, "Timed out waiting for test app to update")
					;
					co_return {};
				}
			;

			auto fTagApp = [&](CStr _Name, TCSet<CStr> _Tags) -> CUnsafeFuture
				{
					CVersionManager::CChangeTags ChangeTags;
					ChangeTags.m_AddTags = _Tags;
					ChangeTags.m_Application = _Name;
					ChangeTags.m_VersionID = PackageInfo.m_VersionID.m_VersionID;
					co_await VersionManager.f_CallActor(&CVersionManager::f_ChangeTags)(fg_Move(ChangeTags)).f_Timeout(g_Timeout, "Timed out waiting change tags to finish");

					co_return {};
				}
			;

			auto fProcessApplicationState = [](CUpdateNotificationsApplicationState &_State, CAppManagerInterface::CUpdateNotification const &_Notification, CApplicationKey const &_ApplicationKey)
				{
					if (!_Notification.m_bCoordinateWait)
					{
						if (auto pInStage = _State.m_LastInStage.f_FindEqual(_ApplicationKey))
							_State.m_InStage[*pInStage].f_Remove(_ApplicationKey);
						_State.m_InStage[_Notification.m_Stage][_ApplicationKey];
						_State.m_MaxInStage[_Notification.m_Stage] = fg_Max
							(
								_State.m_MaxInStage[_Notification.m_Stage]
								, _State.m_InStage[_Notification.m_Stage].f_GetLen()
							)
						;
						_State.m_LastInStage[_ApplicationKey] = _Notification.m_Stage;
					}

					if (auto pInStage = _State.m_LastInStageCoordination.f_FindEqual(_ApplicationKey))
						_State.m_InStageCoordination[*pInStage].f_Remove(_ApplicationKey);
					_State.m_InStageCoordination[_Notification.m_Stage][_ApplicationKey];
					_State.m_MaxInStageCoordination[_Notification.m_Stage] = fg_Max
						(
							_State.m_MaxInStageCoordination[_Notification.m_Stage]
							, _State.m_InStageCoordination[_Notification.m_Stage].f_GetLen()
						)
					;
					_State.m_LastInStageCoordination[_ApplicationKey] = _Notification.m_Stage;

					if (_Notification.m_Stage == CAppManagerInterface::EUpdateStage_Failed)
					{
						_State.m_InProgress.f_Remove(_ApplicationKey);
						++_State.m_nFinished;
					}
					else if (!_Notification.m_bCoordinateWait && _Notification.m_Stage == CAppManagerInterface::EUpdateStage_Finished)
					{
						_State.m_InProgress.f_Remove(_ApplicationKey);
						++_State.m_nFinished;
						++_State.m_nSuccess;
					}
					else if (!_Notification.m_bCoordinateWait && _Notification.m_Stage >= CAppManagerInterface::EUpdateStage_StopOldApp)
					{
						_State.m_InProgress[_ApplicationKey];
						_State.m_nMaxInProgress = fg_Max(_State.m_nMaxInProgress, _State.m_InProgress.f_GetLen());
					}
				}
			;

			TCSharedPointer<CUpdateNotificationsState> pUpdateNotificationsState = fg_Construct();

			pUpdateNotificationsState->m_Applications["TestApp"];
			pUpdateNotificationsState->m_Applications["TestApp2"];

			auto CleanupNotifications = g_OnScopeExit / [&]
				{
					pUpdateNotificationsState->f_Destroy();
				}
			;

			auto &UpdateNotificationState = *pUpdateNotificationsState;

			TCSharedPointer<CStr> pUpdateType = fg_Construct();

			// Subscribe for notifications
			{
				TCActorResultVector<void> AppCommandResults;
				mint iAppManager = 0;
				for (auto &AppManager : AppManagers)
				{
					TCPromise<void> Promise;
					AppManager.m_Interface.f_CallActor(&CAppManagerInterface::f_SubscribeUpdateNotifications)
						(
							g_ActorFunctor / [pUpdateNotificationsState, iAppManager, fProcessApplicationState]
							(CAppManagerInterface::CUpdateNotification const &_Notification) -> TCFuture<void>
							{
								CApplicationKey ApplicationKey{_Notification.m_Application, iAppManager};

								auto &WholeState = *pUpdateNotificationsState;

								fProcessApplicationState(WholeState.m_Applications[_Notification.m_Application], _Notification, ApplicationKey);
								fProcessApplicationState(WholeState.m_ApplicationsPerAppmanager[iAppManager][_Notification.m_Application], _Notification, ApplicationKey);
								fProcessApplicationState(WholeState.m_AllApplications, _Notification, ApplicationKey);
								//DMibConOut2("{} {a-,sj9} {} {vs}\n", iAppManager, _Notification.m_Application, _Notification.m_Stage, WholeState.m_ApplicationsPerAppmanager[iAppManager][_Notification.m_Application].m_MaxInStage);
								//DMibConOut2("{} N {a-,sj9} {} {vs}\n", iAppManager, _Notification.m_Application, _Notification.m_Stage, WholeState.m_Applications[_Notification.m_Application].m_MaxInStage);
								//DMibConOut2("{} C {a-,sj9} {} {vs}\n", iAppManager, _Notification.m_Application, _Notification.m_Stage, WholeState.m_Applications[_Notification.m_Application].m_MaxInStageCoordination);
								//DMibConOut2("{a-,sj9} {} {vs}\n", _Notification.m_Application, CAppManagerInterface::EUpdateStage_StopOldApp, WholeState.m_AllApplications.m_MaxInStage);

								WholeState.m_nAppsInProgress = 0;
								for (auto &App : WholeState.m_Applications)
								{
									if (!App.m_InProgress.f_IsEmpty())
										++WholeState.m_nAppsInProgress;
								}
								WholeState.m_nMaxAppsInProgress = fg_Max(WholeState.m_nAppsInProgress, WholeState.m_nMaxAppsInProgress);

								WholeState.m_nMaxAppsInProgressPerAppManager = 0;
								for (auto &AppManager : WholeState.m_ApplicationsPerAppmanager)
								{
									auto &iAppManager = WholeState.m_ApplicationsPerAppmanager.fs_GetKey(AppManager);
									WholeState.m_AppsInProgressPerAppManager[iAppManager] = 0;
									for (auto &App : AppManager)
									{
										if (!App.m_InProgress.f_IsEmpty())
											++WholeState.m_AppsInProgressPerAppManager[iAppManager];
									}
									WholeState.m_MaxAppsInProgressPerAppManager[iAppManager] = fg_Max
										(
											WholeState.m_AppsInProgressPerAppManager[iAppManager]
											, WholeState.m_MaxAppsInProgressPerAppManager[iAppManager]
										)
									;
									WholeState.m_nMaxAppsInProgressPerAppManager = fg_Max
										(
											WholeState.m_nMaxAppsInProgressPerAppManager
											, WholeState.m_MaxAppsInProgressPerAppManager[iAppManager]
										)
									;
								}

								WholeState.f_Signal();

								co_return {};
							}
						)
						> Promise / [pUpdateNotificationsState, Promise](NConcurrency::TCActorSubscriptionWithID<> &&_Subscription)
						{
							pUpdateNotificationsState->m_Subscriptions.f_Insert(fg_Move(_Subscription));
							Promise.f_SetResult();
						}
					;

					Promise.f_MoveFuture() > AppCommandResults.f_AddResult();
					++iAppManager;
				}
				DMibTestMark;
				co_await (co_await AppCommandResults.f_GetResults().f_Timeout(g_Timeout, "Timed out waiting update notification subscriptions") | g_Unwrap);
			}

			auto fWaitForAllUpdated = [&](CStr _Application) -> CUnsafeFuture
				{
					auto &ApplicationState = pUpdateNotificationsState->m_Applications[_Application];
					NTime::CClock Clock{true};
					while (true)
					{
						if (ApplicationState.m_nFinished == nAppManagers)
							break;
						if (Clock.f_GetTime() > g_Timeout * 4.0)
							DMibError("Timed out waiting for all apps to update.\n{}\n"_f << ApplicationState);
						co_await pUpdateNotificationsState->f_Wait();
					}

					DMibTestMark;

					co_return {};
				}
			;

			auto fSetUpdateType = [&](CStr _AppName, CStr _UpdateType) -> CUnsafeFuture
				{
					PackageInfo.m_VersionInfo.m_ExtraInfo["ExecutableParameters"] = {"--update-type", _UpdateType, "--daemon-run-standalone"};
					TCActorResultVector<void> AppCommandResults;
					for (auto &AppManager : AppManagers)
					{
						CAppManagerInterface::CApplicationChangeSettings ChangeSettings;
						CAppManagerInterface::CApplicationSettings Settings;
						Settings.m_ExecutableParameters = {"--update-type", _UpdateType, "--daemon-run-standalone"};

						AppManager.m_Interface.f_CallActor(&CAppManagerInterface::f_ChangeSettings)(_AppName, ChangeSettings, Settings) > AppCommandResults.f_AddResult();
					}
					co_await (co_await AppCommandResults.f_GetResults().f_Timeout(g_Timeout, "Timed out waiting for update type change") | g_Unwrap);
					*pUpdateType = _UpdateType;

					co_return {};
				}
			;

			auto fSetUpdateGroup = [&](CStr _AppName, CStr _Group) -> CUnsafeFuture
				{
					TCActorResultVector<void> AppCommandResults;
					for (auto &AppManager : AppManagers)
					{
						CAppManagerInterface::CApplicationChangeSettings ChangeSettings;
						CAppManagerInterface::CApplicationSettings Settings;
						Settings.m_UpdateGroup = _Group;

						AppManager.m_Interface.f_CallActor(&CAppManagerInterface::f_ChangeSettings)(_AppName, ChangeSettings, Settings) > AppCommandResults.f_AddResult();
					}
					co_await (co_await AppCommandResults.f_GetResults().f_Timeout(g_Timeout, "Timed out waiting for update group change") | g_Unwrap);

					co_return {};
				}
			;

			co_await AppManagerTestHelper.f_CheckCloudManager(1);

			{
				DMibTestPath("Update Independent");
				DMibTestMark;
				co_await fSetUpdateType("TestApp", "Independent");
				DMibTestMark;
				UpdateNotificationState.f_Clear();
				DMibTestMark;
				co_await fUpdateTestApp({"TestTag"});
				DMibTestMark;
				co_await fWaitForAllUpdated("TestApp");

				DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_nSuccess.f_Load(), ==, nAppManagers);
				DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_nMaxInProgress, >= , 1u);
			}
			{
				DMibTestPath("Update OneAtATime");
				DMibTestMark;
				co_await fSetUpdateType("TestApp", "OneAtATime");
				DMibTestMark;
				UpdateNotificationState.f_Clear();
				DMibTestMark;
				co_await fUpdateTestApp({"TestTag"});
				DMibTestMark;
				co_await fWaitForAllUpdated("TestApp");

				DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_nSuccess.f_Load(), ==, nAppManagers);
				DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_nMaxInProgress, ==, 1);
				DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_MaxInStage[CAppManagerInterface::EUpdateStage_StopOldApp], ==, 1);
			}
			{
				DMibTestPath("Update AllAtOnce");
				DMibTestMark;
				co_await fSetUpdateType("TestApp", "AllAtOnce");
				DMibTestMark;
				UpdateNotificationState.f_Clear();
				DMibTestMark;
				co_await fUpdateTestApp({"TestTag"});
				DMibTestMark;
				co_await fWaitForAllUpdated("TestApp");

				DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_nSuccess.f_Load(), ==, nAppManagers);
				DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_nMaxInProgress, >= , 1u);
				DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_MaxInStageCoordination[CAppManagerInterface::EUpdateStage_StopOldApp], ==, nAppManagers);
			}

			if (!NMib::NTest::fg_GroupActive("Expensive"))
				co_return {};

			co_await AppManagerTestHelper.f_InstallTestApp("TestApp2", "TestTag2", "TestGroup2");

			for (mint i = 0; i < 2; ++i)
			{
				CStr Path;
				if (i == 0)
				{
					Path = "Same Update Group";
					DMibTestMark;
					co_await fSetUpdateGroup("TestApp", "TestGroup");
					DMibTestMark;
					co_await fSetUpdateGroup("TestApp2", "TestGroup");
				}
				else
				{
					Path = "Separate Update Group";
					DMibTestMark;
					co_await fSetUpdateGroup("TestApp", "TestGroup");
					DMibTestMark;
					co_await fSetUpdateGroup("TestApp2", "TestGroup2");
				}
				DMibTestPath(Path);
				{
					DMibTestPath("AllAtOnce AllAtOnce");
					co_await fSetUpdateType("TestApp", "AllAtOnce");
					DMibTestMark;
					co_await fSetUpdateType("TestApp2", "AllAtOnce");
					UpdateNotificationState.f_Clear();
					DMibTestMark;
					co_await fUpdateTestApp({});
					DMibTestMark;
					co_await fTagApp("TestApp", {"TestTag", "TestTag2"});
					DMibTestMark;
					co_await fWaitForAllUpdated("TestApp");
					DMibTestMark;
					co_await fWaitForAllUpdated("TestApp2");

					if (i == 0)
						DMibExpect(UpdateNotificationState.m_nMaxAppsInProgress, ==, 2u);
					else
						DMibExpect(UpdateNotificationState.m_nMaxAppsInProgress, ==, 1u);

					DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_nSuccess.f_Load(), ==, nAppManagers);
					DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_nMaxInProgress, >= , 1u);
					DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_MaxInStageCoordination[CAppManagerInterface::EUpdateStage_StopOldApp], ==, nAppManagers);

					DMibExpect(UpdateNotificationState.m_Applications["TestApp2"].m_nSuccess.f_Load(), ==, nAppManagers);
					DMibExpect(UpdateNotificationState.m_Applications["TestApp2"].m_nMaxInProgress, >= , 1u);
					DMibExpect(UpdateNotificationState.m_Applications["TestApp2"].m_MaxInStageCoordination[CAppManagerInterface::EUpdateStage_StopOldApp], ==, nAppManagers);

					DMibExpect(UpdateNotificationState.m_AllApplications.m_nSuccess.f_Load(), ==, nAppManagers * 2u);
					if (i == 0)
						DMibExpect(UpdateNotificationState.m_AllApplications.m_nMaxInProgress, ==, nAppManagers * 2u);
					else
						DMibExpect(UpdateNotificationState.m_AllApplications.m_nMaxInProgress, ==, nAppManagers);
					DMibExpect(UpdateNotificationState.m_AllApplications.m_MaxInStageCoordination[CAppManagerInterface::EUpdateStage_StopOldApp], ==, nAppManagers * 2u);
				}
				{
					DMibTestPath("OneAtATime OneAtATime");
					co_await fSetUpdateType("TestApp", "OneAtATime");
					DMibTestMark;
					co_await fSetUpdateType("TestApp2", "OneAtATime");
					UpdateNotificationState.f_Clear();
					DMibTestMark;
					co_await fUpdateTestApp({});
					DMibTestMark;
					co_await fTagApp("TestApp", {"TestTag", "TestTag2"});
					DMibTestMark;
					co_await fWaitForAllUpdated("TestApp");
					DMibTestMark;
					co_await fWaitForAllUpdated("TestApp2");

					DMibExpect(UpdateNotificationState.m_nMaxAppsInProgress, ==, 1u);

					DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_nSuccess.f_Load(), ==, nAppManagers);
					DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_nMaxInProgress, ==, 1u);
					DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_MaxInStage[CAppManagerInterface::EUpdateStage_StopOldApp], ==, 1u);

					DMibExpect(UpdateNotificationState.m_Applications["TestApp2"].m_nSuccess.f_Load(), ==, nAppManagers);
					DMibExpect(UpdateNotificationState.m_Applications["TestApp2"].m_nMaxInProgress, ==, 1u);
					DMibExpect(UpdateNotificationState.m_Applications["TestApp2"].m_MaxInStage[CAppManagerInterface::EUpdateStage_StopOldApp], ==, 1u);

					DMibExpect(UpdateNotificationState.m_AllApplications.m_nSuccess.f_Load(), ==, nAppManagers * 2u);
					DMibExpect(UpdateNotificationState.m_AllApplications.m_nMaxInProgress, ==, 1u);
					DMibExpect(UpdateNotificationState.m_AllApplications.m_MaxInStage[CAppManagerInterface::EUpdateStage_StopOldApp], ==, 1u);
				}
				{
					DMibTestPath("AllAtOnce OneAtATime");
					co_await fSetUpdateType("TestApp", "AllAtOnce");
					DMibTestMark;
					co_await fSetUpdateType("TestApp2", "OneAtATime");
					UpdateNotificationState.f_Clear();
					DMibTestMark;
					co_await fUpdateTestApp({});
					DMibTestMark;
					co_await fTagApp("TestApp", {"TestTag", "TestTag2"});
					DMibTestMark;
					co_await fWaitForAllUpdated("TestApp");
					DMibTestMark;
					co_await fWaitForAllUpdated("TestApp2");

					DMibExpect(UpdateNotificationState.m_nMaxAppsInProgress, >=, 1u);

					DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_nSuccess.f_Load(), ==, nAppManagers);
					DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_nMaxInProgress, >= , 1u);
					DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_MaxInStageCoordination[CAppManagerInterface::EUpdateStage_StopOldApp], ==, nAppManagers);

					DMibExpect(UpdateNotificationState.m_Applications["TestApp2"].m_nSuccess.f_Load(), ==, nAppManagers);
					DMibExpect(UpdateNotificationState.m_Applications["TestApp2"].m_nMaxInProgress, ==, 1u);
					DMibExpect(UpdateNotificationState.m_Applications["TestApp2"].m_MaxInStage[CAppManagerInterface::EUpdateStage_StopOldApp], ==, 1u);

					DMibExpect(UpdateNotificationState.m_AllApplications.m_nSuccess.f_Load(), ==, nAppManagers * 2u);
					DMibExpect(UpdateNotificationState.m_AllApplications.m_nMaxInProgress, >=, 1u);
					DMibExpect(UpdateNotificationState.m_AllApplications.m_MaxInStage[CAppManagerInterface::EUpdateStage_StopOldApp], >=, 1u);
				}
				{
					DMibTestPath("AllAtOnce Independent");
					co_await fSetUpdateType("TestApp", "AllAtOnce");
					DMibTestMark;
					co_await fSetUpdateType("TestApp2", "Independent");
					UpdateNotificationState.f_Clear();
					DMibTestMark;
					co_await fUpdateTestApp({});
					DMibTestMark;
					co_await fTagApp("TestApp", {"TestTag"});
					DMibTestMark;
					co_await fTagApp("TestApp", {"TestTag2"});
					DMibTestMark;
					co_await fWaitForAllUpdated("TestApp");
					DMibTestMark;
					co_await fWaitForAllUpdated("TestApp2");

					DMibExpect(UpdateNotificationState.m_nMaxAppsInProgressPerAppManager, ==, 1u);

					DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_nSuccess.f_Load(), ==, nAppManagers);
					DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_nMaxInProgress, >= , 1u);
					DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_MaxInStageCoordination[CAppManagerInterface::EUpdateStage_StopOldApp], ==, nAppManagers);

					DMibExpect(UpdateNotificationState.m_Applications["TestApp2"].m_nSuccess.f_Load(), ==, nAppManagers);
					DMibExpect(UpdateNotificationState.m_Applications["TestApp2"].m_nMaxInProgress, >= , 1u);
				}
				{
					DMibTestPath("OneAtATime Independent");
					co_await fSetUpdateType("TestApp", "OneAtATime");
					DMibTestMark;
					co_await fSetUpdateType("TestApp2", "Independent");
					UpdateNotificationState.f_Clear();
					DMibTestMark;
					co_await fUpdateTestApp({});
					DMibTestMark;
					co_await fTagApp("TestApp", {"TestTag"});
					DMibTestMark;
					co_await fTagApp("TestApp", {"TestTag2"});
					DMibTestMark;
					co_await fWaitForAllUpdated("TestApp");
					DMibTestMark;
					co_await fWaitForAllUpdated("TestApp2");

					DMibExpect(UpdateNotificationState.m_nMaxAppsInProgressPerAppManager, ==, 1u);

					DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_nSuccess.f_Load(), ==, nAppManagers);
					DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_nMaxInProgress, ==, 1u);
					DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_MaxInStage[CAppManagerInterface::EUpdateStage_StopOldApp], ==, 1u);
					DMibExpect(UpdateNotificationState.m_Applications["TestApp2"].m_nSuccess.f_Load(), ==, nAppManagers);
					DMibExpect(UpdateNotificationState.m_Applications["TestApp2"].m_nMaxInProgress, >= , 1u);
				}
				{
					DMibTestPath("Independent Independent");
					co_await fSetUpdateType("TestApp", "Independent");
					DMibTestMark;
					co_await fSetUpdateType("TestApp2", "Independent");
					UpdateNotificationState.f_Clear();
					DMibTestMark;
					co_await fUpdateTestApp({});
					DMibTestMark;
					co_await fTagApp("TestApp", {"TestTag"});
					DMibTestMark;
					co_await fTagApp("TestApp", {"TestTag2"});
					DMibTestMark;
					co_await fWaitForAllUpdated("TestApp");
					DMibTestMark;
					co_await fWaitForAllUpdated("TestApp2");

					if (i == 0)
						DMibExpect(UpdateNotificationState.m_nMaxAppsInProgressPerAppManager, ==, 2u);
					else
						DMibExpect(UpdateNotificationState.m_nMaxAppsInProgressPerAppManager, ==, 1u);

					DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_nSuccess.f_Load(), ==, nAppManagers);
					DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_nMaxInProgress, >= , 1u);
					DMibExpect(UpdateNotificationState.m_Applications["TestApp2"].m_nSuccess.f_Load(), ==, nAppManagers);
					DMibExpect(UpdateNotificationState.m_Applications["TestApp2"].m_nMaxInProgress, >= , 1u);
				}

			}
			co_return {};
		};
	}
};

DMibTestRegister(CAppManager_General_Tests, Malterlib::Cloud);
