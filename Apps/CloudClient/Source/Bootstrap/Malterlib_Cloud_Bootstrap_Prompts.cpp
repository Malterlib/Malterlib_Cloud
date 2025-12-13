// Copyright © 2025 Unbroken AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Concurrency/AsyncDestroy>
#include <Mib/Concurrency/ActorSequencerActor>
#include <Mib/CommandLine/TableRenderer>
#include <Mib/CommandLine/AnsiEncodingParse>
#include <Mib/Container/Set>

#include "Malterlib_Cloud_Bootstrap_Prompts.h"

namespace NMib::NCloud::NBootstrap
{
	TCFuture<CStr> fg_PromptWithDefault
		(
			TCSharedPointer<CCommandLineControl> _pCommandLine
			, CStr _CurrentValue
			, CStr _Prompt
			, CStr _Default
		)
	{
		if (!_CurrentValue.f_IsEmpty())
			co_return _CurrentValue;

		CStr PromptText = "{} [{}]: "_f << _Prompt << _Default;
		CStr Input = co_await _pCommandLine->f_ReadPrompt({PromptText, false});

		Input = Input.f_Trim();

		if (Input.f_IsEmpty())
			co_return _Default;

		co_return Input;
	}

	// Arrow key escape sequences
	static constexpr ch8 gc_UpArrow[] = "\x1b[A";
	static constexpr ch8 gc_DownArrow[] = "\x1b[B";
	static constexpr ch8 gc_Escape[] = "\x1b";

	struct CSelectState
	{
		CSelectState(CAnsiEncoding _AnsiEncoding, bool _bMultiSelect)
			: m_AnsiEncoding(_AnsiEncoding)
			, m_bMultiSelect(_bMultiSelect)
		{
		}

		TCSharedPointer<CCommandLineControl> m_pCommandLine;
		TCVector<TCVector<CStr>> m_Items;
		CStr m_Prompt;
		CStr m_Default;
		TCVector<CStr> m_Headings;
		CAnsiEncoding m_AnsiEncoding;

		CStr m_Filter;
		mint m_SelectedIndex = 0;
		mint m_ScrollOffset = 0;
		mint m_LastRenderedLines = 0;
		TCVector<mint> m_FilteredIndices;
		bool m_bDone = false;
		NConcurrency::CSequencer m_InputSequencer{"SelectFromListFilter InputSequencer"};

		// Multi-select support
		bool m_bMultiSelect = false;
		TCSet<mint> m_SelectedIndices;  // Set of selected item indices (for multi-select)
	};

