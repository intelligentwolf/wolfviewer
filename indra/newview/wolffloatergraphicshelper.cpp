/**
 * @file wolffloatergraphicshelper.cpp
 * @brief WolfViewer: the Graphics Helper. See wolffloatergraphicshelper.h.
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

#include "wolffloatergraphicshelper.h"

#include <ctime>
#include <set>

#include "indra_constants.h"     // APP_NAME: WolfViewer.log / WolfViewer.old
#include "llappviewer.h"
#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llerrorcontrol.h"      // LLError::logFileName
#include "llfeaturemanager.h"
#include "llfilepicker.h"
#include "llgl.h"
#include "llsdserialize.h"       // LLSDNotationStreamer
#include "llsdutil.h"            // llsd_equals
#include "llsys.h"
#include "lltextbox.h"
#include "lltexteditor.h"
#include "lluicolortable.h"
#include "llversioninfo.h"
#include "llviewercontrol.h"
#include "llviewermenufile.h"    // LLFilePickerReplyThread
#include "llviewershadermgr.h"

namespace
{
    // One switch. On = any value but mOff. Switching on puts back the value it had when the window was
    // first opened this session, or mOn when that was already "off" (for the photo effect, mOn 0: it can
    // only be switched off). Settings and values: app_settings/settings.xml; every one is applied live by
    // its own listener (llviewercontrol.cpp settings_setup_listeners, pipeline.cpp refreshCachedSettings,
    // or a LLCachedControl read each frame), so the picture changes as the box is clicked.
    struct Toggle
    {
        const char* mCtrl;      // the checkbox in floater_wolf_graphics_helper.xml
        const char* mSetting;
        S32         mOff;
        S32         mOn;
    };
    const Toggle TOGGLES[] =
    {
        // Lighting and reflections
        { "t_shadows",       "RenderShadowDetail",           0,  2 },   // 0 none, 1 sun/moon, 2 + projectors
        { "t_skylight",      "RenderReflectionsEnabled",     0,  1 },   // off "removes all sky ambiance" (its comment)
        { "t_reflections",   "RenderReflectionProbeDetail", -1,  1 },   // -1 disabled, 0 static, 1 static + dynamic, 2 realtime
        { "t_ssr",           "RenderScreenSpaceReflections", 0,  1 },
        { "t_mirrors",       "RenderMirrors",                0,  1 },
        { "t_ssao",          "RenderDeferredSSAO",           0,  1 },
        { "t_hdr",           "RenderHDREnabled",             0,  1 },
        { "t_glow",          "RenderGlow",                   0,  1 },
        { "t_dof",           "RenderDepthOfField",           0,  1 },
        { "t_water",         "RenderTransparentWater",       0,  1 },
        // Wolf's own (each also checks it is on Wolf Territories where that applies)
        { "t_horizon_sea",   "WolfViewerHorizonSea",         0,  1 },
        { "t_altitude_sky",  "WolfViewerAltitudeSky",        0,  1 },
        { "t_fly_clouds",    "WolfViewerFlyClouds",          0,  1 },
        { "t_far_terrain",   "WolfViewerFarTerrain",         0,  1 },
        { "t_weather",       "WolfWeatherEnabled",           0,  1 },
        { "t_lightning",     "WolfViewerLightning",          0,  1 },
        { "t_fx",            "WolfFXEnabled",                0,  1 },
        { "t_waves",         "WolfViewerWaterFFT",           0,  1 },
        { "t_god_rays",      "WolfViewerWaterGodRays",       0,  1 },
        { "t_caustics",      "WolfViewerWaterCaustics",      0,  1 },
        { "t_photo",         "WolfViewerPhotoFilter",        0,  0 },
    };
    const size_t TOGGLE_COUNT = sizeof(TOGGLES) / sizeof(TOGGLES[0]);
    // Also put back by "Put everything back" (the slider is bound to it in the XUI).
    const char* const EXPOSURE = "RenderExposure";

    // How every switch stood when the window was first opened this session: what "Put everything back"
    // returns to, and what a switch turned back on goes to.
    LLSD sBaseline;

    S32 get_int(const char* name)
    {
        LLControlVariable* c = gSavedSettings.getControl(name);
        if (!c) return 0;
        const LLSD v = c->getValue();
        return v.isBoolean() ? (v.asBoolean() ? 1 : 0) : v.asInteger();
    }

    void set_int(const char* name, S32 value)
    {
        LLControlVariable* c = gSavedSettings.getControl(name);
        if (!c) return;
        if (c->type() == TYPE_BOOLEAN)
        {
            c->setValue(LLSD(value != 0));
        }
        else
        {
            c->setValue(LLSD(value));
        }
    }

    void take_baseline()
    {
        if (sBaseline.isMap()) return;
        sBaseline = LLSD::emptyMap();
        for (const Toggle& t : TOGGLES)
        {
            sBaseline[t.mSetting] = get_int(t.mSetting);
        }
        sBaseline[EXPOSURE] = gSavedSettings.getF32(EXPOSURE);
    }

    S32 on_value(const Toggle& t)
    {
        const S32 base = sBaseline.has(t.mSetting) ? sBaseline[t.mSetting].asInteger() : t.mOn;
        return base != t.mOff ? base : t.mOn;
    }

    // ---- the logs ----
    std::string old_log_path()
    {
        // Source: llappviewer.cpp initLoggingAndGetLastDuration - APP_NAME + ".old" in LL_PATH_LOGS.
        return gDirUtilp->getExpandedFilename(LL_PATH_LOGS, APP_NAME + ".old");
    }

    // The markers the shader manager writes. Sources: llviewershadermgr.cpp setShaders ("Shader group
    // ... would not build on this GPU; switching off <what> and trying again"); llshadermgr.cpp
    // ("GLSL Compilation Error:", "GLSL Linker Error:", "GLSL program not valid"); llglslshader.cpp
    // ("Failed to link shader: ").
    const char* const FALLBACK_MARK = "would not build on this";
    const char* const ERROR_MARKS[] = { "GLSL Compilation Error", "GLSL Linker Error", "GLSL program not valid", "Failed to link shader" };

    struct LogScan
    {
        bool mRead = false;
        std::vector<std::string> mLost;     // what the fallback switched off, in order, once each
        std::vector<std::string> mLines;    // "line N: text" for every fallback and error line
        S32 mErrors = 0;
    };

    // The first and the last SCAN_CAP bytes: the start-up's shader lines (and its fallback) are at the
    // top, a later reload's near the end, and a verbose log of hundreds of MB must not freeze the window.
    const std::streamoff SCAN_CAP = 8 * 1024 * 1024;

    void scan_line(LogScan& scan, const std::string& line, const std::string& where)
    {
        bool hit = false;
        const size_t f = line.find(FALLBACK_MARK);
        if (f != std::string::npos)
        {
            hit = true;
            const std::string from = "switching off ", to = " and trying again";
            const size_t a = line.find(from, f), b = line.find(to, f);
            if (a != std::string::npos && b != std::string::npos && b > a + from.size())
            {
                const std::string what = line.substr(a + from.size(), b - a - from.size());
                if (std::find(scan.mLost.begin(), scan.mLost.end(), what) == scan.mLost.end())
                {
                    scan.mLost.push_back(what);
                }
            }
        }
        for (const char* m : ERROR_MARKS)
        {
            if (line.find(m) != std::string::npos)
            {
                hit = true;
                ++scan.mErrors;
                break;
            }
        }
        if (hit && scan.mLines.size() < 200)
        {
            scan.mLines.push_back(where + line);
        }
    }

    LogScan scan_log(const std::string& path)
    {
        LogScan scan;
        llifstream in(path.c_str(), std::ios::binary);
        if (!in.is_open()) return scan;
        scan.mRead = true;
        in.seekg(0, std::ios::end);
        const std::streamoff size = in.tellg();
        in.seekg(0, std::ios::beg);
        std::string line;
        S32 n = 0;
        // the head (all of it when the file is small)
        while (in.tellg() >= 0 && in.tellg() < SCAN_CAP && std::getline(in, line))
        {
            ++n;
            scan_line(scan, line, llformat("line %d: ", n));
        }
        // the tail, past the head, from the first whole line
        const std::streamoff head_end = in.tellg();
        if (head_end >= 0 && size > head_end)
        {
            const std::streamoff from = llmax(head_end, size - SCAN_CAP);
            in.clear();
            in.seekg(from, std::ios::beg);
            if (from > head_end)
            {
                std::getline(in, line);   // cut in the middle
            }
            while (std::getline(in, line))
            {
                scan_line(scan, line, "near the end: ");
            }
        }
        return scan;
    }

    std::string join(const std::vector<std::string>& v)
    {
        std::string out;
        for (const std::string& s : v)
        {
            if (!out.empty()) out += ", ";
            out += s;
        }
        return out;
    }

    std::string system_text()
    {
        LLFeatureManager* fm = LLFeatureManager::getInstance();
        std::string s = llformat("Graphics card: %s (%s)\n", gGLManager.mGLRenderer.c_str(), gGLManager.mGLVendor.c_str());
        s += llformat("OpenGL %s, GLSL %d.%d, %u MB graphics memory, GPU class %d%s\n",
                      gGLManager.mGLVersionString.c_str(), gGLManager.mGLSLVersionMajor, gGLManager.mGLSLVersionMinor,
                      gGLManager.mVRAM, (S32)fm->getGPUClass(), fm->isGPUSupported() ? "" : " (not on the supported list)");
        s += LLOSInfo::instance().getOSString() + "\n";
        s += LLVersionInfo::instance().getChannelAndVersion();
        return s;
    }

    // Plain words, in order of how likely each is to be the cause.
    std::vector<std::string> find_problems(const LogScan& now_log, const LogScan& old_log)
    {
        std::vector<std::string> out;
        const std::vector<std::string>& session = LLViewerShaderMgr::getGraphicsFallbacks();
        if (!session.empty())
        {
            out.push_back("A shader would not build on this graphics card, so the viewer switched off: " + join(session)
                          + ". They stay off until switched back on below. If the picture breaks when you do, switch them off again.");
        }
        else if (!old_log.mLost.empty())
        {
            out.push_back("Last time the viewer ran, a shader would not build on this graphics card and it switched off: "
                          + join(old_log.mLost) + ". They stay off until switched back on below.");
        }
        const S32 errors = now_log.mErrors + old_log.mErrors;
        if (errors > 0)
        {
            out.push_back(llformat("%d shader error line%s in the logs. The diagnostic file includes them.", errors, errors == 1 ? "" : "s"));
        }
        if (!gSavedSettings.getBOOL("RenderReflectionsEnabled"))
        {
            out.push_back("Sky light on objects is off: the insides of buildings and shaded sides look very dark. Switch it on below.");
        }
        if (gSavedSettings.getS32("WolfViewerPhotoFilter") != 0)
        {
            out.push_back("A photo effect is on (Wolf World > Photo Effects): it changes the colours of everything. Switch it off below.");
        }
        const F32 exposure = gSavedSettings.getF32(EXPOSURE);
        if (fabsf(exposure - 1.f) > 0.05f)
        {
            out.push_back(llformat("Exposure is %.1f (normal is 1.0): above 1 brightens everything, below 1 darkens it.", exposure));
        }
        const F32 gamma = gSavedSettings.getF32("RenderGamma");
        if (gamma != 0.f)
        {
            out.push_back(llformat("An old gamma setting is set (RenderGamma %.2f; normal is 0).", gamma));
        }
        if (gSavedSettings.getS32("RenderTerrainPBRDetail") < 0)
        {
            out.push_back("Detailed terrain textures are reduced (RenderTerrainPBRDetail is below 0), so the ground looks flat.");
        }
        if (!LLFeatureManager::getInstance()->isGPUSupported())
        {
            out.push_back("This graphics card is not on the viewer's supported list.");
        }
        if (gGLManager.mVRAM > 0 && gGLManager.mVRAM < 1024)
        {
            out.push_back(llformat("Only %u MB of graphics memory was found: textures will be blurry and slow to load.", gGLManager.mVRAM));
        }
        if (gGLManager.mGLVersion > 0.f && gGLManager.mGLVersion < 4.f)
        {
            out.push_back(llformat("OpenGL %.1f: the viewer needs 4.0 or newer for its full graphics.", gGLManager.mGLVersion));
        }
        return out;
    }

    // A log file as it is; when bigger than `cap`, its first quarter of `cap` (the start-up: GPU, driver,
    // shaders) and its last three quarters (what happened most recently).
    std::string read_file_tail(const std::string& path, size_t cap)
    {
        llifstream in(path.c_str(), std::ios::binary);
        if (!in.is_open()) return std::string("(not found: ") + path + ")\n";
        in.seekg(0, std::ios::end);
        const std::streamoff size = in.tellg();
        in.seekg(0, std::ios::beg);
        if (size <= (std::streamoff)cap)
        {
            std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            return body + (body.empty() || body.back() == '\n' ? "" : "\n");
        }
        const std::streamoff head_len = (std::streamoff)(cap / 4), tail_len = (std::streamoff)cap - head_len;
        std::string head((size_t)head_len, '\0'), tail((size_t)tail_len, '\0');
        in.read(&head[0], head_len);
        head.resize((size_t)llmax((std::streamsize)0, in.gcount()));
        in.clear();
        in.seekg(size - tail_len, std::ios::beg);
        in.read(&tail[0], tail_len);
        tail.resize((size_t)llmax((std::streamsize)0, in.gcount()));
        return llformat("(the first %u KB and the last %u KB of %u KB)\n", (U32)(head_len / 1024), (U32)(tail_len / 1024), (U32)(size / 1024))
               + head + "\n\n[... " + llformat("%u KB left out", (U32)((size - head_len - tail_len) / 1024)) + " ...]\n\n" + tail + "\n";
    }

    // One value as short text (ll_print_sd shares one static buffer, so two in one expression clash).
    std::string notation(const LLSD& v)
    {
        std::ostringstream o;
        o << LLSDNotationStreamer(v);
        return o.str();
    }

    std::string utc_now()
    {
        time_t t = time(nullptr);
        struct tm g;
#if LL_WINDOWS
        gmtime_s(&g, &t);
#else
        gmtime_r(&t, &g);
#endif
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &g);
        return buf;
    }

    std::string file_stamp()
    {
        time_t t = time(nullptr);
        struct tm g;
#if LL_WINDOWS
        gmtime_s(&g, &t);
#else
        gmtime_r(&t, &g);
#endif
        char buf[32];
        strftime(buf, sizeof(buf), "%Y%m%d-%H%M", &g);
        return buf;
    }

    // The logs as read when the window opened: a switch click re-checks the settings, not megabytes of log.
    LogScan sNowScan, sOldScan;

    const size_t LOG_CAP = 8 * 1024 * 1024;   // per log in the file: the start-up (GPU, shaders) is near the top of a short run

    std::string diagnostic_text()
    {
        const std::string now_path = LLError::logFileName();
        const std::string old_path = old_log_path();
        const LogScan now_log = scan_log(now_path);
        const LogScan old_log = scan_log(old_path);

        std::string s = "WolfViewer graphics diagnostic\nSaved " + utc_now() + "\n\n";
        s += "== What looks wrong ==\n";
        const std::vector<std::string> found = find_problems(now_log, old_log);
        if (found.empty()) s += "Nothing found in the settings or the logs.\n";
        for (const std::string& p : found) s += "* " + p + "\n";

        s += "\n== Computer ==\n" + system_text() + "\n";

        s += "\n== Graphics switches (now / when the Graphics Helper was first opened) ==\n";
        for (const Toggle& t : TOGGLES)
        {
            s += llformat("%-30s %d / %s\n", t.mSetting, get_int(t.mSetting),
                          sBaseline.has(t.mSetting) ? std::to_string(sBaseline[t.mSetting].asInteger()).c_str() : "-");
        }
        s += llformat("%-30s %.2f / %s\n", EXPOSURE, gSavedSettings.getF32(EXPOSURE),
                      sBaseline.has(EXPOSURE) ? llformat("%.2f", sBaseline[EXPOSURE].asReal()).c_str() : "-");

        s += "\n== Every Render and Wolf setting not at its default ==\n";
        struct Changed : public LLControlGroup::ApplyFunctor
        {
            std::vector<std::string> mLines;
            void apply(const std::string& name, LLControlVariable* control) override
            {
                if (!control || (name.compare(0, 6, "Render") != 0 && name.compare(0, 4, "Wolf") != 0)) return;
                if (llsd_equals(control->getValue(), control->getDefault())) return;
                mLines.push_back(name + " = " + notation(control->getValue()) + "   (default " + notation(control->getDefault()) + ")");
            }
        } changed;
        gSavedSettings.applyToAll(&changed);
        std::sort(changed.mLines.begin(), changed.mLines.end());
        for (const std::string& l : changed.mLines) s += l + "\n";

        s += "\n== Viewer (Help > About) ==\n" + LLAppViewer::instance()->getViewerInfoString(true) + "\n";

        s += "\n== Shader lines in this session's log ==\n";
        for (const std::string& l : now_log.mLines) s += l + "\n";
        s += "\n== Shader lines in the last session's log ==\n";
        for (const std::string& l : old_log.mLines) s += l + "\n";

        s += "\n== Files in the logs folder ==\n";
        for (const std::string& f : gDirUtilp->getFilesInDir(gDirUtilp->getExpandedFilename(LL_PATH_LOGS, "")))
        {
            s += f + "\n";
        }

        s += "\n== " + now_path + " ==\n" + read_file_tail(now_path, LOG_CAP);
        s += "\n== " + old_path + " ==\n" + read_file_tail(old_path, LOG_CAP);
        return s;
    }
}

WolfFloaterGraphicsHelper::WolfFloaterGraphicsHelper(const LLSD& key) : LLFloater(key)
{
}

WolfFloaterGraphicsHelper::~WolfFloaterGraphicsHelper()
{
    for (boost::signals2::connection& c : mConns)
    {
        c.disconnect();
    }
}

bool WolfFloaterGraphicsHelper::postBuild()
{
    mSystem = getChild<LLTextBox>("system");
    mProblems = getChild<LLTextEditor>("problems");
    mStatus = getChild<LLTextBox>("status");
    for (size_t i = 0; i < TOGGLE_COUNT; ++i)
    {
        LLCheckBoxCtrl* box = getChild<LLCheckBoxCtrl>(TOGGLES[i].mCtrl);
        box->setCommitCallback([this, i](LLUICtrl*, const LLSD&) { onToggle(i); });
        mChecks.push_back(box);
        // Changed anywhere else too (Preferences, the menus, the fallback): follow it.
        if (LLControlVariable* c = gSavedSettings.getControl(TOGGLES[i].mSetting))
        {
            mConns.push_back(c->getSignal()->connect([this](LLControlVariable*, const LLSD&, const LLSD&)
            {
                refreshToggles();
                refreshProblems();
            }));
        }
    }
    if (LLControlVariable* c = gSavedSettings.getControl(EXPOSURE))
    {
        mConns.push_back(c->getSignal()->connect([this](LLControlVariable*, const LLSD&, const LLSD&) { refreshProblems(); }));
    }
    getChild<LLButton>("put_back_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPutBack(); });
    getChild<LLButton>("save_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSave(); });
    return true;
}

void WolfFloaterGraphicsHelper::onOpen(const LLSD& key)
{
    LLFloater::onOpen(key);
    take_baseline();
    sNowScan = scan_log(LLError::logFileName());
    sOldScan = scan_log(old_log_path());
    mSystem->setText(system_text());
    refreshToggles();
    refreshProblems();
    setStatus("Switches change the picture at once. Try one at a time.", false);
}

void WolfFloaterGraphicsHelper::onToggle(size_t index)
{
    const Toggle& t = TOGGLES[index];
    set_int(t.mSetting, mChecks[index]->getValue().asBoolean() ? on_value(t) : t.mOff);
    refreshToggles();
    refreshProblems();
}

void WolfFloaterGraphicsHelper::onPutBack()
{
    for (const Toggle& t : TOGGLES)
    {
        if (sBaseline.has(t.mSetting) && get_int(t.mSetting) != sBaseline[t.mSetting].asInteger())
        {
            set_int(t.mSetting, sBaseline[t.mSetting].asInteger());
        }
    }
    if (sBaseline.has(EXPOSURE))
    {
        gSavedSettings.setF32(EXPOSURE, (F32)sBaseline[EXPOSURE].asReal());
    }
    refreshToggles();
    refreshProblems();
    setStatus("Everything is back as it was when you first opened this window.", false);
}

void WolfFloaterGraphicsHelper::refreshToggles()
{
    for (size_t i = 0; i < mChecks.size(); ++i)
    {
        const Toggle& t = TOGGLES[i];
        const bool on = get_int(t.mSetting) != t.mOff;
        mChecks[i]->set(on);
        // A switch that can only go off (the photo effect) is greyed out once it is off.
        mChecks[i]->setEnabled(on || on_value(t) != t.mOff);
    }
}

void WolfFloaterGraphicsHelper::refreshProblems()
{
    const std::vector<std::string> found = find_problems(sNowScan, sOldScan);
    std::string text;
    for (const std::string& p : found)
    {
        text += "• " + p + "\n";
    }
    if (text.empty())
    {
        text = "Nothing wrong found in the settings or the logs. Try the switches below, one at a time.";
    }
    mProblems->setText(text);
}

void WolfFloaterGraphicsHelper::setStatus(const std::string& text, bool error)
{
    mStatus->setText(text);
    mStatus->setColor(LLUIColorTable::instance().getColor(error ? "AlertCautionTextColor" : "LabelTextColor"));
}

void WolfFloaterGraphicsHelper::onSave()
{
    LLHandle<LLFloater> handle = getHandle();
    LLFilePickerReplyThread::startPicker(
        [handle](const std::vector<std::string>& filenames, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
        {
            if (!filenames.empty())
            {
                saveTo(handle, filenames[0]);
            }
        },
        LLFilePicker::FFSAVE_ALL, "WolfViewer-graphics-" + file_stamp() + ".txt");
}

// static
void WolfFloaterGraphicsHelper::saveTo(LLHandle<LLFloater> handle, const std::string& filename)
{
    const std::string text = diagnostic_text();
    bool ok = false;
    {
        llofstream out(filename.c_str(), std::ios::binary);
        if (out.is_open())
        {
            out << text;
            out.close();
            ok = !out.fail();
        }
    }
    LL_INFOS("WolfGraphicsHelper") << "diagnostic file " << filename << (ok ? " saved, " : " FAILED, ") << text.size() << " bytes" << LL_ENDL;
    WolfFloaterGraphicsHelper* self = dynamic_cast<WolfFloaterGraphicsHelper*>(handle.get());
    if (self)
    {
        if (ok)
        {
            self->setStatus("Saved " + gDirUtilp->getBaseFileName(filename) + ". Send it to Wolf Territories support (Discord).", false);
        }
        else
        {
            self->setStatus("Could not save " + filename + ". Pick another folder and try again.", true);
        }
    }
}
