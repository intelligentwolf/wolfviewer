/**
 * @file wolffloaterchooseavatar.cpp
 * @brief WolfViewer: Choose an Avatar. See wolffloaterchooseavatar.h.
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

#include "wolffloaterchooseavatar.h"

#include <boost/json.hpp>

#include "apr_base64.h"
#include "llagent.h"
#include "llagentwearables.h"
#include "llappearancemgr.h"
#include "llbutton.h"
#include "llcorehttputil.h"
#include "lldraghandle.h"
#include "llcoros.h"
#include "lleventcoro.h"   // llcoro::suspendUntilTimeout (lleventcoro.h:85)
#include "llgl.h"
#include "llimage.h"
#include "llinventorymodel.h"
#include "llpanel.h"
#include "llrender2dutils.h"
#include "llsdjson.h"
#include "lltextbox.h"
#include "lltexteditor.h"
#include "lluictrlfactory.h"
#include "llviewercontrol.h"   // gSavedPerAccountSettings
#include "llviewerinventory.h"
#include "llviewertexture.h"
#include "llvoavatarself.h"
#include "wolfgrid.h"
#include "wolfpaint.h"

using namespace WolfPaint;

namespace
{
    // gridmanager/starteravatarsapi.php on the grid website (as viewerbugreport.php: wolffloaterbugreport.cpp REPORT_API)
    const char* LOOKS_API = "https://www.wolf-grid.com/starteravatarsapi.php";
    const char* const KINDS[3] = { "people", "animal", "scifi" };     // starteravatarslib.php SA_KINDS, in its order
    // The picture: llfloaterwolfavatars.cpp:70-71 PICTURE_B64_MAX / PICTURE_SIDE_MAX (starteravatarsapi.php sends at most 2 MB)
    const size_t PICTURE_B64_MAX = 4u * 1024u * 1024u;
    const S32 PICTURE_SIDE_MAX = 2048;
    const F32 FOLDER_WAIT_SECS = 45.f;     // the new folder to reach the inventory after the Clothing folder is asked again
    const F32 WEAR_WAIT_SECS = 90.f;       // every item of the look to be worn (attachments come back from the region)
    const F32 POLL_SECS = 0.5f;

    // The night the window is painted in, and its ink. Contrast on PLATE (#141B2B): CREAM 15:1, SOFT 8.9:1, AMBER 9.6:1;
    // INK on AMBER 11:1; ERR_INK on PLATE 7.6:1 (WCAG 1.4.3 needs 4.5:1).
    const LLColor4 SKY_TOP(0.035f, 0.055f, 0.110f, 1.f);
    const LLColor4 SKY_BOTTOM(0.090f, 0.120f, 0.200f, 1.f);
    const LLColor4 PLATE(0.078f, 0.106f, 0.169f, 1.f);
    const LLColor4 PLATE_TOP(0.110f, 0.145f, 0.220f, 1.f);
    const LLColor4 SILVER(0.66f, 0.72f, 0.82f, 1.f);
    const LLColor4 CREAM(0.97f, 0.95f, 0.88f, 1.f);
    const LLColor4 SOFT(0.78f, 0.80f, 0.86f, 1.f);
    const LLColor4 AMBER(1.00f, 0.74f, 0.25f, 1.f);
    const LLColor4 INK(0.06f, 0.05f, 0.03f, 1.f);
    const LLColor4 ERR_INK(1.f, 0.62f, 0.60f, 1.f);
    const LLColor4 MOON(0.93f, 0.93f, 0.86f, 1.f);
    const LLColor4 SHADOW(0.f, 0.f, 0.f, 0.35f);

    std::string json_string(const LLSD& v, const char* key)
    {
        return v.isMap() && v.has(key) ? v[key].asString() : std::string();
    }

    /** {mime, data: base64} -> a texture. The same steps as llfloaterwolfavatars.cpp:706-733 pictureTexture (apr base64,
     *  LLImageFormatted::loadFromMemory, decode, LLViewerTextureManager::getLocalTexture), for a JPEG or a PNG. */
    bool pictureTexture(const std::string& mime, const std::string& b64, LLPointer<LLViewerTexture>& out, std::string& why)
    {
        out = nullptr;
        if (mime != "image/jpeg" && mime != "image/png") { why = "not a picture WolfViewer shows"; return false; }
        if (b64.empty() || b64.size() > PICTURE_B64_MAX) { why = "the picture was empty or too large"; return false; }
        const int max_len = apr_base64_decode_len(b64.c_str());
        if (max_len <= 0) { why = "the picture could not be read"; return false; }
        std::vector<U8> bytes((size_t)max_len);
        const int len = apr_base64_decode_binary(bytes.data(), b64.c_str());
        if (len <= 0 || len > max_len) { why = "the picture could not be read"; return false; }
        bytes.resize((size_t)len);
        LLPointer<LLImageFormatted> img = LLImageFormatted::loadFromMemory(bytes.data(), (U32)bytes.size(), mime);
        if (img.isNull() || img->getWidth() <= 0 || img->getHeight() <= 0 || img->getWidth() > PICTURE_SIDE_MAX || img->getHeight() > PICTURE_SIDE_MAX)
        {
            why = "the picture could not be read";
            return false;
        }
        LLPointer<LLImageRaw> raw = new LLImageRaw;
        if (!img->decode(raw, 0.f) || raw->isBufferInvalid() || raw->getWidth() <= 0 || raw->getHeight() <= 0)
        {
            why = "the picture could not be decoded";
            return false;
        }
        out = LLViewerTextureManager::getLocalTexture(raw.get(), false);
        if (out.isNull()) { why = "the picture could not be put on screen"; return false; }
        return true;
    }

    /** Ask the inventory service for folder `id` again, so what the website just wrote into it shows (as
     *  llfloaterwolfavatars.cpp:1580-1587 does for Objects after a WolfAvatars delivery). */
    void refetchFolder(const LLUUID& id)
    {
        if (LLViewerInventoryCategory* cat = id.notNull() ? gInventory.getCategory(id) : nullptr)
        {
            cat->setVersion(LLViewerInventoryCategory::VERSION_UNKNOWN);
            gInventory.fetchDescendentsOf(id);
        }
    }
}

