// Copyright © 2025 Unbroken AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Concurrency/AsyncDestroy>
#include <Mib/Concurrency/ActorSequencerActor>
#include <Mib/CommandLine/TableRenderer>
#include <Mib/CommandLine/AnsiEncodingParse>
#include <Mib/File/File>
#include <Mib/Web/AWS/EC2>

// Include pricing directly until build system is regenerated
#include "../../../../../Web/Source/Malterlib_Web_AWS_Pricing.h"
#include <Mib/Web/Curl>

#include "Malterlib_Cloud_Bootstrap_ConfigUI.h"
#include "Malterlib_Cloud_Bootstrap_Prompts.h"
#include "Malterlib_Cloud_Bootstrap_Aws.h"

namespace NMib::NCloud::NBootstrap
{
	namespace
	{
		// Arrow key escape sequences
		[[maybe_unused]] static constexpr ch8 gc_UpArrow[] = "\x1b[A";
		[[maybe_unused]] static constexpr ch8 gc_DownArrow[] = "\x1b[B";
		static constexpr ch8 gc_RightArrow[] = "\x1b[C";
		static constexpr ch8 gc_LeftArrow[] = "\x1b[D";
		static constexpr ch8 gc_ShiftUpArrow[] = "\x1b[1;2A";
		static constexpr ch8 gc_ShiftDownArrow[] = "\x1b[1;2B";
		static constexpr ch8 gc_Escape[] = "\x1b";
		static constexpr ch8 gc_Tab[] = "\t";
		static constexpr ch8 gc_Backspace[] = "\x7f";
		static constexpr ch8 gc_Delete[] = "\x1b[3~";

		static constexpr pfp64 const gc_HoursPerMonth = 730.0;

		// Memory requirements per application (in GB) - duplicated from Config.cpp for cycling
		static constexpr pfp64 gc_MemoryGB_KeyManager = 0.5;
		static constexpr pfp64 gc_MemoryGB_CloudManager = 4.0;
		static constexpr pfp64 gc_MemoryGB_VersionManager = 0.5;
		static constexpr pfp64 gc_MemoryGB_SecretsManager = 0.5;
		static constexpr pfp64 gc_MemoryGB_OS = 0.5;

		// Get memory requirement for an application type
		static pfp64 fg_GetMemoryRequirement(EApplicationType _Type)
		{
			switch (_Type)
			{
			case EApplicationType::mc_KeyManager:
				return gc_MemoryGB_KeyManager;
			case EApplicationType::mc_CloudManager:
				return gc_MemoryGB_CloudManager;
			case EApplicationType::mc_VersionManager:
				return gc_MemoryGB_VersionManager;
			case EApplicationType::mc_SecretsManager:
				return gc_MemoryGB_SecretsManager;
			case EApplicationType::mc_AppManager:
				return gc_MemoryGB_OS;
			}
			return 0;
		}

		// Button indices
		enum class EButton : mint
		{
			mc_Region = 0
			, mc_Isolation
			, mc_StorageIsolation
			, mc_Encryption
			, mc_Snapshots
			, mc_NAT
			, mc_KeyManagers
			, mc_CPUType
			, mc_Pricing
			, mc_Cancel
			, mc_Confirm

			, mc_Count
		};

		// Editable table columns (0-indexed from left)
		enum class ETableColumn : mint
		{
			mc_Host = 0
			, mc_Application
			, mc_Instance		// Editable - only for apps that own root volume
			, mc_VCPUs
			, mc_Memory
			, mc_Storage			// Editable
			, mc_Bandwidth		// Editable
			, mc_IOPs			// Editable
			, mc_Snapshots		// Editable
			, mc_Cost

			, mc_Count
		};

		// UI mode - buttons or table cell editing
		enum class EUIMode : mint
		{
			mc_Buttons
			, mc_TableCells
		};

		struct CConfigUIState
		{
			CConfigUIState(CAnsiEncoding _AnsiEncoding)
				: m_AnsiEncoding(_AnsiEncoding)
			{
			}

			TCSharedPointer<CCommandLineControl> m_pCommandLine;
			CBootstrapConfig m_Config;
			CAwsCredentials m_Credentials;
			CAnsiEncoding m_AnsiEncoding;
			TCSharedPointer<CRegionCache> m_pRegionCache = fg_Construct();
			TCMap<CStr, CAwsPricingData> m_PricingCache;
			CStr m_RootDirectory;

			// Button navigation state
			EButton m_SelectedButton = EButton::mc_Confirm;

			// Table cell navigation state
			EUIMode m_UIMode = EUIMode::mc_Buttons;
			mint m_SelectedRow = 0;			// Row in the applications table (0-indexed)
			ETableColumn m_SelectedColumn = ETableColumn::mc_Instance;

			mint m_LastRenderedLines = 0;
			bool m_bDone = false;
			bool m_bConfirmed = false;
			bool m_bNeedsRedraw = true;
			bool m_bOpeningDialog = false;
			NConcurrency::CSequencer m_InputSequencer{"ConfigUI InputSequencer"};

			// Check if a column is editable for a given application
			static bool fs_IsColumnEditable(ETableColumn _Column, CDeploymentApplication const &_App)
			{
				switch (_Column)
				{
				case ETableColumn::mc_Instance:
					return _App.m_bOwnsRootVolume;	// Only hosts can change instance type
				case ETableColumn::mc_Storage:
					return true;	// All apps can have storage customized
				case ETableColumn::mc_Bandwidth:
				case ETableColumn::mc_IOPs:
					return _App.f_GetStorageGB() > 0;	// Only if has storage
				case ETableColumn::mc_Snapshots:
					return _App.f_GetStorageGB() > 0;	// Only if has storage
				default:
					return false;
				}
			}

			// Get next/previous editable column
			ETableColumn f_GetNextEditableColumn(bool _bForward) const
			{
				if (m_SelectedRow >= m_Config.m_Applications.f_GetLen())
					return m_SelectedColumn;

				CDeploymentApplication const &App = m_Config.m_Applications[m_SelectedRow];
				mint nCol = (mint)m_SelectedColumn;
				mint nDir = _bForward ? 1 : -1;

				for (mint i = 0; i < (mint)ETableColumn::mc_Count; ++i)
				{
					nCol += nDir;
					if (nCol >= (mint)ETableColumn::mc_Count)
						nCol = 0;
					if (nCol < 0)
						nCol = (mint)ETableColumn::mc_Count - 1;

					if (fs_IsColumnEditable((ETableColumn)nCol, App))
						return (ETableColumn)nCol;
				}
				return m_SelectedColumn;
			}

			// Get first editable column for a row
			ETableColumn f_GetFirstEditableColumn(mint _Row) const
			{
				if (_Row >= m_Config.m_Applications.f_GetLen())
					return ETableColumn::mc_Instance;

				CDeploymentApplication const &App = m_Config.m_Applications[_Row];
				for (mint nCol = 0; nCol < (mint)ETableColumn::mc_Count; ++nCol)
				{
					if (fs_IsColumnEditable((ETableColumn)nCol, App))
						return (ETableColumn)nCol;
				}
				return ETableColumn::mc_Instance;
			}
		};

		// Format a cost value as USD
		CStr fg_FormatCost(fp64 _Cost)
		{
			if (_Cost == 0)
				return "-";
			return "${fe2}"_f << _Cost;
		}

		// Format storage as GB
		CStr fg_FormatStorage(uint32 _GB)
		{
			if (_GB == 0)
				return "-";
			return "{} GB"_f << _GB;
		}

		TCFuture<void> fg_SaveConfig(CStr _RootDirectory, CBootstrapConfig _Config);

		// Render a single button with background color
		CStr fg_RenderButton(CStr const &_Label, CStr const &_Value, bool _bSelected, CAnsiEncoding const &_Ansi, mint _MinWidth = 0)
		{
			// Calculate content width: " Label: Value "
			mint nContentWidth = 1 + CAnsiEncodingParse::fs_RenderedStrLen(_Label) + 2 + CAnsiEncodingParse::fs_RenderedStrLen(_Value) + 1;
			CStr Padding;
			if (_MinWidth > nContentWidth)
				Padding.f_AddChars(' ', _MinWidth - nContentWidth);

			if (_bSelected)
			{
				// Selected: bright blue background, white text, bold
				return "{}{}{} {}: {}{} {}"_f
					<< _Ansi.f_Bold()
					<< _Ansi.f_BackgroundRGB(59, 130, 246)   // Bright blue (#3B82F6)
					<< _Ansi.f_ForegroundRGB(255, 255, 255)  // White
					<< _Label
					<< _Value
					<< Padding
					<< _Ansi.f_Default()
				;
			}
			else
			{
				// Unselected: dark blue background, light blue text
				return "{}{} {}: {}{} {}"_f
					<< _Ansi.f_BackgroundRGB(30, 58, 138)    // Dark blue (#1E3A8A)
					<< _Ansi.f_ForegroundRGB(147, 197, 253)  // Light blue (#93C5FD)
					<< _Label
					<< _Value
					<< Padding
					<< _Ansi.f_Default()
				;
			}
		}

		// Helper to calculate button content width: " Label: Value "
		mint fg_ButtonContentWidth(CStr const &_Label, CStr const &_Value)
		{
			return 1 + CAnsiEncodingParse::fs_RenderedStrLen(_Label) + 2 + CAnsiEncodingParse::fs_RenderedStrLen(_Value) + 1;
		}

		// Render all buttons in a table
		CStr fg_RenderButtons(CConfigUIState const &_State)
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

			// Headers: first column is call-to-action, rest empty
			TableRenderer.f_AddHeadings("", "", "");
			TableRenderer.f_AddDescription("AWS Configuration");
			TableRenderer.f_SetAlignRight(1);
			TableRenderer.f_SetAlignRight(2);

			// Pre-compute button values for width calculation
			CStr Col0Labels[] = {"Region", "Encryption", "KeyManagers"};
			CStr Col0Values[] =
				{
					_State.m_Config.m_Region
					, _State.m_Config.m_bStorageEncryption ? CStr{"Yes"} : CStr{"No"}
					, "{}"_f << _State.m_Config.m_KeyManagerCount
				}
			;

			CStr Col1Labels[] = {"Isolation", "Snapshots", "CPU"};
			CStr Col1Values[] =
				{
					CBootstrapConfig::fs_GetIsolationLevelDisplayName(_State.m_Config.m_IsolationLevel)
					, CBootstrapConfig::fs_GetSnapshotLevelDisplayName(_State.m_Config.m_SnapshotLevel)
					, CBootstrapConfig::fs_GetCPUTypeDisplayName(_State.m_Config.m_CPUType)
				}
			;

			CStr Col2Labels[] = {"Storage", "NAT", "Pricing"};
			CStr Col2Values[] =
				{
					CBootstrapConfig::fs_GetStorageIsolationDisplayName(_State.m_Config.m_StorageIsolation)
					, CBootstrapConfig::fs_GetNATConfigurationDisplayName(_State.m_Config.m_NATConfiguration)
					, CPricingSelection::fs_GetShortName(_State.m_Config.m_Pricing)
				}
			;

