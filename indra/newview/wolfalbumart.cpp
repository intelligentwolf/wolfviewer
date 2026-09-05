/**
 * @file wolfalbumart.cpp
 * @brief Album cover lookup for the stream currently playing (WolfViewer)
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — IntelligentWolf Ltd
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "wolfalbumart.h"

#include "llaudioengine.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "llstreamingaudio.h"
#include "lluri.h"
#include "llviewercontrol.h"

// iTunes Search API. Response verified 2026-09-05:
//   {"resultCount":1,"results":[{"artistName":..,"trackName":..,"collectionName":..,
//     "artworkUrl100":"https://is1-ssl.mzstatic.com/image/thumb/.../100x100bb.jpg", ..}]}
// and {"resultCount":0,"results":[]} for no match. The artwork URL's trailing size
// segment can be rewritten (100x100 -> 300x300 returned 200 image/jpeg).
static const std::string ITUNES_SEARCH_URL = "https://itunes.apple.com/search?media=music&entity=song&limit=1&term=";
static const std::string ITUNES_ART_SIZE_IN = "100x100";
static const std::string ITUNES_ART_SIZE_OUT = "300x300";

WolfAlbumArt::~WolfAlbumArt()
{
    if (mMetadataUpdateConnection.connected())
    {
        mMetadataUpdateConnection.disconnect();
    }
    if (mSettingConnection.connected())
    {
        mSettingConnection.disconnect();
    }
    // Shutdown: just drop the reference. No forceActive() (the texture list may already be
    // gone) and no signal (the floater disconnects in its own destructor).
    mArtTexture = nullptr;
}

void WolfAlbumArt::initSingleton()
{
    // Same hook as StreamTitleDisplay / FSStreamTitleManager (created together in
    // LLStartUp::multimediaInit).
    if (!gAudiop || !gAudiop->getStreamingAudioImpl())
    {
        return;
    }
    mSettingConnection = gSavedSettings.getControl("WolfStreamAlbumArt")->getSignal()->connect(boost::bind(&WolfAlbumArt::onSettingChanged, this));
    mMetadataUpdateConnection = gAudiop->getStreamingAudioImpl()->setMetadataUpdateCallback(std::bind(&WolfAlbumArt::onMetadataUpdate, this, std::placeholders::_1));
    onMetadataUpdate(gAudiop->getStreamingAudioImpl()->getCurrentMetadata());
}

bool WolfAlbumArt::isEnabled() const noexcept
{
    static LLCachedControl<bool> enabled(gSavedSettings, "WolfStreamAlbumArt", false);
    return enabled;
}

void WolfAlbumArt::onSettingChanged()
{
    if (isEnabled())
    {
        startLookup();
    }
    else
    {
        mLookedUpTerm.clear();
        ++mSeq;              // any lookup in flight is dropped
        setArt("", "");
    }
}

void WolfAlbumArt::onMetadataUpdate(const LLSD& metadata)
{
    mMetadata = metadata;
    if (isEnabled())
    {
        startLookup();
    }
}

// The plugins deliver the ICY string "Artist - Song" as TITLE (FMOD does the same);
// a separate ARTIST arrives only when the stream sends one.
void WolfAlbumArt::startLookup()
{
    std::string artist = mMetadata.has("ARTIST") ? mMetadata["ARTIST"].asString() : "";
    std::string song = mMetadata.has("TITLE") ? mMetadata["TITLE"].asString() : "";
    if (artist.empty())
    {
        size_t sep = song.find(" - ");
        if (sep != std::string::npos)
        {
            artist = song.substr(0, sep);
            song = song.substr(sep + 3);
        }
    }
    LLStringUtil::trim(artist);
    LLStringUtil::trim(song);

    std::string term = artist.empty() ? song : artist + " " + song;
    if (term == mLookedUpTerm)
    {
        return;
    }
    mLookedUpTerm = term;
    ++mSeq;
    if (song.empty())
    {
        setArt("", "");      // stream stopped or no title yet
        return;
    }
    LLCoros::instance().launch("WolfAlbumArt::lookupCoro", boost::bind(&WolfAlbumArt::lookupCoro, this, term, mSeq));
}

void WolfAlbumArt::lookupCoro(std::string term, U32 seq)
{
    LLCore::HttpRequest::policy_t httpPolicy(LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t httpAdapter = std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfAlbumArt::lookupCoro", httpPolicy);
    LLCore::HttpRequest::ptr_t httpRequest = std::make_shared<LLCore::HttpRequest>();

    std::string url = ITUNES_SEARCH_URL + LLURI::escapeQueryValue(term);
    // getJsonAndSuspend returns the JSON body converted to LLSD, with the status under HTTP_RESULTS
    LLSD result = httpAdapter->getJsonAndSuspend(httpRequest, url);

    if (seq != mSeq)
    {
        return;              // the song changed (or the setting went off) while we waited
    }
    LLSD httpResults = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
    LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(httpResults);
    if (!status)
    {
        LL_WARNS("WolfAlbumArt") << "Cover lookup failed for '" << term << "': " << status.toString() << LL_ENDL;
        setArt("", "");
        return;
    }
    if (!result.has("results") || !result["results"].isArray() || result["results"].size() < 1)
    {
        setArt("", "");      // no match — the floater shows just the title
        return;
    }
    const LLSD& hit = result["results"][0];
    std::string art_url = hit["artworkUrl100"].asString();
    if (art_url.empty())
    {
        setArt("", "");
        return;
    }
    size_t size_pos = art_url.rfind(ITUNES_ART_SIZE_IN);
    if (size_pos != std::string::npos)
    {
        art_url.replace(size_pos, ITUNES_ART_SIZE_IN.length(), ITUNES_ART_SIZE_OUT);
    }
    std::string label = hit["artistName"].asString();
    std::string album = hit["collectionName"].asString();
    if (!album.empty())
    {
        label += (label.empty() ? "" : " \xE2\x80\x94 ") + album;   // em dash, UTF-8
    }
    LL_INFOS("WolfAlbumArt") << "Cover for '" << term << "': " << label << " " << art_url << LL_ENDL;
    setArt(art_url, label);
}

void WolfAlbumArt::setArt(const std::string& url, const std::string& label)
{
    if (mArtTexture.notNull())
    {
        // BOOST_PREVIEW pins the texture (NO_DELETE); release it the way LLTextureCtrl does
        mArtTexture->forceActive();
        mArtTexture = nullptr;
    }
    mArtLabel = label;
    if (!url.empty())
    {
        // FTT_MAP_TILE = "fetch this URL as-is, decode by its extension (.jpg), do not cache,
        // do not complain on 404" — the same path the world map uses for its JPEG tiles
        // (llworldmipmap.cpp). The id is generated from the URL, so a repeated song reuses
        // the texture already in memory.
        mArtTexture = LLViewerTextureManager::getFetchedTextureFromUrl(url, FTT_MAP_TILE, true, LLGLTexture::BOOST_NONE);
        if (mArtTexture.notNull())
        {
            mArtTexture->setBoostLevel(LLGLTexture::BOOST_PREVIEW);
        }
    }
    mUpdateSignal();
}
