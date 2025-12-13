// Copyright © 2025 Unbroken AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Concurrency/AsyncDestroy>
#include <Mib/Concurrency/ActorSequencerActor>
#include <Mib/CommandLine/TableRenderer>
#include <Mib/CommandLine/AnsiEncodingParse>
#include <Mib/File/File>

#include "Malterlib_Cloud_Bootstrap_Packages.h"
#include "Malterlib_Cloud_Bootstrap_Prompts.h"

namespace NMib::NCloud::NBootstrap
{
	namespace
	{
		using namespace NStr;

		// Maximum visible table lines for scrolling (not rows - accounts for multi-line rows)
		static constexpr mint gc_MaxDisplayLines = 20;

		// Arrow key escape sequences
		static constexpr ch8 gc_UpArrow[] = "\x1b[A";
		static constexpr ch8 gc_DownArrow[] = "\x1b[B";
		static constexpr ch8 gc_RightArrow[] = "\x1b[C";
		static constexpr ch8 gc_LeftArrow[] = "\x1b[D";
		static constexpr ch8 gc_Escape[] = "\x1b";
		static constexpr ch8 gc_Tab[] = "\t";
		static constexpr ch8 gc_Space[] = " ";

		// Default included applications (Malterlib Cloud core)
		static constexpr ch8 const* gc_pDefaultIncludedApps[] =
		{
			"AcmeManager"
			, "AppDistributionManager"
			, "AppManager"
			, "BackupManager"
			, "CloudManager"
			, "DebugManager"
			, "DebugManagerClient"
			, "DebugManagerClientServe"
			, "GitPolicyManager"
			, "KeyManager"
			, "MalterlibCloud"
			, "SecretsManager"
			, "TunnelProxyManager"
			, "VersionManager"
			, "WebCertificateManager"
		};

		// Check if an application is in the default included list
		static bool fs_IsDefaultSelected(CStr const &_AppName)
		{
			// Check if in default included list
			for (auto const *pDefault : gc_pDefaultIncludedApps)
			{
				if (_AppName == pDefault)
					return true;
			}

			// Not in either list - default to not selected
			return false;
		}

		// Button indices for package UI
		enum class EPackageButton : mint
		{
			mc_Branch = 0
			, mc_Tag
			, mc_Platform
			, mc_Cancel
			, mc_Download
			, mc_Count
		};

		// Table columns
		enum class EPackageTableColumn : mint
		{
			mc_Selected = 0
			, mc_Application
			, mc_Version
			, mc_Platforms
			, mc_Size
			, mc_Count
		};

		// UI mode
		enum class EPackageUIMode : mint
		{
			mc_Buttons
			, mc_Table
		};

		// Table row data
		struct CPackageTableRow
		{
			CStr m_Application;
			// Store version and size per platform (filtered by global platform filter)
			TCMap<CStr, CVersionManager::CVersionIDAndPlatform> m_VersionByPlatform;
			TCMap<CStr, uint64> m_BytesByPlatform;
			TCSet<CStr> m_AvailablePlatforms;  // All platforms for this app (filtered by global filter)
			bool m_bSelected = false;

			// Calculate total bytes across all platforms
			uint64 f_GetTotalBytes() const
			{
				uint64 nTotal = 0;
				for (auto const &Entry : m_BytesByPlatform)
					nTotal += Entry;
				return nTotal;
			}
		};

		// Returns true if row is multi-line (has more than one platform)
		static bool fs_IsMultiLine(CPackageTableRow const &_Row)
		{
			return _Row.m_AvailablePlatforms.f_GetLen() > 1;
		}

		// Result of calculating visible rows for table scrolling
		struct CTableScrollResult
		{
			mint m_nVisibleRowCount = 0;
			mint m_nLinesUsed = 0;
			bool m_bHasMoreBelow = false;
			bool m_bLastRowWasMultiLine = false;
		};

		// Calculate how many table rows fit within a line budget
		static CTableScrollResult fs_CalculateVisibleRows
			(
				TCVector<CPackageTableRow> const &_Rows
				, mint _nScrollOffset
				, mint _nMaxLines
				, bool _bHasMoreAbove
			)
		{
			CTableScrollResult Result;
			mint nTotalRows = _Rows.f_GetLen();
			bool bPrevWasMultiLine = false;

			for (mint iRow = _nScrollOffset; iRow < nTotalRows; ++iRow)
			{
				bool bCurrMulti = fs_IsMultiLine(_Rows[iRow]);
				mint nContentLines = fg_Max(mint(1), _Rows[iRow].m_AvailablePlatforms.f_GetLen());

				bool bIsFirstDataRow = (iRow == _nScrollOffset);
				mint nSeparator = 0;
				if (!bIsFirstDataRow || _bHasMoreAbove)
					nSeparator = (bPrevWasMultiLine || bCurrMulti) ? 1 : 0;

				mint nRowTotal = nSeparator + nContentLines;

				if (Result.m_nLinesUsed + nRowTotal > _nMaxLines)
					break;

				Result.m_nLinesUsed += nRowTotal;
				++Result.m_nVisibleRowCount;
				bPrevWasMultiLine = bCurrMulti;
			}

			Result.m_bLastRowWasMultiLine = bPrevWasMultiLine;
			Result.m_bHasMoreBelow = _nScrollOffset + Result.m_nVisibleRowCount < nTotalRows;

			return Result;
		}

		// Calculate visible rows with two-pass optimization for "more below" indicator
		static CTableScrollResult fs_CalculateVisibleRowsOptimized
			(
				TCVector<CPackageTableRow> const &_Rows
				, mint _nScrollOffset
				, mint _nMaxDisplayLines
				, bool _bHasMoreAbove
			)
		{
			mint nReservedForMoreAbove = _bHasMoreAbove ? 1 : 0;

			// First pass: reserve 2 lines for potential "more below"
			mint nLinesForData = _nMaxDisplayLines - nReservedForMoreAbove - 2;
			auto Result = fs_CalculateVisibleRows(_Rows, _nScrollOffset, nLinesForData, _bHasMoreAbove);

			// If no "more below", recalculate with full budget
			if (!Result.m_bHasMoreBelow)
			{
				nLinesForData = _nMaxDisplayLines - nReservedForMoreAbove;
				Result = fs_CalculateVisibleRows(_Rows, _nScrollOffset, nLinesForData, _bHasMoreAbove);
			}

			// Edge case: at least show one row even if too big
			if (Result.m_nVisibleRowCount == 0 && _Rows.f_GetLen() > _nScrollOffset)
			{
				Result.m_nVisibleRowCount = 1;
				bool bMulti = fs_IsMultiLine(_Rows[_nScrollOffset]);
				Result.m_nLinesUsed = fg_Max(mint(1), _Rows[_nScrollOffset].m_AvailablePlatforms.f_GetLen());
				if (_bHasMoreAbove && bMulti)
					Result.m_nLinesUsed += 1;
				Result.m_bHasMoreBelow = _nScrollOffset + 1 < _Rows.f_GetLen();
				Result.m_bLastRowWasMultiLine = bMulti;
			}

			return Result;
		}

		// UI state
		struct CPackageUIState
		{
			CPackageUIState(CAnsiEncoding _AnsiEncoding)
				: m_AnsiEncoding(_AnsiEncoding)
			{
			}

			TCSharedPointer<CCommandLineControl> m_pCommandLine;
			CAnsiEncoding m_AnsiEncoding;

			// VersionManager connection
			TCTrustedActorSubscription<CVersionManager> m_VersionManagers;
			bool m_bVersionManagerConnected = false;

			// Cached data from VersionManager
			CVersionManagerCache m_Cache;