/**
 * One look: its picture in a silver frame, its name below. A click, or Enter / Space with the keyboard on it (a tab
 * stop), chooses it; the chosen one has an amber frame and a "Chosen" plaque with a tick (never colour alone).
 */
class WolfStarterCard : public LLUICtrl
{
public:
    explicit WolfStarterCard(const LLUICtrl::Params& p) : LLUICtrl(p) {}

    std::function<void()> mOnPick;
    const WolfFloaterChooseAvatar::Look* mLook = nullptr;
    bool mSelected = false;
    std::string mChosenWord;

    bool handleMouseDown(S32 x, S32 y, MASK mask) override
    {
        if (!getEnabled() || !mLook) return true;
        setFocus(true);
        if (mOnPick) mOnPick();
        return true;
    }
    bool handleKeyHere(KEY key, MASK mask) override
    {
        if (mask == MASK_NONE && (key == KEY_RETURN || key == ' '))
        {
            if (getEnabled() && mLook && mOnPick) mOnPick();
            return true;
        }
        return LLUICtrl::handleKeyHere(key, mask);
    }
    void draw() override
    {
        const S32 w = getRect().getWidth(), h = getRect().getHeight();
        if (!mLook || w < 40 || h < 40) return;
        LLGLSUIDefault ui;
        untextured();
        const LLRect outer(0, h, w, 0);
        LLRect sh = outer;
        sh.translate(2, -3);
        fillRounded(sh, allCorners(8.f), SHADOW, SHADOW);
        fillRounded(outer, allCorners(8.f), PLATE_TOP, PLATE);
        const bool hover = getEnabled() && mouseOver(this);
        const LLColor4 rim = mSelected ? AMBER : (hasFocus() ? CREAM : (hover ? withAlpha(AMBER, 0.7f) : withAlpha(SILVER, 0.55f)));
        strokeRounded(outer, allCorners(8.f), 0.f, mSelected ? 3.f : 1.5f, rim, rim);

        // the picture fills the card above a 30 px name strip
        const S32 strip = 30;
        const LLRect pic(6, h - 6, w - 6, strip);
        if (mLook->mPictureState == 2 && mLook->mTexture.notNull() && mLook->mTexture->getWidth() > 0)
        {
            // cover the frame: the picture's middle square (the pictures are square or tall), cut by uv so it never stretches
            LLViewerTexture* tex = mLook->mTexture.get();
            const F32 tw = (F32)tex->getWidth(), th = (F32)tex->getHeight();
            const F32 fa = (F32)pic.getWidth() / (F32)llmax(1, pic.getHeight());
            F32 u0 = 0.f, u1 = 1.f, v0 = 0.f, v1 = 1.f;
            if (tw / th > fa) { const F32 k = fa * th / tw; u0 = 0.5f - k * 0.5f; u1 = 0.5f + k * 0.5f; }
            else { const F32 k = (tw / fa) / th; v1 = 1.f; v0 = 1.f - k; }   // keep the top (faces), drop the feet end
            gGL.color4f(1.f, 1.f, 1.f, 1.f);
            gl_draw_scaled_image(pic.mLeft, pic.mBottom, pic.getWidth(), pic.getHeight(), tex, LLColor4::white, LLRectf(u0, v1, u1, v0));
            untextured();
        }
        else
        {
            fillRounded(pic, allCorners(5.f), withAlpha(SKY_BOTTOM, 1.f), withAlpha(SKY_TOP, 1.f));
            const std::string words = mLook->mPictureState == 3 ? "No picture (" + mLook->mPictureWhy + ")" : std::string("Loading the picture...");
            LLFontGL::getFontSansSerifSmall()->renderUTF8(words, 0, (F32)pic.getCenterX(), (F32)pic.getCenterY(),
                                                         mLook->mPictureState == 3 ? ERR_INK : SOFT, LLFontGL::HCENTER, LLFontGL::VCENTER,
                                                         LLFontGL::NORMAL, LLFontGL::NO_SHADOW, S32_MAX, pic.getWidth() - 10, nullptr, true);
        }
        text(LLFontGL::getFontSansSerifBold(), mLook->mName, (F32)w * 0.5f, strip * 0.5f + 1.f, CREAM, LLFontGL::HCENTER, (F32)w - 16.f);
        if (mSelected)
        {
            // the plaque: tick + word, INK on AMBER (11:1)
            const LLFontGL* f = LLFontGL::getFontSansSerifBold();
            const F32 lh = (F32)f->getLineHeight();
            const F32 pw = f->getWidthF32(mChosenWord) + lh * 1.6f;
            const LLRect pr(10, h - 10, 10 + (S32)pw, h - 10 - (S32)(lh + 8.f));
            fillRounded(pr, allCorners(lh * 0.5f + 4.f), lighten(AMBER, 0.08f), AMBER);
            checkMark(pr.mLeft + lh * 0.55f, (F32)pr.getCenterY(), lh * 0.6f, llmax(1.5f, lh * 0.13f), INK);
            text(f, mChosenWord, pr.mLeft + lh * 1.05f, (F32)pr.getCenterY(), INK, LLFontGL::LEFT);
        }
        untextured();
        LLUICtrl::draw();
    }
};