			// Calculate max width for each column
			mint nCol0Width = 0;
			mint nCol1Width = 0;
			mint nCol2Width = 0;

			for (mint i = 0; i < 3; ++i)
			{
				nCol0Width = fg_Max(nCol0Width, fg_ButtonContentWidth(Col0Labels[i], Col0Values[i]));
				nCol1Width = fg_Max(nCol1Width, fg_ButtonContentWidth(Col1Labels[i], Col1Values[i]));
				nCol2Width = fg_Max(nCol2Width, fg_ButtonContentWidth(Col2Labels[i], Col2Values[i]));
			}

			// Helper to render a single button cell with column width
			auto fRenderButton = [&](EButton _Button, CStr const &_Label, CStr const &_Value, mint _Width) -> CStr
				{
					return fg_RenderButton(_Label, _Value, _State.m_SelectedButton == _Button && _State.m_UIMode == EUIMode::mc_Buttons, _State.m_AnsiEncoding, _Width);
				}
			;

			// Helper for Cancel button (red background styling)
			auto fRenderCancelButton = [&]() -> CStr
				{
					bool bSelected = (_State.m_SelectedButton == EButton::mc_Cancel) && (_State.m_UIMode == EUIMode::mc_Buttons);
					if (bSelected)
					{
						// Selected: bright red background, white text, bold
						return "{}{}{} Cancel {}"_f
							<< _State.m_AnsiEncoding.f_Bold()
							<< _State.m_AnsiEncoding.f_Background256(160)  // Bright red background
							<< _State.m_AnsiEncoding.f_Foreground256(255)  // White text
							<< _State.m_AnsiEncoding.f_Default()
						;
					}
					else
					{
						// Unselected: dark red background, light red text
						return "{}{} Cancel {}"_f
							<< _State.m_AnsiEncoding.f_Background256(52)   // Dark red background
							<< _State.m_AnsiEncoding.f_Foreground256(217)  // Light red text
							<< _State.m_AnsiEncoding.f_Default()
						;
					}
				}
			;

			// Helper for Confirm button (green background styling)
			auto fRenderConfirmButton = [&]() -> CStr
				{
					bool bSelected = (_State.m_SelectedButton == EButton::mc_Confirm) && (_State.m_UIMode == EUIMode::mc_Buttons);
					if (bSelected)
					{
						// Selected: bright green background, white text, bold
						return "{}{}{} Confirm {}"_f
							<< _State.m_AnsiEncoding.f_Bold()
							<< _State.m_AnsiEncoding.f_Background256(34)   // Bright green background
							<< _State.m_AnsiEncoding.f_Foreground256(255)  // White text
							<< _State.m_AnsiEncoding.f_Default()
						;
					}
					else
					{
						// Unselected: dark green background, light green text
						return "{}{} Confirm {}"_f
							<< _State.m_AnsiEncoding.f_Background256(22)   // Dark green background
							<< _State.m_AnsiEncoding.f_Foreground256(157)  // Light green text
							<< _State.m_AnsiEncoding.f_Default()
						;
					}
				}
			;

			// Row 1: Region, Isolation, Storage
			TableRenderer.f_AddRow("", "", "");
			TableRenderer.f_AddRow
				(
					fRenderButton(EButton::mc_Region, Col0Labels[0], Col0Values[0], nCol0Width)
					, fRenderButton(EButton::mc_Isolation, Col1Labels[0], Col1Values[0], nCol1Width)
					, fRenderButton(EButton::mc_StorageIsolation, Col2Labels[0], Col2Values[0], nCol2Width)
				)
			;

			TableRenderer.f_AddRow("", "", "");

			// Row 2: Encryption, Snapshots, NAT
			TableRenderer.f_AddRow
				(
					fRenderButton(EButton::mc_Encryption, Col0Labels[1], Col0Values[1], nCol0Width)
					, fRenderButton(EButton::mc_Snapshots, Col1Labels[1], Col1Values[1], nCol1Width)
					, fRenderButton(EButton::mc_NAT, Col2Labels[1], Col2Values[1], nCol2Width)
				)
			;
			TableRenderer.f_AddRow("", "", "");

			// Row 3: KeyManagers, CPU, Pricing
			TableRenderer.f_AddRow
				(
					fRenderButton(EButton::mc_KeyManagers, Col0Labels[2], Col0Values[2], nCol0Width)
					, fRenderButton(EButton::mc_CPUType, Col1Labels[2], Col1Values[2], nCol1Width)
					, fRenderButton(EButton::mc_Pricing, Col2Labels[2], Col2Values[2], nCol2Width)
				)
			;
			TableRenderer.f_AddRow("", "", "");

			// Separator before action row
			//TableRenderer.f_ForceRowSeparator();

			// Row 4: Cancel and Confirm buttons right-aligned
			TableRenderer.f_AddRow("", fRenderCancelButton(), fRenderConfirmButton());
			TableRenderer.f_AddRow("", "", "");

			TableRenderer.f_Output(CTableRenderHelper::EOutputType_HumanReadable);

			return Output;
		}

		// Helper to format a cell value with optional highlighting and customization indicator
		CStr fg_FormatCell
			(
				CStr const &_Value
				, bool _bSelected
				, bool _bCustomized
				, bool _bEditable
				, CAnsiEncoding const &_Ansi
			)
		{
			CStr Result;

			if (_bSelected)
			{
				// Selected cell: cyan background, white text, bold
				Result = "{}{}{}{}{}{}"_f
					<< _Ansi.f_Bold()
					<< _Ansi.f_Background256(30)	// Dark cyan background
					<< _Ansi.f_Foreground256(255)	// White text
					<< _Value
					<< (_bCustomized ? "*" : "")
					<< _Ansi.f_Default()
				;
			}
			else if (_bCustomized)
			{
				// Customized cell: yellow text with asterisk
				Result = "{}{}*{}"_f
					<< _Ansi.f_Foreground256(220)	// Yellow text
					<< _Value
					<< _Ansi.f_Default()
				;
			}
			else if (_bEditable)
			{
				// Editable but not customized: dim underline hint
				Result = "{}{}{}"_f
					<< _Ansi.f_Foreground256(249)	// Light gray
					<< _Value
					<< _Ansi.f_Default()
				;
			}
			else
			{
				Result = _Value;
			}

			return Result;
		}

		// Render the configuration table
		CStr fg_RenderTable(CConfigUIState const &_State)
		{
			CStr TableOutput;
			CTableRenderHelper TableRenderer
				(
					[&](CStr const &_Line) { TableOutput += _Line; }
					, CTableRenderHelper::EOption_Rounded | CTableRenderHelper::EOption_AvoidRowSeparators
					, _State.m_AnsiEncoding.f_Flags()
					, _State.m_pCommandLine->m_CommandLineWidth
				)
			;

			TableRenderer.f_AddHeadings("Host", "Application", "Instance", "VCPUs", "Memory", "Storage", "Bandwidth", "IOPs", "Snapshots", "Cost/mo");

			// Right-align cost column
			TableRenderer.f_SetAlignRight(9);

			// Track unique hosts for grouping
			CStr LastHost;

			// Calculate costs per host (instance cost goes on root volume owner)
			TCMap<CStr, fp64> HostInstanceCosts;
			for (auto const &App : _State.m_Config.m_Applications)
			{
				if (App.m_bOwnsRootVolume)
					HostInstanceCosts[App.m_Host] = App.m_InstanceCostPerHour * gc_HoursPerMonth;
			}

			bool bTableMode = (_State.m_UIMode == EUIMode::mc_TableCells);

			// Add application rows
			mint nRow = 0;
			for (auto const &App : _State.m_Config.m_Applications)
			{
				// Add separator between different hosts
				if (!LastHost.f_IsEmpty() && LastHost != App.m_Host)
					TableRenderer.f_ForceRowSeparator();

				LastHost = App.m_Host;

				// Calculate this row's cost contribution
				fp64 RowCost = 0;
				if (App.m_bOwnsRootVolume)
					RowCost += App.m_InstanceCostPerHour * gc_HoursPerMonth;
				RowCost += App.m_StorageCostPerMonth;
				RowCost += App.m_SnapshotCostPerMonth;

				// Check if cells are selected/customized/editable
				bool bRowSelected = bTableMode && (nRow == _State.m_SelectedRow);

				auto fCell = [&](ETableColumn _Col, CStr const &_Value, bool _bCustomized) -> CStr
					{
						bool bSelected = bRowSelected && (_State.m_SelectedColumn == _Col);
						bool bEditable = CConfigUIState::fs_IsColumnEditable(_Col, App);
						return fg_FormatCell(_Value, bSelected, _bCustomized, bEditable, _State.m_AnsiEncoding);
					}
				;

				// Prepare cell values with customization info (using getters to apply overrides)
				uint32 nStorageGB = App.f_GetStorageGB();
				CStr InstanceValue = App.m_bOwnsRootVolume ? App.f_GetInstanceType() : CStr{"-"};
				CStr VCPUsValue = App.m_bOwnsRootVolume ? "{}"_f << App.m_VCPUs : CStr{"-"};
				CStr MemoryValue = App.m_bOwnsRootVolume ? "{} GB"_f << App.m_MemoryGB : CStr{"-"};
				CStr StorageValue = fg_FormatStorage(nStorageGB);
				CStr BandwidthValue = nStorageGB > 0 ? "{} MB/s"_f << App.f_GetBandwidthMBps() : CStr{"-"};
				CStr IOPsValue = nStorageGB > 0 ? "{}"_f << App.f_GetIOPs() : CStr{"-"};
				CStr SnapshotValue = nStorageGB > 0 ? (App.f_GetHasSnapshot() ? CStr{"Yes"} : CStr{"No"}) : CStr{"-"};

				TableRenderer.f_AddRow
					(
						App.m_Host
						, App.m_Name
						, fCell(ETableColumn::mc_Instance, InstanceValue, (bool)App.m_InstanceTypeOverride)
						, VCPUsValue
						, MemoryValue
						, fCell(ETableColumn::mc_Storage, StorageValue, (bool)App.m_StorageGBOverride)
						, fCell(ETableColumn::mc_Bandwidth, BandwidthValue, (bool)App.m_BandwidthMBpsOverride)
						, fCell(ETableColumn::mc_IOPs, IOPsValue, (bool)App.m_IOPsOverride)
						, fCell(ETableColumn::mc_Snapshots, SnapshotValue, (bool)App.m_SnapshotOverride)
						, fg_FormatCost(RowCost)
					)
				;

				++nRow;
			}

			// Add separator before other costs
			TableRenderer.f_ForceRowSeparator();

			// Add other costs section header
			TableRenderer.f_AddRow("Other Costs", "", "", "", "", "", "", "", "", "");

			for (auto const &Other : _State.m_Config.m_OtherCosts)
			{
				TableRenderer.f_AddRow
					(
						""
						, Other.m_Description
						, ""
						, ""
						, ""
						, ""
						, ""
						, ""
						, ""
						, fg_FormatCost(Other.m_MonthlyCost)
					)
				;
			}

			// Add separator before total
			TableRenderer.f_ForceRowSeparator();

			// Add total row
			fp64 TotalCost = _State.m_Config.f_CalculateTotalMonthlyCost();
			TableRenderer.f_AddRow
				(
					"{}Total{}"_f << _State.m_AnsiEncoding.f_Bold() << _State.m_AnsiEncoding.f_Default()
					, ""
					, ""
					, ""
					, ""
					, ""
					, ""
					, ""
					, ""
					, "{}{}{}"_f << _State.m_AnsiEncoding.f_Bold() << fg_FormatCost(TotalCost) << _State.m_AnsiEncoding.f_Default()
				)
			;

			TableRenderer.f_Output(CTableRenderHelper::EOutputType_HumanReadable);

			return TableOutput;
		}

