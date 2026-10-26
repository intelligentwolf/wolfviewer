/**
 * @file wolffloaterdob.cpp
 * @brief WolfViewer: Your Date of Birth. See wolffloaterdob.h.
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

#include "llviewerprecompiledheaders.h"

#include "wolffloaterdob.h"

#include <ctime>

#include <boost/json.hpp>

#include "llagent.h"
#include "llbutton.h"
#include "llcombobox.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "llfloaterreg.h"
#include "llframetimer.h"
#include "llhttpconstants.h"
#include "llsdjson.h"
#include "llstartup.h"
#include "lltextbox.h"
#include "lltexteditor.h"
#include "lluri.h"
#include "llweb.h"
#include "wolfgrid.h"

namespace
{
    // gridmanager/viewerdob.php on the grid website (the rules: gridmanager/doblib.php, shared with the website page
    // index.php?f=dateofbirth). Terms section 1 is terms.php's id="eligibility".
    const char* DOB_API = "https://www.wolf-grid.com/viewerdob.php";
    const char* TERMS_URL = "https://www.wolf-grid.com/index.php?f=terms#eligibility";
    const char* TICKETS_URL = "https://www.wolf-grid.com/index.php?f=tk";
    // signuplib.php WT_SIGNUP_MAX_AGE: the oldest year offered (the server checks the exact date again)
    const int MAX_AGE_YEARS = 120;
    // After login: wait for the world to settle before asking; on a failed check try again a little later, at most this
    // many times a login (the next login asks again anyway).
    const F64 FIRST_ASK_S = 20.0;
    const F64 RETRY_S = 60.0;
    const S32 MAX_TRIES = 3;

    const char* const MONTHS[12] = { "January", "February", "March", "April", "May", "June",
                                     "July", "August", "September", "October", "November", "December" };

    // Source: wolfviewerkey.cpp json_to_llsd (the helper every Wolf service reader has)
    LLSD json_to_llsd(const LLSD::Binary& bytes)
    {
        std::string text(bytes.begin(), bytes.end());
        boost::system::error_code ec;
        boost::json::value v = boost::json::parse(text, ec);
        if (ec) return LLSD();
        return LlsdFromJson(v);
    }

    std::string json_string(const LLSD& v, const char* key)
    {
        return v.isMap() && v.has(key) ? v[key].asString() : std::string();
    }

    /** POST action (+ dob) to viewerdob.php with this login's agent and session. Returns the JSON reply as LLSD (an
     *  empty LLSD when there was none) and the HTTP status. Runs in a coroutine. */
    LLSD post_dob(const std::string& action, const std::string& dob, LLCore::HttpStatus& status)
    {
        LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
            std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfDob", LLCore::HttpRequest::DEFAULT_POLICY_ID);
        LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
        // Source: wolfviewerkey.cpp fetchCoro - verified TLS, no redirects (the session id is in the headers)
        LLCore::HttpOptions::ptr_t options = WolfGrid::makeVerifiedHttpOptions();
        options->setTimeout(30);
        LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
        headers->append(HTTP_OUT_HEADER_ACCEPT, "application/json");
        headers->append(HTTP_OUT_HEADER_CONTENT_TYPE, "application/x-www-form-urlencoded");
        headers->append("X-Wolf-Agent", gAgentID.asString());
        headers->append("X-Wolf-Session", gAgentSessionID.asString());
        std::string body = "action=" + LLURI::escape(action);
        if (!dob.empty()) body += "&dob=" + LLURI::escape(dob);
        LLCore::BufferArray::ptr_t raw(new LLCore::BufferArray());
        raw->append(body.data(), body.size());
        LLSD result = adapter->postRawAndSuspend(request, DOB_API, raw, options, headers);
        status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
        LLSD reply;
        if (result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW))
            reply = json_to_llsd(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary());
        return reply;
    }

    // WolfDob::idle state: one check per login (session id)
    LLUUID sAskedSession;
    S32 sTries = 0;
    F64 sNextAsk = 0.0;
    bool sAsking = false;
    bool sSettled = false;   // this login has its answer (a date on the account, the window shown, or not a resident)

    void statusCoro(LLUUID session)
    {
        LLCore::HttpStatus status;
        LLSD reply = post_dob("status", std::string(), status);
        sAsking = false;
        if (session != gAgentSessionID) return;   // a new login since: it asks for itself
        if (reply.isMap() && reply["success"].asBoolean())
        {
            sSettled = true;
            if (!reply["has_dob"].asBoolean())
            {
                LL_INFOS("WolfDob") << "this account has no date of birth: asking" << LL_ENDL;
                LLFloaterReg::showInstance("wolf_dob", LLSD("login"));
            }
            return;
        }
        // 401/403 (a visitor from another grid, a closed account): never ask this login; anything else: try later
        // Source: httpcommon.h - an HTTP answer's code is the status type (100-999, isHttpStatus)
        const int code = status.isHttpStatus() ? static_cast<int>(status.getType()) : 0;
        if (code == 401 || code == 403)
        {
            sSettled = true;
            return;
        }
        sNextAsk = LLFrameTimer::getElapsedSeconds() + RETRY_S;
        LL_WARNS("WolfDob") << "date of birth check failed (" << status.toString() << " " << json_string(reply, "error")
                            << "); trying again in " << RETRY_S << " s" << LL_ENDL;
    }
}

