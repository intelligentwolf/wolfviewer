/**
 * @file wolfalbumart.h
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

#ifndef WOLF_ALBUMART_H
#define WOLF_ALBUMART_H

#include "llsingleton.h"
#include "llviewertexture.h"

// Turns the stream metadata (ARTIST/TITLE from the media plugin, see
// LLStreamingAudio_MediaPlugins::updateMetadata) into a cover image.
//
// A radio stream never carries artwork — ICY sends only a title string — so the cover is
// looked up online (iTunes Search API) and fetched as a URL texture. Because that sends the
// listener's play history to a third party it is OPT-IN: setting WolfStreamAlbumArt
// (Preferences > Sound & Media > Music), off by default.
class WolfAlbumArt : public LLSingleton<WolfAlbumArt>
{
    LLSINGLETON_EMPTY_CTOR(WolfAlbumArt);

public:
    ~WolfAlbumArt() override;

    using art_update_callback_t = boost::signals2::signal<void()>;
    boost::signals2::connection setUpdateCallback(const art_update_callback_t::slot_type& cb) noexcept
    {
        return mUpdateSignal.connect(cb);
    }

    // Null until a lookup has succeeded for the current song.
    LLPointer<LLViewerFetchedTexture> getArtTexture() const noexcept { return mArtTexture; }
    // "Artist — Album" for the tooltip; empty when there is no art.
    const std::string& getArtLabel() const noexcept { return mArtLabel; }
    bool isEnabled() const noexcept;

protected:
    void initSingleton() override;

    void onMetadataUpdate(const LLSD& metadata);
    void onSettingChanged();
    void startLookup();
    void lookupCoro(std::string term, U32 seq);
    void setArt(const std::string& url, const std::string& label);

    boost::signals2::connection mMetadataUpdateConnection{};
    boost::signals2::connection mSettingConnection{};
    art_update_callback_t mUpdateSignal;

    LLPointer<LLViewerFetchedTexture> mArtTexture;
    std::string mArtLabel{};
    LLSD mMetadata;              // last ARTIST/TITLE seen, so enabling the setting mid-song looks it up
    std::string mLookedUpTerm{}; // term of the newest lookup, to avoid repeating it
    U32 mSeq{ 0 };               // newest lookup; older coroutines drop their result
};

#endif // WOLF_ALBUMART_H
