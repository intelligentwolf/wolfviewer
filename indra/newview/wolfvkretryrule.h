/**
 * @file wolfvkretryrule.h
 * @brief When an automatic switch to OpenGL is retried: the failure record's stamp and count (wolfvkbridge.cpp).
 *
 * [W106 VKRETRY 2026-11-13] Miss Zenn (tickets 805/808/832, AMD RX 9070 XT, driver 26.10.1, Windows): her starts
 * crashed inside the AMD OpenGL driver during the shared-layer self-test (WolfVkShare::selfTest). The viewer fell back
 * to OpenGL after AUTO_RETRY_LIMIT such starts, but the failure record was stamped with the viewer version, so every
 * new release cleared it (decideBackend: a stamp that does not match starts the count again) and crashed her again.
 * A crash in that self-test (with crash evidence: review W2) is now recorded for the OpenGL driver alone (stamp version "*"): retried while fewer than
 * AUTO_RETRY_LIMIT are counted, as before, but once the limit is reached only a different driver (or GPU) tries Vulkan
 * again; a new viewer version does not.
 *
 * Pure functions only (no settings, no viewer state), so the rule can be tested on its own (harness:
 * ~/.cache/w106/vkretry/).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer Source Code
 * Copyright (C) 2026, IntelligentWolf Ltd
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef WOLF_VK_RETRY_RULE_H
#define WOLF_VK_RETRY_RULE_H

#include <cstdint>
#include <string>

namespace WolfVkRetryRule
{
    // Source: wolfvkbridge.cpp [G5 2026-11-11] AUTO_RETRY_LIMIT = 2 (moved here unchanged)
    constexpr uint32_t AUTO_RETRY_LIMIT = 2;

    enum class Failure
    {
        // counted per viewer version and OpenGL driver: a new version or driver starts the count again (the G5 rule)
        PER_VERSION,
        // [W106] a start that crashed in the shared-layer self-test: counted per OpenGL driver only, so a new viewer
        // version keeps the fall-back once AUTO_RETRY_LIMIT are counted; only a driver change retries
        SAME_DRIVER_KEEPS,
        // a failure no retry can mend (no Vulkan, no usable Vulkan graphics card): the limit at once, driver only
        DRIVER_CHANGE_ONLY
    };

    // The driver string is WolfVulkanLastGLDriver: "<GL_VENDOR> / <GL_RENDERER> / <GL_VERSION>" (wolfvkbridge.cpp
    // initAfterGL). Known when any of the three had text - i.e. something other than the two " / " separators.
    inline bool driverKnown(const std::string& driver)
    {
        return driver.find_first_not_of(" /") != std::string::npos;
    }

    // Source: wolfvkbridge.cpp [G5] failureStamp(): "<version>|<driver>", version "*" for a driver-only record
    inline std::string stamp(bool any_version, const std::string& version, const std::string& driver)
    {
        return (any_version ? std::string("*") : version) + "|" + driver;
    }

    // Source: wolfvkbridge.cpp [G5] stampMatches(): a "*|" stamp ignores the viewer version
    inline bool matches(const std::string& saved, const std::string& version, const std::string& driver)
    {
        if (saved.compare(0, 2, "*|") == 0) return saved == stamp(true, version, driver);
        return saved == stamp(false, version, driver);
    }

    struct Record
    {
        uint32_t mCount = 0;    // WolfVulkanAutoFailures
        std::string mStamp;     // WolfVulkanAutoFailStamp
    };

    // The record after one more automatic failure (wolfvkbridge.cpp recordAutomaticFailure).
    inline Record afterFailure(const Record& saved, Failure kind, const std::string& version, const std::string& driver)
    {
        // [W106] a SAME_DRIVER_KEEPS failure is tied to the driver only when the driver is known: without it nothing
        // proves a later start runs the same driver, so it is counted per version as before (a viewer update then tries
        // once more, today's behaviour) rather than keeping OpenGL on a guess
        const bool any_version = kind == Failure::DRIVER_CHANGE_ONLY || (kind == Failure::SAME_DRIVER_KEEPS && driverKnown(driver));
        Record r;
        r.mStamp = stamp(any_version, version, driver);
        if (kind == Failure::DRIVER_CHANGE_ONLY)
        {
            r.mCount = AUTO_RETRY_LIMIT;
        }
        else
        {
            // the count goes on while the saved record still applies to this version and driver (either stamp form);
            // otherwise this failure is the first. [G5] compared the stamps for equality, which is the same thing for
            // every pair the G5 kinds can leave (a "*" record is always at the limit, so it is never retried on the same
            // driver to fail again); matches() also lets a self-test crash follow a per-version failure, and back.
            const uint32_t before = (saved.mCount > 0 && matches(saved.mStamp, version, driver)) ? saved.mCount : 0;
            r.mCount = before + 1;
        }
        return r;
    }

    // [review W2 2026-11-13] The failure of a start that never confirmed its login screen (decideBackend, WolfVulkanStartOK
    // false): driver-only only when it ended in the shared-layer self-test AND left crash evidence (its crash stack, or an
    // LLError / out-of-memory exec marker). A start ended there by a kill (Task Manager, a power cut, the resident closing
    // a slow start) leaves none and is counted per version as before, so kills on a healthy driver cannot keep Vulkan
    // off across viewer versions.
    inline Failure unconfirmedStartFailure(bool ended_in_layer_self_test, bool crash_evidence)
    {
        return (ended_in_layer_self_test && crash_evidence) ? Failure::SAME_DRIVER_KEEPS : Failure::PER_VERSION;
    }

    enum class Start
    {
        AS_SAVED,       // the saved "opengl" stays
        RETRY,          // Vulkan once more, the count kept (fewer than AUTO_RETRY_LIMIT for this version/driver)
        RETRY_FRESH     // Vulkan once more, the record cleared (another version for a per-version record, or another driver)
    };

    // A saved "opengl" the viewer set itself (count > 0) at the next start (wolfvkbridge.cpp decideBackend [G5]).
    inline Start atStart(const Record& saved, const std::string& version, const std::string& driver)
    {
        if (saved.mCount == 0) return Start::AS_SAVED;
        if (!matches(saved.mStamp, version, driver)) return Start::RETRY_FRESH;
        return saved.mCount < AUTO_RETRY_LIMIT ? Start::RETRY : Start::AS_SAVED;
    }

    // Source: wolfvkbridge.cpp [round 3] nextStartRetries(): the same stamp at the next start
    inline bool nextStartRetries(uint32_t count)
    {
        return count > 0 && count < AUTO_RETRY_LIMIT;
    }
}

#endif // WOLF_VK_RETRY_RULE_H
