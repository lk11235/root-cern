//===--- FormatToken.cpp - Format C++ code --------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// \brief This file implements specific functions of \c FormatTokens and their
/// roles.
///
//===----------------------------------------------------------------------===//

#include "FormatToken.h"
#include "ContinuationIndenter.h"
#include "clang/Format/Format.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Debug.h"

namespace clang {
namespace format {

TokenRole::~TokenRole() {}

void TokenRole::precomputeFormattingInfos(const FormatToken *Token) {}

unsigned CommaSeparatedList::format(LineState &State,
                                    ContinuationIndenter *Indenter,
                                    bool DryRun) {
  if (!State.NextToken->Previous || !State.NextToken->Previous->Previous ||
      Commas.size() <= 2)
    return 0;

  // Ensure that we start on the opening brace.
  const FormatToken *LBrace = State.NextToken->Previous->Previous;
  if (LBrace->isNot(tok::l_brace) ||
      LBrace->BlockKind == BK_Block ||
      LBrace->Next->Type == TT_DesignatedInitializerPeriod)
    return 0;

  // Calculate the number of code points we have to format this list. As the
  // first token is already placed, we have to subtract it.
  unsigned RemainingCodePoints = Style.ColumnLimit - State.Column +
                                 State.NextToken->Previous->ColumnWidth;

  // Find the best ColumnFormat, i.e. the best number of columns to use.
  const ColumnFormat *Format = getColumnFormat(RemainingCodePoints);
  if (!Format)
    return 0;

  // Format the entire list.
  unsigned Penalty = 0;
  unsigned Column = 0;
  unsigned Item = 0;
  while (State.NextToken != LBrace->MatchingParen) {
    bool NewLine = false;
    unsigned ExtraSpaces = 0;

    // If the previous token was one of our commas, we are now on the next item.
    if (Item < Commas.size() && State.NextToken->Previous == Commas[Item]) {
      if (!State.NextToken->isTrailingComment()) {
        ExtraSpaces += Format->ColumnSizes[Column] - ItemLengths[Item];
        ++Column;
      }
      ++Item;
    }

    if (Column == Format->Columns || State.NextToken->MustBreakBefore) {
      Column = 0;
      NewLine = true;
    }

    // Place token using the continuation indenter and store the penalty.
    Penalty += Indenter->addTokenToState(State, NewLine, DryRun, ExtraSpaces);
  }
  return Penalty;
}

// Returns the lengths in code points between Begin and End (both included),
// assuming that the entire sequence is put on a single line.
static unsigned CodePointsBetween(const FormatToken *Begin,
                                  const FormatToken *End) {
  assert(End->TotalLength >= Begin->TotalLength);
  return End->TotalLength - Begin->TotalLength + Begin->ColumnWidth;
}

void CommaSeparatedList::precomputeFormattingInfos(const FormatToken *Token) {
  // FIXME: At some point we might want to do this for other lists, too.
  if (!Token->MatchingParen || Token->isNot(tok::l_brace))
    return;

  FormatToken *ItemBegin = Token->Next;
  SmallVector<bool, 8> MustBreakBeforeItem;

  // The lengths of an item if it is put at the end of the line. This includes
  // trailing comments which are otherwise ignored for column alignment.
  SmallVector<unsigned, 8> EndOfLineItemLength;

  for (unsigned i = 0, e = Commas.size() + 1; i != e; ++i) {
    // Skip comments on their own line.
    while (ItemBegin->HasUnescapedNewline && ItemBegin->isTrailingComment())
      ItemBegin = ItemBegin->Next;

    MustBreakBeforeItem.push_back(ItemBegin->MustBreakBefore);
    const FormatToken *ItemEnd = NULL;
    if (i == Commas.size()) {
      ItemEnd = Token->MatchingParen;
      const FormatToken *NonCommentEnd = ItemEnd->getPreviousNonComment();
      ItemLengths.push_back(CodePointsBetween(ItemBegin, NonCommentEnd));
      if (Style.Cpp11BracedListStyle) {
        // In Cpp11 braced list style, the } and possibly other subsequent
        // tokens will need to stay on a line with the last element.
        while (ItemEnd->Next && !ItemEnd->Next->CanBreakBefore)
          ItemEnd = ItemEnd->Next;
      } else {
        // In other braced lists styles, the "}" can be wrapped to the new line.
        ItemEnd = Token->MatchingParen->Previous;
      }
    } else {
      ItemEnd = Commas[i];
      // The comma is counted as part of the item when calculating the length.
      ItemLengths.push_back(CodePointsBetween(ItemBegin, ItemEnd));
      // Consume trailing comments so the are included in EndOfLineItemLength.
      if (ItemEnd->Next && !ItemEnd->Next->HasUnescapedNewline &&
          ItemEnd->Next->isTrailingComment())
        ItemEnd = ItemEnd->Next;
    }
    EndOfLineItemLength.push_back(CodePointsBetween(ItemBegin, ItemEnd));
    // If there is a trailing comma in the list, the next item will start at the
    // closing brace. Don't create an extra item for this.
    if (ItemEnd->getNextNonComment() == Token->MatchingParen)
      break;
    ItemBegin = ItemEnd->Next;
  }

  // We can never place more than ColumnLimit / 3 items in a row (because of the
  // spaces and the comma).
  for (unsigned Columns = 1; Columns <= Style.ColumnLimit / 3; ++Columns) {
    ColumnFormat Format;
    Format.Columns = Columns;
    Format.ColumnSizes.resize(Columns);
    Format.LineCount = 0;
    bool HasRowWithSufficientColumns = false;
    unsigned Column = 0;
    for (unsigned i = 0, e = ItemLengths.size(); i != e; ++i) {
      if (MustBreakBeforeItem[i] || Column == Columns) {
        ++Format.LineCount;
        Column = 0;
      }
      if (Column == Columns - 1)
        HasRowWithSufficientColumns = true;
      unsigned length =
          (Column == Columns - 1) ? EndOfLineItemLength[i] : ItemLengths[i];
      Format.ColumnSizes[Column] =
          std::max(Format.ColumnSizes[Column], length);
      ++Column;
    }
    // If all rows are terminated early (e.g. by trailing comments), we don't
    // need to look further.
    if (!HasRowWithSufficientColumns)
      break;
    Format.TotalWidth = Columns - 1; // Width of the N-1 spaces.
    for (unsigned i = 0; i < Columns; ++i) {
      Format.TotalWidth += Format.ColumnSizes[i];
    }

    // Ignore layouts that are bound to violate the column limit.
    if (Format.TotalWidth > Style.ColumnLimit)
      continue;

    Formats.push_back(Format);
  }
}

const CommaSeparatedList::ColumnFormat *
CommaSeparatedList::getColumnFormat(unsigned RemainingCharacters) const {
  const ColumnFormat *BestFormat = NULL;
  for (SmallVector<ColumnFormat, 4>::const_reverse_iterator
           I = Formats.rbegin(),
           E = Formats.rend();
       I != E; ++I) {
    if (I->TotalWidth <= RemainingCharacters) {
      if (BestFormat && I->LineCount > BestFormat->LineCount)
        break;
      BestFormat = &*I;
    }
  }
  return BestFormat;
}

} // namespace format
} // namespace clang