			// UI state
			EPackageButton m_SelectedButton = EPackageButton::mc_Branch;
			EPackageUIMode m_UIMode = EPackageUIMode::mc_Buttons;
			mint m_SelectedRow = 0;
			mint m_TableScrollOffset = 0;
			EPackageTableColumn m_SelectedColumn = EPackageTableColumn::mc_Selected;

			// Configuration being edited
			CPackageConfig m_Config;

			// Table data (computed from cache + filters)
			TCVector<CPackageTableRow> m_TableRows;

			mint m_LastRenderedLines = 0;
			bool m_bDone = false;
			bool m_bConfirmed = false;
			bool m_bNeedsRedraw = true;
			bool m_bOpeningDialog = false;
			NConcurrency::CSequencer m_InputSequencer{"PackageUI InputSequencer"};

			// Get unique branches from cache
			TCVector<CStr> f_GetUniqueBranches() const
			{
				TCSet<CStr> BranchSet;
				for (auto const &AppEntry : m_Cache.m_Applications)
				{
					for (auto const &VersionEntry : AppEntry)
					{
						auto const &VersionID = m_Cache.m_Applications.fs_GetKey(AppEntry);
						(void)VersionID; // Unused - we iterate the inner map
						auto const &InnerVersionID = AppEntry.fs_GetKey(VersionEntry);
						if (!InnerVersionID.m_VersionID.m_Branch.f_IsEmpty())
							BranchSet[InnerVersionID.m_VersionID.m_Branch];
					}
				}

				TCVector<CStr> Result;
				for (auto const &Branch : BranchSet)
					Result.f_Insert(BranchSet.fs_GetKey(Branch));
				return Result;
			}

			// Get unique tags from cache
			TCVector<CStr> f_GetUniqueTags() const
			{
				TCSet<CStr> TagSet;
				for (auto const &AppEntry : m_Cache.m_Applications)
				{
					for (auto const &VersionEntry : AppEntry)
					{
						for (auto const &Tag : VersionEntry.m_Tags)
							TagSet[Tag];
					}
				}

				TCVector<CStr> Result;
				for (auto const &Tag : TagSet)
					Result.f_Insert(TagSet.fs_GetKey(Tag));
				return Result;
			}

			// Get unique platforms from cache
			TCVector<CStr> f_GetUniquePlatforms() const
			{
				TCVector<CStr> Result;
				for (auto const &Platform : m_Cache.m_AvailablePlatforms)
					Result.f_Insert(m_Cache.m_AvailablePlatforms.fs_GetKey(Platform));
				return Result;
			}

			// Rebuild table rows from cache based on current filters
			void f_RefreshTableFromCache()
			{
				m_TableRows.f_Clear();
				m_TableScrollOffset = 0;
				m_SelectedRow = 0;

				for (auto &AppEntry : m_Cache.m_Applications)
				{
					CStr const &AppName = m_Cache.m_Applications.fs_GetKey(AppEntry);

					CPackageTableRow Row;
					Row.m_Application = AppName;

					// First pass: find the single latest version across all platforms
					CVersionManager::CVersionID LatestVersionID;
					NTime::CTime LatestTime;

					for (auto &VersionEntry : AppEntry)
					{
						auto const &VersionID = AppEntry.fs_GetKey(VersionEntry);
						auto const &VersionInfo = VersionEntry;

						// Apply branch filter
						if (!m_Config.m_Branch.f_IsEmpty() && !VersionID.m_VersionID.m_Branch.f_StartsWith(m_Config.m_Branch))
							continue;

						// Apply tag filter
						if (!m_Config.m_Tag.f_IsEmpty() && !VersionInfo.m_Tags.f_FindEqual(m_Config.m_Tag))
							continue;

						// Apply global platform filter
						if (!m_Config.m_SelectedPlatforms.f_IsEmpty() && !m_Config.m_SelectedPlatforms.f_FindEqual(VersionID.m_Platform))
							continue;

						// Track latest version by time
						if (!LatestVersionID.f_IsValid() || VersionInfo.m_Time > LatestTime)
						{
							LatestVersionID = VersionID.m_VersionID;
							LatestTime = VersionInfo.m_Time;
						}
					}

					// Skip apps with no matching versions after filtering
					if (!LatestVersionID.f_IsValid())
						continue;

					// Second pass: find all platforms that have this exact version
					for (auto &VersionEntry : AppEntry)
					{
						auto const &VersionID = AppEntry.fs_GetKey(VersionEntry);
						auto const &VersionInfo = VersionEntry;

						// Must match the latest version ID (branch + major.minor.revision)
						if (VersionID.m_VersionID != LatestVersionID)
							continue;

						// Apply global platform filter
						if (!m_Config.m_SelectedPlatforms.f_IsEmpty() && !m_Config.m_SelectedPlatforms.f_FindEqual(VersionID.m_Platform))
							continue;

						Row.m_VersionByPlatform[VersionID.m_Platform] = VersionID;
						Row.m_AvailablePlatforms[VersionID.m_Platform];
						Row.m_BytesByPlatform[VersionID.m_Platform] = VersionInfo.m_nBytes;
					}

					// Set selection based on config or defaults
					if (auto *pSelected = m_Config.m_SelectedApplications.f_FindEqual(AppName))
						Row.m_bSelected = *pSelected;
					else
						Row.m_bSelected = fs_IsDefaultSelected(AppName);

					m_TableRows.f_Insert(fg_Move(Row));
				}

				// Sort by application name
				m_TableRows.f_Sort([](CPackageTableRow const &_A, CPackageTableRow const &_B)
					{
						return _A.m_Application <=> _B.m_Application;
					}
				);
			}
		};

		// Query VersionManager for available applications and versions
		// Writes results directly to _pState->m_Cache
		TCFuture<void> fg_QueryVersionManager(TCSharedPointer<CPackageUIState> _pState)
		{
			_pState->m_Cache = CVersionManagerCache{};

			CStr Error;
			auto *pVM = _pState->m_VersionManagers.f_GetOneActor("", Error);
			if (!pVM)
				co_return DMibErrorInstance("No VersionManager available: {}"_f << Error);

			// List all applications
			auto Apps = co_await pVM->m_Actor.f_CallActor(&CVersionManager::f_ListApplications)
				(CVersionManager::CListApplications{})
				.f_Timeout(30.0, "Timeout listing applications")
			;

			// List versions for each application
			for (auto const &AppName : Apps.m_Applications)
			{
				CVersionManager::CListVersions Query;
				Query.m_ForApplication = AppName;

				auto Versions = co_await pVM->m_Actor.f_CallActor(&CVersionManager::f_ListVersions)(Query)
					.f_Timeout(30.0, "Timeout listing versions")
				;

				if (auto *pAppVersions = Versions.m_Versions.f_FindEqual(AppName))
				{
					// Store all versions without filtering - filtering happens in f_RefreshTableFromCache
					for (auto &VersionEntry : *pAppVersions)
					{
						auto const &VersionID = pAppVersions->fs_GetKey(VersionEntry);
						auto const &VersionInfo = VersionEntry;

						_pState->m_Cache.m_Applications[AppName][VersionID] = VersionInfo;
						_pState->m_Cache.m_AvailablePlatforms[VersionID.m_Platform];
					}
				}
			}

			_pState->m_Cache.m_bQueried = true;
			_pState->m_Cache.m_QueryTime = NTime::CTime::fs_NowUTC();

			co_return {};
		}

		// Maximum concurrent downloads
		static constexpr mint gc_MaxParallelDownloads = 3;