void WolfDob::idle()
{
    // Source: wolfviewerkey.cpp WolfViewerKey::idle - logged in and in a region
    if (LLStartUp::getStartupState() < STATE_STARTED || !gAgent.getRegion())
    {
        return;
    }
    // A Wolf Territories LOGIN (wherever the avatar is now): the session id only ever goes to our own website
    if (!WolfGrid::isWolfTerritories())
    {
        return;
    }
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (gAgentSessionID != sAskedSession)
    {
        // a new login (llstartup.cpp sets a new session id): "Not now" last time means asking again now
        sAskedSession = gAgentSessionID;
        sTries = 0;
        sSettled = false;
        sNextAsk = now + FIRST_ASK_S;
    }
    if (sSettled || sAsking || sTries >= MAX_TRIES || now < sNextAsk)
    {
        return;
    }
    sAsking = true;
    ++sTries;
    const LLUUID session = gAgentSessionID;
    LLCoros::instance().launch("WolfDobStatus", [session]() { statusCoro(session); });
}

WolfFloaterDob::WolfFloaterDob(const LLSD& key) : LLFloater(key)
{
}

bool WolfFloaterDob::postBuild()
{
    mDay = getChild<LLComboBox>("day");
    mMonth = getChild<LLComboBox>("month");
    mYear = getChild<LLComboBox>("year");
    mCheckText = getChild<LLTextBox>("check_text");
    mStatus = getChild<LLTextEditor>("status");
    mNotNow = getChild<LLButton>("not_now");
    mContinue = getChild<LLButton>("continue");
    mChange = getChild<LLButton>("change");
    mSave = getChild<LLButton>("save");
    mClose = getChild<LLButton>("close_btn");

    mDay->add("Day", LLSD(std::string()));
    for (int d = 1; d <= 31; ++d) mDay->add(llformat("%d", d), LLSD(llformat("%02d", d)));
    mMonth->add("Month", LLSD(std::string()));
    for (int m = 0; m < 12; ++m) mMonth->add(MONTHS[m], LLSD(llformat("%02d", m + 1)));
    mYear->add("Year", LLSD(std::string()));
    const std::time_t t = std::time(nullptr);
    std::tm utc {};
#if LL_WINDOWS
    gmtime_s(&utc, &t);
#else
    gmtime_r(&t, &utc);
#endif
    const int thisYear = utc.tm_year + 1900;
    for (int y = thisYear; y >= thisYear - MAX_AGE_YEARS; --y) mYear->add(llformat("%d", y), LLSD(llformat("%d", y)));
    mDay->selectFirstItem();
    mMonth->selectFirstItem();
    mYear->selectFirstItem();

    mNotNow->setCommitCallback([this](LLUICtrl*, const LLSD&) { closeFloater(); });
    mContinue->setCommitCallback([this](LLUICtrl*, const LLSD&) { onContinue(); });
    mChange->setCommitCallback([this](LLUICtrl*, const LLSD&) { setStatus(std::string(), EStatus::INFO); showStep(EStep::ENTER); });
    mSave->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSave(); });
    mClose->setCommitCallback([this](LLUICtrl*, const LLSD&) { closeFloater(); });
    getChild<LLButton>("terms")->setCommitCallback([](LLUICtrl*, const LLSD&) { LLWeb::loadURLExternal(TERMS_URL); });
    getChild<LLButton>("tickets")->setCommitCallback([](LLUICtrl*, const LLSD&) { LLWeb::loadURLExternal(TICKETS_URL); });
    showStep(EStep::LOADING);
    return true;
}

void WolfFloaterDob::onOpen(const LLSD& key)
{
    LLFloater::onOpen(key);
    if (mBusy) return;
    setStatus(std::string(), EStatus::INFO);
    if (!WolfGrid::isWolfTerritories())
    {
        showStep(EStep::DONE);
        setStatus("Your date of birth is for Wolf Territories residents. Log in to Wolf Territories to add it.", EStatus::ERR);
        return;
    }
    if (key.asString() == "login")
    {
        showStep(EStep::ENTER);   // WolfDob::idle has just been told there is none
        return;
    }
    showStep(EStep::LOADING);
    send("status", std::string());
}

