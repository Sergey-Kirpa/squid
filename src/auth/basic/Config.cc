/*
 * Copyright (C) 1996-2019 The Squid Software Foundation and contributors
 *
 * Squid software is distributed under GPLv2+ license and includes
 * contributions from numerous individuals and organizations.
 * Please see the COPYING and CONTRIBUTORS files for details.
 */

/* DEBUG: section 29    Authenticator */

/* The functions in this file handle authentication.
 * They DO NOT perform access control or auditing.
 * See acl.c for access control and client_side.c for auditing */

#include "squid.h"
#include "auth/basic/Config.h"
#include "auth/basic/Scheme.h"
#include "auth/basic/User.h"
#include "auth/basic/UserRequest.h"
#include "auth/CredentialsCache.h"
#include "auth/Gadgets.h"
#include "auth/State.h"
#include "base64.h"
#include "cache_cf.h"
#include "charset.h"
#include "helper.h"
#include "HttpHeaderTools.h"
#include "HttpReply.h"
#include "mgr/Registration.h"
#include "rfc1738.h"
#include "SquidTime.h"
#include "Store.h"
#include "util.h"
#include "wordlist.h"
#include "helper/protocol_defines.h"

/* Basic Scheme */
static AUTHSSTATS authenticateBasicStats;

helper *basicauthenticators = NULL;

static int authbasic_initialised = 0;

/*
 *
 * Public Functions
 *
 */

/* internal functions */

bool
Auth::Basic::Config::active() const
{
    return authbasic_initialised == 1;
}

bool
Auth::Basic::Config::configured() const
{
    if ((authenticateProgram != NULL) && (authenticateChildren.n_max != 0) && !realm.isEmpty()) {
        debugs(29, 9, HERE << "returning configured");
        return true;
    }

    debugs(29, 9, HERE << "returning unconfigured");
    return false;
}

const char *
Auth::Basic::Config::type() const
{
    return Auth::Basic::Scheme::GetInstance()->type();
}

void
Auth::Basic::Config::fixHeader(Auth::UserRequest::Pointer, HttpReply *rep, Http::HdrType hdrType, HttpRequest *)
{
    if (authenticateProgram) {
        debugs(29, 9, "Sending type:" << hdrType << " header: 'Basic realm=\"" << realm << "\"'");
        httpHeaderPutStrf(&rep->header, hdrType, "Basic realm=\"" SQUIDSBUFPH "\"", SQUIDSBUFPRINT(realm));
    }
}

void
Auth::Basic::Config::rotateHelpers()
{
    /* schedule closure of existing helpers */
    if (basicauthenticators) {
        helperShutdown(basicauthenticators);
    }

    /* NP: dynamic helper restart will ensure they start up again as needed. */
}

/** shutdown the auth helpers and free any allocated configuration details */
void
Auth::Basic::Config::done()
{
    Auth::SchemeConfig::done();

    authbasic_initialised = 0;

    if (basicauthenticators) {
        helperShutdown(basicauthenticators);
    }

    delete basicauthenticators;
    basicauthenticators = NULL;

    if (authenticateProgram)
        wordlistDestroy(&authenticateProgram);
}

bool
Auth::Basic::Config::dump(StoreEntry * entry, const char *name, Auth::SchemeConfig * scheme) const
{
    if (!Auth::SchemeConfig::dump(entry, name, scheme))
        return false; // not configured

    storeAppendPrintf(entry, "%s basic credentialsttl %d seconds\n", name, (int) credentialsTTL);
    storeAppendPrintf(entry, "%s basic casesensitive %s\n", name, casesensitive ? "on" : "off");
    return true;
}

Auth::Basic::Config::Config() :
    credentialsTTL( 2*60*60 ),
    casesensitive(0)
{
    static const SBuf defaultRealm("Squid proxy-caching web server");
    realm = defaultRealm;
}

void
Auth::Basic::Config::parse(Auth::SchemeConfig * scheme, int n_configured, char *param_str)
{
    if (strcmp(param_str, "credentialsttl") == 0) {
        parse_time_t(&credentialsTTL);
    } else if (strcmp(param_str, "casesensitive") == 0) {
        parse_onoff(&casesensitive);
    } else
        Auth::SchemeConfig::parse(scheme, n_configured, param_str);
}

