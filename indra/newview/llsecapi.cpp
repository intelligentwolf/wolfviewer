/**
 * @file llsecapi.cpp
 * @brief Security API for services such as certificate handling
 * secure local storage, etc.
 *
 * $LicenseInfo:firstyear=2009&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */


#include "llviewerprecompiledheaders.h"
#include "llsecapi.h"
#include "llsechandler_basic.h"
#include "llexception.h"
#include "stringize.h"
#include <openssl/evp.h>
#include <openssl/err.h>
#include <map>

#include <openssl/opensslv.h>
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
#include <openssl/provider.h> // <WolfViewer 2026-09-26/> OpenSSL 3 providers
#else
#include <openssl/engine.h> // <FS:ND> To enable rdrand engine if possible
#endif

std::map<std::string, LLPointer<LLSecAPIHandler> > gHandlerMap;
LLPointer<LLSecAPIHandler> gSecAPIHandler;
// <WolfViewer 2026-09-26> OpenSSL 3 moved RC4 into the "legacy" provider, which is not loaded by
// default; the saved-credential store (llsechandler_basic.cpp, EVP_rc4) cannot be read or
// written without it. Our OpenSSL is built with no-module, so the provider is inside libcrypto.
// Source: AlchemyViewer cd3d29cfd3 (llsecapi.cpp) — the same load, retaining the default
// provider as the fallback.
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
static OSSL_PROVIDER* gOSSLLegacyProvider = nullptr;
#endif

void initializeSecHandler()
{
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    // <WolfViewer 2026-09-26> see gOSSLLegacyProvider
    if (!gOSSLLegacyProvider && OSSL_PROVIDER_available(nullptr, "legacy") == 0)
    {
        gOSSLLegacyProvider = OSSL_PROVIDER_try_load(nullptr, "legacy", 1);
    }
    if (OSSL_PROVIDER_available(nullptr, "legacy") == 0)
    {
        LL_WARNS() << "Failed to load the OpenSSL legacy provider; saved logins cannot be read or written." << LL_ENDL;
    }
#endif

    ERR_load_crypto_strings();
    OpenSSL_add_all_algorithms();

#if OPENSSL_VERSION_NUMBER < 0x30000000L
    // <FS:ND If we can, enabled the rdrand engine. It is available as a CPU instruction on newer Intel CPUs
    ENGINE_load_builtin_engines();

    ENGINE* engRdRand = ENGINE_by_id("rdrand");
    if( engRdRand )
        ENGINE_set_default(engRdRand, ENGINE_METHOD_RAND);
#endif
    // <WolfViewer 2026-09-26> OpenSSL 3: no rdrand ENGINE. ENGINEs are deprecated there, and its
    // random generator seeds from the operating system itself (Alchemy dropped it the same way).
    unsigned long lErr ( ERR_get_error() );
    while( lErr )
    {
        char aError[128];
        ERR_error_string_n( lErr, aError, sizeof( aError ) );
        LL_WARNS() << aError << LL_ENDL;

        lErr = ERR_get_error();
    }
    // </FS:ND>

    gHandlerMap[BASIC_SECHANDLER] = new LLSecAPIBasicHandler();


    // Currently, we only have the Basic handler, so we can point the main sechandler
    // pointer to the basic handler.  Later, we'll create a wrapper handler that
    // selects the appropriate sechandler as needed, for instance choosing the
    // mac keyring handler, with fallback to the basic sechandler
    gSecAPIHandler = gHandlerMap[BASIC_SECHANDLER];

    // initialize all SecAPIHandlers
    std::string exception_msg;
    std::map<std::string, LLPointer<LLSecAPIHandler> >::const_iterator itr;
    for(itr = gHandlerMap.begin(); itr != gHandlerMap.end(); ++itr)
    {
        LLPointer<LLSecAPIHandler> handler = (*itr).second;
        try
        {
            handler->init();
        }
        catch (LLProtectedDataException& e)
        {
            exception_msg = e.what();
        }
    }
    if (!exception_msg.empty())  // an exception was thrown.
    {
        LLTHROW(LLProtectedDataException(exception_msg));
    }

}

void clearSecHandler()
{
    gSecAPIHandler = NULL;
    gHandlerMap.clear();
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    if (gOSSLLegacyProvider)   // <WolfViewer 2026-09-26/>
    {
        OSSL_PROVIDER_unload(gOSSLLegacyProvider);
        gOSSLLegacyProvider = nullptr;
    }
#endif
}
// start using a given security api handler.  If the string is empty
// the default is used
LLPointer<LLSecAPIHandler> getSecHandler(const std::string& handler_type)
{
    if (gHandlerMap.find(handler_type) != gHandlerMap.end())
    {
        return gHandlerMap[handler_type];
    }
    else
    {
        return LLPointer<LLSecAPIHandler>(NULL);
    }
}
// register a handler
void registerSecHandler(const std::string& handler_type,
                        LLPointer<LLSecAPIHandler>& handler)
{
    gHandlerMap[handler_type] = handler;
}

std::ostream& operator <<(std::ostream& s, const LLCredential& cred)
{
    return s << (std::string)cred;
}

LLSD LLCredential::getLoginParams()
{
    LLSD result = LLSD::emptyMap();
    std::string username;
    try
    {
        if (mIdentifier["type"].asString() == "agent")
        {
            // legacy credential
            result["passwd"] = "$1$" + mAuthenticator["secret"].asString();
            result["first"] = mIdentifier["first_name"];
            result["last"] = mIdentifier["last_name"];
            username = result["first"].asString() + " " + result["last"].asString();
        }
        else if (mIdentifier["type"].asString() == "account")
        {
            result["username"] = mIdentifier["account_name"];
            result["passwd"] = mAuthenticator["secret"].asString();
            username = result["username"].asString();
        }
    }
    catch (...)
    {
        // nat 2016-08-18: not clear what exceptions the above COULD throw?!
        LOG_UNHANDLED_EXCEPTION(STRINGIZE("for '" << username << "'"));
        // we could have corrupt data, so simply return a null login param if so
        LL_WARNS("AppInit") << "Invalid credential" << LL_ENDL;
    }
    return result;
}

void LLCredential::identifierType(std::string &idType)
{
    if(mIdentifier.has("type"))
    {
        idType = mIdentifier["type"].asString();
    }
    else {
        idType = std::string();

    }
}

void LLCredential::authenticatorType(std::string &idType)
{
    if(mAuthenticator.has("type"))
    {
        idType = mAuthenticator["type"].asString();
    }
    else {
        idType = std::string();

    }
}

LLCertException::LLCertException(const LLSD& cert_data, const std::string& msg)
  : LLException(msg),
    mCertData(cert_data)
{
    LL_WARNS("SECAPI") << "Certificate Error: " << msg << LL_ENDL;
}
