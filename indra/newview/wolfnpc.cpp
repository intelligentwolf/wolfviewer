/**
 * @file wolfnpc.cpp
 * @brief WolfViewer: WolfNPC - the link to the region's WolfNPCModule. See wolfnpc.h.
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

#include "wolfnpc.h"

#include <cmath>
#include <limits>

#include <boost/json.hpp>

#include "llagent.h"
#include "lldispatcher.h"
#include "llframetimer.h"
#include "llsdjson.h"
#include "llviewergenericmessage.h"
#include "llviewerregion.h"
#include "wolfai.h"
#include "wolfgame.h"
#include "wolfgrid.h"

namespace
{
    // Source: PROTOCOL.md section 2 - GenericMessage method "wolfnpc" (viewer -> region) and "WolfNPC" (region -> viewer)
    const std::string METHOD_TO_SIM("wolfnpc");
    const std::string MESSAGE_FROM_SIM("WolfNPC");
    // Source: PROTOCOL.md section 2 / WolfGameModule.cs:69-70 CHUNK_BYTES 240, CHUNKS_PER_PACKET 3
    constexpr size_t CHUNK_BYTES = 240;
    constexpr size_t CHUNKS_PER_PACKET = 3;
    // Source: PROTOCOL.md section 2 - "refuses a message over 16 KiB"
    constexpr size_t MAX_MESSAGE_BYTES = 16u * 1024u;
    // Source: PROTOCOL.md section 2 - "drops an incomplete message after 30 s" (the region's rule; the same here)
    constexpr F64 PARTIAL_LIFE = 30.0;
    // Source: PROTOCOL.md section 2 - "at most 4 messages per second per agent (bucket 8)". The viewer keeps under it
    // (design values): 3 a second, bucket 6, so its own clock running a little apart from the region's never trips it.
    constexpr F64 TOKEN_REFILL = 3.0, TOKEN_BUCKET = 6.0;
    // Design values: how long a request waits for its reply. A create asks the brain and fetches the body's asset first.
    constexpr F64 ANSWER_SEC = 15.0, CREATE_ANSWER_SEC = 60.0;
    // Design values (as wolfgame.cpp MAX_PARTS / MAX_PENDING): anything claiming more parts is not ours; at most this
    // many half-built messages are kept.
    constexpr S32 MAX_PARTS = 256;
    constexpr size_t MAX_PARTIAL = 16;
    constexpr size_t MAX_QUEUE = 32;

    F64 now() { return LLFrameTimer::getTotalSeconds(); }

    LLSD failure(const std::string& op, const std::string& why)
    {
        LLSD r = LLSD::emptyMap();
        r["op"] = op;
        r["ok"] = false;
        r["error"] = why;
        return r;
    }

    // Source: wolfgame.cpp:228-268 jsonToLLSD - llsdjson.cpp LlsdFromJson, except that an integer past 32 bits becomes a
    // real (LLSD integers are 32-bit and would wrap).
    LLSD jsonToLLSD(const boost::json::value& val)
    {
        LLSD result;
        switch (val.kind())
        {
        case boost::json::kind::int64:
        {
            const int64_t i = val.get_int64();
            if (i >= std::numeric_limits<S32>::min() && i <= std::numeric_limits<S32>::max()) result = LLSD((LLSD::Integer)i);
            else result = LLSD((LLSD::Real)i);
            break;
        }
        case boost::json::kind::uint64:
        {
            const uint64_t u = val.get_uint64();
            if (u <= (uint64_t)std::numeric_limits<S32>::max()) result = LLSD((LLSD::Integer)u);
            else result = LLSD((LLSD::Real)u);
            break;
        }
        case boost::json::kind::double_:
            result = LLSD(val.get_double());
            break;
        case boost::json::kind::string:
            result = LLSD(std::string(val.get_string().c_str(), val.get_string().size()));
            break;
        case boost::json::kind::bool_:
            result = LLSD(val.get_bool());
            break;
        case boost::json::kind::array:
            result = LLSD::emptyArray();
            for (const boost::json::value& e : val.get_array()) result.append(jsonToLLSD(e));
            break;
        case boost::json::kind::object:
            result = LLSD::emptyMap();
            for (const auto& kv : val.get_object()) result[std::string(kv.key())] = jsonToLLSD(kv.value());
            break;
        default:   // null
            break;
        }
        return result;
    }

    // Source: wolfgame.cpp:271-284 parseHeader - "id/part/total" (WolfGameModule.cs Send: id + "/" + part + "/" + total)
    bool parseHeader(const std::string& h, S64& id, S32& part, S32& total)
    {
        const size_t a = h.find('/');
        if (a == std::string::npos) return false;
        const size_t b = h.find('/', a + 1);
        if (b == std::string::npos || h.find('/', b + 1) != std::string::npos) return false;
        auto digits = [](const std::string& s) { return !s.empty() && s.size() <= 10 && s.find_first_not_of("0123456789") == std::string::npos; };
        const std::string sid = h.substr(0, a), spart = h.substr(a + 1, b - a - 1), stotal = h.substr(b + 1);
        if (!digits(sid) || !digits(spart) || !digits(stotal)) return false;
        id = std::stoll(sid);
        const S64 p = std::stoll(spart), t = std::stoll(stotal);
        if (t < 1 || t > MAX_PARTS || p < 0 || p >= t) return false;
        part = (S32)p;
        total = (S32)t;
        return true;
    }

    // Source: WolfGameModule.cs:4001-4015 Chunk - pieces of at most CHUNK_BYTES, backing off a continuation byte
    // (10xxxxxx) so a piece ends on a whole character
    std::vector<std::string> chunk(const std::string& utf8)
    {
        std::vector<std::string> l;
        size_t i = 0;
        while (i < utf8.size())
        {
            size_t n = std::min(CHUNK_BYTES, utf8.size() - i);
            while (n > 0 && i + n < utf8.size() && ((U8)utf8[i + n] & 0xC0) == 0x80) --n;
            if (n == 0) n = std::min(CHUNK_BYTES, utf8.size() - i);   // not UTF-8 at all: never loop forever
            l.push_back(utf8.substr(i, n));
            i += n;
        }
        if (l.empty()) l.emplace_back();
        return l;
    }
}

/** Source: wolfspeed.cpp:83-96 WolfSpeedPushHandler - registered with gGenericDispatcher. */
class WolfNPCPushHandler : public LLDispatchHandler
{
public:
    bool operator()(const LLDispatcher*, const std::string&, const LLUUID& invoice, const sparam_t& strings) override
    {
        if (WolfNPC::instanceExists())   // never brings a deleted singleton back during shutdown
        {
            WolfNPC::instance().receivePacket(invoice, strings);
        }
        return true;
    }
};

