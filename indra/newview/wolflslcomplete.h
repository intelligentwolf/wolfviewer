/**
 * @file wolflslcomplete.h
 * @brief LSL autocomplete for the script editor — the word list WolfStorm's editor offers.
 *
 * [AUTOCOMPLETE 2026-09-11] Paul: "can you enable autocomplete in the wolfviewer script editor
 * like wolfstorm has". WolfStorm's editor has had one since js/lsl/lsl_autocomplete.js; this is
 * the same idea in the C++ viewer.
 *
 * WHERE THE WORDS COME FROM. Nowhere new. LLScriptEditor already loads every LSL and OSSL
 * function, event, constant and type to colour them, and exposes them as
 * keywordsBegin()/keywordsEnd() (llscripteditor.h:57-58) — the same list the "Insert..." combo
 * is built from. Each LLKeywordToken carries its type and its tooltip (llkeywords.h:79-95), so
 * the popup can show a signature without a second data source to keep in step.
 *
 * HOW IT IS DRIVEN. Modelled on LLChatMentionHelper (indra/llui/llchatmentionhelper.cpp), which
 * is the codebase's existing as-you-type popup: a singleton that owns a small floater, is told
 * where the caret is, and gets first refusal on keys.
 *   - LLScriptEdCore sets a keystroke callback on the editor and calls update() from it.
 *   - LLScriptEditor::handleKeyHere gives this first refusal, so Up/Down/Enter/Tab/Esc drive the
 *     list instead of the text.
 * Nothing is inserted until the user accepts a match, and Esc or any non-word character closes
 * the popup.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid. Modified by IntelligentWolf Ltd, 2026.
 * $/LicenseInfo$
 */

#ifndef WOLF_LSL_COMPLETE_H
#define WOLF_LSL_COMPLETE_H

#include "llfloater.h"
#include "llsingleton.h"
#include <string>
#include <vector>

class LLScriptEditor;
class LLScrollListCtrl;

/** One offerable word. */
struct WolfLSLMatch
{
    std::string mName;
    std::string mTooltip;   // the token's own tool tip: the signature, where it has one
    std::string mKind;      // "function", "event", "constant", "type", "flow"
};

/**
 * The popup list. Deliberately a floater, like the mention picker, because the editor lives
 * inside a floater and a child list would be clipped by it.
 */
class WolfFloaterLSLComplete : public LLFloater
{
public:
    WolfFloaterLSLComplete(const LLSD& key);
    bool postBuild() override;
    void onOpen(const LLSD& key) override;

    /** Replace the offered words. `prefix` is what the user has typed so far. */
    void setMatches(const std::vector<WolfLSLMatch>& matches, const std::string& prefix);
    /** The highlighted word, or empty if there is none. */
    std::string selected() const;
    /** Move the highlight. */
    void step(S32 delta);
    S32 matchCount() const { return (S32)mMatches.size(); }

private:
    void onDoubleClick();

    LLScrollListCtrl*         mList = nullptr;
    std::vector<WolfLSLMatch> mMatches;
};

class WolfLSLComplete : public LLSingleton<WolfLSLComplete>
{
    LLSINGLETON(WolfLSLComplete);

public:
    /** Is the popup open for this editor? */
    bool isActive(const LLScriptEditor* ed) const;

    /**
     * The text or caret changed: offer words for the partial identifier at the caret, or close
     * the popup if there is nothing to offer. Cheap enough for a keystroke: it walks back over
     * the current word only, and the candidate scan is over the keyword map.
     */
    void update(LLScriptEditor* ed);

    /** First refusal on a key while the popup is open. True = the key was consumed. */
    bool handleKey(LLScriptEditor* ed, KEY key, MASK mask);

    void hide();

    /** Put the highlighted word into the editor, replacing the partial one. */
    void accept();

    /** The preference gate, so it can be turned off. */
    static bool enabled();

    /** Shortest prefix that opens the popup. Two, because one letter matches far too much. */
    static const S32 MIN_PREFIX = 2;
    /** Most words offered at once; the list is scrollable but an unbounded one is useless. */
    static const S32 MAX_MATCHES = 40;

private:
    /** The identifier ending at the caret, and where it starts. Empty if the caret is not in one. */
    static std::string partialAt(const LLScriptEditor* ed, S32& start_pos);

    LLHandle<LLFloater> mFloaterHandle;
    LLHandle<LLView>    mEditorHandle;
    S32                 mPartialStart = 0;
    S32                 mPartialLen   = 0;
};

#endif // WOLF_LSL_COMPLETE_H