	// Static implementation that supports both single-select and multi-select modes
	static TCFuture<TCOptional<TCVector<CStr>>> fsg_SelectFromListWithFilterImpl
		(
			TCSharedPointer<CCommandLineControl> _pCommandLine
			, TCVector<TCVector<CStr>> _Items
			, TCVector<CStr> _Headings
			, CStr _Prompt
			, CStr _Default
			, TCVector<CStr> _DefaultSelected
			, bool _bMultiSelect
		)
	{
		using namespace NStr;

		if (_Items.f_IsEmpty())
			co_return {};

		// Validate column counts match
		mint nColumnCount = _Headings.f_GetLen();
		if (nColumnCount == 0)
			co_return DMibErrorInstance("Empty columns");

		for (auto const &Row : _Items)
		{
			if (Row.f_GetLen() != nColumnCount)
				co_return DMibErrorInstance("Item count doesn't match column count");
		}

		TCSharedPointer<CSelectState> pState = fg_Construct(_pCommandLine->f_AnsiEncoding(), _bMultiSelect);
		pState->m_pCommandLine = _pCommandLine;
		pState->m_Items = fg_Move(_Items);
		pState->m_Prompt = fg_Move(_Prompt);
		pState->m_Default = fg_Move(_Default);
		pState->m_Headings = fg_Move(_Headings);

		// Find default index (compare first column only) - for single-select cursor position
		for (mint i = 0; i < pState->m_Items.f_GetLen(); ++i)
		{
			if (!pState->m_Items[i].f_IsEmpty() && pState->m_Items[i][0] == pState->m_Default)
			{
				pState->m_SelectedIndex = i;
				break;
			}
		}

		// Initialize default selections for multi-select mode
		if (_bMultiSelect)
		{
			for (auto const &DefaultItem : _DefaultSelected)
			{
				for (mint i = 0; i < pState->m_Items.f_GetLen(); ++i)
				{
					if (!pState->m_Items[i].f_IsEmpty() && pState->m_Items[i][0] == DefaultItem)
					{
						pState->m_SelectedIndices.f_Insert(i);
						break;
					}
				}
			}
		}

		// Initialize filtered indices to include all items
		for (mint i = 0; i < pState->m_Items.f_GetLen(); ++i)
			pState->m_FilteredIndices.f_Insert(i);

		auto fUpdateFilter = [pState]()
			{
				pState->m_FilteredIndices.f_Clear();
				for (mint i = 0; i < pState->m_Items.f_GetLen(); ++i)
				{
					auto const &Row = pState->m_Items[i];
					bool bMatch = pState->m_Filter.f_IsEmpty();
					if (!bMatch)
					{
						// Search all columns for the filter text
						for (auto const &Cell : Row)
						{
							if (Cell.f_FindNoCase(pState->m_Filter) >= 0)
							{
								bMatch = true;
								break;
							}
						}
					}
					if (bMatch)
						pState->m_FilteredIndices.f_Insert(i);
				}
				pState->m_SelectedIndex = fg_Min(pState->m_SelectedIndex, fg_Max(mint(0), pState->m_FilteredIndices.f_GetLen() - 1));
			}
		;

		auto fRenderList = [pState]() -> TCFuture<void>
			{
				CStr Output;
				Output += pState->m_AnsiEncoding.f_SyncronizeOutputStart();

				// Clear previous output by moving up and clearing lines
				if (pState->m_LastRenderedLines > 0)
				{
					Output += pState->m_AnsiEncoding.f_MovePreviousLine(pState->m_LastRenderedLines);
					pState->m_LastRenderedLines = 0;
					Output += pState->m_AnsiEncoding.f_MoveToColumn(0);
					Output += pState->m_AnsiEncoding.f_ClearToEndOfScreen();
				}

				Output += pState->m_AnsiEncoding.f_ShowCursor(false);

				mint nLineCount = 0;

				bool bColumnsAllEmpty = true;

				for (auto &Heading : pState->m_Headings)
				{
					if (!Heading.f_IsEmpty())
					{
						bColumnsAllEmpty = false;
						break;
					}
				}

				// Build table with CTableRenderHelper
				CStr TableOutput;
				CTableRenderHelper TableRenderer
					(
						[&](CStr const &_Line) { TableOutput += _Line; }
						, (CTableRenderHelper::EOption_AvoidRowSeparators | CTableRenderHelper::EOption_Rounded | CTableRenderHelper::EOption_NoExtraLines)
						| (bColumnsAllEmpty ? CTableRenderHelper::EOption_NoHeadings : CTableRenderHelper::EOption_None)
						, pState->m_AnsiEncoding.f_Flags()
						, pState->m_pCommandLine->m_CommandLineWidth
					)
				;

				mint nColumnCount = pState->m_Headings.f_GetLen();

				// Calculate min width for each column from headings and all filtered items for stable column widths
				TCVector<uint32> MinColumnWidths;
				for (mint iCol = 0; iCol < nColumnCount; ++iCol)
					MinColumnWidths.f_Insert((uint32)CAnsiEncodingParse::fs_RenderedStrLen(pState->m_Headings[iCol]));

				// First column needs extra width for description/filter prompt
				MinColumnWidths[0] = fg_Max(MinColumnWidths[0], (uint32)CAnsiEncodingParse::fs_RenderedStrLen(pState->m_Prompt) + 1u);
				MinColumnWidths[0] = fg_Max(MinColumnWidths[0], (uint32)CAnsiEncodingParse::fs_RenderedStrLen("Filter: {}"_f << pState->m_Filter) + 1u);

				// Calculate max width from all filtered items
				for (mint iIdx : pState->m_FilteredIndices)
				{
					auto const &Row = pState->m_Items[iIdx];
					for (mint iCol = 0; iCol < nColumnCount; ++iCol)
						MinColumnWidths[iCol] = fg_Max(MinColumnWidths[iCol], (uint32)CAnsiEncodingParse::fs_RenderedStrLen(Row[iCol]));
				}

				TableRenderer.f_AddDescription(pState->m_Prompt);

				TableRenderer.f_AddDescription
					(
						"{}{}Filter: {}{}"_f
						<< pState->m_AnsiEncoding.f_Default()
						<< pState->m_AnsiEncoding.f_Foreground256(246)
						<< pState->m_AnsiEncoding.f_Default()
						<< pState->m_Filter
					)
				;

				TableRenderer.f_AddHeadingsVector(pState->m_Headings);

				for (mint iCol = 0; iCol < nColumnCount; ++iCol)
				{
					TableRenderer.f_SetMinColumnWidth(iCol, MinColumnWidths[iCol]);
					TableRenderer.f_SetMaxColumnWidth(iCol, 60);
				}

				mint DisplayCount = fg_Min(pState->m_FilteredIndices.f_GetLen(), mint(10));

				// Adjust scroll offset only when selection goes outside visible range
				if (pState->m_SelectedIndex < pState->m_ScrollOffset)
					pState->m_ScrollOffset = pState->m_SelectedIndex;
				else if (pState->m_SelectedIndex >= pState->m_ScrollOffset + DisplayCount)
					pState->m_ScrollOffset = pState->m_SelectedIndex - DisplayCount + 1;

				// Clamp scroll offset to always show DisplayCount items
				pState->m_ScrollOffset = fg_Max(mint(0), fg_Min(pState->m_ScrollOffset, pState->m_FilteredIndices.f_GetLen() - DisplayCount));

				bool bHasMoreAbove = pState->m_ScrollOffset > 0;
				bool bHasMoreBelow = pState->m_ScrollOffset + DisplayCount < pState->m_FilteredIndices.f_GetLen();

				// Helper to create a row vector with the right number of columns
				auto fMakeEmptyRow = [nColumnCount]() -> TCVector<CStr>
					{
						TCVector<CStr> Row;
						for (mint i = 0; i < nColumnCount; ++i)
							Row.f_Insert(CStr());
						return Row;
					}
				;

				// Add "more above" indicator (always present for consistent height)
				{
					TCVector<CStr> Row = fMakeEmptyRow();
					if (bHasMoreAbove)
						Row[0] = "{}...{}"_f << pState->m_AnsiEncoding.f_Foreground256(246) << pState->m_AnsiEncoding.f_Default();
					TableRenderer.f_AddRowVector(Row);
				}

				for (mint i = 0; i < DisplayCount; ++i)
				{
					mint FilteredIndex = pState->m_ScrollOffset + i;
					mint ItemIndex = pState->m_FilteredIndices[FilteredIndex];
					auto const &ItemRow = pState->m_Items[ItemIndex];

					bool bIsCursor = (FilteredIndex == pState->m_SelectedIndex);
					bool bIsSelected = pState->m_bMultiSelect && pState->m_SelectedIndices.f_FindEqual(ItemIndex);

					// Build styled row
					TCVector<CStr> Row;
					for (mint iCol = 0; iCol < nColumnCount; ++iCol)
					{
						CStr Cell;

						// Apply styling based on cursor and selection state
						if (bIsCursor && bIsSelected)
						{
							// Both cursor and selected: bold cyan foreground + teal background
							Cell = "{}{}{}{}{}"_f
								<< pState->m_AnsiEncoding.f_Bold()
								<< pState->m_AnsiEncoding.f_Foreground256(87)
								<< pState->m_AnsiEncoding.f_BackgroundRGB(34, 87, 122)
								<< ItemRow[iCol]
								<< pState->m_AnsiEncoding.f_Default()
							;
						}
						else if (bIsCursor)
						{
							// Cursor only: bold cyan foreground
							Cell = "{}{}{}{}"_f
								<< pState->m_AnsiEncoding.f_Bold()
								<< pState->m_AnsiEncoding.f_Foreground256(87)
								<< ItemRow[iCol]
								<< pState->m_AnsiEncoding.f_Default()
							;
						}
						else if (bIsSelected)
						{
							// Selected only: teal background
							Cell = "{}{}{}"_f
								<< pState->m_AnsiEncoding.f_BackgroundRGB(34, 87, 122)
								<< ItemRow[iCol]
								<< pState->m_AnsiEncoding.f_Default()
							;
						}
						else
						{
							Cell = ItemRow[iCol];
						}

						Row.f_Insert(fg_Move(Cell));
					}
					TableRenderer.f_AddRowVector(Row);
				}

				// Add "more below" indicator (always present for consistent height)
				{
					TCVector<CStr> Row = fMakeEmptyRow();
					if (bHasMoreBelow)
						Row[0] = "{}...{}"_f << pState->m_AnsiEncoding.f_Foreground256(246) << pState->m_AnsiEncoding.f_Default();
					TableRenderer.f_AddRowVector(Row);
				}

				TableRenderer.f_Output(CTableRenderHelper::EOutputType_HumanReadable);

				// Count lines in table output
				for (ch8 Ch : TableOutput)
				{
					if (Ch == '\n')
						++nLineCount;
				}

				Output += TableOutput;
				Output += "\n";
				++nLineCount;

				// Position cursor at filter input point
				// Filter line is line 3 (after top border, prompt line), so move up (nLineCount - 2) lines
				Output += pState->m_AnsiEncoding.f_MovePreviousLine(nLineCount - 2);
				// Move to column after "│ Filter: " (10 display chars) + filter text
				Output += pState->m_AnsiEncoding.f_MoveToColumn(10 + CAnsiEncodingParse::fs_RenderedStrLen(pState->m_Filter));

				Output += pState->m_AnsiEncoding.f_ShowCursor(true);
				Output += pState->m_AnsiEncoding.f_SyncronizeOutputFinish();

				// Cursor is at filter line (line 3), so we need to move up 2 lines to clear from top
				pState->m_LastRenderedLines = 2;

				co_await pState->m_pCommandLine->f_StdOut(Output);
				co_return {};
			}
		;

		// Initial render
		co_await fg_CallSafe(fRenderList);

		// Input loop using f_RegisterForStdIn
		TCPromiseFuturePair<TCOptional<TCVector<CStr>>> ResultPromise;

		auto StdInSubscription = co_await pState->m_pCommandLine->f_RegisterForStdIn
			(
				g_ActorFunctor / [pState, fUpdateFilter, fRenderList, ResultPromise = fg_Move(ResultPromise.m_Promise)]
				(NProcess::EStdInReaderOutputType _Type, CStrIO _Input) mutable -> TCFuture<void>
				{
					if (_Type != NProcess::EStdInReaderOutputType_StdIn)
						co_return {};

					auto SequenceSubscription = co_await pState->m_InputSequencer.f_Sequence();

					if (pState->m_bDone)
						co_return {};

					CStr Input = _Input;

					// Check for escape sequences
					if (Input == gc_UpArrow)
					{
						if (pState->m_SelectedIndex > 0)
							--pState->m_SelectedIndex;
					}
					else if (Input == gc_DownArrow)
					{
						if (pState->m_SelectedIndex < pState->m_FilteredIndices.f_GetLen() - 1)
							++pState->m_SelectedIndex;
					}
					else if (Input == " " && pState->m_bMultiSelect)
					{
						// Space: Toggle selection of current item (multi-select only)
						if (!pState->m_FilteredIndices.f_IsEmpty())
						{
							mint ItemIndex = pState->m_FilteredIndices[pState->m_SelectedIndex];
							if (pState->m_SelectedIndices.f_FindEqual(ItemIndex))
								pState->m_SelectedIndices.f_Remove(ItemIndex);
							else
								pState->m_SelectedIndices.f_Insert(ItemIndex);
						}
					}
					else if (Input == "\x01" && pState->m_bMultiSelect)
					{
						// Ctrl+A: Select all visible (filtered) items
						for (mint iIdx : pState->m_FilteredIndices)
							pState->m_SelectedIndices.f_Insert(iIdx);
					}
					else if (Input == "\x04" && pState->m_bMultiSelect)
					{
						// Ctrl+D: Deselect all items
						pState->m_SelectedIndices.f_Clear();
					}
					else if (Input == "\n" || Input == "\r")
					{
						// Enter pressed - confirm selection
						pState->m_bDone = true;
						if (!ResultPromise.f_IsSet())
						{
							TCVector<CStr> Results;

							if (pState->m_bMultiSelect)
							{
								// Multi-select: return all selected items (first column values)
								// Maintain original order by iterating through items
								for (mint i = 0; i < pState->m_Items.f_GetLen(); ++i)
								{
									if (pState->m_SelectedIndices.f_FindEqual(i))
										Results.f_Insert(pState->m_Items[i][0]);
								}
							}
							else
							{
								// Single-select: return cursor item
								if (!pState->m_FilteredIndices.f_IsEmpty())
								{
									mint ItemIndex = pState->m_FilteredIndices[pState->m_SelectedIndex];
									Results.f_Insert(pState->m_Items[ItemIndex][0]);
								}
							}

							ResultPromise.f_SetResult(TCOptional<TCVector<CStr>>(fg_Move(Results)));
						}
						co_return {};
					}
					else if (Input == "\x7f" || Input == "\b")
					{
						// Backspace
						if (!pState->m_Filter.f_IsEmpty())
						{
							pState->m_Filter = pState->m_Filter.f_Extract(0, pState->m_Filter.f_GetLen() - 1);
							fUpdateFilter();
						}
					}
					else if (Input == gc_Escape)
					{
						// Escape key alone - cancel selection (return nullopt)
						pState->m_bDone = true;
						if (!ResultPromise.f_IsSet())
							ResultPromise.f_SetResult(TCOptional<TCVector<CStr>>());
						co_return {};
					}
					else if (Input == "\x03")
					{
						// Ctrl+C - terminate application
						pState->m_bDone = true;
						if (!ResultPromise.f_IsSet())
							ResultPromise.f_SetException(DMibErrorInstance("Selection cancelled"));
						co_return {};
					}
					else if (Input.f_GetLen() == 1 && Input[0] >= 32)
					{
						// Regular character - add to filter
						pState->m_Filter += Input;
						fUpdateFilter();
					}

					co_await fg_CallSafe(fRenderList);
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
					if (pLocalState->m_LastRenderedLines)
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

		TCOptional<TCVector<CStr>> Result = co_await fg_Move(ResultPromise.m_Future);

		co_return Result;
	}

	// Single-select wrapper - returns TCOptional<CStr>
	TCFuture<TCOptional<CStr>> fg_SelectFromListWithFilter
		(
			TCSharedPointer<CCommandLineControl> _pCommandLine
			, TCVector<TCVector<CStr>> _Items
			, TCVector<CStr> _Headings
			, CStr _Prompt
			, CStr _Default
		)
	{
		auto Result = co_await fsg_SelectFromListWithFilterImpl
			(
				fg_Move(_pCommandLine)
				, fg_Move(_Items)
				, fg_Move(_Headings)
				, fg_Move(_Prompt)
				, fg_Move(_Default)
				, {}  // No default selections for single-select
				, false  // Single-select mode
			)
		;

		if (!Result || Result->f_IsEmpty())
			co_return {};

		co_return TCOptional<CStr>((*Result)[0]);
	}

	// Multi-select wrapper - returns TCOptional<TCVector<CStr>>
	TCFuture<TCOptional<TCVector<CStr>>> fg_MultiSelectFromListWithFilter
		(
			TCSharedPointer<CCommandLineControl> _pCommandLine
			, TCVector<TCVector<CStr>> _Items
			, TCVector<CStr> _Headings
			, CStr _Prompt
			, TCVector<CStr> _DefaultSelected
		)
	{
		co_return co_await fsg_SelectFromListWithFilterImpl
			(
				fg_Move(_pCommandLine)
				, fg_Move(_Items)
				, fg_Move(_Headings)
				, fg_Move(_Prompt)
				, {}  // No cursor default for multi-select
				, fg_Move(_DefaultSelected)
				, true  // Multi-select mode
			)
		;
	}
}