namespace
{
    WolfNPCPushHandler sNPCHandler;
}

WolfNPC::WolfNPC()
{
    if (!gGenericDispatcher.isHandlerPresent(MESSAGE_FROM_SIM))
    {
        gGenericDispatcher.addHandler(MESSAGE_FROM_SIM, &sNPCHandler);
    }
    // Source: wolfgame.cpp:317 - llagent.cpp setRegion fires this inside the region switch
    mRegionChangedConnection = gAgent.addRegionChangedCallback([this]() { onRegionChanged(); });
    mTokens = TOKEN_BUCKET;
    mTokenAt = now();
}

WolfNPC::~WolfNPC()
{
    if (mRegionChangedConnection.connected()) mRegionChangedConnection.disconnect();
}

void WolfNPC::onRegionChanged()
{
    // A request to the old region can no longer be answered (its reply would carry the old region's Invoice).
    mPartial.clear();
    mQueue.clear();
    failAll("You moved to another region before the answer came. Check the list and try again.");
}

void WolfNPC::failAll(const std::string& why)
{
    std::map<S32, Waiting> waiting;
    waiting.swap(mWaiting);   // a callback may send a new request
    for (auto& kv : waiting)
    {
        if (kv.second.mDone) kv.second.mDone(failure(kv.second.mOp, why));
    }
}