		// Download selected packages to destination directory
		TCFuture<void> fg_DownloadPackages(TCSharedPointer<CPackageUIState> _pState, CStr _DestinationDirectory)
		{
			CVersionManagerHelper Helper(_DestinationDirectory);

			CStr Error;
			auto *pVM = _pState->m_VersionManagers.f_GetOneActor("", Error);
			if (!pVM)
				co_return DMibErrorInstance("No VersionManager available: {}"_f << Error);

			// Build download queue - find latest version per app+platform
			struct CDownloadItem
			{
				CStr m_Application;
				CVersionManager::CVersionIDAndPlatform m_VersionID;
				uint64 m_nBytes;
			};
			TCVector<CDownloadItem> Queue;

			for (auto const &AppEntry : _pState->m_Config.m_SelectedApplications)
			{
				CStr const &AppName = _pState->m_Config.m_SelectedApplications.fs_GetKey(AppEntry);
				bool bSelected = AppEntry;

				if (!bSelected)
					continue;

				auto *pAppVersions = _pState->m_Cache.m_Applications.f_FindEqual(AppName);
				if (!pAppVersions)
					continue;

				// Group by platform, find latest for each
				TCMap<CStr, CDownloadItem> LatestByPlatform;
				TCMap<CStr, NTime::CTime> LatestTimeByPlatform;

				for (auto const &VersionEntry : *pAppVersions)
				{
					auto const &VersionID = pAppVersions->fs_GetKey(VersionEntry);
					auto const &VersionInfo = VersionEntry;

					// If platform filter is set, skip platforms not in the filter
					// If filter is empty, download all platforms
					if (!_pState->m_Config.m_SelectedPlatforms.f_IsEmpty() && !_pState->m_Config.m_SelectedPlatforms.f_FindEqual(VersionID.m_Platform))
						continue;

					auto *pExistingTime = LatestTimeByPlatform.f_FindEqual(VersionID.m_Platform);
					if (!pExistingTime || VersionInfo.m_Time > *pExistingTime)
					{
						LatestByPlatform[VersionID.m_Platform] = CDownloadItem{AppName, VersionID, VersionInfo.m_nBytes};
						LatestTimeByPlatform[VersionID.m_Platform] = VersionInfo.m_Time;
					}
				}

				for (auto &Item : LatestByPlatform)
					Queue.f_Insert(fg_Move(Item));
			}

			*_pState->m_pCommandLine += "Downloading {} packages (up to {} in parallel)...\n"_f << Queue.f_GetLen() << gc_MaxParallelDownloads;

			// Create all directories upfront
			{
				auto BlockingActorCheckout = fg_BlockingActor();
				co_await
					(
						g_Dispatch(BlockingActorCheckout) / [&Queue, _DestinationDirectory]
						{
							for (auto const &Item : Queue)
							{
								CStr DestDir = _DestinationDirectory / Item.m_Application / Item.m_VersionID.m_Platform;
								NFile::CFile::fs_CreateDirectory(DestDir);
							}
						}
						% "Failed to create directories"
					)
				;
			}

			// Launch downloads with sequencer limiting concurrent operations
			TCFutureVector<void> DownloadFutures;
			CSequencer Sequencer("DownloadSequencer", gc_MaxParallelDownloads);

			for (auto const &Item : Queue)
			{
				CStr DestDir = _DestinationDirectory / Item.m_Application / Item.m_VersionID.m_Platform;
				auto Checkout = co_await Sequencer.f_Sequence();

				TCPromiseFuturePair<void> Promise;
				fg_Move(Promise.m_Future) > DownloadFutures;

				Helper.f_Download
					(
						pVM->m_Actor
						, Item.m_Application
						, Item.m_VersionID
						, DestDir
						, CFileTransferReceive::EReceiveFlag_IgnoreExisting
					)
					> [
						Promise = fg_Move(Promise.m_Promise)
						, Checkout = fg_Move(Checkout)
						, pCommandLine = _pState->m_pCommandLine
						, Application = Item.m_Application
						, Platform = Item.m_VersionID.m_Platform
					](TCAsyncResult<CFileTransferResult> &&_Result)
					{
						if (_Result)
						{
							*pCommandLine += "  {}/{}: {ns } at {fe2} MB/s\n"_f
								<< Application
								<< Platform
								<< _Result->m_nBytes
								<< _Result->f_BytesPerSecond() / 1'000'000.0
							;
						}
						Promise.f_SetResult();
					}
				;
			}

			// Wait for all downloads to complete
			co_await fg_AllDone(fg_Move(DownloadFutures));

			*_pState->m_pCommandLine += "Download complete.\n";

			co_return {};
		}

		// Helper to calculate button content width
		mint fg_ButtonContentWidth(CStr const &_Label, CStr const &_Value)
		{
			return 1 + CAnsiEncodingParse::fs_RenderedStrLen(_Label) + 2 + CAnsiEncodingParse::fs_RenderedStrLen(_Value) + 1;
		}

		// Render a single button with background color
		CStr fg_RenderButton(CStr const &_Label, CStr const &_Value, bool _bSelected, CAnsiEncoding const &_Ansi, mint _MinWidth = 0)
		{
			mint nContentWidth = 1 + CAnsiEncodingParse::fs_RenderedStrLen(_Label) + 2 + CAnsiEncodingParse::fs_RenderedStrLen(_Value) + 1;
			CStr Padding;
			if (_MinWidth > nContentWidth)
				Padding.f_AddChars(' ', _MinWidth - nContentWidth);

			if (_bSelected)
			{
				return "{}{}{} {}: {}{} {}"_f
					<< _Ansi.f_Bold()
					<< _Ansi.f_BackgroundRGB(59, 130, 246)
					<< _Ansi.f_ForegroundRGB(255, 255, 255)
					<< _Label
					<< _Value
					<< Padding
					<< _Ansi.f_Default()
				;
			}
			else
			{
				return "{}{} {}: {}{} {}"_f
					<< _Ansi.f_BackgroundRGB(30, 58, 138)
					<< _Ansi.f_ForegroundRGB(147, 197, 253)
					<< _Label
					<< _Value
					<< Padding
					<< _Ansi.f_Default()
				;
			}
		}

		// Render Cancel button
		CStr fg_RenderCancelButton(bool _bSelected, CAnsiEncoding const &_Ansi)
		{
			if (_bSelected)
			{
				return "{}{}{} Cancel {}"_f
					<< _Ansi.f_Bold()
					<< _Ansi.f_Background256(160)
					<< _Ansi.f_Foreground256(255)
					<< _Ansi.f_Default()
				;
			}
			else
			{
				return "{}{} Cancel {}"_f
					<< _Ansi.f_Background256(52)
					<< _Ansi.f_Foreground256(217)
					<< _Ansi.f_Default()
				;
			}
		}

		// Render Download button
		CStr fg_RenderDownloadButton(bool _bSelected, CAnsiEncoding const &_Ansi)
		{
			if (_bSelected)
			{
				return "{}{}{} Download {}"_f
					<< _Ansi.f_Bold()
					<< _Ansi.f_Background256(34)
					<< _Ansi.f_Foreground256(255)
					<< _Ansi.f_Default()
				;
			}
			else
			{
				return "{}{} Download {}"_f
					<< _Ansi.f_Background256(22)
					<< _Ansi.f_Foreground256(157)
					<< _Ansi.f_Default()
				;
			}
		}