		// Render the full UI
		TCFuture<void> fg_RenderUI(TCSharedPointer<CConfigUIState> _pState)
		{
			CStr Output;
			Output += _pState->m_AnsiEncoding.f_SyncronizeOutputStart();

			// Clear previous output
			if (_pState->m_LastRenderedLines > 0)
			{
				Output += _pState->m_AnsiEncoding.f_MovePreviousLine(_pState->m_LastRenderedLines);
				_pState->m_LastRenderedLines = 0;
				Output += _pState->m_AnsiEncoding.f_MoveToColumn(0);
				Output += _pState->m_AnsiEncoding.f_ClearToEndOfScreen();
			}

			Output += _pState->m_AnsiEncoding.f_ShowCursor(false);

			Output += fg_RenderButtons(*_pState);
			Output += fg_RenderTable(*_pState);

			// Instructions - different for button vs table mode
			if (_pState->m_UIMode == EUIMode::mc_Buttons)
			{
				Output += "{}Use arrow keys to navigate, Enter to configure, Tab to edit table cells, Esc to cancel{}\n"_f
					<< _pState->m_AnsiEncoding.f_Foreground256(246)
					<< _pState->m_AnsiEncoding.f_Default()
				;
			}
			else
			{
				Output += "{}Table edit mode: arrow keys to move, Shift+Up/Down to cycle values, Enter to edit cell, Tab to return to buttons, Esc to cancel{}\n"_f
					<< _pState->m_AnsiEncoding.f_Foreground256(246)
					<< _pState->m_AnsiEncoding.f_Default()
				;
			}

			// Count lines for clearing
			mint nLineCount = 0;
			for (ch8 Ch : Output)
			{
				if (Ch == '\n')
					++nLineCount;
			}
			_pState->m_LastRenderedLines = nLineCount;

			Output += _pState->m_AnsiEncoding.f_ShowCursor(true);
			Output += _pState->m_AnsiEncoding.f_SyncronizeOutputFinish();

			co_await _pState->m_pCommandLine->f_StdOut(Output);
			co_return {};
		}

		// Handle table cell editing
		TCFuture<void> fg_HandleCellEdit(TCSharedPointer<CConfigUIState> _pState)
		{
			if (_pState->m_SelectedRow >= _pState->m_Config.m_Applications.f_GetLen())
				co_return {};

			CDeploymentApplication &App = _pState->m_Config.m_Applications[_pState->m_SelectedRow];

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

			switch (_pState->m_SelectedColumn)
			{
			case ETableColumn::mc_Instance:
				{
					if (!App.m_bOwnsRootVolume)
						break;

					// Collect instance types with prices for sorting
					struct CInstanceEntry
					{
						CStr m_InstanceType;
						uint32 m_VCPUs;
						fp64 m_MemoryGB;
						CStr m_Architecture;
						fp64 m_Price;
					};
					TCVector<CInstanceEntry> Entries;

					for (auto const &Entry : _pState->m_Config.m_InstanceInfo.f_Entries())
					{
						CStr const &InstanceType = Entry.f_Key();
						CInstanceInfo const &Info = Entry.f_Value();

						// Skip instances without pricing
						fp64 Price = _pState->m_Config.f_GetEffectiveInstancePrice(InstanceType);
						if (Price <= 0)
							continue;

						CInstanceEntry &NewEntry = Entries.f_Insert();
						NewEntry.m_InstanceType = InstanceType;
						NewEntry.m_VCPUs = Info.m_VCPUs;
						NewEntry.m_MemoryGB = Info.m_MemoryGB;
						NewEntry.m_Architecture = CBootstrapConfig::fs_GetArchitectureDisplayName(Info.m_Architecture);
						NewEntry.m_Price = Price;
					}

					// Sort by price (lowest first)
					Entries.f_Sort([](CInstanceEntry const &_A, CInstanceEntry const &_B)
						{
							return _A.m_Price <=> _B.m_Price;
						}
					);

					// Build list of instance types
					TCVector<TCVector<CStr>> Items;

					// Add "Auto" option to reset to automatic selection
					Items.f_Insert(TCVector<CStr>{"Auto", "-", "-", "-", "Let system choose optimal instance"});

					// Add sorted instance types
					for (auto const &Entry : Entries)
					{
						TCVector<CStr> Row;
						Row.f_Insert(Entry.m_InstanceType);
						Row.f_Insert("{}"_f << Entry.m_VCPUs);
						Row.f_Insert("{} GB"_f << Entry.m_MemoryGB);
						Row.f_Insert(Entry.m_Architecture);
						Row.f_Insert("${fe2}/mo"_f << (Entry.m_Price * gc_HoursPerMonth));
						Items.f_Insert(fg_Move(Row));
					}

					// Default to current instance type (not "Auto")
					CStr Current = App.f_GetInstanceType();

					TCOptional<CStr> Selected = co_await fg_SelectFromListWithFilter
						(
							_pState->m_pCommandLine
							, Items
							, {"Instance Type", "VCPUs", "Memory", "Architecture", "Cost"}
							, "Select Instance Type for {}"_f << App.m_Host
							, Current
						)
					;

					if (!Selected)
						break;

					if (*Selected == "Auto")
						App.m_InstanceTypeOverride.f_Clear();
					else if (!Selected->f_IsEmpty())
						App.m_InstanceTypeOverride = *Selected;

					_pState->m_Config.f_UpdateCosts();
					break;
				}

			case ETableColumn::mc_Storage:
				{
					TCVector<TCVector<CStr>> Items;

					// Add "Auto" option
					Items.f_Insert(TCVector<CStr>{"Auto", "Use default storage for this app type"});

					// Add storage size options
					// Only allow 0 GB for apps that don't own the root volume (can share with AppManager)
					if (!App.m_bOwnsRootVolume)
						Items.f_Insert(TCVector<CStr>{"0 GB", "No dedicated storage (share root volume)"});
					Items.f_Insert(TCVector<CStr>{"5 GB", "Minimal storage"});
					Items.f_Insert(TCVector<CStr>{"10 GB", "Small storage"});
					Items.f_Insert(TCVector<CStr>{"20 GB", "Default root volume size"});
					Items.f_Insert(TCVector<CStr>{"50 GB", "Medium storage"});
					Items.f_Insert(TCVector<CStr>{"100 GB", "Large storage"});
					Items.f_Insert(TCVector<CStr>{"130 GB", "CloudManager default"});
					Items.f_Insert(TCVector<CStr>{"250 GB", "VersionManager default"});
					Items.f_Insert(TCVector<CStr>{"500 GB", "Extra large storage"});
					Items.f_Insert(TCVector<CStr>{"1000 GB", "1 TB storage"});

					CStr Current = "{} GB"_f << App.f_GetStorageGB();

					TCOptional<CStr> Selected = co_await fg_SelectFromListWithFilter
						(
							_pState->m_pCommandLine
							, Items
							, {"Size", "Description"}
							, "Select Storage Size for {}"_f << App.m_Name
							, Current
						)
					;

					if (!Selected)
						break;

					if (*Selected == "Auto")
						App.m_StorageGBOverride.f_Clear();
					else if (!Selected->f_IsEmpty())
					{
						// Parse GB value from selection
						mint nPos = Selected->f_Find(" GB");
						if (nPos >= 0)
							App.m_StorageGBOverride = Selected->f_Left(nPos).f_ToInt(uint32(0));
					}

					_pState->m_Config.f_UpdateCosts();
					break;
				}

			case ETableColumn::mc_Bandwidth:
				{
					TCVector<TCVector<CStr>> Items;

					Items.f_Insert(TCVector<CStr>{"Auto", "Use default bandwidth (125 MB/s)"});
					Items.f_Insert(TCVector<CStr>{"125 MB/s", "Default gp3 bandwidth"});
					Items.f_Insert(TCVector<CStr>{"250 MB/s", "Double default bandwidth"});
					Items.f_Insert(TCVector<CStr>{"500 MB/s", "High bandwidth"});
					Items.f_Insert(TCVector<CStr>{"750 MB/s", "Very high bandwidth"});
					Items.f_Insert(TCVector<CStr>{"1000 MB/s", "Maximum gp3 bandwidth"});

					CStr Current = "{} MB/s"_f << App.f_GetBandwidthMBps();

					TCOptional<CStr> Selected = co_await fg_SelectFromListWithFilter
						(
							_pState->m_pCommandLine
							, Items
							, {"Bandwidth", "Description"}
							, "Select EBS Bandwidth for {}"_f << App.m_Name
							, Current
						)
					;

					if (!Selected)
						break;

					if (*Selected == "Auto")
						App.m_BandwidthMBpsOverride.f_Clear();
					else if (!Selected->f_IsEmpty())
					{
						// Parse MB/s value from selection
						mint nPos = Selected->f_Find(" MB/s");
						if (nPos >= 0)
							App.m_BandwidthMBpsOverride = Selected->f_Left(nPos).f_ToInt(uint32(0));
					}

					_pState->m_Config.f_UpdateCosts();
					break;
				}

			case ETableColumn::mc_IOPs:
				{
					TCVector<TCVector<CStr>> Items;

					Items.f_Insert(TCVector<CStr>{"Auto", "Use default IOPs (3000)"});
					Items.f_Insert(TCVector<CStr>{"3000", "Default gp3 IOPs (included)"});
					Items.f_Insert(TCVector<CStr>{"4000", "Slightly higher IOPs"});
					Items.f_Insert(TCVector<CStr>{"5000", "Medium IOPs"});
					Items.f_Insert(TCVector<CStr>{"6000", "Higher IOPs"});
					Items.f_Insert(TCVector<CStr>{"8000", "High IOPs"});
					Items.f_Insert(TCVector<CStr>{"10000", "Very high IOPs"});
					Items.f_Insert(TCVector<CStr>{"12000", "Premium IOPs"});
					Items.f_Insert(TCVector<CStr>{"16000", "Maximum gp3 IOPs"});

					CStr Current = "{}"_f << App.f_GetIOPs();

					TCOptional<CStr> Selected = co_await fg_SelectFromListWithFilter
						(
							_pState->m_pCommandLine
							, Items
							, {"IOPs", "Description"}
							, "Select EBS IOPs for {}"_f << App.m_Name
							, Current
						)
					;

					if (!Selected)
						break;

					if (*Selected == "Auto")
						App.m_IOPsOverride.f_Clear();
					else if (!Selected->f_IsEmpty())
						App.m_IOPsOverride = Selected->f_ToInt(uint32(0));

					_pState->m_Config.f_UpdateCosts();
					break;
				}

			case ETableColumn::mc_Snapshots:
				{
					TCVector<TCVector<CStr>> Items;

					Items.f_Insert(TCVector<CStr>{"Auto", "Use snapshot level setting"});
					Items.f_Insert(TCVector<CStr>{"No", "Disable snapshots for this volume"});
					Items.f_Insert(TCVector<CStr>{"Yes", "Enable snapshots for this volume"});

					CStr Current = App.f_GetHasSnapshot() ? "Yes" : "No";

					TCOptional<CStr> Selected = co_await fg_SelectFromListWithFilter
						(
							_pState->m_pCommandLine
							, Items
							, {"Snapshots", "Description"}
							, "Enable Snapshots for {}?"_f << App.m_Name
							, Current
						)
					;

					if (!Selected)
						break;

					if (*Selected == "Auto")
						App.m_SnapshotOverride.f_Clear();
					else if (*Selected == "Yes")
						App.m_SnapshotOverride = true;
					else if (*Selected == "No")
						App.m_SnapshotOverride = false;

					_pState->m_Config.f_UpdateCosts();
					break;
				}

			default:
				break;
			}

			// Save config after change
			co_await fg_SaveConfig(_pState->m_RootDirectory, _pState->m_Config);

			_pState->m_bOpeningDialog = false;
			_pState->m_LastRenderedLines = 0;
			_pState->m_bNeedsRedraw = true;

			co_return {};
		}