static void
authenticateBasicStats(StoreEntry * sentry)
{
    if (basicauthenticators)
        basicauthenticators->packStatsInto(sentry, "Basic Authenticator Statistics");
}

inline size_t
inlineUTF8SequenceLengthNonASCII(char b0)
{
    if ((b0 & 0xC0) != 0xC0)
        return 0;
    if ((b0 & 0xE0) == 0xC0)
        return 2;
    if ((b0 & 0xF0) == 0xE0)
        return 3;
    if ((b0 & 0xF8) == 0xF0)
        return 4;
    return 0;
}

inline size_t
inlineUTF8SequenceLength(char b0)
{
    return (b0 & 0x80) == 0 ? 1 : inlineUTF8SequenceLengthNonASCII(b0);
}

/*
 * Utility routine to tell whether a sequence of bytes is legal UTF-8.
 * This must be called with the length pre-determined by the first byte.
 * If presented with a length > 4, this returns false.  The Unicode
 * definition of UTF-8 goes up to 4-byte sequences.
 */
static bool
isLegalUTF8(const unsigned char* source, int length)
{
    unsigned char a;
    const unsigned char* srcptr = source + length;
    switch (length) {
        default: return false;
        // Everything else falls through when "true"...
        case 4: if ((a = (*--srcptr)) < 0x80 || a > 0xBF) return false;
        case 3: if ((a = (*--srcptr)) < 0x80 || a > 0xBF) return false;
        case 2: if ((a = (*--srcptr)) > 0xBF) return false;


        switch (*source) {
            // no fall-through in this inner switch
            case 0xE0: if (a < 0xA0) return false; break;
            case 0xED: if (a > 0x9F) return false; break;
            case 0xF0: if (a < 0x90) return false; break;
            case 0xF4: if (a > 0x8F) return false; break;
            default:   if (a < 0x80) return false;
        }


        case 1: if (*source >= 0x80 && *source < 0xC2) return false;
    }
    if (*source > 0xF4)
        return false;
    return true;
}

/*
 * Returns whether a UTF-8 string is legal or not.
 */
static bool
isLegalUTF8String(const char *source, const char *sourceEnd) {
    while (source != sourceEnd) {
        int length = inlineUTF8SequenceLength(*source);
        if (length > sourceEnd - source || !isLegalUTF8((const unsigned char*) source, length))
            return false;
        source += length;
    }
    return true;
}

char *
Auth::Basic::Config::decodeCleartext(const char *httpAuthHeader)
{
    const char *proxy_auth = httpAuthHeader;

    /* trim BASIC from string */
    while (xisgraph(*proxy_auth))
        ++proxy_auth;

    /* Trim leading whitespace before decoding */
    while (xisspace(*proxy_auth))
        ++proxy_auth;

    /* Trim trailing \n before decoding */
    // XXX: really? is the \n actually still there? does the header parse not drop it?
    char *eek = xstrdup(proxy_auth);
    strtok(eek, "\n");

    const size_t srcLen = strlen(eek);
    char *cleartext = static_cast<char*>(xmalloc(BASE64_DECODE_LENGTH(srcLen)+1));

    struct base64_decode_ctx ctx;
    base64_decode_init(&ctx);

    size_t dstLen = 0;
    if (base64_decode_update(&ctx, &dstLen, reinterpret_cast<uint8_t*>(cleartext), srcLen, eek) && base64_decode_final(&ctx)) {
        cleartext[dstLen] = '\0';

	if (!isLegalUTF8String(cleartext, cleartext + dstLen)) {
	    char buf[HELPER_INPUT_BUFFER];
	    latin1_to_utf8(buf, sizeof(buf), cleartext);
            safe_free(cleartext);
	    cleartext = xstrdup(buf);
	}

        /*
         * Don't allow NL or CR in the credentials.
         * Oezguer Kesim <oec@codeblau.de>
         */
        debugs(29, 9, HERE << "'" << cleartext << "'");

        if (strcspn(cleartext, "\r\n") != strlen(cleartext)) {
            debugs(29, DBG_IMPORTANT, "WARNING: Bad characters in authorization header '" << httpAuthHeader << "'");
            safe_free(cleartext);
        }
    } else {
        debugs(29, 2, "WARNING: Invalid Base64 character in authorization header '" << httpAuthHeader << "'");
        safe_free(cleartext);
    }

    safe_free(eek);
    return cleartext;
}