/** Paints the night behind every control; not mouse-opaque, so the window still drags by its sky
 *  (llfloaterwolfavatars.cpp:861-882 WolfAvatarsBackdrop, wolffloaterradio.cpp:499-591). */
class WolfChooseAvatarBackdrop : public LLView
{
public:
    struct Params : public LLInitParam::Block<Params, LLView::Params>
    {
        Params() {}
    };
    WolfChooseAvatarBackdrop(const Params& p, WolfFloaterChooseAvatar* owner) : LLView(p), mOwner(owner) {}
    void draw() override { mOwner->drawBackdrop(); }

private:
    WolfFloaterChooseAvatar* mOwner;
};

WolfFloaterChooseAvatar::WolfFloaterChooseAvatar(const LLSD& key) : LLFloater(key)
{
}

WolfFloaterChooseAvatar::~WolfFloaterChooseAvatar()
{
}

bool WolfFloaterChooseAvatar::postBuild()
{
    mCardsPanel = getChild<LLPanel>("cards");
    mEmpty = getChild<LLTextBox>("cards_empty");
    mChosenName = getChild<LLTextBox>("chosen_name");
    mChosenBlurb = getChild<LLTextBox>("chosen_blurb");
    mReplaceNote = getChild<LLTextBox>("replace_note");
    mStatus = getChild<LLTextEditor>("status");
    mWear = getChild<LLButton>("wear_btn");
    mWear->setCommitCallback([this](LLUICtrl*, const LLSD&) { onWear(); });
    for (S32 k = 0; k < 3; ++k)
    {
        mKindBtn[k] = getChild<LLButton>(std::string("kind_") + KINDS[k]);
        const std::string kind = KINDS[k];
        mKindBtn[k]->setCommitCallback([this, kind](LLUICtrl*, const LLSD&) { showKind(kind); });
    }

    // 3 x 2 cards in the "cards" panel (llfloaterwolfavatars.cpp:977-996: drawn controls made with LLUICtrlFactory::create)
    const S32 cols = 3, rows = 2, gap = 12;
    const LLRect area = mCardsPanel->getLocalRect();
    const S32 cw = (area.getWidth() - gap * (cols - 1)) / cols;
    const S32 ch = (area.getHeight() - gap * (rows - 1)) / rows;
    for (S32 i = 0; i < CARDS; ++i)
    {
        const S32 c = i % cols, r = i / cols;
        LLUICtrl::Params pp;
        pp.name = llformat("card_%d", i);
        const S32 left = c * (cw + gap), top = area.getHeight() - r * (ch + gap);
        pp.rect = LLRect(left, top, left + cw, top - ch);
        pp.follows.flags = FOLLOWS_LEFT | FOLLOWS_TOP;
        pp.tab_stop = true;
        pp.mouse_opaque = true;
        mCards[i] = LLUICtrlFactory::create<WolfStarterCard>(pp);
        mCards[i]->mChosenWord = getString("picked");
        const S32 n = i;
        mCards[i]->mOnPick = [this, n]() { if (n < (S32)mShown.size()) select(mShown[n]); };
        mCards[i]->setVisible(false);
        mCardsPanel->addChild(mCards[i]);
    }

    WolfChooseAvatarBackdrop::Params bp;
    bp.name("wolf_choose_avatar_backdrop");
    bp.rect(getLocalRect());
    bp.follows.flags(FOLLOWS_ALL);
    bp.mouse_opaque(false);
    mBackdrop = new WolfChooseAvatarBackdrop(bp, this);
    addChild(mBackdrop);
    sendChildToBack(mBackdrop);
    if (getDragHandle()) sendChildToBack(getDragHandle());

    showKind("people");
    refreshChosen();
    return true;
}