		// Cycle table cell value without opening dropdown
		TCFuture<void> fg_CycleCellValue(TCSharedPointer<CConfigUIState> _pState, bool _bNext)
		{
			using namespace NStr;

			if (_pState->m_SelectedRow >= _pState->m_Config.m_Applications.f_GetLen())
				co_return {};

			CDeploymentApplication &App = _pState->m_Config.m_Applications[_pState->m_SelectedRow];

			switch (_pState->m_SelectedColumn)
			{
			case ETableColumn::mc_Instance:
				{
					if (!App.m_bOwnsRootVolume)
						break;

					// Calculate host's total memory requirement
					fp64 HostMemoryGB = 0.0;
					for (auto const &OtherApp : _pState->m_Config.m_Applications)
					{
						if (OtherApp.m_Host == App.m_Host)
							HostMemoryGB += fg_GetMemoryRequirement(OtherApp.m_Type);
					}

					// Get filtered and sorted list of instance types
					auto Entries = _pState->m_Config.f_GetMatchingInstances(HostMemoryGB);

					if (Entries.f_IsEmpty())
						break;

					mint nCount = Entries.f_GetLen();

					// Use effective value (getter applies override or default)
					CStr Current = App.f_GetInstanceType();
					smint nCurrent = -1;
					for (mint i = 0; i < nCount; ++i)
					{
						if (Entries[i].m_InstanceType == Current)
						{
							nCurrent = i;
							break;
						}
					}

					if (nCurrent < 0)
						nCurrent = 0;  // Not found, start at first

					if (_bNext)
					{
						if (nCurrent < nCount - 1)
							App.m_InstanceTypeOverride = Entries[nCurrent + 1].m_InstanceType;
						// else: stay at last value (no wrap)
					}
					else
					{
						if (nCurrent > 0)
							App.m_InstanceTypeOverride = Entries[nCurrent - 1].m_InstanceType;
						// else: stay at first value (no wrap, use backspace/delete for Auto)
					}

					_pState->m_Config.f_UpdateCosts();
					break;
				}

			case ETableColumn::mc_Storage:
				{
					// Storage values
					static constexpr uint32 gc_StorageValues[] = {0, 5, 10, 20, 50, 100, 130, 250, 500, 1000};
					mint nCount = sizeof(gc_StorageValues) / sizeof(gc_StorageValues[0]);

					// Skip 0 GB for root volume owners
					mint nFirstValid = App.m_bOwnsRootVolume ? 1 : 0;

					// Use effective value (getter applies override or default)
					uint32 Current = App.f_GetStorageGB();

					// Find current index
					smint nCurrent = -1;
					for (mint i = nFirstValid; i < nCount; ++i)
					{
						if (gc_StorageValues[i] == Current)
						{
							nCurrent = i;
							break;
						}
					}

					if (nCurrent < 0)
						nCurrent = nFirstValid;  // Not found, start at first valid

					if (_bNext)
					{
						if (nCurrent < nCount - 1)
							App.m_StorageGBOverride = gc_StorageValues[nCurrent + 1];
						// else: stay at last value (no wrap)
					}
					else
					{
						if (nCurrent > nFirstValid)
							App.m_StorageGBOverride = gc_StorageValues[nCurrent - 1];
						// else: stay at first value (no wrap, use backspace/delete for Auto)
					}

					_pState->m_Config.f_UpdateCosts();
					break;
				}

			case ETableColumn::mc_Bandwidth:
				{
					// Bandwidth values
					static constexpr uint32 gc_BandwidthValues[] = {125, 250, 500, 750, 1000};
					mint nCount = sizeof(gc_BandwidthValues) / sizeof(gc_BandwidthValues[0]);

					// Use effective value (getter applies override or default)
					uint32 Current = App.f_GetBandwidthMBps();

					// Find current index
					smint nCurrent = -1;
					for (mint i = 0; i < nCount; ++i)
					{
						if (gc_BandwidthValues[i] == Current)
						{
							nCurrent = i;
							break;
						}
					}

					if (nCurrent < 0)
						nCurrent = 0;  // Not found, start at first

					if (_bNext)
					{
						if (nCurrent < nCount - 1)
							App.m_BandwidthMBpsOverride = gc_BandwidthValues[nCurrent + 1];
						// else: stay at last value (no wrap)
					}
					else
					{
						if (nCurrent > 0)
							App.m_BandwidthMBpsOverride = gc_BandwidthValues[nCurrent - 1];
						// else: stay at first value (no wrap, use backspace/delete for Auto)
					}

					_pState->m_Config.f_UpdateCosts();
					break;
				}

			case ETableColumn::mc_IOPs:
				{
					// IOPs values
					static constexpr uint32 gc_IOPsValues[] = {3000, 4000, 5000, 6000, 8000, 10000, 12000, 16000};
					mint nCount = sizeof(gc_IOPsValues) / sizeof(gc_IOPsValues[0]);

					// Use effective value (getter applies override or default)
					uint32 Current = App.f_GetIOPs();

					// Find current index
					smint nCurrent = -1;
					for (mint i = 0; i < nCount; ++i)
					{
						if (gc_IOPsValues[i] == Current)
						{
							nCurrent = i;
							break;
						}
					}

					if (nCurrent < 0)
						nCurrent = 0;  // Not found, start at first

					if (_bNext)
					{
						if (nCurrent < nCount - 1)
							App.m_IOPsOverride = gc_IOPsValues[nCurrent + 1];
						// else: stay at last value (no wrap)
					}
					else
					{
						if (nCurrent > 0)
							App.m_IOPsOverride = gc_IOPsValues[nCurrent - 1];
						// else: stay at first value (no wrap, use backspace/delete for Auto)
					}

					_pState->m_Config.f_UpdateCosts();
					break;
				}

			case ETableColumn::mc_Snapshots:
				{
					// Use effective value (getter applies override or default)
					// Cycling: Yes <-> No (use backspace/delete for Auto)
					bool bCurrent = App.f_GetHasSnapshot();

					if (_bNext)
					{
						// Going down
						if (!bCurrent)
							App.m_SnapshotOverride = true;  // No -> Yes
						// else: already at No, stay
					}
					else
					{
						// Going up
						if (bCurrent)
							App.m_SnapshotOverride = false;  // Yes -> No
						// else: already at Yes, stay (use backspace/delete for Auto)
					}

					_pState->m_Config.f_UpdateCosts();
					break;
				}

			default:
				break;
			}

			// Save config after change
			co_await fg_SaveConfig(_pState->m_RootDirectory, _pState->m_Config);

			co_return {};
		}

		// Reset table cell value to auto (clear override)
		TCFuture<void> fg_ResetCellToAuto(TCSharedPointer<CConfigUIState> _pState)
		{
			if (_pState->m_SelectedRow >= _pState->m_Config.m_Applications.f_GetLen())
				co_return {};

			CDeploymentApplication &App = _pState->m_Config.m_Applications[_pState->m_SelectedRow];

			switch (_pState->m_SelectedColumn)
			{
			case ETableColumn::mc_Instance:
				if (App.m_bOwnsRootVolume)
					App.m_InstanceTypeOverride.f_Clear();
				break;

			case ETableColumn::mc_Storage:
				App.m_StorageGBOverride.f_Clear();
				break;

			case ETableColumn::mc_Bandwidth:
				App.m_BandwidthMBpsOverride.f_Clear();
				break;

			case ETableColumn::mc_IOPs:
				App.m_IOPsOverride.f_Clear();
				break;

			case ETableColumn::mc_Snapshots:
				App.m_SnapshotOverride.f_Clear();
				break;

			default:
				break;
			}

			_pState->m_Config.f_UpdateCosts();

			// Save config after change
			co_await fg_SaveConfig(_pState->m_RootDirectory, _pState->m_Config);

			co_return {};
		}

