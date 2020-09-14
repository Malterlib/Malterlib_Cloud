// Copyright © 2020 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Test_Malterlib_Cloud_AppManager.h"

#ifdef DPlatformFamily_Windows
#include <Windows.h>
#endif

static fp64 g_Timeout = 120.0;

class CAppManager_General_Tests : public NMib::NTest::CTest
{
public:
	void f_DoTests()
	{
		DMibTestSuite("Upgrades")
		{
#ifdef DPlatformFamily_Windows
			AllocConsole();
			SetConsoleCtrlHandler
				(
					nullptr
					, true
				)
			;
			fg_GetSys()->f_SetEnvironmentVariable("Path", "c:\\Program Files\\Git\\usr\\bin;{}"_f << fg_GetSys()->f_GetEnvironmentVariable("Path"));
			NSys::fg_Process_SetEnvironmentVariable_Unsafe("Path", "c:\\Program Files\\Git\\usr\\bin;{}"_f << fg_GetSys()->f_GetEnvironmentVariable("Path"));
#endif
			CAppManagerTestHelper::EOption Options = CAppManagerTestHelper::EOption_EnableVersionManager;
			//Options |= CAppManagerTestHelper::EOption_EnableLogging;
			//Options |= CAppManagerTestHelper::EOption_EnableOtherOutput;

			CAppManagerTestHelper AppManagerTestHelper("AppManagerTests", Options, g_Timeout);

			auto &PackageInfo = AppManagerTestHelper.m_PackageInfo;
			auto &TestAppArchive = AppManagerTestHelper.m_TestAppArchive;
			auto &VersionManager = AppManagerTestHelper.m_VersionManager;
			auto &AppManagers = AppManagerTestHelper.m_AppManagerInfos;

			// Copy AppManagers to their directories
			mint nAppManagers = 10;
#if DMibPPtrBits <= 32
			nAppManagers = 2;
#endif
			AppManagerTestHelper.f_Setup(nAppManagers);

			// Update Application
 			auto fUpdateTestApp = [&](TCSet<CStr> const &_Tags)
				{
					++PackageInfo.m_VersionID.m_VersionID.m_Revision;
					PackageInfo.m_VersionInfo.m_Tags = _Tags;
					(
						g_Dispatch(AppManagerTestHelper.m_HelperActor) /
						[=, VersionManagerHelper = AppManagerTestHelper.m_VersionManagerHelper]() -> TCFuture<void>
						{
							co_await VersionManagerHelper.f_Upload(VersionManager, "TestApp", PackageInfo.m_VersionID, PackageInfo.m_VersionInfo, TestAppArchive);
							co_return {};
						}
					)
					.f_CallSync(g_Timeout);
				}
			;

			auto fTagApp = [&](CStr const &_Name, TCSet<CStr> const &_Tags)
				{
					CVersionManager::CChangeTags ChangeTags;
					ChangeTags.m_AddTags = _Tags;
					ChangeTags.m_Application = _Name;
					ChangeTags.m_VersionID = PackageInfo.m_VersionID.m_VersionID;
					VersionManager.f_CallActor(&CVersionManager::f_ChangeTags)(fg_Move(ChangeTags)).f_CallSync(g_Timeout);
				}
			;

			struct CApplicationKey
			{
				auto f_Tuple() const
				{
					return fg_TupleReferences(m_AppName, m_iAppManager);
				}

				bool operator < (CApplicationKey const &_Right) const
				{
					return f_Tuple() < _Right.f_Tuple();
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

			struct CUpdateNotificationsState
			{
				void f_Destroy()
				{
					TCActorResultVector<void> Destroys;
					for (auto &Subscription : m_Subscriptions)
						Subscription->f_Destroy() > Destroys.f_AddResult();;
					Destroys.f_GetResults().f_CallSync();
					f_Clear();
				}

				void f_Clear()
				{
					DMibLock(m_Lock);

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

				NThread::CMutual m_Lock;
				TCVector<CActorSubscription> m_Subscriptions;
				NThread::CEventAutoReset m_Event;
				TCMap<CStr, CUpdateNotificationsApplicationState> m_Applications;
				TCMap<mint, TCMap<CStr, CUpdateNotificationsApplicationState>> m_ApplicationsPerAppmanager;
				CUpdateNotificationsApplicationState m_AllApplications;
				mint m_nAppsInProgress = 0;
				mint m_nMaxAppsInProgress = 0;
				TCMap<mint, zmint> m_AppsInProgressPerAppManager;
				TCMap<mint, zmint> m_MaxAppsInProgressPerAppManager;
				mint m_nMaxAppsInProgressPerAppManager = 0;
			};

			TCSharedPointer<CUpdateNotificationsState> pUpdateNotificationsState = fg_Construct();

			pUpdateNotificationsState->m_Applications["TestApp"];
			pUpdateNotificationsState->m_Applications["TestApp2"];

			auto CleanupNotifications = g_OnScopeExit > [&]
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
							g_ActorFunctor / [pUpdateNotificationsState, iAppManager]
							(CAppManagerInterface::CUpdateNotification const &_Notification) -> TCFuture<void>
							{
								CApplicationKey ApplicationKey{_Notification.m_Application, iAppManager};

								auto &WholeState = *pUpdateNotificationsState;
								DMibLock(WholeState.m_Lock);

								auto fProcessApplicationState = [&](CUpdateNotificationsApplicationState &_State)
									{
										if (!_Notification.m_bCoordinateWait)
										{
											if (auto pInStage = _State.m_LastInStage.f_FindEqual(ApplicationKey))
												_State.m_InStage[*pInStage].f_Remove(ApplicationKey);
											_State.m_InStage[_Notification.m_Stage][ApplicationKey];
											_State.m_MaxInStage[_Notification.m_Stage] = fg_Max
												(
													_State.m_MaxInStage[_Notification.m_Stage]
												 	, _State.m_InStage[_Notification.m_Stage].f_GetLen()
												)
											;
											_State.m_LastInStage[ApplicationKey] = _Notification.m_Stage;
										}

										if (auto pInStage = _State.m_LastInStageCoordination.f_FindEqual(ApplicationKey))
											_State.m_InStageCoordination[*pInStage].f_Remove(ApplicationKey);
										_State.m_InStageCoordination[_Notification.m_Stage][ApplicationKey];
										_State.m_MaxInStageCoordination[_Notification.m_Stage] = fg_Max
											(
											 	_State.m_MaxInStageCoordination[_Notification.m_Stage]
											 	, _State.m_InStageCoordination[_Notification.m_Stage].f_GetLen()
											)
										;
										_State.m_LastInStageCoordination[ApplicationKey] = _Notification.m_Stage;

										if (_Notification.m_Stage == CAppManagerInterface::EUpdateStage_Failed)
										{
											_State.m_InProgress.f_Remove(ApplicationKey);
											++_State.m_nFinished;
										}
										else if (!_Notification.m_bCoordinateWait && _Notification.m_Stage == CAppManagerInterface::EUpdateStage_Finished)
										{
											_State.m_InProgress.f_Remove(ApplicationKey);
											++_State.m_nFinished;
											++_State.m_nSuccess;
										}
										else if (!_Notification.m_bCoordinateWait && _Notification.m_Stage >= CAppManagerInterface::EUpdateStage_StopOldApp)
										{
											_State.m_InProgress[ApplicationKey];
											_State.m_nMaxInProgress = fg_Max(_State.m_nMaxInProgress, _State.m_InProgress.f_GetLen());
										}
									}
								;

								fProcessApplicationState(WholeState.m_Applications[_Notification.m_Application]);
								fProcessApplicationState(WholeState.m_ApplicationsPerAppmanager[iAppManager][_Notification.m_Application]);
								fProcessApplicationState(WholeState.m_AllApplications);
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

								WholeState.m_Event.f_Signal();
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
				fg_CombineResults(AppCommandResults.f_GetResults().f_CallSync(g_Timeout));
			}

			auto fWaitForAllUpdated = [&](CStr const &_Application)
				{
					auto &ApplicationState = pUpdateNotificationsState->m_Applications[_Application];
					NTime::CClock Clock{true};
					while (true)
					{
						if (ApplicationState.m_nFinished == nAppManagers)
							break;
						if (Clock.f_GetTime() > g_Timeout)
							DMibError("Timed out waiting for all apps to update");
						pUpdateNotificationsState->m_Event.f_WaitTimeout(10.0);
					}
				}
			;

			auto fSetUpdateType = [&](CStr const &_AppName, CStr const &_UpdateType)
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
					fg_CombineResults(AppCommandResults.f_GetResults().f_CallSync(g_Timeout));
					*pUpdateType = _UpdateType;
				}
			;

			auto fSetUpdateGroup = [&](CStr const &_AppName, CStr const &_Group)
				{
					TCActorResultVector<void> AppCommandResults;
					for (auto &AppManager : AppManagers)
					{
						CAppManagerInterface::CApplicationChangeSettings ChangeSettings;
						CAppManagerInterface::CApplicationSettings Settings;
						Settings.m_UpdateGroup = _Group;

						AppManager.m_Interface.f_CallActor(&CAppManagerInterface::f_ChangeSettings)(_AppName, ChangeSettings, Settings) > AppCommandResults.f_AddResult();
					}
					fg_CombineResults(AppCommandResults.f_GetResults().f_CallSync(g_Timeout));
				}
			;

			AppManagerTestHelper.f_CheckCloudManager(1);

			{
				DMibTestPath("Update Independent");
				fSetUpdateType("TestApp", "Independent");
				UpdateNotificationState.f_Clear();
				DMibTestMark;
				fUpdateTestApp({"TestTag"});
				DMibTestMark;
				fWaitForAllUpdated("TestApp");

				DMibLock(UpdateNotificationState.m_Lock);
				DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_nSuccess.f_Load(), ==, nAppManagers);
				DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_nMaxInProgress, >= , 1u);
			}
			{
				DMibTestPath("Update OneAtATime");
				fSetUpdateType("TestApp", "OneAtATime");
				UpdateNotificationState.f_Clear();
				DMibTestMark;
				fUpdateTestApp({"TestTag"});
				DMibTestMark;
				fWaitForAllUpdated("TestApp");

				DMibLock(UpdateNotificationState.m_Lock);
				DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_nSuccess.f_Load(), ==, nAppManagers);
				DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_nMaxInProgress, ==, 1);
				DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_MaxInStage[CAppManagerInterface::EUpdateStage_StopOldApp], ==, 1);
			}
			{
				DMibTestPath("Update AllAtOnce");
				fSetUpdateType("TestApp", "AllAtOnce");
				UpdateNotificationState.f_Clear();
				DMibTestMark;
				fUpdateTestApp({"TestTag"});
				DMibTestMark;
				fWaitForAllUpdated("TestApp");

				DMibLock(UpdateNotificationState.m_Lock);
				DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_nSuccess.f_Load(), ==, nAppManagers);
				DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_nMaxInProgress, >= , 1u);
				DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_MaxInStageCoordination[CAppManagerInterface::EUpdateStage_StopOldApp], ==, nAppManagers);
			}

			if (!NMib::NTest::fg_GroupActive("Expensive"))
				return;

			AppManagerTestHelper.f_InstallTestApp("TestApp2", "TestTag2", "TestGroup2");

			for (mint i = 0; i < 2; ++i)
			{
				CStr Path;
				if (i == 0)
				{
					Path = "Same Update Group";
					DMibTestMark;
					fSetUpdateGroup("TestApp", "TestGroup");
					DMibTestMark;
					fSetUpdateGroup("TestApp2", "TestGroup");
				}
				else
				{
					Path = "Separate Update Group";
					DMibTestMark;
					fSetUpdateGroup("TestApp", "TestGroup");
					DMibTestMark;
					fSetUpdateGroup("TestApp2", "TestGroup2");
				}
				DMibTestPath(Path);
				{
					DMibTestPath("AllAtOnce AllAtOnce");
					fSetUpdateType("TestApp", "AllAtOnce");
					DMibTestMark;
					fSetUpdateType("TestApp2", "AllAtOnce");
					UpdateNotificationState.f_Clear();
					DMibTestMark;
					fUpdateTestApp({});
					DMibTestMark;
					fTagApp("TestApp", {"TestTag", "TestTag2"});
					DMibTestMark;
					fWaitForAllUpdated("TestApp");
					DMibTestMark;
					fWaitForAllUpdated("TestApp2");

					DMibLock(UpdateNotificationState.m_Lock);
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
					fSetUpdateType("TestApp", "OneAtATime");
					DMibTestMark;
					fSetUpdateType("TestApp2", "OneAtATime");
					UpdateNotificationState.f_Clear();
					DMibTestMark;
					fUpdateTestApp({});
					DMibTestMark;
					fTagApp("TestApp", {"TestTag", "TestTag2"});
					DMibTestMark;
					fWaitForAllUpdated("TestApp");
					DMibTestMark;
					fWaitForAllUpdated("TestApp2");

					DMibLock(UpdateNotificationState.m_Lock);
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
					fSetUpdateType("TestApp", "AllAtOnce");
					DMibTestMark;
					fSetUpdateType("TestApp2", "OneAtATime");
					UpdateNotificationState.f_Clear();
					DMibTestMark;
					fUpdateTestApp({});
					DMibTestMark;
					fTagApp("TestApp", {"TestTag", "TestTag2"});
					DMibTestMark;
					fWaitForAllUpdated("TestApp");
					DMibTestMark;
					fWaitForAllUpdated("TestApp2");

					DMibLock(UpdateNotificationState.m_Lock);
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
					fSetUpdateType("TestApp", "AllAtOnce");
					DMibTestMark;
					fSetUpdateType("TestApp2", "Independent");
					UpdateNotificationState.f_Clear();
					DMibTestMark;
					fUpdateTestApp({});
					DMibTestMark;
					fTagApp("TestApp", {"TestTag"});
					DMibTestMark;
					fTagApp("TestApp", {"TestTag2"});
					DMibTestMark;
					fWaitForAllUpdated("TestApp");
					DMibTestMark;
					fWaitForAllUpdated("TestApp2");

					DMibLock(UpdateNotificationState.m_Lock);
					DMibExpect(UpdateNotificationState.m_nMaxAppsInProgressPerAppManager, ==, 1u);

					DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_nSuccess.f_Load(), ==, nAppManagers);
					DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_nMaxInProgress, >= , 1u);
					DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_MaxInStageCoordination[CAppManagerInterface::EUpdateStage_StopOldApp], ==, nAppManagers);

					DMibExpect(UpdateNotificationState.m_Applications["TestApp2"].m_nSuccess.f_Load(), ==, nAppManagers);
					DMibExpect(UpdateNotificationState.m_Applications["TestApp2"].m_nMaxInProgress, >= , 1u);
				}
				{
					DMibTestPath("OneAtATime Independent");
					fSetUpdateType("TestApp", "OneAtATime");
					DMibTestMark;
					fSetUpdateType("TestApp2", "Independent");
					UpdateNotificationState.f_Clear();
					DMibTestMark;
					fUpdateTestApp({});
					DMibTestMark;
					fTagApp("TestApp", {"TestTag"});
					DMibTestMark;
					fTagApp("TestApp", {"TestTag2"});
					DMibTestMark;
					fWaitForAllUpdated("TestApp");
					DMibTestMark;
					fWaitForAllUpdated("TestApp2");

					DMibLock(UpdateNotificationState.m_Lock);
					DMibExpect(UpdateNotificationState.m_nMaxAppsInProgressPerAppManager, ==, 1u);

					DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_nSuccess.f_Load(), ==, nAppManagers);
					DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_nMaxInProgress, ==, 1u);
					DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_MaxInStage[CAppManagerInterface::EUpdateStage_StopOldApp], ==, 1u);
					DMibExpect(UpdateNotificationState.m_Applications["TestApp2"].m_nSuccess.f_Load(), ==, nAppManagers);
					DMibExpect(UpdateNotificationState.m_Applications["TestApp2"].m_nMaxInProgress, >= , 1u);
				}
				{
					DMibTestPath("Independent Independent");
					fSetUpdateType("TestApp", "Independent");
					DMibTestMark;
					fSetUpdateType("TestApp2", "Independent");
					UpdateNotificationState.f_Clear();
					DMibTestMark;
					fUpdateTestApp({});
					DMibTestMark;
					fTagApp("TestApp", {"TestTag"});
					DMibTestMark;
					fTagApp("TestApp", {"TestTag2"});
					DMibTestMark;
					fWaitForAllUpdated("TestApp");
					DMibTestMark;
					fWaitForAllUpdated("TestApp2");

					DMibLock(UpdateNotificationState.m_Lock);
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
		};
	}
};

DMibTestRegister(CAppManager_General_Tests, Malterlib::Cloud);