void WolfFloaterChooseAvatar::onOpen(const LLSD& key)
{
    LLFloater::onOpen(key);
    if (!WolfGrid::isWolfTerritories())
    {
        setStatus("Choose an Avatar is for Wolf Territories residents: log in to Wolf Territories to use it.", EStatus::ERR);
        mWear->setEnabled(false);
        return;
    }
    if (!mListAsked) requestList();
}

void WolfFloaterChooseAvatar::drawBackdrop()
{
    const S32 w = getRect().getWidth(), h = getRect().getHeight();
    LLGLSUIDefault ui;
    untextured();
    box(0.f, (F32)h, (F32)w, 0.f, SKY_TOP, SKY_BOTTOM);
    // a few fixed stars and the moon over the group buttons; nothing moves (no needless redraw work)
    for (S32 i = 0; i < 40; ++i)
    {
        const F32 x = hash01((F32)i * 7.13f) * w, y = h - 20.f - hash01((F32)i * 3.71f + 1.f) * 90.f;
        disc(x, y, 0.6f + hash01((F32)i) * 0.9f, withAlpha(CREAM, 0.55f), withAlpha(CREAM, 0.f));
    }
    disc((F32)w - 70.f, (F32)h - 70.f, 26.f, MOON, lerpColour(MOON, SILVER, 0.4f));
    halo(LLRect(w - 96, h - 44, w - 44, h - 96), 26.f, 14, withAlpha(MOON, 0.35f));
    // the plate behind the chosen look's words and the status
    const LLRect plateR(10, h - 446, w - 10, 40);
    fillRounded(plateR, allCorners(8.f), withAlpha(PLATE_TOP, 0.92f), withAlpha(PLATE, 0.92f));
    strokeRounded(plateR, allCorners(8.f), 0.f, 1.f, withAlpha(SILVER, 0.5f), withAlpha(SILVER, 0.3f));
    untextured();
}