		// Cycle button value without opening dropdown
		TCFuture<void> fg_CycleButtonValue(TCSharedPointer<CConfigUIState> _pState, bool _bNext)
		{
			using namespace NStr;

			switch (_pState->m_SelectedButton)
			{
			case EButton::mc_Region:
				{
					// Fetch regions if not cached
					if (!_pState->m_pRegionCache->m_Regions)
					{
						TCActor<NWeb::CCurlActor> CurlActor{fg_Construct(), "Curl"};
						auto DestroyCurl = co_await fg_AsyncDestroy(CurlActor);

						TCActor<NWeb::CAwsEc2Actor> Ec2Actor{fg_Construct(CurlActor, _pState->m_Credentials)};
						auto DestroyEc2 = co_await fg_AsyncDestroy(Ec2Actor);

						_pState->m_pRegionCache->m_Regions = co_await Ec2Actor(&NWeb::CAwsEc2Actor::f_DescribeRegions);
					}

					// Build sorted region list
					TCVector<CStr> Regions;
					for (auto const &RegionInfo : *_pState->m_pRegionCache->m_Regions)
						Regions.f_InsertLast(RegionInfo.m_RegionName);
					Regions.f_Sort();

					if (Regions.f_IsEmpty())
						break;

					// Find current index
					mint nCurrent = 0;
					for (mint i = 0; i < Regions.f_GetLen(); ++i)
					{
						if (Regions[i] == _pState->m_Config.m_Region)
						{
							nCurrent = i;
							break;
						}
					}

					// Clamp to boundaries
					mint nCount = Regions.f_GetLen();
					if (_bNext)
					{
						if (nCurrent < nCount - 1)
							++nCurrent;
					}
					else
					{
						if (nCurrent > 0)
							--nCurrent;
					}

					CStr NewRegion = Regions[nCurrent];
					if (NewRegion != _pState->m_Config.m_Region)
					{
						_pState->m_Config.m_Region = NewRegion;

						// Re-fetch pricing for new region
						CAwsPricingData Pricing;
						if (auto pCachedPricing = _pState->m_PricingCache.f_FindEqual(NewRegion))
						{
							Pricing = *pCachedPricing;
						}
						else
						{
							// Clear the current UI before fetching (shows progress message)
							if (_pState->m_LastRenderedLines > 0)
							{
								CStr ClearOutput;
								ClearOutput += _pState->m_AnsiEncoding.f_MovePreviousLine(_pState->m_LastRenderedLines);
								_pState->m_LastRenderedLines = 0;
								ClearOutput += _pState->m_AnsiEncoding.f_MoveToColumn(0);
								ClearOutput += _pState->m_AnsiEncoding.f_ClearToEndOfScreen();
								co_await _pState->m_pCommandLine->f_StdOut(ClearOutput);
							}

							Pricing = _pState->m_PricingCache[NewRegion] = co_await fg_FetchAwsPricing(_pState->m_pCommandLine, _pState->m_Credentials, NewRegion, _pState->m_RootDirectory);
						}

						_pState->m_Config.f_ApplyPricing(fg_Move(Pricing));
						_pState->m_Config.f_RecalculateLayout();
					}
					break;
				}

			case EButton::mc_Isolation:
				{
					mint nCurrent = (mint)_pState->m_Config.m_IsolationLevel;
					if (_bNext)
					{
						if (nCurrent < 2)
							++nCurrent;
					}
					else
					{
						if (nCurrent > 0)
							--nCurrent;
					}
					_pState->m_Config.m_IsolationLevel = (EIsolationLevel)nCurrent;
					_pState->m_Config.f_RecalculateLayout();
					break;
				}

			case EButton::mc_StorageIsolation:
				{
					mint nCurrent = (mint)_pState->m_Config.m_StorageIsolation;
					if (_bNext)
					{
						if (nCurrent < 2)
							++nCurrent;
					}
					else
					{
						if (nCurrent > 0)
							--nCurrent;
					}
					_pState->m_Config.m_StorageIsolation = (EStorageIsolation)nCurrent;
					_pState->m_Config.f_RecalculateLayout();
					break;
				}

			case EButton::mc_Encryption:
				{
					_pState->m_Config.m_bStorageEncryption = !_pState->m_Config.m_bStorageEncryption;
					_pState->m_Config.f_RecalculateLayout();
					break;
				}

			case EButton::mc_Snapshots:
				{
					mint nCurrent = (mint)_pState->m_Config.m_SnapshotLevel;
					if (_bNext)
					{
						if (nCurrent < 2)
							++nCurrent;
					}
					else
					{
						if (nCurrent > 0)
							--nCurrent;
					}
					_pState->m_Config.m_SnapshotLevel = (ESnapshotLevel)nCurrent;
					_pState->m_Config.f_RecalculateLayout();
					break;
				}

			case EButton::mc_NAT:
				{
					mint nCurrent = (mint)_pState->m_Config.m_NATConfiguration;
					if (_bNext)
					{
						if (nCurrent < 3)
							++nCurrent;
					}
					else
					{
						if (nCurrent > 0)
							--nCurrent;
					}
					_pState->m_Config.m_NATConfiguration = (ENATConfiguration)nCurrent;
					_pState->m_Config.f_RecalculateLayout();
					break;
				}

			case EButton::mc_KeyManagers:
				{
					uint32 nCurrent = _pState->m_Config.m_KeyManagerCount;
					if (_bNext)
					{
						if (nCurrent < 3)
							++nCurrent;
					}
					else
					{
						if (nCurrent > 1)
							--nCurrent;
					}
					_pState->m_Config.m_KeyManagerCount = nCurrent;
					_pState->m_Config.f_RecalculateLayout();
					break;
				}

			case EButton::mc_CPUType:
				{
					mint nCurrent = (mint)_pState->m_Config.m_CPUType;
					if (_bNext)
					{
						if (nCurrent < 5)
							++nCurrent;
					}
					else
					{
						if (nCurrent > 0)
							--nCurrent;
					}
					_pState->m_Config.m_CPUType = (ECPUType)nCurrent;
					_pState->m_Config.f_RecalculateLayout();
					break;
				}

			case EButton::mc_Pricing:
				{
					// Pricing has 13 options: On-Demand + 12 reserved combinations
					// Index 0: On-Demand
					// Index 1-6: 1yr (Standard NoUpfront, Partial, AllUpfront, Convertible NoUpfront, Partial, AllUpfront)
					// Index 7-12: 3yr (same pattern)

					// Convert current selection to flat index
					mint nCurrent = 0;
					if (_pState->m_Config.m_Pricing.f_IsOnDemand())
					{
						nCurrent = 0;
					}
					else
					{
						mint nTermOffset = (_pState->m_Config.m_Pricing.m_Term == EReservedTerm::mc_OneYear) ? 0 : 6;
						mint nClassOffset = (_pState->m_Config.m_Pricing.m_OfferingClass == EOfferingClass::mc_Standard) ? 0 : 3;
						mint nPaymentOffset = (mint)_pState->m_Config.m_Pricing.m_Payment;
						nCurrent = 1 + nTermOffset + nClassOffset + nPaymentOffset;
					}

					// Clamp to boundaries
					if (_bNext)
					{
						if (nCurrent < 12)
							++nCurrent;
					}
					else
					{
						if (nCurrent > 0)
							--nCurrent;
					}

					// Convert back to CPricingSelection
					if (nCurrent == 0)
					{
						_pState->m_Config.m_Pricing.m_Term = EReservedTerm::mc_OnDemand;
					}
					else
					{
						mint nIdx = nCurrent - 1;  // 0-11
						_pState->m_Config.m_Pricing.m_Term = (nIdx < 6) ? EReservedTerm::mc_OneYear : EReservedTerm::mc_ThreeYear;
						mint nWithinTerm = nIdx % 6;  // 0-5
						_pState->m_Config.m_Pricing.m_OfferingClass = (nWithinTerm < 3) ? EOfferingClass::mc_Standard : EOfferingClass::mc_Convertible;
						_pState->m_Config.m_Pricing.m_Payment = (EPaymentOption)(nWithinTerm % 3);
					}

					_pState->m_Config.f_RecalculateLayout();
					break;
				}

			case EButton::mc_Cancel:
			case EButton::mc_Confirm:
			default:
				// No cycling for action buttons
				co_return {};
			}

			// Save config after change
			co_await fg_SaveConfig(_pState->m_RootDirectory, _pState->m_Config);

			co_return {};
		}

		// Handle button selection
		TCFuture<void> fg_HandleButtonPress(TCSharedPointer<CConfigUIState> _pState)
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