		// Render all buttons
		CStr fg_RenderPackageButtons(CPackageUIState const &_State)
		{
			CStr Output;
			CTableRenderHelper TableRenderer
				(
					[&](CStr const &_Line) { Output += _Line; }
					, CTableRenderHelper::EOption_Rounded | CTableRenderHelper::EOption_AvoidRowSeparators | CTableRenderHelper::EOption_NoHeadings
					, _State.m_AnsiEncoding.f_Flags()
					, _State.m_pCommandLine->m_CommandLineWidth
				)
			;

			TableRenderer.f_AddHeadings("", "", "");
			TableRenderer.f_AddDescription("Package Configuration");
			TableRenderer.f_SetAlignRight(2);

			CStr BranchValue = _State.m_Config.m_Branch.f_IsEmpty() ? CStr{"(any)"} : _State.m_Config.m_Branch;
			CStr TagValue = _State.m_Config.m_Tag.f_IsEmpty() ? CStr{"(any)"} : _State.m_Config.m_Tag;

			// Build platform value string
			CStr PlatformValue;
			if (_State.m_Config.m_SelectedPlatforms.f_IsEmpty())
				PlatformValue = "(all)";
			else if (_State.m_Config.m_SelectedPlatforms.f_GetLen() == 1)
				PlatformValue = *_State.m_Config.m_SelectedPlatforms.f_FindAny();
			else
			{
				using namespace NStr;
				PlatformValue = "{} platforms"_f << _State.m_Config.m_SelectedPlatforms.f_GetLen();
			}

			mint nBranchWidth = fg_ButtonContentWidth("Branch", BranchValue);
			mint nTagWidth = fg_ButtonContentWidth("Tag", TagValue);
			mint nPlatformWidth = fg_ButtonContentWidth("Platform", PlatformValue);
			mint nMaxWidth = fg_Max(fg_Max(nBranchWidth, nTagWidth), nPlatformWidth);

			auto fRenderButton = [&](EPackageButton _Button, CStr const &_Label, CStr const &_Value, mint _Width) -> CStr
				{
					return fg_RenderButton(_Label, _Value, _State.m_SelectedButton == _Button && _State.m_UIMode == EPackageUIMode::mc_Buttons, _State.m_AnsiEncoding, _Width);
				}
			;

			TableRenderer.f_AddRow("", "", "");
			TableRenderer.f_AddRow
				(
					fRenderButton(EPackageButton::mc_Branch, "Branch", BranchValue, nMaxWidth)
					, fRenderButton(EPackageButton::mc_Tag, "Tag", TagValue, nMaxWidth)
					, fRenderButton(EPackageButton::mc_Platform, "Platform", PlatformValue, nMaxWidth)
				)
			;
			TableRenderer.f_AddRow("", "", "");
			TableRenderer.f_AddRow
				(
					""
					, fg_RenderCancelButton(_State.m_SelectedButton == EPackageButton::mc_Cancel && _State.m_UIMode == EPackageUIMode::mc_Buttons, _State.m_AnsiEncoding)
					, fg_RenderDownloadButton(_State.m_SelectedButton == EPackageButton::mc_Download && _State.m_UIMode == EPackageUIMode::mc_Buttons, _State.m_AnsiEncoding)
				)
			;
			TableRenderer.f_AddRow("", "", "");

			TableRenderer.f_Output();

			return Output;
		}

		// Render the package table
		CStr fg_RenderPackageTable(CPackageUIState const &_State)
		{
			CStr Output;
			CTableRenderHelper TableRenderer
				(
					[&](CStr const &_Line) { Output += _Line; }
					, CTableRenderHelper::EOption_Rounded | CTableRenderHelper::EOption_AvoidRowSeparators
					, _State.m_AnsiEncoding.f_Flags()
					, _State.m_pCommandLine->m_CommandLineWidth
				)
			;

			TableRenderer.f_AddHeadings("", "Application", "Version", "Platforms", "Size");

			// Calculate minimum column widths from ALL rows for stable layout
			mint nMaxAppNameLen = CAnsiEncodingParse::fs_RenderedStrLen("Application");
			mint nMaxVersionLen = CAnsiEncodingParse::fs_RenderedStrLen("Version");
			mint nMaxPlatformLen = CAnsiEncodingParse::fs_RenderedStrLen("Platforms");
			mint nMaxSizeLen = CAnsiEncodingParse::fs_RenderedStrLen("Size");

			uint64 nTotalSelectedBytes = 0;
			mint nSelectedApps = 0;

			for (auto const &Row : _State.m_TableRows)
			{
				nMaxAppNameLen = fg_Max(nMaxAppNameLen, (mint)CAnsiEncodingParse::fs_RenderedStrLen(Row.m_Application));

				// Version from first platform
				if (!Row.m_VersionByPlatform.f_IsEmpty())
				{
					auto iFirst = Row.m_VersionByPlatform.f_GetIterator();
					auto const &VersionID = (*iFirst).m_VersionID;
					if (VersionID.f_IsValid())
					{
						CStr Version = "{}/{}.{}.{}"_f << VersionID.m_Branch << VersionID.m_Major << VersionID.m_Minor << VersionID.m_Revision;
						nMaxVersionLen = fg_Max(nMaxVersionLen, (mint)CAnsiEncodingParse::fs_RenderedStrLen(Version));
					}
				}

				// Platform names (check each individually since they're on separate lines)
				for (auto const &Platform : Row.m_AvailablePlatforms)
				{
					CStr const &PlatformName = Row.m_AvailablePlatforms.fs_GetKey(Platform);
					nMaxPlatformLen = fg_Max(nMaxPlatformLen, (mint)CAnsiEncodingParse::fs_RenderedStrLen(PlatformName));
				}

				// Size strings (check each individually)
				for (auto const &Platform : Row.m_AvailablePlatforms)
				{
					CStr const &PlatformName = Row.m_AvailablePlatforms.fs_GetKey(Platform);
					if (auto *pBytes = Row.m_BytesByPlatform.f_FindEqual(PlatformName))
					{
						CStr SizeStr = "{ns }"_f << *pBytes;
						nMaxSizeLen = fg_Max(nMaxSizeLen, (mint)CAnsiEncodingParse::fs_RenderedStrLen(SizeStr));
					}
				}

				if (Row.m_bSelected)
				{
					nTotalSelectedBytes += Row.f_GetTotalBytes();
					++nSelectedApps;
				}
			}

			// Set minimum column widths
			TableRenderer.f_SetMinColumnWidth(1, (uint32)nMaxAppNameLen);
			TableRenderer.f_SetMinColumnWidth(2, (uint32)nMaxVersionLen);
			TableRenderer.f_SetMinColumnWidth(3, (uint32)nMaxPlatformLen);
			TableRenderer.f_SetMinColumnWidth(4, (uint32)nMaxSizeLen);

			// Calculate which rows fit in line budget using line-based scrolling
			mint nScrollOffset = _State.m_TableScrollOffset;
			bool bHasMoreAbove = nScrollOffset > 0;

			auto ScrollResult = fs_CalculateVisibleRowsOptimized(
				_State.m_TableRows, nScrollOffset, gc_MaxDisplayLines, bHasMoreAbove);

			mint nVisibleRowCount = ScrollResult.m_nVisibleRowCount;
			mint nLinesUsed = ScrollResult.m_nLinesUsed;
			bool bHasMoreBelow = ScrollResult.m_bHasMoreBelow;
			bool bPrevWasMultiLine = ScrollResult.m_bLastRowWasMultiLine;

			// Add indicator lines to the total
			if (bHasMoreAbove)
				nLinesUsed += 1;
			if (bHasMoreBelow)
				nLinesUsed += 1;

			// Separator after last data row appears if it's multi-line AND there's something after
			// (either "..." below or padding rows)
			bool bWouldHavePadding = nLinesUsed < gc_MaxDisplayLines;
			if (bPrevWasMultiLine && (bHasMoreBelow || bWouldHavePadding))
				nLinesUsed += 1;

			// Calculate padding lines needed for stable height (guard against underflow since mint is unsigned)
			mint nPaddingLines = (nLinesUsed < gc_MaxDisplayLines) ? (gc_MaxDisplayLines - nLinesUsed) : 0;

			// Add "more above" indicator row if needed
			if (bHasMoreAbove)
			{
				TableRenderer.f_AddRow
					(
						"{}...{}"_f << _State.m_AnsiEncoding.f_Foreground256(246) << _State.m_AnsiEncoding.f_Default()
						, ""
						, ""
						, ""
						, ""
					)
				;
			}

			// Render visible rows
			for (mint iRow = nScrollOffset; iRow < nScrollOffset + nVisibleRowCount; ++iRow)
			{
				auto const &Row = _State.m_TableRows[iRow];

				bool bRowSelected = (_State.m_UIMode == EPackageUIMode::mc_Table && _State.m_SelectedRow == iRow);

				CStr CheckBox = Row.m_bSelected ? "[x]" : "[ ]";
				if (bRowSelected && _State.m_SelectedColumn == EPackageTableColumn::mc_Selected)
				{
					CheckBox = "{}{}{}{}"_f
						<< _State.m_AnsiEncoding.f_Bold()
						<< _State.m_AnsiEncoding.f_BackgroundRGB(59, 130, 246)
						<< CStr(CheckBox)
						<< _State.m_AnsiEncoding.f_Default()
					;
				}

				CStr AppName = Row.m_Application;
				if (bRowSelected && _State.m_SelectedColumn == EPackageTableColumn::mc_Application)
				{
					AppName = "{}{}{}{}"_f
						<< _State.m_AnsiEncoding.f_Bold()
						<< _State.m_AnsiEncoding.f_BackgroundRGB(59, 130, 246)
						<< CStr(AppName)
						<< _State.m_AnsiEncoding.f_Default()
					;
				}

				// Get version from first available platform
				CStr Version = "-";
				if (!Row.m_VersionByPlatform.f_IsEmpty())
				{
					auto iFirst = Row.m_VersionByPlatform.f_GetIterator();
					auto const &VersionID = (*iFirst).m_VersionID;
					if (VersionID.f_IsValid())
						Version = "{}/{}.{}.{}"_f << VersionID.m_Branch << VersionID.m_Major << VersionID.m_Minor << VersionID.m_Revision;
				}

				// Format platforms - newline-separated for multi-line display
				CStr Platforms;
				for (auto const &Platform : Row.m_AvailablePlatforms)
				{
					if (!Platforms.f_IsEmpty())
						Platforms += "\n";
					Platforms += Row.m_AvailablePlatforms.fs_GetKey(Platform);
				}
				if (Platforms.f_IsEmpty())
					Platforms = "-";

				if (bRowSelected && _State.m_SelectedColumn == EPackageTableColumn::mc_Platforms)
				{
					Platforms = "{}{}{}{}"_f
						<< _State.m_AnsiEncoding.f_Bold()
						<< _State.m_AnsiEncoding.f_BackgroundRGB(59, 130, 246)
						<< CStr(Platforms)
						<< _State.m_AnsiEncoding.f_Default()
					;
				}

				// Format sizes - newline-separated to match platform entries
				CStr Sizes;
				for (auto const &Platform : Row.m_AvailablePlatforms)
				{
					CStr const &PlatformName = Row.m_AvailablePlatforms.fs_GetKey(Platform);
					if (!Sizes.f_IsEmpty())
						Sizes += "\n";
					if (auto *pBytes = Row.m_BytesByPlatform.f_FindEqual(PlatformName))
						Sizes += "{ns }"_f << *pBytes;
					else
						Sizes += "-";
				}
				if (Sizes.f_IsEmpty())
					Sizes = "-";

				TableRenderer.f_AddRow(CheckBox, AppName, Version, Platforms, Sizes);
			}

			// Add "more below" indicator row if needed
			if (bHasMoreBelow)
			{
				TableRenderer.f_AddRow
					(
						"{}...{}"_f << _State.m_AnsiEncoding.f_Foreground256(246) << _State.m_AnsiEncoding.f_Default()
						, ""
						, ""
						, ""
						, ""
					)
				;
			}

			// Add padding rows to maintain stable table height
			for (mint i = 0; i < nPaddingLines; ++i)
				TableRenderer.f_AddRow("", "", "", "", "");

			// Add total row
			TableRenderer.f_ForceRowSeparator();
			TableRenderer.f_AddRow("", "{} selected"_f << nSelectedApps, "", "Total:", "{ns }"_f << nTotalSelectedBytes);

			TableRenderer.f_Output();

			return Output;
		}

