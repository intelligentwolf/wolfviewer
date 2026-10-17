/**
 * @file wolffloateradventureteacher.h
 * @brief WolfViewer: Wolf Adventure Teacher Console (STUDIO_AND_CONSOLE_V1.md §4).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef WOLF_FLOATER_ADVENTURE_TEACHER_H
#define WOLF_FLOATER_ADVENTURE_TEACHER_H

#include <string>

#include "llfloater.h"
#include "wolffloateradventuregm.h"

/**
 * [WOLF ADVENTURE P4 2026-10-16] STUDIO_AND_CONSOLE_V1.md §4: a role-limited view of the GM Console's
 * operations (the teacher column of BRAIN_API_V1.md §8.1). Pages: Classes (a new class and its join code),
 * Live (every student from class_get, polled every 5 s; pause, gather, a question, a message to the class, free
 * typing, end), Answers (by student; essays marked), Report (CSV and JSON), Connections (an LRS for xAPI).
 * Classes and LRS connections are kept in this viewer's per-account setting WolfAdventureTeacher because the
 * spec (BRAIN_API_V1.md §5.5) has no class_list or lrs_list action: a class made in another viewer is not listed.
 */
class WolfAdventureTeacherView : public WolfAdventureDeskView
{
public:
    // Own empty param block - see WolfAdventureGmView::Params (wolffloateradventuregm.h).
    struct Params : public LLInitParam::Block<Params, WolfAdventureDeskView::Params>
    {
        Params() {}
    };
    WolfAdventureTeacherView(const Params& p);
    void onOpened(const LLSD& key);

protected:
    void face(F32 W, F32 H, F32 top, F32 bottom, const WolfGameDraw::Palette& p) override;
    void press(S32 id, S32 arg) override;
    void fieldEnter(S32 field) override;
    std::string titleWord() const override { return "TEACHER CONSOLE"; }
    std::string titleText() const override;
    size_t fieldCap(S32 field) const override;
    bool fieldMasked(S32 field) const override { return field == F_LRS_SECRET; }
    void tick() override;

private:
    enum EPage { P_CLASSES = 0, P_LIVE, P_ANSWERS, P_REPORT, P_CONNECTIONS, P_COUNT };
    enum EHit
    {
        H_PAGE = DH_FIRST, H_CLASS_NEXT, H_CLASS_FORGET,
        // Classes
        H_NEW_ADV, H_NEW_TEAMS, H_NEW_FREE, H_CREATE, H_COPY_CODE, H_OPEN_CLASS,
        // Live
        H_PAUSE, H_RESUME, H_GATHER_SPOT, H_GATHER, H_QUIZ_PICK, H_ASK, H_MESSAGE, H_FREE, H_END,
        // Answers / Report
        H_LOAD_ANSWERS, H_MARK, H_REPORT_LOAD, H_SAVE_CSV, H_SAVE_JSON, H_CONVO,
        // Connections
        H_LRS_VERSION, H_LRS_ADD, H_LRS_TEST, H_LRS_REMOVE
    };
    enum EField
    {
        F_NONE = 0, F_NAME = 1, F_LANGUAGE, F_SPOT, F_QUIZ, F_MESSAGE, F_POINTS, F_LRS_ENDPOINT, F_LRS_KEY, F_LRS_SECRET
    };

    F32 pageClasses(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    F32 pageLive(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    F32 pageAnswers(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    F32 pageReport(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    F32 pageConnections(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);

    void loadAdventures();
    void loadClass(bool force);
    void loadPack();
    void loadReport(const std::string& format, bool save);
    void gmOp(const std::string& op, const LLSD& fields, const std::string& doing);
    void remember(const char* list, const LLSD& entry, const char* key);
    void forget(const char* list, const std::string& id, const char* key);
    const LLSD& remembered(const char* list) const;
    std::string selectedClass() const;
    /** The quizzes / spots to pick from: class_get's when it has them, else the pack's when it could be read. */
    LLSD quizList() const;
    LLSD spotList() const;
    std::string studentName(const std::string& agent) const;
    void saveFile(const std::string& name, const std::string& content);

    EPage mPage = P_CLASSES;
    LLSD mAdvs;                     // list_mine, published
    S32 mNewAdv = 0;
    S32 mNewTeams = 0;              // 0 = one party for the class, else 2..12 (wag_class_settings_in)
    bool mNewFree = false;
    std::string mNewCode;           // the code of the class just made, shown large
    S32 mClassSel = 0;              // in the remembered classes
    mutable LLSD mRemembered;       // the per-account SETTING, read once (remembered), refreshed by remember / forget
    mutable bool mRememberedLoaded = false;
    LLSD mClass;                    // class_get: the class model
    std::string mClassFor;          // the class mClass is
    F64 mClassAt = 0.0;
    bool mClassLoading = false;
    LLSD mState;                    // gm_state of the class session (parties, control)
    LLSD mPack;                     // get_revision when the teacher may read it (owner or GM), for quizzes and spots
    std::string mPackFor;
    S32 mSpot = 0, mQuiz = 0;       // picks from the pack's lists (when it could be read)
    LLSD mAnswers;                  // class_report json: answers
    LLSD mReportRows;               // class_report json: the class model's students (the table)
    S32 mLrsVersion = 0;            // 2.0.0 | 1.0.3 (wolfadventure_gm_lib.php wa_api_lrs_add)
    std::string mLrsResult;
};

class WolfFloaterAdventureTeacher : public LLFloater
{
public:
    WolfFloaterAdventureTeacher(const LLSD& key);
    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;

private:
    WolfAdventureTeacherView* mView = nullptr;
};

#endif // WOLF_FLOATER_ADVENTURE_TEACHER_H