			switch (_pState->m_SelectedButton)
			{
			case EButton::mc_Region:
				{
					TCOptional<CStr> NewRegion = co_await fg_SelectAwsRegion(_pState->m_pCommandLine, _pState->m_Credentials, _pState->m_Config.m_Region, _pState->m_pRegionCache);

					if (!NewRegion)
						break;

					if (*NewRegion != _pState->m_Config.m_Region)
					{
						_pState->m_Config.m_Region = *NewRegion;

						// Refetch pricing for the new region
						CAwsPricingData Pricing;
						if (auto pCachedPricing = _pState->m_PricingCache.f_FindEqual(*NewRegion))
							Pricing = *pCachedPricing;
						else
						{
							Pricing = _pState->m_PricingCache[*NewRegion] =
								co_await fg_FetchAwsPricing(_pState->m_pCommandLine, _pState->m_Credentials, *NewRegion, _pState->m_RootDirectory)
							;
						}

						_pState->m_Config.f_ApplyPricing(fg_Move(Pricing));
						_pState->m_Config.f_RecalculateLayout();
					}
					break;
				}

			case EButton::mc_Isolation:
				{
					TCVector<TCVector<CStr>> Items =
						{
							{"Minimal", "All core apps on one host - lowest cost"}
							, {"Moderate", "KeyManagers isolated, management consolidated"}
							, {"Maximum", "One app per host - strongest security"}
						}
					;

					CStr Current = CBootstrapConfig::fs_GetIsolationLevelDisplayName(_pState->m_Config.m_IsolationLevel);
					TCOptional<CStr> Selected = co_await fg_SelectFromListWithFilter
						(
							_pState->m_pCommandLine
							, Items
							, {"Level", "Description"}
							, "Select Isolation Level"
							, Current
						)
					;

					if (!Selected)
						break;

					if (*Selected == "Maximum")
						_pState->m_Config.m_IsolationLevel = EIsolationLevel::mc_Maximum;
					else if (*Selected == "Moderate")
						_pState->m_Config.m_IsolationLevel = EIsolationLevel::mc_Moderate;
					else if (*Selected == "Minimal")
						_pState->m_Config.m_IsolationLevel = EIsolationLevel::mc_Minimal;

					_pState->m_Config.f_RecalculateLayout();
					break;
				}

			case EButton::mc_StorageIsolation:
				{
					TCVector<TCVector<CStr>> Items =
						{
							{"Shared", "Apps share host's root volume"}
							, {"Separate", "Management apps get separate volumes, KeyManagers on root"}
							, {"Separate (All)", "All apps including KeyManagers get separate volumes"}
						}
					;

					CStr Current = CBootstrapConfig::fs_GetStorageIsolationDisplayName(_pState->m_Config.m_StorageIsolation);
					TCOptional<CStr> Selected = co_await fg_SelectFromListWithFilter
						(
							_pState->m_pCommandLine
							, Items
							, {"Level", "Description"}
							, "Select Storage Isolation"
							, Current
						)
					;

					if (!Selected)
						break;

					if (*Selected == "Shared")
						_pState->m_Config.m_StorageIsolation = EStorageIsolation::mc_Shared;
					else if (*Selected == "Separate")
						_pState->m_Config.m_StorageIsolation = EStorageIsolation::mc_Separate;
					else if (*Selected == "Separate (All)")
						_pState->m_Config.m_StorageIsolation = EStorageIsolation::mc_SeparateAll;

					_pState->m_Config.f_RecalculateLayout();
					break;
				}

			case EButton::mc_Encryption:
				{
					TCVector<TCVector<CStr>> Items =
						{
							{"Yes", "Enable storage encryption (requires KeyManager)"}
							, {"No", "Disable storage encryption"}
						}
					;

					CStr Current = _pState->m_Config.m_bStorageEncryption ? "Yes" : "No";
					TCOptional<CStr> Selected = co_await fg_SelectFromListWithFilter
						(
							_pState->m_pCommandLine
							, Items
							, {"Enabled", "Description"}
							, "Enable Storage Encryption?"
							, Current
						)
					;

					if (!Selected)
						break;

					_pState->m_Config.m_bStorageEncryption = (*Selected == "Yes");
					_pState->m_Config.f_RecalculateLayout();
					break;
				}

			case EButton::mc_Snapshots:
				{
					TCVector<TCVector<CStr>> Items =
						{
							{"None", "No EBS snapshots"}
							, {"Critical", "Only KeyManager and SecretsManager volumes"}
							, {"All", "Snapshot all volumes"}
						}
					;

					CStr Current = CBootstrapConfig::fs_GetSnapshotLevelDisplayName(_pState->m_Config.m_SnapshotLevel);
					TCOptional<CStr> Selected = co_await fg_SelectFromListWithFilter
						(
							_pState->m_pCommandLine
							, Items
							, {"Level", "Description"}
							, "Select Snapshot Level"
							, Current
						)
					;

					if (!Selected)
						break;

					if (*Selected == "None")
						_pState->m_Config.m_SnapshotLevel = ESnapshotLevel::mc_None;
					else if (*Selected == "Critical")
						_pState->m_Config.m_SnapshotLevel = ESnapshotLevel::mc_Critical;
					else if (*Selected == "All")
						_pState->m_Config.m_SnapshotLevel = ESnapshotLevel::mc_All;

					_pState->m_Config.f_RecalculateLayout();
					break;
				}

			case EButton::mc_NAT:
				{
					TCVector<TCVector<CStr>> Items =
						{
							{"None", "No NAT - all hosts get public IPs directly"}
							, {"Single", "Single NAT Gateway in one AZ (lowest cost with NAT)"}
							, {"Regional", "Regional NAT Gateway spanning all AZs (single IP)"}
							, {"Per AZ", "One NAT Gateway per AZ (highest availability)"}
						}
					;

					CStr Current = CBootstrapConfig::fs_GetNATConfigurationDisplayName(_pState->m_Config.m_NATConfiguration);
					TCOptional<CStr> Selected = co_await fg_SelectFromListWithFilter
						(
							_pState->m_pCommandLine
							, Items
							, {"Mode", "Description"}
							, "Select NAT Configuration"
							, Current
						)
					;

					if (!Selected)
						break;

					if (*Selected == "None")
						_pState->m_Config.m_NATConfiguration = ENATConfiguration::mc_None;
					else if (*Selected == "Single")
						_pState->m_Config.m_NATConfiguration = ENATConfiguration::mc_Single;
					else if (*Selected == "Regional")
						_pState->m_Config.m_NATConfiguration = ENATConfiguration::mc_Regional;
					else if (*Selected == "Per AZ")
						_pState->m_Config.m_NATConfiguration = ENATConfiguration::mc_PerAZ;

					_pState->m_Config.f_RecalculateLayout();
					break;
				}

			case EButton::mc_KeyManagers:
				{
					TCVector<TCVector<CStr>> Items =
						{
							{"1", "Single KeyManager (no redundancy)"}
							, {"2", "Two KeyManagers (recommended)"}
							, {"3", "Three KeyManagers (maximum redundancy)"}
						}
					;

					CStr Current = "{}"_f << _pState->m_Config.m_KeyManagerCount;
					TCOptional<CStr> Selected = co_await fg_SelectFromListWithFilter
						(
							_pState->m_pCommandLine
							, Items
							, {"Count", "Description"}
							, "Select KeyManager Redundancy"
							, Current
						)
					;

					if (!Selected)
						break;

					if (*Selected == "1")
						_pState->m_Config.m_KeyManagerCount = 1;
					else if (*Selected == "2")
						_pState->m_Config.m_KeyManagerCount = 2;
					else if (*Selected == "3")
						_pState->m_Config.m_KeyManagerCount = 3;

					_pState->m_Config.f_RecalculateLayout();
					break;
				}

			case EButton::mc_CPUType:
				{
					TCVector<TCVector<CStr>> Items =
						{
							{"Any", "Auto-select cheapest (any generation)"}
							, {"Any (Current Gen)", "Auto-select cheapest (current generation only)"}
							, {"x64", "Intel/AMD processors (any generation)"}
							, {"x64 (Current Gen)", "Intel/AMD processors (current generation only)"}
							, {"ARM64", "AWS Graviton processors (any generation)"}
							, {"ARM64 (Current Gen)", "AWS Graviton processors (current generation only)"}
						}
					;

					CStr Current = CBootstrapConfig::fs_GetCPUTypeDisplayName(_pState->m_Config.m_CPUType);
					TCOptional<CStr> Selected = co_await fg_SelectFromListWithFilter
						(
							_pState->m_pCommandLine
							, Items
							, {"Architecture", "Description"}
							, "Select CPU Architecture"
							, Current
						)
					;

					if (!Selected)
						break;

					if (*Selected == "Any")
						_pState->m_Config.m_CPUType = ECPUType::mc_Any;
					else if (*Selected == "Any (Current Gen)")
						_pState->m_Config.m_CPUType = ECPUType::mc_AnyCurrentGen;
					else if (*Selected == "x64")
						_pState->m_Config.m_CPUType = ECPUType::mc_X64;
					else if (*Selected == "x64 (Current Gen)")
						_pState->m_Config.m_CPUType = ECPUType::mc_X64CurrentGen;
					else if (*Selected == "ARM64")
						_pState->m_Config.m_CPUType = ECPUType::mc_Arm64;
					else if (*Selected == "ARM64 (Current Gen)")
						_pState->m_Config.m_CPUType = ECPUType::mc_Arm64CurrentGen;

					_pState->m_Config.f_RecalculateLayout();
					break;
				}

			case EButton::mc_Pricing:
				{
					TCVector<TCVector<CStr>> Items =
						{
							{"On-Demand", "Pay as you go, no commitment"}
							, {"1yr Standard NoUpfront", "~30% savings, instance family locked"}
							, {"1yr Standard Partial", "~35% savings, instance family locked"}
							, {"1yr Standard AllUpfront", "~40% savings, instance family locked"}
							, {"1yr Convertible NoUpfront", "~25% savings, can change instance type"}
							, {"1yr Convertible Partial", "~30% savings, can change instance type"}
							, {"1yr Convertible AllUpfront", "~35% savings, can change instance type"}
							, {"3yr Standard NoUpfront", "~50% savings, instance family locked"}
							, {"3yr Standard Partial", "~55% savings, instance family locked"}
							, {"3yr Standard AllUpfront", "~60% savings, instance family locked"}
							, {"3yr Convertible NoUpfront", "~45% savings, can change instance type"}
							, {"3yr Convertible Partial", "~50% savings, can change instance type"}
							, {"3yr Convertible AllUpfront", "~55% savings, can change instance type"}
						}
					;

					CStr Current = CPricingSelection::fs_GetDisplayName(_pState->m_Config.m_Pricing);
					TCOptional<CStr> Selected = co_await fg_SelectFromListWithFilter
						(
							_pState->m_pCommandLine
							, Items
							, {"Pricing Model", "Description"}
							, "Select Pricing Model"
							, Current
						)
					;

					if (!Selected)
						break;

					// Parse the selection
					CPricingSelection NewPricing;
					if (*Selected == "On-Demand")
					{
						NewPricing.m_Term = EReservedTerm::mc_OnDemand;
					}
					else
					{
						// Parse format: "1yr Standard NoUpfront" or "3yr Convertible Partial"
						if (Selected->f_StartsWith("1yr"))
							NewPricing.m_Term = EReservedTerm::mc_OneYear;
						else if (Selected->f_StartsWith("3yr"))
							NewPricing.m_Term = EReservedTerm::mc_ThreeYear;

						if (Selected->f_Find("Standard") >= 0)
							NewPricing.m_OfferingClass = EOfferingClass::mc_Standard;
						else if (Selected->f_Find("Convertible") >= 0)
							NewPricing.m_OfferingClass = EOfferingClass::mc_Convertible;

						if (Selected->f_Find("NoUpfront") >= 0)
							NewPricing.m_Payment = EPaymentOption::mc_NoUpfront;
						else if (Selected->f_Find("Partial") >= 0)
							NewPricing.m_Payment = EPaymentOption::mc_PartialUpfront;
						else if (Selected->f_Find("AllUpfront") >= 0)
							NewPricing.m_Payment = EPaymentOption::mc_AllUpfront;
					}

					_pState->m_Config.m_Pricing = NewPricing;
					_pState->m_Config.f_RecalculateLayout();
					break;
				}

			case EButton::mc_Cancel:
				{
					_pState->m_bDone = true;
					_pState->m_bOpeningDialog = false;
					_pState->m_LastRenderedLines = 0;
					_pState->m_bNeedsRedraw = true;
					co_return {};
				}

			case EButton::mc_Confirm:
				{
					_pState->m_bConfirmed = true;
					_pState->m_bDone = true;
					_pState->m_bOpeningDialog = false;
					_pState->m_LastRenderedLines = 0;
					_pState->m_bNeedsRedraw = true;
					co_return {};
				}

				default:
					break;
			}

			// Save config after change (only for non-action buttons that reach here)
			co_await fg_SaveConfig(_pState->m_RootDirectory, _pState->m_Config);

			_pState->m_bOpeningDialog = false;
			_pState->m_LastRenderedLines = 0;
			_pState->m_bNeedsRedraw = true;