		// Render the full UI
		TCFuture<void> fg_RenderPackageUI(TCSharedPointer<CPackageUIState> _pState)
		{
			CStr Output;
			Output += _pState->m_AnsiEncoding.f_SyncronizeOutputStart();

			// Clear previous lines
			if (_pState->m_LastRenderedLines > 0)
			{
				Output += _pState->m_AnsiEncoding.f_MovePreviousLine(_pState->m_LastRenderedLines);
				_pState->m_LastRenderedLines = 0;
				Output += _pState->m_AnsiEncoding.f_MoveToColumn(0);
				Output += _pState->m_AnsiEncoding.f_ClearToEndOfScreen();
			}

			Output += _pState->m_AnsiEncoding.f_ShowCursor(false);

			Output += fg_RenderPackageButtons(*_pState);
			Output += "\n";
			Output += fg_RenderPackageTable(*_pState);
			Output += "\n";

			// Help text
			if (_pState->m_UIMode == EPackageUIMode::mc_Buttons)
				Output += "Arrow keys: Navigate | Enter: Edit | Tab: Switch to table | Esc: Cancel\n";
			else
				Output += "Arrow keys: Navigate | Space/Enter: Toggle selection | Tab: Back to buttons | Esc: Cancel\n";

			Output += _pState->m_AnsiEncoding.f_ShowCursor(true);
			Output += _pState->m_AnsiEncoding.f_SyncronizeOutputFinish();

			co_await _pState->m_pCommandLine->f_StdOut(Output);

			// Count lines for next clear
			_pState->m_LastRenderedLines = 0;
			for (ch8 c : Output)
			{
				if (c == '\n')
					++_pState->m_LastRenderedLines;
			}

			co_return {};
		}