void WolfFloaterDob::showStep(EStep step)
{
    const bool enter = step == EStep::ENTER, check = step == EStep::CHECK, done = step == EStep::DONE;
    for (const char* n : { "day_label", "month_label", "year_label", "day", "month", "year", "enter_hint" })
        getChild<LLUICtrl>(n)->setVisible(enter);
    mNotNow->setVisible(enter);
    mContinue->setVisible(enter);
    mCheckText->setVisible(check);
    getChild<LLUICtrl>("check_hint")->setVisible(check);
    mChange->setVisible(check);
    mSave->setVisible(check);
    mClose->setVisible(done || step == EStep::LOADING);
    getChild<LLUICtrl>("tickets")->setVisible(done);
    getChild<LLUICtrl>("loading")->setVisible(step == EStep::LOADING);
    if (enter) mDay->setFocus(true);
    if (check) mSave->setFocus(true);
}

void WolfFloaterDob::setStatus(const std::string& text, EStatus kind)
{
    // Source: wolffloaterbugreport.cpp setStatus - light red / light green on the dark floater, and the words say it too
    static const LLUIColor err(LLColor4(1.f, 0.6f, 0.6f, 1.f));
    static const LLUIColor ok(LLColor4(0.79f, 1.f, 0.79f, 1.f));
    static const LLUIColor info(LLColor4::white);
    const LLUIColor& c = kind == EStatus::ERR ? err : kind == EStatus::OK ? ok : info;
    mStatus->setReadOnlyColor(c);
    mStatus->setColor(c);
    mStatus->setText(text);
    mStatus->setVisible(!text.empty());
}

std::string WolfFloaterDob::pickedDate() const
{
    const std::string d = mDay->getValue().asString(), m = mMonth->getValue().asString(), y = mYear->getValue().asString();
    if (d.empty() || m.empty() || y.empty()) return std::string();
    return y + "-" + m + "-" + d;
}

void WolfFloaterDob::onContinue()
{
    const std::string dob = pickedDate();
    if (dob.empty())
    {
        setStatus("Choose the day, the month and the year you were born.", EStatus::ERR);
        return;
    }
    send("check", dob);
}

void WolfFloaterDob::onSave()
{
    if (mChecked.empty())
    {
        showStep(EStep::ENTER);
        return;
    }
    send("set", mChecked);
}

void WolfFloaterDob::send(const std::string& action, const std::string& dob)
{
    mBusy = true;
    mContinue->setEnabled(false);
    mSave->setEnabled(false);
    if (action != "status") setStatus(action == "set" ? "Saving..." : "Checking...", EStatus::INFO);
    LLHandle<LLFloater> handle = getHandle();
    LLCoros::instance().launch("WolfDob", [handle, action, dob]() { sendCoro(handle, action, dob); });
}

// static
void WolfFloaterDob::sendCoro(LLHandle<LLFloater> handle, std::string action, std::string dob)
{
    LLCore::HttpStatus status;
    LLSD reply = post_dob(action, dob, status);
    std::string error;
    if (!reply.isMap() || !reply["success"].asBoolean())
    {
        error = json_string(reply, "error");
        if (error.empty())
        {
            error = "The website did not answer properly (" + status.toString() + "). Nothing was saved. Please try again "
                    "in a moment, or add it on wolf-grid.com: Control Panel > Date of Birth.";
        }
        LL_WARNS("WolfDob") << action << ": " << status.toString() << " " << error << LL_ENDL;
    }
    WolfFloaterDob* self = static_cast<WolfFloaterDob*>(handle.get());
    if (!self) return;
    self->onReply(action, reply, error);
}

void WolfFloaterDob::onReply(const std::string& action, const LLSD& reply, const std::string& error)
{
    mBusy = false;
    mContinue->setEnabled(true);
    mSave->setEnabled(true);
    if (action == "status")
    {
        if (!error.empty())
        {
            showStep(EStep::DONE);
            setStatus(error, EStatus::ERR);
        }
        else if (reply["has_dob"].asBoolean())
        {
            showStep(EStep::DONE);
            setStatus("Your date of birth is already on your account. If it's wrong, open a support ticket and our "
                      "support team will correct it.", EStatus::INFO);
        }
        else
        {
            showStep(EStep::ENTER);
        }
        return;
    }
    if (!error.empty())
    {
        // a date the server will not take stays on the enter step, next to the boxes; anything else says so here
        if (action == "set" && json_string(reply, "field").empty()) showStep(EStep::CHECK);
        else showStep(EStep::ENTER);
        setStatus(error, EStatus::ERR);
        return;
    }
    if (action == "check")
    {
        mChecked = json_string(reply, "dob");
        const S32 age = reply["age"].asInteger();
        mCheckText->setText(llformat("You were born on %s, so you're %d. Is that right?", json_string(reply, "words").c_str(), age));
        setStatus(std::string(), EStatus::INFO);
        showStep(EStep::CHECK);
        return;
    }
    // set
    const std::string state = json_string(reply, "state");
    showStep(EStep::DONE);
    if (state == "saved")
    {
        setStatus(json_string(reply, "message") + " (" + json_string(reply, "words") + ")", EStatus::OK);
    }
    else if (state == "closed")
    {
        setStatus(json_string(reply, "message"), EStatus::ERR);
    }
    else
    {
        setStatus(json_string(reply, "message"), EStatus::INFO);
    }
}