			co_return {};
		}

		// Load cached pricing data from disk (async)
		TCFuture<TCOptional<CAwsPricingData>> fg_LoadCachedPricing(CStr _RootDirectory, CStr _Region)
		{
			if (_RootDirectory.f_IsEmpty())
				co_return {};

			CStr CacheDir = CFile::fs_AppendPath(_RootDirectory, "AWSPricingCache");
			CStr CacheFile = CFile::fs_AppendPath(CacheDir, "{}.bin"_f << _Region);

			auto BlockingActorCheckout = fg_BlockingActor();
			auto Result = co_await
				(
					g_Dispatch(BlockingActorCheckout) / [CacheFile]() -> TCOptional<CAwsPricingData>
					{
						if (!CFile::fs_FileExists(CacheFile))
							return {};

						try
						{
							TCBinaryStreamFile<> Stream;
							Stream.f_Open(CacheFile, EFileOpen_Read | EFileOpen_ShareAll);

							CAwsPricingDataCached Cached;
							Stream >> Cached;

							// Check if cache is less than 24 hours old
							auto Now = NTime::CTime::fs_NowUTC();
							if (CTimeSpanConvert(Now - Cached.m_CacheTime).f_GetHoursFloat() < 24.0)
								return Cached.m_Pricing;
						}
						catch (...)
						{
							// Ignore cache read errors
						}

						return {};
					}
				)
			;

			co_return Result;
		}

		// Save pricing data to cache on disk (async)
		TCFuture<void> fg_SaveCachedPricing(CStr _RootDirectory, CStr _Region, CAwsPricingData _Pricing)
		{
			if (_RootDirectory.f_IsEmpty())
				co_return {};

			CStr CacheDir = CFile::fs_AppendPath(_RootDirectory, "AWSPricingCache");
			CStr CacheFile = CFile::fs_AppendPath(CacheDir, "{}.bin"_f << _Region);

			auto BlockingActorCheckout = fg_BlockingActor();
			co_await
				(
					g_Dispatch(BlockingActorCheckout) / [CacheFile, CacheDir, Pricing = fg_Move(_Pricing)]()
					{
						CFile::fs_CreateDirectory(CacheDir);

						try
						{
							TCBinaryStreamFile<> Stream;
							Stream.f_Open(CacheFile, EFileOpen_Write | EFileOpen_ShareAll);

							CAwsPricingDataCached Cached;
							Cached.m_CacheTime = NTime::CTime::fs_NowUTC();
							Cached.m_Pricing = Pricing;
							Stream << Cached;
						}
						catch (...)
						{
							// Ignore cache write errors
						}
					}
				)
			;

			co_return {};
		}

		// Load config from disk (async)
		TCFuture<TCOptional<CBootstrapConfig>> fg_LoadConfig(CStr _RootDirectory)
		{
			if (_RootDirectory.f_IsEmpty())
				co_return {};

			CStr ConfigFile = CFile::fs_AppendPath(_RootDirectory, "config.bin");

			auto BlockingActorCheckout = fg_BlockingActor();
			auto Result = co_await
				(
					g_Dispatch(BlockingActorCheckout) / [ConfigFile]() -> TCOptional<CBootstrapConfig>
					{
						if (!CFile::fs_FileExists(ConfigFile))
							return {};

						TCBinaryStreamFile<> Stream;
						Stream.f_Open(ConfigFile, EFileOpen_Read | EFileOpen_ShareAll);

						CBootstrapConfig Config;
						Stream >> Config;
						return Config;
					}
				)
			;

			co_return fg_Move(Result);
		}

		// Save config to disk (async)
		TCFuture<void> fg_SaveConfig(CStr _RootDirectory, CBootstrapConfig _Config)
		{
			if (_RootDirectory.f_IsEmpty())
				co_return {};

			CStr ConfigFile = CFile::fs_AppendPath(_RootDirectory, "config.bin");

			auto BlockingActorCheckout = fg_BlockingActor();
			co_await
				(
					g_Dispatch(BlockingActorCheckout) / [ConfigFile, RootDirectory = _RootDirectory, Config = _Config]()
					{
						CFile::fs_CreateDirectory(RootDirectory);

						TCBinaryStreamFile<> Stream;
						Stream.f_Open(ConfigFile, EFileOpen_Write | EFileOpen_ShareAll);
						Stream << Config;
					}
				)
			;

			co_return {};
		}

		TCFuture<void> fg_HandleInput(CStrIO _Input, TCSharedPointer<CConfigUIState> _pState, TCSharedPointer<TCPromise<CBootstrapConfig>> _pPromise)
		{
			CStr Input = _Input;

			// Handle escape first - works in both modes
			if (Input == gc_Escape)
			{
				// Escape - cancel
				_pState->m_bDone = true;
				if (!_pPromise->f_IsSet())
					_pPromise->f_SetException(DMibErrorInstance("Configuration cancelled"));
				co_return {};
			}

			// Handle Ctrl+C - terminate application
			if (Input == "\x03")
			{
				_pState->m_bDone = true;
				if (!_pPromise->f_IsSet())
					_pPromise->f_SetException(DMibErrorInstance("Configuration cancelled"));
				co_return {};
			}

			// Handle input based on current UI mode
			if (_pState->m_UIMode == EUIMode::mc_Buttons)
			{
				// Button mode navigation
				if (Input == gc_LeftArrow || Input == "\x1b[D")
				{
					// Move to previous button
					mint nButton = (mint)_pState->m_SelectedButton;
					if (nButton > 0)
						_pState->m_SelectedButton = (EButton)(nButton - 1);
					else
						_pState->m_SelectedButton = (EButton)((mint)EButton::mc_Count - 1);
					_pState->m_bNeedsRedraw = true;
				}
				else if (Input == gc_RightArrow || Input == "\x1b[C")
				{
					// Move to next button
					mint nButton = (mint)_pState->m_SelectedButton;
					if (nButton < (mint)EButton::mc_Count - 1)
						_pState->m_SelectedButton = (EButton)(nButton + 1);
					else
						_pState->m_SelectedButton = (EButton)0;
					_pState->m_bNeedsRedraw = true;
				}
				else if (Input == gc_Tab)
				{
					// Tab switches to table cell mode
					if (!_pState->m_Config.m_Applications.f_IsEmpty())
					{
						_pState->m_UIMode = EUIMode::mc_TableCells;
						_pState->m_SelectedRow = 0;
						_pState->m_SelectedColumn = _pState->f_GetFirstEditableColumn(0);
						_pState->m_bNeedsRedraw = true;
					}
				}
				else if (Input == gc_UpArrow)
				{
					// Move up in the button grid (3x3 layout)
					// Row 0: Region(col 0), Isolation(col 1), Storage(col 2)
					// Row 1: Encryption(col 0), Snapshots(col 1), NAT(col 2)
					// Row 2: KeyManagers(col 0), CPUType(col 1), Pricing(col 2)
					// Row 3: Cancel(col 1), Confirm(col 2)
					switch (_pState->m_SelectedButton)
					{
					case EButton::mc_Encryption:
						_pState->m_SelectedButton = EButton::mc_Region;
						break;
					case EButton::mc_Snapshots:
						_pState->m_SelectedButton = EButton::mc_Isolation;
						break;
					case EButton::mc_NAT:
						_pState->m_SelectedButton = EButton::mc_StorageIsolation;
						break;
					case EButton::mc_KeyManagers:
						_pState->m_SelectedButton = EButton::mc_Encryption;
						break;
					case EButton::mc_CPUType:
						_pState->m_SelectedButton = EButton::mc_Snapshots;
						break;
					case EButton::mc_Pricing:
						_pState->m_SelectedButton = EButton::mc_NAT;
						break;
					case EButton::mc_Cancel:
						_pState->m_SelectedButton = EButton::mc_CPUType;
						break;
					case EButton::mc_Confirm:
						_pState->m_SelectedButton = EButton::mc_Pricing;
						break;
					default:
						break;  // Already on top row
					}
					_pState->m_bNeedsRedraw = true;
				}
				else if (Input == gc_DownArrow)
				{
					// Move down in the button grid (3x3 layout)
					switch (_pState->m_SelectedButton)
					{
					case EButton::mc_Region:
						_pState->m_SelectedButton = EButton::mc_Encryption;
						break;
					case EButton::mc_Isolation:
						_pState->m_SelectedButton = EButton::mc_Snapshots;
						break;
					case EButton::mc_StorageIsolation:
						_pState->m_SelectedButton = EButton::mc_NAT;
						break;
					case EButton::mc_Encryption:
						_pState->m_SelectedButton = EButton::mc_KeyManagers;
						break;
					case EButton::mc_Snapshots:
						_pState->m_SelectedButton = EButton::mc_CPUType;
						break;
					case EButton::mc_NAT:
						_pState->m_SelectedButton = EButton::mc_Pricing;
						break;
					case EButton::mc_KeyManagers:
					case EButton::mc_CPUType:
						_pState->m_SelectedButton = EButton::mc_Cancel;
						break;
					case EButton::mc_Pricing:
						_pState->m_SelectedButton = EButton::mc_Confirm;
						break;
					case EButton::mc_Cancel:
					case EButton::mc_Confirm:
						// At bottom of buttons - move to table
						if (!_pState->m_Config.m_Applications.f_IsEmpty())
						{
							_pState->m_UIMode = EUIMode::mc_TableCells;
							_pState->m_SelectedRow = 0;
							_pState->m_SelectedColumn = _pState->f_GetFirstEditableColumn(0);
						}
						break;
					default:
						break;
					}
					_pState->m_bNeedsRedraw = true;
				}
				else if (Input == gc_ShiftUpArrow)
				{
					// Shift+Up: cycle to previous value without opening dropdown
					co_await fg_CycleButtonValue(_pState, false);
					_pState->m_bNeedsRedraw = true;
				}
				else if (Input == gc_ShiftDownArrow)
				{
					// Shift+Down: cycle to next value without opening dropdown
					co_await fg_CycleButtonValue(_pState, true);
					_pState->m_bNeedsRedraw = true;
				}
				else if (Input == "\n" || Input == "\r")
				{
					// Enter pressed - handle button
					co_await fg_HandleButtonPress(_pState);

					if (_pState->m_bDone)
					{
						if (!_pPromise->f_IsSet())
						{
							if (_pState->m_bConfirmed)
								_pPromise->f_SetResult(_pState->m_Config);
							else
								_pPromise->f_SetException(DMibErrorInstance("Configuration cancelled"));
						}
						co_return {};
					}
				}
			}
			else
			{
				// Table cell mode navigation
				if (Input == gc_Tab)
				{
					// Tab returns to button mode
					_pState->m_UIMode = EUIMode::mc_Buttons;
					_pState->m_bNeedsRedraw = true;
				}
				else if (Input == gc_LeftArrow || Input == "\x1b[D")
				{
					// Move to previous editable column
					_pState->m_SelectedColumn = _pState->f_GetNextEditableColumn(false);
					_pState->m_bNeedsRedraw = true;
				}
				else if (Input == gc_RightArrow || Input == "\x1b[C")
				{
					// Move to next editable column
					_pState->m_SelectedColumn = _pState->f_GetNextEditableColumn(true);
					_pState->m_bNeedsRedraw = true;
				}
				else if (Input == gc_UpArrow)
				{
					// Move up a row
					if (_pState->m_SelectedRow > 0)
					{
						--_pState->m_SelectedRow;
						// Make sure the current column is valid for the new row
						if (!CConfigUIState::fs_IsColumnEditable(_pState->m_SelectedColumn, _pState->m_Config.m_Applications[_pState->m_SelectedRow]))
							_pState->m_SelectedColumn = _pState->f_GetFirstEditableColumn(_pState->m_SelectedRow);
					}
					else
					{
						// At top of table - move back to buttons (Cancel button)
						_pState->m_UIMode = EUIMode::mc_Buttons;
						_pState->m_SelectedButton = EButton::mc_Confirm;
					}
					_pState->m_bNeedsRedraw = true;
				}
				else if (Input == gc_DownArrow)
				{
					// Move down a row
					if (_pState->m_SelectedRow < _pState->m_Config.m_Applications.f_GetLen() - 1)
					{
						++_pState->m_SelectedRow;
						// Make sure the current column is valid for the new row
						if (!CConfigUIState::fs_IsColumnEditable(_pState->m_SelectedColumn, _pState->m_Config.m_Applications[_pState->m_SelectedRow]))
							_pState->m_SelectedColumn = _pState->f_GetFirstEditableColumn(_pState->m_SelectedRow);
						_pState->m_bNeedsRedraw = true;
					}
				}
				else if (Input == gc_ShiftUpArrow)
				{
					// Shift+Up: cycle to previous value without opening dropdown
					co_await fg_CycleCellValue(_pState, false);
					_pState->m_bNeedsRedraw = true;
				}
				else if (Input == gc_ShiftDownArrow)
				{
					// Shift+Down: cycle to next value without opening dropdown
					co_await fg_CycleCellValue(_pState, true);
					_pState->m_bNeedsRedraw = true;
				}
				else if (Input == "\n" || Input == "\r")
				{
					// Enter pressed - edit the cell
					co_await fg_HandleCellEdit(_pState);
				}
				else if (Input == gc_Backspace || Input == gc_Delete)
				{
					// Backspace/Delete: reset cell to Auto value
					co_await fg_ResetCellToAuto(_pState);
					_pState->m_bNeedsRedraw = true;
				}
			}

			if (_pState->m_bNeedsRedraw && !_pState->m_bOpeningDialog)
			{
				_pState->m_bNeedsRedraw = false;
				co_await fg_RenderUI(_pState);
			}

			co_return {};
		}

	}

	TCFuture<CAwsPricingData> fg_FetchAwsPricing
		(
			TCSharedPointer<CCommandLineControl> _pCommandLine
			, CAwsCredentials _Credentials
			, CStr _Region
			, CStr _RootDirectory
		)
	{
		// Try to load from cache first
		if (auto CachedPricing = co_await fg_LoadCachedPricing(_RootDirectory, _Region))
		{
			*_pCommandLine += "Using cached AWS pricing information.\n";
			co_return *CachedPricing;
		}

		*_pCommandLine += "Fetching AWS pricing information...\n";

		CAwsPricingData Result;

		TCActor<CCurlActor> CurlActor{fg_Construct(), "Curl"};
		auto DestroyCurl = co_await fg_AsyncDestroy(CurlActor);

		TCActor<CAwsPricingActor> PricingActor{fg_Construct(CurlActor, _Credentials)};
		auto DestroyPricing = co_await fg_AsyncDestroy(PricingActor);

		// Fetch EC2 prices for all instance types
		auto EC2Prices = co_await PricingActor(&CAwsPricingActor::f_GetEC2Prices, _Region);
		for (auto const &Price : EC2Prices)
		{
			Result.m_InstancePrices[Price.m_InstanceType] = Price.m_OnDemandPricePerHour;

			// Copy reserved prices with instance type prefix
			for (auto const &Reserved : Price.m_ReservedPrices.f_Entries())
			{
				CStr Key = "{}|{}"_f << Price.m_InstanceType << Reserved.f_Key();
				Result.m_ReservedPrices[Key] = Reserved.f_Value();
			}

			// Store instance metadata (vCPUs, memory, architecture)
			CInstanceInfo &Info = Result.m_InstanceInfo[Price.m_InstanceType];

			// Parse vCPU string (e.g., "48" -> 48)
			Info.m_VCPUs = Price.m_Vcpu.f_ToInt(uint32(0));

			// Parse memory string (e.g., "192 GiB" -> 192.0)
			Info.m_MemoryGB = Price.m_Memory.f_ToFloat(fp64(0));

			// Detect architecture from PhysicalProcessor
			// ARM64: "AWS Graviton", "AWS Graviton2", "AWS Graviton3"
			// x64: "Intel...", "AMD..."
			if (Price.m_PhysicalProcessor.f_Find("Graviton") >= 0)
				Info.m_Architecture = ECPUArchitecture::mc_Arm64;
			else if (Price.m_PhysicalProcessor.f_Find("Intel") >= 0 || Price.m_PhysicalProcessor.f_Find("AMD") >= 0)
				Info.m_Architecture = ECPUArchitecture::mc_X64;
			else
				Info.m_Architecture = ECPUArchitecture::mc_Unknown;

			// Check if current generation
			Info.m_bCurrentGeneration = (Price.m_CurrentGeneration == "Yes");
		}

		// Fetch EBS prices
		auto EBSPrices = co_await PricingActor(&CAwsPricingActor::f_GetEBSPrices, _Region);
		Result.m_EBSPricePerGBMonth = EBSPrices.m_GP3PricePerGBMonth;
		Result.m_GP3IOPSPricePerMonth = EBSPrices.m_GP3IOPSPricePerMonth;
		Result.m_GP3ThroughputPricePerMonth = EBSPrices.m_GP3ThroughputPricePerMonth;
		Result.m_SnapshotPricePerGBMonth = EBSPrices.m_SnapshotPricePerGBMonth;

		// Fetch network prices
		auto NetworkPrices = co_await PricingActor(&CAwsPricingActor::f_GetNetworkPrices, _Region);
		Result.m_NATGatewayPricePerHour = NetworkPrices.m_NATGatewayPricePerHour;
		Result.m_RegionalNATGatewayPricePerHour = NetworkPrices.m_RegionalNATGatewayPricePerHour;
		Result.m_NATGatewayDataProcessedPerGB = NetworkPrices.m_NATGatewayDataProcessedPerGB;
		Result.m_RegionalNATGatewayDataProcessedPerGB = NetworkPrices.m_RegionalNATGatewayDataProcessedPerGB;
		Result.m_PublicIPPricePerHour = NetworkPrices.m_PublicIPv4PricePerHour;

		// Save to cache for future use
		co_await fg_SaveCachedPricing(_RootDirectory, _Region, Result);

		co_return Result;
	}

	TCFuture<CBootstrapConfig> fg_ConfigureBootstrapDeployment
		(
			TCSharedPointer<CCommandLineControl> _pCommandLine
			, CAwsCredentials _Credentials
			, CStr _Region
			, CStr _RootDirectory
		)
	{
		TCSharedPointer<CConfigUIState> pState = fg_Construct(_pCommandLine->f_AnsiEncoding());
		pState->m_pCommandLine = _pCommandLine;
		pState->m_Credentials = _Credentials;
		pState->m_RootDirectory = _RootDirectory;

		// Try to load saved config
		TCVector<CDeploymentApplication> SavedCustomizations;
		if (auto LoadedConfig = co_await fg_LoadConfig(_RootDirectory))
		{
			// Apply loaded user settings
			pState->m_Config.m_Region = LoadedConfig->m_Region;
			pState->m_Config.m_IsolationLevel = LoadedConfig->m_IsolationLevel;
			pState->m_Config.m_StorageIsolation = LoadedConfig->m_StorageIsolation;
			pState->m_Config.m_bStorageEncryption = LoadedConfig->m_bStorageEncryption;
			pState->m_Config.m_SnapshotLevel = LoadedConfig->m_SnapshotLevel;
			pState->m_Config.m_NATConfiguration = LoadedConfig->m_NATConfiguration;
			pState->m_Config.m_NATDataTransferGBPerMonth = LoadedConfig->m_NATDataTransferGBPerMonth;
			pState->m_Config.m_KeyManagerCount = LoadedConfig->m_KeyManagerCount;
			pState->m_Config.m_CPUType = LoadedConfig->m_CPUType;
			pState->m_Config.m_Pricing = LoadedConfig->m_Pricing;

			// Store saved customizations to apply after layout recalculation
			SavedCustomizations = fg_Move(LoadedConfig->m_Applications);
		}
		else
		{
			// No saved config, use passed-in region
			pState->m_Config.m_Region = _Region;
		}

		// Use the config's region (from saved config or passed-in)
		CStr Region = pState->m_Config.m_Region;

		// Fetch pricing information (with file caching and in-memory session cache)
		CAwsPricingData Pricing;
		if (auto pCachedPricing = pState->m_PricingCache.f_FindEqual(Region))
			Pricing = *pCachedPricing;
		else
			Pricing = pState->m_PricingCache[Region] = co_await fg_FetchAwsPricing(_pCommandLine, _Credentials, Region, _RootDirectory);

		pState->m_Config.f_ApplyPricing(fg_Move(Pricing));

		// Initialize the configuration layout
		pState->m_Config.f_RecalculateLayout();

		// Apply saved customizations to regenerated application list
		for (auto &App : pState->m_Config.m_Applications)
		{
			// Find matching saved customization by name and type
			for (auto const &SavedApp : SavedCustomizations)
			{
				if (SavedApp.m_Name == App.m_Name && SavedApp.m_Type == App.m_Type)
				{
					App.m_InstanceTypeOverride = SavedApp.m_InstanceTypeOverride;
					App.m_StorageGBOverride = SavedApp.m_StorageGBOverride;
					App.m_BandwidthMBpsOverride = SavedApp.m_BandwidthMBpsOverride;
					App.m_IOPsOverride = SavedApp.m_IOPsOverride;
					App.m_SnapshotOverride = SavedApp.m_SnapshotOverride;
					break;
				}
			}
		}

		// Update costs with customizations applied
		pState->m_Config.f_UpdateCosts();

		// Initial render
		co_await fg_RenderUI(pState);

		// Input loop
		TCPromiseFuturePair<CBootstrapConfig> ResultPromise;

		auto StdInSubscription = co_await pState->m_pCommandLine->f_RegisterForStdIn
			(
				g_ActorFunctor / [pState, pResultPromise = TCSharedPointer<TCPromise<CBootstrapConfig>>(fg_Construct(fg_Move(ResultPromise.m_Promise)))]
					(NProcess::EStdInReaderOutputType _Type, CStrIO _Input) mutable -> TCFuture<void>
				{
					if (_Type != NProcess::EStdInReaderOutputType_StdIn)
						co_return {};

					if (pState->m_bDone || pState->m_bOpeningDialog)
						co_return {};

					auto SequenceSubscription = co_await pState->m_InputSequencer.f_Sequence();

					auto HandleResult = co_await fg_HandleInput(fg_Move(_Input), pState, pResultPromise).f_Wrap();
					if (!HandleResult && !pResultPromise->f_IsSet())
						pResultPromise->f_SetException(HandleResult);

					co_return {};
				}
				, NProcess::EStdInReaderFlag_None
			)
		;

		auto SubscriptionDestroy = co_await fg_AsyncDestroy(fg_Move(StdInSubscription));

		auto ClearScreen = co_await fg_AsyncDestroy
			(
				[&] -> TCFuture<void>
				{
					auto pLocalState = pState;
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

		auto SequencerDestroy = co_await fg_AsyncDestroy
			(
				[&] -> TCFuture<void>
				{
					co_await fg_Move(pState->m_InputSequencer).f_Destroy();
					co_return {};
				}
			)
		;

		CBootstrapConfig Result = co_await fg_Move(ResultPromise.m_Future);

		co_return Result;
	}
}