void WolfFloaterChooseAvatar::setStatus(const std::string& text, EStatus kind)
{
    // Light red / light green / white on the dark plate, and the words say it too ("Not changed", "done") - never colour
    // alone; a read-only text editor, so an error can be selected and copied (wolffloaterbugreport.cpp setStatus)
    static const LLUIColor err(ERR_INK);
    static const LLUIColor ok(LLColor4(0.79f, 1.f, 0.79f, 1.f));
    static const LLUIColor info(LLColor4::white);
    const LLUIColor& c = kind == EStatus::ERR ? err : kind == EStatus::OK ? ok : info;
    mStatus->setReadOnlyColor(c);
    mStatus->setColor(c);
    mStatus->setText(text);
    mStatus->setVisible(!text.empty());
}

void WolfFloaterChooseAvatar::setBusy(bool busy)
{
    mBusy = busy;
    for (WolfStarterCard* c : mCards) c->setEnabled(!busy);
    for (LLButton* b : mKindBtn) b->setEnabled(!busy);
    refreshChosen();
}

WolfFloaterChooseAvatar::Look* WolfFloaterChooseAvatar::lookById(S32 id)
{
    for (Look& l : mLooks) if (l.mId == id) return &l;
    return nullptr;
}

// static
void WolfFloaterChooseAvatar::apiCoro(LLHandle<LLFloater> handle, LLSD body,
                                      std::function<void(WolfFloaterChooseAvatar*, const LLSD&, const std::string&)> done)
{
    // POST JSON with this login's agent and session ids, as wolfadventure.cpp WolfAdventure::api (LlsdToJson, boost::json)
    // and wolffloaterbugreport.cpp sendCoro (verified TLS, no redirects: WolfGrid::makeVerifiedHttpOptions)
    const std::string json = boost::json::serialize(LlsdToJson(body));
    LLCore::BufferArray::ptr_t raw(new LLCore::BufferArray());
    raw->append(json.data(), json.size());
    LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
        std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfChooseAvatar", LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
    LLCore::HttpOptions::ptr_t options = WolfGrid::makeVerifiedHttpOptions();
    options->setTimeout(60);
    options->setRetries(0);
    LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
    headers->append(HTTP_OUT_HEADER_ACCEPT, "application/json");
    headers->append(HTTP_OUT_HEADER_CONTENT_TYPE, "application/json");
    headers->append("X-Wolf-Agent", gAgentID.asString());
    headers->append("X-Wolf-Session", gAgentSessionID.asString());
    LLSD result = adapter->postRawAndSuspend(request, LOOKS_API, raw, options, headers);

    LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
    LLSD reply;
    if (result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW))
    {
        const LLSD::Binary& bin = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary();
        boost::system::error_code ec;
        boost::json::value v = boost::json::parse(std::string(bin.begin(), bin.end()), ec);
        if (!ec) reply = LlsdFromJson(v);
    }
    std::string error;
    if (!reply.isMap() || !reply["success"].asBoolean())
    {
        error = json_string(reply, "error");
        if (error.empty()) error = "the website did not answer properly (" + status.toString() + "). Please try again in a moment.";
        LL_WARNS("WolfChooseAvatar") << body["action"].asString() << ": " << status.toString() << " " << error << LL_ENDL;
    }
    WolfFloaterChooseAvatar* self = static_cast<WolfFloaterChooseAvatar*>(handle.get());
    if (self) done(self, reply, error);
}