bool WolfNPC::takeToken()
{
    const F64 t = now();
    mTokens = std::min(TOKEN_BUCKET, mTokens + (t - mTokenAt) * TOKEN_REFILL);
    mTokenAt = t;
    if (mTokens < 1.0) return false;
    mTokens -= 1.0;
    return true;
}

void WolfNPC::request(const std::string& op, const LLSD& fields, reply_fn done)
{
    // Source: PROTOCOL.md section 2 "Both viewers send ONLY on Wolf Territories (WolfGrid::isOnWolfTerritoriesConfirmed)"
    if (!WolfGrid::isOnWolfTerritories())
    {
        if (done) done(failure(op, "WolfNPC works on Wolf Territories only."));
        return;
    }
    if (!gAgent.getRegion() || !WolfGrid::isOnWolfTerritoriesConfirmed())
    {
        if (done) done(failure(op, "Moving between regions - try again in a moment."));
        return;
    }
    if (mQueue.size() >= MAX_QUEUE)
    {
        if (done) done(failure(op, "Too many requests at once - wait a moment and try again."));
        return;
    }
    const S32 req = mNextReq;
    mNextReq = mNextReq >= 0x3fffffff ? 1 : mNextReq + 1;
    LLSD m = fields.isMap() ? fields : LLSD::emptyMap();
    m["op"] = op;
    m["req"] = req;
    const std::string json = boost::json::serialize(LlsdToJson(m));
    if (json.size() > MAX_MESSAGE_BYTES)
    {
        if (done) done(failure(op, llformat("That is too much to send at once (%u KB, the most is 16 KB). Shorten the personality, the routine or the reactions.",
                                            (U32)((json.size() + 1023) / 1024))));
        return;
    }
    const std::vector<std::string> pieces = chunk(json);
    const size_t total = (pieces.size() + CHUNKS_PER_PACKET - 1) / CHUNKS_PER_PACKET;
    const S32 id = mNextMessageId & 0x7fffffff;
    mNextMessageId = mNextMessageId >= 0x7ffffffe ? 1 : mNextMessageId + 1;
    Outgoing out;
    out.mReq = req;
    for (size_t part = 0; part < total; ++part)
    {
        std::vector<std::string> params;
        params.push_back(llformat("%d/%u/%u", id, (U32)part, (U32)total));
        for (size_t k = 0; k < CHUNKS_PER_PACKET; ++k)
        {
            const size_t i = part * CHUNKS_PER_PACKET + k;
            if (i >= pieces.size()) break;
            params.push_back(pieces[i]);
        }
        out.mPackets.push_back(params);
    }
    Waiting w;
    w.mDone = done;
    w.mOp = op;
    w.mDeadline = now() + (op == "create" ? CREATE_ANSWER_SEC : ANSWER_SEC);
    mWaiting[req] = w;
    mQueue.push_back(out);
    idle();   // goes at once while there is a token
}

void WolfNPC::idle()
{
    const F64 t = now();
    while (!mQueue.empty())
    {
        if (!gAgent.getRegion() || !WolfGrid::isOnWolfTerritoriesConfirmed())
        {
            mQueue.clear();
            failAll("Moving between regions - try again in a moment.");
            break;
        }
        if (!takeToken()) break;
        Outgoing out = mQueue.front();
        mQueue.pop_front();
        // Source: wolfgame.cpp:336-338 / llviewergenericmessage.cpp send_generic_message - every parameter goes out with its
        // NUL, the byte LLClientView's Util.FieldToString removes
        for (const std::vector<std::string>& params : out.mPackets) send_generic_message(METHOD_TO_SIM, params);
    }
    for (auto it = mPartial.begin(); it != mPartial.end();)
    {
        if (t - it->second.mFirst > PARTIAL_LIFE) it = mPartial.erase(it);
        else ++it;
    }
    if (!mWaiting.empty())
    {
        std::vector<std::pair<S32, Waiting>> late;
        for (auto it = mWaiting.begin(); it != mWaiting.end();)
        {
            if (t > it->second.mDeadline)
            {
                late.emplace_back(it->first, it->second);
                it = mWaiting.erase(it);
            }
            else ++it;
        }
        for (auto& l : late)
        {
            if (l.second.mDone)
            {
                l.second.mDone(failure(l.second.mOp, "The region did not answer. It may not have WolfNPC yet, or it is busy - try again in a minute."));
            }
        }
    }
}