		// Handle button activation
		TCFuture<void> fg_HandleButtonActivation(TCSharedPointer<CPackageUIState> _pState)
		{
			switch (_pState->m_SelectedButton)
			{
			case EPackageButton::mc_Branch:
				{
					_pState->m_bOpeningDialog = true;

					// Clear the current UI
					if (_pState->m_LastRenderedLines > 0)
					{
						CStr ClearOutput;
						ClearOutput += _pState->m_AnsiEncoding.f_MovePreviousLine(_pState->m_LastRenderedLines);
						_pState->m_LastRenderedLines = 0;
						ClearOutput += _pState->m_AnsiEncoding.f_MoveToColumn(0);
						ClearOutput += _pState->m_AnsiEncoding.f_ClearToEndOfScreen();
						co_await _pState->m_pCommandLine->f_StdOut(ClearOutput);
					}

					// Build branch list from cache
					TCVector<TCVector<CStr>> BranchItems;
					BranchItems.f_Insert(TCVector<CStr>{"(any)"});

					auto Branches = _pState->f_GetUniqueBranches();
					for (auto const &Branch : Branches)
						BranchItems.f_Insert(TCVector<CStr>{Branch});

					auto Selected = co_await fg_SelectFromListWithFilter
						(
							_pState->m_pCommandLine
							, BranchItems
							, {"Branch"}
							, "Select branch filter"
							, _pState->m_Config.m_Branch
						)
					;

					if (Selected)
					{
						_pState->m_Config.m_Branch = (*Selected == "(any)") ? CStr{} : *Selected;
						_pState->f_RefreshTableFromCache();
					}

					_pState->m_bOpeningDialog = false;
					_pState->m_LastRenderedLines = 0;
					_pState->m_bNeedsRedraw = true;
				}
				break;

			case EPackageButton::mc_Tag:
				{
					_pState->m_bOpeningDialog = true;

					// Clear the current UI
					if (_pState->m_LastRenderedLines > 0)
					{
						CStr ClearOutput;
						ClearOutput += _pState->m_AnsiEncoding.f_MovePreviousLine(_pState->m_LastRenderedLines);
						_pState->m_LastRenderedLines = 0;
						ClearOutput += _pState->m_AnsiEncoding.f_MoveToColumn(0);
						ClearOutput += _pState->m_AnsiEncoding.f_ClearToEndOfScreen();
						co_await _pState->m_pCommandLine->f_StdOut(ClearOutput);
					}

					// Build tag list from cache
					TCVector<TCVector<CStr>> TagItems;
					TagItems.f_Insert(TCVector<CStr>{"(any)"});

					auto Tags = _pState->f_GetUniqueTags();
					for (auto const &Tag : Tags)
						TagItems.f_Insert(TCVector<CStr>{Tag});

					auto Selected = co_await fg_SelectFromListWithFilter
						(
							_pState->m_pCommandLine
							, TagItems
							, {"Tag"}
							, "Select tag filter"
							, _pState->m_Config.m_Tag
						)
					;

					if (Selected)
					{
						_pState->m_Config.m_Tag = (*Selected == "(any)") ? CStr{} : *Selected;
						_pState->f_RefreshTableFromCache();
					}

					_pState->m_bOpeningDialog = false;
					_pState->m_LastRenderedLines = 0;
					_pState->m_bNeedsRedraw = true;
				}
				break;

			case EPackageButton::mc_Platform:
				{
					_pState->m_bOpeningDialog = true;

					// Clear the current UI
					if (_pState->m_LastRenderedLines > 0)
					{
						CStr ClearOutput;
						ClearOutput += _pState->m_AnsiEncoding.f_MovePreviousLine(_pState->m_LastRenderedLines);
						_pState->m_LastRenderedLines = 0;
						ClearOutput += _pState->m_AnsiEncoding.f_MoveToColumn(0);
						ClearOutput += _pState->m_AnsiEncoding.f_ClearToEndOfScreen();
						co_await _pState->m_pCommandLine->f_StdOut(ClearOutput);
					}

					// Build platform list from cache
					TCVector<TCVector<CStr>> PlatformItems;

					auto Platforms = _pState->f_GetUniquePlatforms();
					for (auto const &Platform : Platforms)
						PlatformItems.f_Insert(TCVector<CStr>{Platform});

					// Build default selection from current state
					TCVector<CStr> DefaultSelected;
					for (auto const &Platform : _pState->m_Config.m_SelectedPlatforms)
						DefaultSelected.f_Insert(Platform);

					auto Selected = co_await fg_MultiSelectFromListWithFilter
						(
							_pState->m_pCommandLine
							, PlatformItems
							, {"Platform"}
							, "Select platform filter"
							, DefaultSelected
						)
					;

					if (Selected)
					{
						_pState->m_Config.m_SelectedPlatforms = fg_SetFromContainer(*Selected);
						_pState->f_RefreshTableFromCache();
					}

					_pState->m_bOpeningDialog = false;
					_pState->m_LastRenderedLines = 0;
					_pState->m_bNeedsRedraw = true;
				}
				break;

			case EPackageButton::mc_Cancel:
				_pState->m_bDone = true;
				_pState->m_bConfirmed = false;
				break;

			case EPackageButton::mc_Download:
				_pState->m_bDone = true;
				_pState->m_bConfirmed = true;
				break;

			default:
				break;
			}

			co_return {};
		}

		// Handle table cell activation
		TCFuture<void> fg_HandleTableCellActivation(TCSharedPointer<CPackageUIState> _pState)
		{
			if (_pState->m_SelectedRow >= _pState->m_TableRows.f_GetLen())
				co_return {};

			auto &Row = _pState->m_TableRows[_pState->m_SelectedRow];

			switch (_pState->m_SelectedColumn)
			{
			case EPackageTableColumn::mc_Selected:
				Row.m_bSelected = !Row.m_bSelected;
				_pState->m_Config.m_SelectedApplications[Row.m_Application] = Row.m_bSelected;
				_pState->m_bNeedsRedraw = true;
				break;

			// Platforms are now selected globally via the Platform filter button
			// No per-row platform selection
			default:
				break;
			}

			co_return {};
		}