void WolfFloaterChooseAvatar::requestList()
{
    mListAsked = true;
    mEmpty->setText(std::string("Loading the looks..."));
    mEmpty->setVisible(true);
    LLSD body;
    body["action"] = "list";
    const LLHandle<LLFloater> handle = getHandle();
    LLCoros::instance().launch("WolfChooseAvatarList", [handle, body]()
    {
        apiCoro(handle, body, [](WolfFloaterChooseAvatar* self, const LLSD& reply, const std::string& error) { self->onList(reply, error); });
    });
}

void WolfFloaterChooseAvatar::onList(const LLSD& reply, const std::string& error)
{
    if (!error.empty())
    {
        mListAsked = false;   // close and open the window to ask again
        mEmpty->setText("The looks could not be loaded: " + error + " Close this window and open it again to retry.");
        mEmpty->setVisible(true);
        return;
    }
    mLooks.clear();
    mSelected = -1;
    for (const LLSD& s : llsd::inArray(reply["starters"]))
    {
        Look l;
        l.mId = s["id"].asInteger();
        l.mName = s["name"].asString();
        l.mKind = s["kind"].asString();
        l.mBlurb = s["blurb"].asString();
        if (l.mId > 0 && !l.mName.empty()) mLooks.push_back(l);
    }
    for (S32 i = 0; i < (S32)mLooks.size(); ++i) requestPicture(i);
    showKind(mKind);
    refreshChosen();
}

void WolfFloaterChooseAvatar::requestPicture(S32 index)
{
    Look& l = mLooks[index];
    l.mPictureState = 1;
    LLSD body;
    body["action"] = "picture";
    body["id"] = l.mId;
    const S32 id = l.mId;
    const LLHandle<LLFloater> handle = getHandle();
    LLCoros::instance().launch("WolfChooseAvatarPicture", [handle, body, id]()
    {
        apiCoro(handle, body, [id](WolfFloaterChooseAvatar* self, const LLSD& reply, const std::string& error) { self->onPicture(id, reply, error); });
    });
}

void WolfFloaterChooseAvatar::onPicture(S32 id, const LLSD& reply, const std::string& error)
{
    Look* l = lookById(id);
    if (!l) return;
    std::string why = error;
    if (why.empty() && pictureTexture(reply["mime"].asString(), reply["data"].asString(), l->mTexture, why))
    {
        l->mPictureState = 2;
        return;
    }
    l->mPictureState = 3;
    l->mPictureWhy = why;
}

void WolfFloaterChooseAvatar::showKind(const std::string& kind)
{
    mKind = kind;
    for (S32 k = 0; k < 3; ++k)
    {
        S32 n = 0;
        for (const Look& l : mLooks) if (l.mKind == KINDS[k]) ++n;
        mKindBtn[k]->setToggleState(kind == KINDS[k]);
        static const char* const LABELS[3] = { "People", "Animals", "Sci-fi" };   // starteravatarslib.php SA_KINDS labels
        mKindBtn[k]->setLabel(n > 0 ? llformat("%s (%d)", LABELS[k], n) : std::string(LABELS[k]));
    }
    refreshCards();
}

void WolfFloaterChooseAvatar::refreshCards()
{
    mShown.clear();
    for (S32 i = 0; i < (S32)mLooks.size() && (S32)mShown.size() < CARDS; ++i)
    {
        if (mLooks[i].mKind == mKind) mShown.push_back(i);
    }
    for (S32 i = 0; i < CARDS; ++i)
    {
        const bool on = i < (S32)mShown.size();
        mCards[i]->mLook = on ? &mLooks[mShown[i]] : nullptr;
        mCards[i]->mSelected = on && mShown[i] == mSelected;
        mCards[i]->setVisible(on);
        mCards[i]->setToolTip(on ? "Choose " + mLooks[mShown[i]].mName : std::string());
    }
    if (!mLooks.empty())
    {
        mEmpty->setVisible(mShown.empty());
        if (mShown.empty()) mEmpty->setText(std::string("No looks in this group yet."));
    }
}

