/**
 * @file wolflslcomplete.cpp
 * @brief LSL autocomplete for the script editor — see wolflslcomplete.h.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid. Modified by IntelligentWolf Ltd, 2026.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "wolflslcomplete.h"

#include "llfloaterreg.h"
#include "llscripteditor.h"
#include "llscrolllistctrl.h"
#include "lluictrlfactory.h"
#include "llviewercontrol.h"

namespace
{
    const char* const LSL_COMPLETE_FLOATER = "wolf_lsl_complete";

    /// An LSL identifier character. Underscore counts; a digit counts except as the first
    /// character, which is the language's own rule and also what makes "llSay" match from "llS".
    bool is_ident_char(llwchar c, bool first)
    {
        if (c == '_') return true;
        if (c >= 'a' && c <= 'z') return true;
        if (c >= 'A' && c <= 'Z') return true;
        if (!first && c >= '0' && c <= '9') return true;
        return false;
    }

    /// Case-insensitive "does `name` start with `prefix`".
    ///
    /// Case-insensitive deliberately: LSL is case-sensitive but people type "llsay" and expect
    /// llSay. The inserted text is always the token's real spelling, so the script stays valid.
    bool starts_with_ci(const std::string& name, const std::string& prefix)
    {
        if (prefix.length() > name.length()) return false;
        for (size_t i = 0; i < prefix.length(); ++i)
        {
            if (tolower((unsigned char)name[i]) != tolower((unsigned char)prefix[i])) return false;
        }
        return true;
    }

    std::string kind_of(LLKeywordToken::ETokenType type)
    {
        switch (type)
        {
        case LLKeywordToken::TT_FUNCTION: return "function";
        case LLKeywordToken::TT_EVENT:    return "event";
        case LLKeywordToken::TT_CONSTANT: return "constant";
        case LLKeywordToken::TT_TYPE:     return "type";
        case LLKeywordToken::TT_SECTION:  return "flow";
        default:                          return "word";
        }
    }

    /// Only words worth completing. Delimiters and whole-line tokens are not identifiers, and
    /// offering them would put punctuation into the script.
    bool completable(LLKeywordToken::ETokenType type)
    {
        switch (type)
        {
        case LLKeywordToken::TT_FUNCTION:
        case LLKeywordToken::TT_EVENT:
        case LLKeywordToken::TT_CONSTANT:
        case LLKeywordToken::TT_TYPE:
        case LLKeywordToken::TT_SECTION:
        case LLKeywordToken::TT_WORD:
            return true;
        default:
            return false;
        }
    }
}

// ═══════════════════════ WolfFloaterLSLComplete ═════════════════════════════════════════════

WolfFloaterLSLComplete::WolfFloaterLSLComplete(const LLSD& key) : LLFloater(key) {}

bool WolfFloaterLSLComplete::postBuild()
{
    mList = getChild<LLScrollListCtrl>("lsl_complete_list");
    mList->setDoubleClickCallback(boost::bind(&WolfFloaterLSLComplete::onDoubleClick, this));
    return true;
}

void WolfFloaterLSLComplete::onOpen(const LLSD& key)
{
    // Nothing to do: setMatches has already filled the list. Overridden so opening does not
    // reset the selection the caller just made.
}

void WolfFloaterLSLComplete::setMatches(const std::vector<WolfLSLMatch>& matches, const std::string& prefix)
{
    mMatches = matches;
    if (!mList) return;
    mList->deleteAllItems();
    for (S32 i = 0; i < (S32)mMatches.size(); ++i)
    {
        const WolfLSLMatch& m = mMatches[i];
        LLSD row;
        row["id"] = i;
        row["columns"][0]["column"] = "name";
        row["columns"][0]["value"]  = m.mName;
        row["columns"][1]["column"] = "kind";
        row["columns"][1]["value"]  = m.mKind;
        LLScrollListItem* item = mList->addElement(row);
        // The token's tool tip is the signature where it has one, which is the whole reason to
        // reuse the highlighter's data rather than keep a separate table.
        if (item && !m.mTooltip.empty())
        {
            if (LLScrollListCell* cell = item->getColumn(0)) cell->setToolTip(m.mTooltip);
        }
    }
    if (mList->getItemCount() > 0)
    {
        mList->selectFirstItem();
    }
}

std::string WolfFloaterLSLComplete::selected() const
{
    if (!mList) return std::string();
    LLScrollListItem* item = mList->getFirstSelected();
    if (!item) return std::string();
    const S32 idx = item->getValue().asInteger();
    if (idx < 0 || idx >= (S32)mMatches.size()) return std::string();
    return mMatches[idx].mName;
}

void WolfFloaterLSLComplete::step(S32 delta)
{
    if (!mList || mList->getItemCount() == 0) return;
    // selectNthItem wraps nothing, so clamp: running off the end should stay at the end rather
    // than lose the selection.
    S32 cur = mList->getFirstSelectedIndex();
    if (cur < 0) cur = 0;
    S32 want = llclamp(cur + delta, 0, mList->getItemCount() - 1);
    mList->selectNthItem(want);
    mList->scrollToShowSelected();
}

void WolfFloaterLSLComplete::onDoubleClick()
{
    WolfLSLComplete::instance().accept();
}

// ═══════════════════════ WolfLSLComplete ════════════════════════════════════════════════════

WolfLSLComplete::WolfLSLComplete() {}

bool WolfLSLComplete::enabled()
{
    // Default true: Paul asked for it to be on. The control is registered in settings.xml so it
    // can be turned off without a rebuild.
    static LLCachedControl<bool> on(gSavedSettings, "WolfLSLAutoComplete", true);
    return on;
}

bool WolfLSLComplete::isActive(const LLScriptEditor* ed) const
{
    return !mEditorHandle.isDead() && mEditorHandle.get() == ed
        && !mFloaterHandle.isDead() && mFloaterHandle.get()->isShown();
}

std::string WolfLSLComplete::partialAt(const LLScriptEditor* ed, S32& start_pos)
{
    start_pos = 0;
    if (!ed) return std::string();
    const LLWString& text = ed->getWText();
    const S32 caret = ed->getCursorPos();
    if (caret <= 0 || caret > (S32)text.length()) return std::string();

    S32 i = caret;
    while (i > 0 && is_ident_char(text[i - 1], false)) --i;
    // The run must START with a letter or underscore, or it is a number, not an identifier.
    if (i >= caret || !is_ident_char(text[i], true)) return std::string();
    start_pos = i;

    std::string out;
    for (S32 k = i; k < caret; ++k) out += (char)text[k];
    return out;
}

void WolfLSLComplete::update(LLScriptEditor* ed)
{
    if (!enabled() || !ed) { hide(); return; }

    S32 start = 0;
    const std::string prefix = partialAt(ed, start);
    if ((S32)prefix.length() < MIN_PREFIX)
    {
        hide();
        return;
    }

    // Candidates come from the editor's own keyword map — the list it already colours with.
    std::vector<WolfLSLMatch> matches;
    for (LLKeywords::keyword_iterator_t it = ed->keywordsBegin(); it != ed->keywordsEnd(); ++it)
    {
        const LLKeywordToken* tok = it->second;
        if (!tok || !completable(tok->getType())) continue;
        const std::string name = wstring_to_utf8str(tok->getToken());
        if (name.empty() || !starts_with_ci(name, prefix)) continue;
        // An exact hit alone is not worth a popup; the user has already typed the whole word.
        WolfLSLMatch m;
        m.mName    = name;
        m.mTooltip = wstring_to_utf8str(tok->getToolTip());
        m.mKind    = kind_of(tok->getType());
        matches.push_back(m);
        if ((S32)matches.size() >= MAX_MATCHES * 4) break;   // bound the scan on a 1-char-ish prefix
    }
    if (matches.empty() || (matches.size() == 1 && matches[0].mName == prefix))
    {
        hide();
        return;
    }
    // Shortest first, then alphabetical: the word the user most likely wants is the one closest
    // to what they typed.
    std::sort(matches.begin(), matches.end(), [](const WolfLSLMatch& a, const WolfLSLMatch& b)
    {
        if (a.mName.length() != b.mName.length()) return a.mName.length() < b.mName.length();
        return a.mName < b.mName;
    });
    if ((S32)matches.size() > MAX_MATCHES) matches.resize(MAX_MATCHES);

    if (mFloaterHandle.isDead())
    {
        LLFloater* f = LLFloaterReg::getInstance(LSL_COMPLETE_FLOATER);
        if (!f) return;
        mFloaterHandle = f->getHandle();
    }
    WolfFloaterLSLComplete* popup = dynamic_cast<WolfFloaterLSLComplete*>(mFloaterHandle.get());
    if (!popup) return;

    mEditorHandle = ed->getHandle();
    mPartialStart = start;
    mPartialLen   = (S32)prefix.length();
    popup->setMatches(matches, prefix);

    // Sit the list just under the caret. Source: llchatmentionhelper.cpp:96-112 — the same
    // localPointToOtherView into gFloaterView, because a floater's rect is in that space.
    const LLRect caret_rect = ed->getLocalRectFromDocIndex(ed->getCursorPos());
    S32 fx = 0, fy = 0;
    if (!ed->localPointToOtherView(caret_rect.mLeft, caret_rect.mBottom, &fx, &fy, gFloaterView))
    {
        return;
    }
    LLRect r = popup->getRect();
    r.setLeftTopAndSize(fx, fy, r.getWidth(), r.getHeight());
    popup->setRect(r);

    if (!popup->isShown())
    {
        popup->openFloater(LLSD());
        // The editor must keep the keys: the popup is a list to look at, not a thing to focus.
        ed->setFocus(true);
    }
}

bool WolfLSLComplete::handleKey(LLScriptEditor* ed, KEY key, MASK mask)
{
    if (!isActive(ed)) return false;
    WolfFloaterLSLComplete* popup = dynamic_cast<WolfFloaterLSLComplete*>(mFloaterHandle.get());
    if (!popup) return false;

    if (mask != MASK_NONE) return false;   // Ctrl/Shift combinations belong to the editor

    switch (key)
    {
    case KEY_ESCAPE:
        hide();
        return true;
    case KEY_UP:
        popup->step(-1);
        return true;
    case KEY_DOWN:
        popup->step(1);
        return true;
    case KEY_PAGE_UP:
        popup->step(-8);
        return true;
    case KEY_PAGE_DOWN:
        popup->step(8);
        return true;
    case KEY_TAB:
    case KEY_RETURN:
        accept();
        return true;
    default:
        return false;
    }
}

void WolfLSLComplete::accept()
{
    WolfFloaterLSLComplete* popup = dynamic_cast<WolfFloaterLSLComplete*>(mFloaterHandle.get());
    LLScriptEditor* ed = dynamic_cast<LLScriptEditor*>(mEditorHandle.get());
    if (!popup || !ed) { hide(); return; }

    const std::string word = popup->selected();
    hide();                     // before the edit, so update() from the keystroke does not reopen
    if (word.empty()) return;

    // Replace exactly the partial identifier. Selecting it and inserting keeps ONE undo step
    // and leaves the caret after the word, which is what typing it would have done.
    ed->setFocus(true);
    ed->wolfReplaceRange(mPartialStart, mPartialLen, word);
}

void WolfLSLComplete::hide()
{
    if (!mFloaterHandle.isDead())
    {
        if (LLFloater* f = mFloaterHandle.get())
        {
            if (f->isShown()) f->closeFloater();
        }
    }
    mEditorHandle.markDead();
    mPartialStart = 0;
    mPartialLen = 0;
}