		// Handle keyboard input
		TCFuture<void> fg_HandlePackageInput(TCSharedPointer<CPackageUIState> _pState, CStr _Input, TCSharedPointer<TCPromise<bool>> _pResultPromise)
		{
			auto SequenceSubscription = co_await _pState->m_InputSequencer.f_Sequence();

			if (_pState->m_bDone)
				co_return {};

			if (_Input == gc_Escape)
			{
				// Escape - cancel
				_pState->m_bDone = true;
				_pState->m_bConfirmed = false;
				if (!_pResultPromise->f_IsSet())
					_pResultPromise->f_SetResult(false);
				co_return {};
			}

			if (_Input == gc_Tab)
			{
				// Tab - toggle between buttons and table
				if (_pState->m_UIMode == EPackageUIMode::mc_Buttons)
				{
					if (!_pState->m_TableRows.f_IsEmpty())
					{
						_pState->m_UIMode = EPackageUIMode::mc_Table;
						_pState->m_SelectedRow = 0;
						_pState->m_SelectedColumn = EPackageTableColumn::mc_Selected;
					}
				}
				else
				{
					_pState->m_UIMode = EPackageUIMode::mc_Buttons;
				}
				_pState->m_bNeedsRedraw = true;
			}
			else if (_Input == gc_Space && _pState->m_UIMode == EPackageUIMode::mc_Table)
			{
				// Toggle selection in table
				if (_pState->m_SelectedRow < _pState->m_TableRows.f_GetLen())
				{
					auto &Row = _pState->m_TableRows[_pState->m_SelectedRow];
					Row.m_bSelected = !Row.m_bSelected;
					_pState->m_Config.m_SelectedApplications[Row.m_Application] = Row.m_bSelected;
					_pState->m_bNeedsRedraw = true;
				}
			}
			else if (_Input == "\r" || _Input == "\n")
			{
				// Enter - activate button or cell
				if (_pState->m_UIMode == EPackageUIMode::mc_Buttons)
					co_await fg_HandleButtonActivation(_pState);
				else
					co_await fg_HandleTableCellActivation(_pState);
				_pState->m_bNeedsRedraw = true;
			}
			else if (_Input == gc_UpArrow)
			{
				if (_pState->m_UIMode == EPackageUIMode::mc_Buttons)
				{
					// Navigate buttons - respect column position
					// Row 0: Branch (col 0), Tag (col 1), Platform (col 2)
					// Row 1: [empty] (col 0), Cancel (col 1), Download (col 2)
					switch (_pState->m_SelectedButton)
					{
					case EPackageButton::mc_Cancel:
						_pState->m_SelectedButton = EPackageButton::mc_Tag;
						break;
					case EPackageButton::mc_Download:
						_pState->m_SelectedButton = EPackageButton::mc_Platform;
						break;
					default:
						break;  // Already on top row
					}
				}
				else
				{
					// Navigate table rows
					if (_pState->m_SelectedRow > 0)
					{
						--_pState->m_SelectedRow;

						// Adjust scroll to keep selection visible
						if (_pState->m_SelectedRow < _pState->m_TableScrollOffset)
							_pState->m_TableScrollOffset = _pState->m_SelectedRow;
					}
					else
					{
						// At top of table - move back to buttons (Download button)
						_pState->m_UIMode = EPackageUIMode::mc_Buttons;
						_pState->m_SelectedButton = EPackageButton::mc_Download;
					}
				}
				_pState->m_bNeedsRedraw = true;
			}
			else if (_Input == gc_DownArrow)
			{
				if (_pState->m_UIMode == EPackageUIMode::mc_Buttons)
				{
					// Navigate buttons - respect column position
					// Row 0: Branch (col 0), Tag (col 1), Platform (col 2)
					// Row 1: [empty] (col 0), Cancel (col 1), Download (col 2)
					switch (_pState->m_SelectedButton)
					{
					case EPackageButton::mc_Branch:
					case EPackageButton::mc_Tag:
						_pState->m_SelectedButton = EPackageButton::mc_Cancel;
						break;
					case EPackageButton::mc_Platform:
						_pState->m_SelectedButton = EPackageButton::mc_Download;
						break;
					case EPackageButton::mc_Cancel:
					case EPackageButton::mc_Download:
						// At bottom of buttons - move to table
						if (!_pState->m_TableRows.f_IsEmpty())
						{
							_pState->m_UIMode = EPackageUIMode::mc_Table;
							_pState->m_SelectedRow = 0;
							_pState->m_SelectedColumn = EPackageTableColumn::mc_Selected;
						}
						break;
					default:
						break;
					}
				}
				else
				{
					if (_pState->m_SelectedRow < _pState->m_TableRows.f_GetLen() - 1)
						++_pState->m_SelectedRow;

					// Adjust scroll to keep selection visible
					auto ScrollResult = fs_CalculateVisibleRowsOptimized(
						_pState->m_TableRows, _pState->m_TableScrollOffset, gc_MaxDisplayLines,
						_pState->m_TableScrollOffset > 0);

					mint nVisibleRowCount = fg_Max(mint(1), ScrollResult.m_nVisibleRowCount);

					// Scroll until selected row is visible
					// Need a loop because scrolling changes bHasMoreAbove which affects nVisibleRowCount
					while (_pState->m_SelectedRow >= _pState->m_TableScrollOffset + nVisibleRowCount)
					{
						++_pState->m_TableScrollOffset;

						ScrollResult = fs_CalculateVisibleRowsOptimized(
							_pState->m_TableRows, _pState->m_TableScrollOffset, gc_MaxDisplayLines,
							_pState->m_TableScrollOffset > 0);

						nVisibleRowCount = fg_Max(mint(1), ScrollResult.m_nVisibleRowCount);
					}
				}
				_pState->m_bNeedsRedraw = true;
			}
			else if (_Input == gc_LeftArrow)
			{
				if (_pState->m_UIMode == EPackageUIMode::mc_Buttons)
				{
					// Top row: Platform -> Tag -> Branch
					if (_pState->m_SelectedButton == EPackageButton::mc_Platform)
						_pState->m_SelectedButton = EPackageButton::mc_Tag;
					else if (_pState->m_SelectedButton == EPackageButton::mc_Tag)
						_pState->m_SelectedButton = EPackageButton::mc_Branch;
					// Bottom row: Download -> Cancel
					else if (_pState->m_SelectedButton == EPackageButton::mc_Download)
						_pState->m_SelectedButton = EPackageButton::mc_Cancel;
				}
				else
				{
					if (_pState->m_SelectedColumn > EPackageTableColumn::mc_Selected)
						_pState->m_SelectedColumn = (EPackageTableColumn)((mint)_pState->m_SelectedColumn - 1);
				}
				_pState->m_bNeedsRedraw = true;
			}
			else if (_Input == gc_RightArrow)
			{
				if (_pState->m_UIMode == EPackageUIMode::mc_Buttons)
				{
					// Top row: Branch -> Tag -> Platform
					if (_pState->m_SelectedButton == EPackageButton::mc_Branch)
						_pState->m_SelectedButton = EPackageButton::mc_Tag;
					else if (_pState->m_SelectedButton == EPackageButton::mc_Tag)
						_pState->m_SelectedButton = EPackageButton::mc_Platform;
					// Bottom row: Cancel -> Download
					else if (_pState->m_SelectedButton == EPackageButton::mc_Cancel)
						_pState->m_SelectedButton = EPackageButton::mc_Download;
				}
				else
				{
					if (_pState->m_SelectedColumn < EPackageTableColumn::mc_Size)
						_pState->m_SelectedColumn = (EPackageTableColumn)((mint)_pState->m_SelectedColumn + 1);
				}
				_pState->m_bNeedsRedraw = true;
			}
			else if (_Input == "\x03") // Ctrl+C
			{
				_pState->m_bDone = true;
				_pState->m_bConfirmed = false;
			}

			if (_pState->m_bNeedsRedraw && !_pState->m_bDone && !_pState->m_bOpeningDialog)
			{
				_pState->m_bNeedsRedraw = false;
				co_await fg_RenderPackageUI(_pState);
			}

			if (_pState->m_bDone && !_pResultPromise->f_IsSet())
				_pResultPromise->f_SetResult(_pState->m_bConfirmed);

			co_return {};
		}

		// Run the package UI input loop and wait for user confirmation
		TCFuture<bool> fg_RunPackageUIInputLoop(TCSharedPointer<CPackageUIState> _pState)
		{
			// Initial render
			co_await fg_RenderPackageUI(_pState);

			// Input loop using RegisterForStdIn pattern
			TCPromiseFuturePair<bool> ResultPromise;

			auto StdInSubscription = co_await _pState->m_pCommandLine->f_RegisterForStdIn
				(
					g_ActorFunctor / [_pState, pResultPromise = TCSharedPointer<TCPromise<bool>>(fg_Construct(fg_Move(ResultPromise.m_Promise)))]
						(NProcess::EStdInReaderOutputType _Type, CStrIO _Input) mutable -> TCFuture<void>
					{
						if (_Type != NProcess::EStdInReaderOutputType_StdIn)
							co_return {};

						if (_pState->m_bDone || _pState->m_bOpeningDialog)
							co_return {};

						auto HandleResult = co_await fg_HandlePackageInput(_pState, _Input, pResultPromise).f_Wrap();
						if (!HandleResult && !pResultPromise->f_IsSet())
							pResultPromise->f_SetException(HandleResult.f_GetException());

						co_return {};
					}
					, NProcess::EStdInReaderFlag_None
				)
			;

			auto SubscriptionDestroy = co_await fg_AsyncDestroy(fg_Move(StdInSubscription));

			// Clear screen on exit (normal or exception)
			auto ClearScreen = co_await fg_AsyncDestroy
				(
					[&] -> TCFuture<void>
					{
						auto pLocalState = _pState;
						CStr ClearOutput;
						if (pLocalState->m_LastRenderedLines > 0)
						{
							ClearOutput += pLocalState->m_AnsiEncoding.f_MovePreviousLine(pLocalState->m_LastRenderedLines);
							pLocalState->m_LastRenderedLines = 0;
							ClearOutput += pLocalState->m_AnsiEncoding.f_MoveToColumn(0);
							ClearOutput += pLocalState->m_AnsiEncoding.f_ClearToEndOfScreen();
							co_await pLocalState->m_pCommandLine->f_StdOut(ClearOutput);
						}

						co_return {};
					}
				)
			;

			// Clean up sequencer when done
			auto SequencerDestroy = co_await fg_AsyncDestroy
				(
					[&] -> TCFuture<void>
					{
						co_await fg_Move(_pState->m_InputSequencer).f_Destroy();
						co_return {};
					}
				)
			;

			co_return co_await fg_Move(ResultPromise.m_Future);
		}