void WolfFloaterChooseAvatar::select(S32 look_index)
{
    if (mBusy || look_index < 0 || look_index >= (S32)mLooks.size()) return;
    mSelected = look_index;
    refreshCards();
    refreshChosen();
}

void WolfFloaterChooseAvatar::refreshChosen()
{
    const Look* l = mSelected >= 0 && mSelected < (S32)mLooks.size() ? &mLooks[mSelected] : nullptr;
    mChosenName->setText(l ? l->mName : std::string("Pick a look above"));
    mChosenBlurb->setText(l ? l->mBlurb : std::string(" "));
    LLStringUtil::format_map_t args;
    args["[NAME]"] = l ? l->mName : std::string("your new look");
    mReplaceNote->setText(getString("note_replace", args));
    mWear->setLabel(l ? getString("wear_label", args) : getString("wear_none"));
    mWear->setEnabled(l != nullptr && !mBusy && WolfGrid::isWolfTerritories());
}

void WolfFloaterChooseAvatar::onWear()
{
    if (mBusy) return;
    if (mSelected < 0 || mSelected >= (S32)mLooks.size())
    {
        setStatus("Not changed: pick a look first.", EStatus::ERR);
        return;
    }
    if (!WolfGrid::isWolfTerritories())
    {
        setStatus("Not changed: Choose an Avatar is for Wolf Territories residents.", EStatus::ERR);
        return;
    }
    if (!isAgentAvatarValid())
    {
        setStatus("Not changed: your avatar is not ready yet - try again in a moment.", EStatus::ERR);
        return;
    }
    const Look& l = mLooks[mSelected];
    setBusy(true);
    setStatus("Changing to " + l.mName + "... (saving your current outfit, then copying the look)", EStatus::INFO);
    LLSD body;
    body["action"] = "choose";
    body["id"] = l.mId;
    const LLHandle<LLFloater> handle = getHandle();
    LLCoros::instance().launch("WolfChooseAvatarChoose", [handle, body]()
    {
        apiCoro(handle, body, [](WolfFloaterChooseAvatar* self, const LLSD& reply, const std::string& error) { self->onChosen(reply, error); });
    });
}

void WolfFloaterChooseAvatar::onChosen(const LLSD& reply, const std::string& error)
{
    if (!error.empty())
    {
        setBusy(false);
        setStatus("Not changed: " + error, EStatus::ERR);
        return;
    }
    const LLUUID folder(json_string(reply, "folder"));
    const LLUUID outfit(reply["outfit"].isMap() ? reply["outfit"]["folder"].asString() : std::string());
    const std::string name = reply["starter"]["name"].asString();
    if (folder.isNull())
    {
        setBusy(false);
        setStatus("Not changed: the website did not say where the look was put. Please try again.", EStatus::ERR);
        return;
    }
    // what the website wrote straight into the inventory service: Clothing (the look) and My Outfits (the saved outfit)
    refetchFolder(gInventory.findUserDefinedCategoryUUIDForType(LLFolderType::FT_CLOTHING));
    refetchFolder(gInventory.findCategoryUUIDForType(LLFolderType::FT_MY_OUTFITS));
    setStatus("Changing to " + name + "... (loading it into your inventory)", EStatus::INFO);
    const LLHandle<LLFloater> handle = getHandle();
    LLCoros::instance().launch("WolfChooseAvatarWear", [handle, folder, outfit, name]() { wearCoro(handle, folder, outfit, name); });
}