/**
 * Decode a Basic [Proxy-]Auth string, linking the passed
 * auth_user_request structure to any existing user structure or creating one
 * if needed. Note that just returning will be treated as
 * "cannot decode credentials". Use the message field to return a
 * descriptive message to the user.
 */
Auth::UserRequest::Pointer
Auth::Basic::Config::decode(char const *proxy_auth, const char *aRequestRealm)
{
    Auth::UserRequest::Pointer auth_user_request = dynamic_cast<Auth::UserRequest*>(new Auth::Basic::UserRequest);
    /* decode the username */

    // retrieve the cleartext (in a dynamically allocated char*)
    char *cleartext = decodeCleartext(proxy_auth);

    // empty header? no auth details produced...
    if (!cleartext)
        return auth_user_request;

    Auth::User::Pointer lb;
    /* permitted because local_basic is purely local function scope. */
    Auth::Basic::User *local_basic = NULL;

    char *separator = strchr(cleartext, ':');

    lb = local_basic = new Auth::Basic::User(this, aRequestRealm);

    if (separator) {
        /* terminate the username */
        *separator = '\0';
        local_basic->passwd = xstrdup(separator+1);
    }

    if (!casesensitive)
        Tolower(cleartext);
    local_basic->username(cleartext);

    if (local_basic->passwd == NULL) {
        debugs(29, 4, HERE << "no password in proxy authorization header '" << proxy_auth << "'");
        auth_user_request->setDenyMessage("no password was present in the HTTP [proxy-]authorization header. This is most likely a browser bug");
    } else {
        if (local_basic->passwd[0] == '\0') {
            debugs(29, 4, HERE << "Disallowing empty password. User is '" << local_basic->username() << "'");
            safe_free(local_basic->passwd);
            auth_user_request->setDenyMessage("Request denied because you provided an empty password. Users MUST have a password.");
        }
    }

    xfree(cleartext);

    if (!local_basic->valid()) {
        lb->auth_type = Auth::AUTH_BROKEN;
        auth_user_request->user(lb);
        return auth_user_request;
    }

    /* now lookup and see if we have a matching auth_user structure in memory. */
    Auth::User::Pointer auth_user;

    if (!(auth_user = Auth::Basic::User::Cache()->lookup(lb->userKey()))) {
        /* the user doesn't exist in the username cache yet */
        /* save the credentials */
        debugs(29, 9, HERE << "Creating new user '" << lb->username() << "'");
        /* set the auth_user type */
        lb->auth_type = Auth::AUTH_BASIC;
        /* current time for timeouts */
        lb->expiretime = current_time.tv_sec;

        /* this basic_user struct is the 'lucky one' to get added to the username cache */
        /* the requests after this link to the basic_user */
        /* store user in hash */
        lb->addToNameCache();

        auth_user = lb;
        assert(auth_user != NULL);
    } else {
        /* replace the current cached password with the new one */
        Auth::Basic::User *basic_auth = dynamic_cast<Auth::Basic::User *>(auth_user.getRaw());
        assert(basic_auth);
        basic_auth->updateCached(local_basic);
        auth_user = basic_auth;
    }

    /* link the request to the in-cache user */
    auth_user_request->user(auth_user);
    return auth_user_request;
}

/** Initialize helpers and the like for this auth scheme. Called AFTER parsing the
 * config file */
void
Auth::Basic::Config::init(Auth::SchemeConfig *)
{
    if (authenticateProgram) {
        authbasic_initialised = 1;

        if (basicauthenticators == NULL)
            basicauthenticators = new helper("basicauthenticator");

        basicauthenticators->cmdline = authenticateProgram;

        basicauthenticators->childs.updateLimits(authenticateChildren);

        basicauthenticators->ipc_type = IPC_STREAM;

        helperOpenServers(basicauthenticators);
    }
}

void
Auth::Basic::Config::registerWithCacheManager(void)
{
    Mgr::RegisterAction("basicauthenticator",
                        "Basic User Authenticator Stats",
                        authenticateBasicStats, 0, 1);
}