		// Main ConfigUI-style loop for VersionManager package selection
		TCFuture<TCOptional<CPackageConfig>> fg_ConfigurePackagesFromVersionManager
			(
				TCSharedPointer<CCommandLineControl> _pCommandLine
				, TCActor<NConcurrency::CDistributedActorTrustManager> _TrustManager
				, CStr _RootDirectory
				, CPackageConfig _InitialConfig
			)
		{
			TCSharedPointer<CPackageUIState> pState = fg_Construct<CPackageUIState>(_pCommandLine->f_AnsiEncoding());
			pState->m_pCommandLine = _pCommandLine;
			pState->m_Config = _InitialConfig;

			// Subscribe to VersionManagers
			*_pCommandLine += "Connecting to VersionManager...\n";
			{
				auto CaptureScope = co_await (g_CaptureExceptions % "Failed to subscribe to VersionManagers");

				pState->m_VersionManagers = co_await _TrustManager->f_SubscribeTrustedActors<CVersionManager>()
					.f_Timeout(30.0, "Timeout subscribing to VersionManagers")
				;
			}

			if (pState->m_VersionManagers.m_Actors.f_IsEmpty())
			{
				*_pCommandLine %= "Error: Not connected to any VersionManagers.\n";
				co_return {};
			}

			pState->m_bVersionManagerConnected = true;

			// Query versions
			*_pCommandLine += "Querying available packages...\n";
			{
				auto CaptureScope = co_await (g_CaptureExceptions % "Failed to query VersionManager");
				co_await fg_QueryVersionManager(pState);
			}

			if (pState->m_Cache.m_Applications.f_IsEmpty())
			{
				*_pCommandLine %= "Error: No packages found matching filters.\n";
				co_return {};
			}

			// Build initial table
			pState->f_RefreshTableFromCache();

			bool bConfirmed = co_await fg_RunPackageUIInputLoop(pState);

			if (!bConfirmed)
				co_return {};

			// Build result config
			CPackageConfig ResultConfig = pState->m_Config;
			ResultConfig.m_Source = EPackageSource::mc_VersionManager;

			// Update selected applications from table rows
			ResultConfig.m_SelectedApplications.f_Clear();
			for (auto const &Row : pState->m_TableRows)
			{
				if (Row.m_bSelected)
					ResultConfig.m_SelectedApplications[Row.m_Application] = true;
			}

			// Platforms are already in pState->m_Config.m_SelectedPlatforms from the global filter

			// Update state config with selections for download
			pState->m_Config = ResultConfig;

			// Download packages
			CStr PackagesDir = _RootDirectory / "Packages";
			*_pCommandLine += "\nDownloading packages to: {}\n"_f << PackagesDir;

			co_await fg_DownloadPackages(pState, PackagesDir);

			co_return ResultConfig;
		}

	} // anonymous namespace

	// Validate local package directory
	TCFuture<NContainer::TCMap<NStr::CStr, NContainer::TCVector<NStr::CStr>>> fg_ValidateLocalPackages(NStr::CStr _PackageDirectory)
	{
		using namespace NStr;

		NContainer::TCMap<CStr, NContainer::TCVector<CStr>> Result;

		auto BlockingActorCheckout = fg_BlockingActor();
		Result = co_await
			(
				g_Dispatch(BlockingActorCheckout) / [_PackageDirectory]() -> NContainer::TCMap<CStr, NContainer::TCVector<CStr>>
				{
					NContainer::TCMap<CStr, NContainer::TCVector<CStr>> Apps;

					if (!NFile::CFile::fs_FileExists(_PackageDirectory, NFile::EFileAttrib_Directory))
						DMibError("Package directory does not exist: {}"_f << _PackageDirectory);

					// Enumerate applications (first level directories)
					for (auto &AppEntry : NFile::CFile::fs_FindFiles({_PackageDirectory + "/*", false}))
					{
						if (!(AppEntry.m_Attribs & NFile::EFileAttrib_Directory))
							continue;

						CStr AppName = NFile::CFile::fs_GetFile(AppEntry.m_Path);

						// Enumerate platforms (second level directories)
						for (auto &PlatformEntry : NFile::CFile::fs_FindFiles({AppEntry.m_Path + "/*", false}))
						{
							if (!(PlatformEntry.m_Attribs & NFile::EFileAttrib_Directory))
								continue;

							CStr PlatformName = NFile::CFile::fs_GetFile(PlatformEntry.m_Path);
							Apps[AppName].f_Insert(PlatformName);
						}
					}

					return Apps;
				}
				% "Failed to validate package directory"
			)
		;

		co_return Result;
	}

	// Main package configuration function
	TCFuture<TCOptional<CBootstrapConfig>> fg_ConfigurePackages
		(
			TCSharedPointer<CCommandLineControl> _pCommandLine
			, TCActor<NConcurrency::CDistributedActorTrustManager> _TrustManager
			, NStr::CStr _RootDirectory
			, CBootstrapConfig _Config
		)
	{
		using namespace NStr;

		*_pCommandLine += "\n{}Package Configuration{}\n\n"_f
			<< _pCommandLine->f_AnsiEncoding().f_Bold()
			<< _pCommandLine->f_AnsiEncoding().f_Default()
		;

		// Ask for package source
		TCVector<TCVector<CStr>> SourceOptions =
			{
				{"Download from VersionManager"}
				, {"Use local directory with existing packages"}
			}
		;

		auto SourceSelection = co_await fg_SelectFromListWithFilter
			(
				_pCommandLine
				, SourceOptions
				, {""}
				, "Select package source"
				, "Download from VersionManager"
			)
		;

		if (!SourceSelection)
			co_return {};

		if (*SourceSelection == "Use local directory with existing packages")
		{
			// Local directory mode
			CStr DefaultDir = _Config.m_PackageConfig.m_LocalPackageDirectory;
			if (DefaultDir.f_IsEmpty())
				DefaultDir = _RootDirectory / "Packages";

			CStr PackageDir = co_await fg_PromptWithDefault
				(
					_pCommandLine
					, ""
					, "Enter package directory path"
					, DefaultDir
				)
			;

			if (PackageDir.f_IsEmpty())
				co_return {};

			*_pCommandLine += "\nValidating package directory...\n";

			auto Apps = co_await fg_ValidateLocalPackages(PackageDir);

			if (Apps.f_IsEmpty())
			{
				*_pCommandLine %= "Error: No packages found in directory.\n";
				*_pCommandLine %= "Expected structure: {dir}/{Application}/{Platform}/\n";
				co_return {};
			}

			*_pCommandLine += "Found {} applications:\n"_f << Apps.f_GetLen();
			for (auto const &AppEntry : Apps)
			{
				CStr const &AppName = Apps.fs_GetKey(AppEntry);
				CStr Platforms;
				for (auto const &Platform : AppEntry)
				{
					if (!Platforms.f_IsEmpty())
						Platforms += ", ";
					Platforms += Platform;
				}
				*_pCommandLine += "  - {}: {}\n"_f << AppName << Platforms;
			}

			_Config.m_PackageConfig.m_Source = EPackageSource::mc_LocalDirectory;
			_Config.m_PackageConfig.m_LocalPackageDirectory = PackageDir;

			*_pCommandLine += "\nPackage directory validated successfully.\n";
			co_return _Config;
		}
		else
		{
			// VersionManager mode
			auto PackageConfig = co_await fg_ConfigurePackagesFromVersionManager
				(
					_pCommandLine
					, _TrustManager
					, _RootDirectory
					, _Config.m_PackageConfig
				)
			;

			if (!PackageConfig)
				co_return {};

			_Config.m_PackageConfig = *PackageConfig;
			co_return _Config;
		}
	}

}