// static
void WolfFloaterChooseAvatar::wearCoro(LLHandle<LLFloater> handle, LLUUID folder, LLUUID outfit, std::string name)
{
    auto self = [&handle]() { return static_cast<WolfFloaterChooseAvatar*>(handle.get()); };
    // 1. the folder reaches the inventory model (wolfai.cpp:739-743 waits for a delivered item the same way)
    for (F32 t = 0.f; !gInventory.getCategory(folder) && t < FOLDER_WAIT_SECS; t += POLL_SECS)
    {
        llcoro::suspendUntilTimeout(POLL_SECS);
        if (t >= 10.f && fmodf(t, 10.f) < POLL_SECS) refetchFolder(gInventory.findUserDefinedCategoryUUIDForType(LLFolderType::FT_CLOTHING));
    }
    LLViewerInventoryCategory* cat = gInventory.getCategory(folder);
    if (!cat)
    {
        if (WolfFloaterChooseAvatar* s = self())
        {
            s->setBusy(false);
            s->setStatus("Not changed yet: " + name + " is in your Clothing folder, but your viewer has not loaded it. "
                         "Open Inventory > Clothing > " + name + " (starter avatar), right-click it and choose Replace Outfit.", EStatus::ERR);
        }
        return;
    }
    // 2. a Hover Height left from another avatar would lift or sink this one: the look's shape sets its height
    //    (the per-account offset, llfloaterhoverheight.cpp / llviewercontrol.cpp:249-256 setHoverIfRegionEnabled)
    bool hover_reset = false;
    if (gSavedPerAccountSettings.getControl("AvatarHoverOffsetZ") && fabsf(gSavedPerAccountSettings.getF32("AvatarHoverOffsetZ")) > 0.0005f)
    {
        gSavedPerAccountSettings.setF32("AvatarHoverOffsetZ", 0.f);
        hover_reset = true;
    }
    // 3. wear it, replacing the outfit: LLAppearanceMgr::wearInventoryCategory(cat, copy false, append false) fetches the
    //    folder's contents first (callAfterCategoryFetch) and keeps body parts the look does not bring (updateCOF)
    LLAppearanceMgr::instance().wearInventoryCategory(cat, false, false);
    if (WolfFloaterChooseAvatar* s = self()) s->setStatus("Changing to " + name + "... (putting it on)", EStatus::INFO);

    // 4. done when every wearable and object in the folder is worn
    bool worn = false;
    for (F32 t = 0.f; t < WEAR_WAIT_SECS && !worn; t += POLL_SECS)
    {
        llcoro::suspendUntilTimeout(POLL_SECS);
        if (!isAgentAvatarValid()) break;
        LLInventoryModel::cat_array_t cats;
        LLInventoryModel::item_array_t items;
        gInventory.collectDescendents(folder, cats, items, LLInventoryModel::EXCLUDE_TRASH);
        S32 wanted = 0, on = 0;
        for (const LLPointer<LLViewerInventoryItem>& it : items)
        {
            const LLAssetType::EType type = it->getActualType();
            if (type == LLAssetType::AT_CLOTHING || type == LLAssetType::AT_BODYPART)
            {
                ++wanted;
                if (gAgentWearables.isWearingItem(it->getUUID())) ++on;
            }
            else if (type == LLAssetType::AT_OBJECT)
            {
                ++wanted;
                if (gAgentAvatarp->isWearingAttachment(it->getUUID())) ++on;
            }
        }
        worn = wanted > 0 && on == wanted;
    }
    WolfFloaterChooseAvatar* s = self();
    if (!s) return;
    s->setBusy(false);
    std::string text = worn ? "Changing to " + name + "... done. It is yours to keep: Clothing > " + name + " (starter avatar)."
                            : "Changing to " + name + "... still putting it on. If something is missing, open Inventory > Clothing > "
                              + name + " (starter avatar), right-click it and choose Replace Outfit.";
    if (outfit.notNull()) text += " What you wore before is saved in My Outfits as \"My outfit before " + name + "\".";
    if (hover_reset) text += " Your Hover Height was set back to 0: this look sets its own height.";
    s->setStatus(text, worn ? EStatus::OK : EStatus::INFO);
}