void WolfNPC::receivePacket(const LLUUID& invoice, const std::vector<std::string>& strings)
{
    // Source: wolfgame.cpp:351-419 WolfGame::receivePacket - the same gate, region check, header and joining
    if (!WolfGrid::isOnWolfTerritoriesConfirmed()) return;
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn || rgn->getRegionID() != invoice)
    {
        LL_DEBUGS("WolfNPC") << "ignoring a WolfNPC message for another region" << LL_ENDL;
        return;
    }
    if (strings.empty()) return;
    S64 id = 0;
    S32 part = 0, total = 0;
    if (!parseHeader(strings[0], id, part, total))
    {
        LL_WARNS("WolfNPC") << "unreadable WolfNPC header '" << strings[0] << "'" << LL_ENDL;
        return;
    }
    std::string piece;
    for (size_t i = 1; i < strings.size(); ++i) piece += strings[i];
    std::string json;
    if (total == 1)
    {
        json.swap(piece);
    }
    else
    {
        if (!mPartial.count(id) && mPartial.size() >= MAX_PARTIAL) mPartial.erase(mPartial.begin());
        Pending& p = mPartial[id];
        if (p.mTotal == 0)
        {
            p.mTotal = total;
            p.mFirst = now();
        }
        if (p.mTotal != total)
        {
            LL_WARNS("WolfNPC") << "WolfNPC message " << id << " changed its part count; dropped" << LL_ENDL;
            mPartial.erase(id);
            return;
        }
        p.mParts[part] = piece;
        if ((S32)p.mParts.size() < total) return;
        for (auto& kv : p.mParts) json += kv.second;   // std::map: in part order
        mPartial.erase(id);
    }
    boost::system::error_code ec;
    boost::json::value v = boost::json::parse(json, ec);
    if (ec)
    {
        LL_WARNS("WolfNPC") << "unreadable WolfNPC JSON (" << json.size() << " bytes)" << LL_ENDL;
        return;
    }
    const LLSD sd = jsonToLLSD(v);
    if (sd.isMap()) dispatch(sd);
}

void WolfNPC::dispatch(const LLSD& m)
{
    const std::string op = m["op"].asString();
    // Source: PROTOCOL.md section 2 Pushes (op without req)
    if (!m.has("req") || m["req"].isUndefined())
    {
        if (op == "credits")
        {
            // "WolfViewer calls WolfAI::instance().creditsUsed()"
            WolfAI::instance().creditsUsed();
        }
        else if (op == "changed")
        {
            if (mChanged) mChanged(m["id"].asString());
        }
        else if (op == "gm_ended")
        {
            // Source: PROTOCOL.md §19 "the master then gets the push gm_ended {id, why}"
            if (mGmEnded) mGmEnded(m["id"].asString(), WolfGame::cleanText(m["why"].asString(), 300));
        }
        else
        {
            LL_DEBUGS("WolfNPC") << "unknown WolfNPC push '" << op << "'" << LL_ENDL;
        }
        return;
    }
    const LLSD& r = m["req"];
    if (!(r.isInteger() || r.isReal())) return;
    const F64 rv = r.asReal();
    if (!std::isfinite(rv) || rv < 1.0 || rv > (F64)0x3fffffff) return;
    const S32 req = (S32)std::llround(rv);
    auto it = mWaiting.find(req);
    if (it == mWaiting.end()) return;   // answered already, timed out, or not ours
    reply_fn done = it->second.mDone;
    mWaiting.erase(it);
    if (done) done(m);
}
