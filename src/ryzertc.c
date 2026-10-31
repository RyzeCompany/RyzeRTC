/*
 * RyzeRTC: native WebRTC PHP extension, Copyright (c) 2026 RyzeCompany (MIT).
 * Phase 1: synchronous peer handle management and bounded SDP operations.
 * Never enter Zend VM from libdatachannel's background threads.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ryzertc.h"
#include "ext/standard/info.h"
#include "Zend/zend_exceptions.h"
#include "rtc/rtc.h"
#include <stdint.h>
#include <string.h>
#include <arpa/inet.h>

#define RYZERTC_MAX_SDP 65536
#define RYZERTC_MAX_ICE_CANDIDATE 8192
#define RYZERTC_MAX_ICE_MID 128


/*
 * All libdatachannel callbacks run without Zend access. A single native mutex
 * protects the bounded event queue and peer/channel ownership registry.
 * Callback "ptr" is an opaque integer token, NEVER a Zend object pointer.
 * Stale callbacks after rtcDeletePeerConnection cannot enqueue new events.
 */
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>

#define RYZERTC_MAX_MESSAGE 262144
#define RYZERTC_MAX_EVENTS 512
#define RYZERTC_MAX_QUEUE_BYTES (4 * 1024 * 1024)
#define RYZERTC_MAX_CHANNELS_PER_PEER 64
#define RYZERTC_MAX_BUFFERED 4194304

typedef struct ryzertc_owner {
    int peer;
    int channel; /* -1 for a peer connection */
    uintptr_t token;
    struct ryzertc_owner *next;
} ryzertc_owner;

typedef struct ryzertc_event {
    uintptr_t token;
    int peer;
    int channel;
    char kind[24];
    char *data;
    size_t data_len;
    char *detail;
    size_t detail_len;
    struct ryzertc_event *next;
} ryzertc_event;

static pthread_mutex_t ryzertc_lock = PTHREAD_MUTEX_INITIALIZER;
static ryzertc_owner *ryzertc_owners = NULL;
static ryzertc_event *ryzertc_first = NULL, *ryzertc_last = NULL;
static size_t ryzertc_queue_count = 0, ryzertc_queue_bytes = 0;
static uintptr_t ryzertc_next_token = 0;

static bool ryzertc_has_owner_locked(uintptr_t token, int channel) {
    for (ryzertc_owner *o = ryzertc_owners; o != NULL; o = o->next) {
        if (o->token == token && o->channel == channel) return true;
    }
    return false;
}
static uintptr_t ryzertc_register_peer(int peer) {
    ryzertc_owner *o = calloc(1, sizeof(*o));
    if (o == NULL) return 0;
    pthread_mutex_lock(&ryzertc_lock);
    o->token = ++ryzertc_next_token;
    if (o->token == 0) o->token = ++ryzertc_next_token;
    o->peer = peer;
    o->channel = -1;
    o->next = ryzertc_owners;
    ryzertc_owners = o;
    pthread_mutex_unlock(&ryzertc_lock);
    return o->token;
}
static bool ryzertc_register_channel(uintptr_t token, int peer, int channel) {
    ryzertc_owner *o = calloc(1, sizeof(*o));
    if (o == NULL) return false;
    pthread_mutex_lock(&ryzertc_lock);
    if (!ryzertc_has_owner_locked(token, -1)) {
        pthread_mutex_unlock(&ryzertc_lock);
        free(o);
        return false;
    }
    size_t count = 0;
    for (ryzertc_owner *it = ryzertc_owners; it; it = it->next)
        if (it->token == token && it->channel >= 0) count++;
    if (count >= RYZERTC_MAX_CHANNELS_PER_PEER) {
        pthread_mutex_unlock(&ryzertc_lock);
        free(o);
        return false;
    }
    o->token = token; o->peer = peer; o->channel = channel;
    o->next = ryzertc_owners;
    ryzertc_owners = o;
    pthread_mutex_unlock(&ryzertc_lock);
    return true;
}
static bool ryzertc_owns_channel(uintptr_t token, int channel) {
    pthread_mutex_lock(&ryzertc_lock);
    bool valid = ryzertc_has_owner_locked(token, -1) &&
        ryzertc_has_owner_locked(token, channel);
    pthread_mutex_unlock(&ryzertc_lock);
    return valid;
}
static void ryzertc_free_event(ryzertc_event *e) {
    free(e->data);
    free(e->detail);
    free(e);
}
static size_t ryzertc_list_channels(uintptr_t token, int *ids, size_t cap) {
    size_t n = 0;
    pthread_mutex_lock(&ryzertc_lock);
    for (ryzertc_owner *it = ryzertc_owners; it != NULL; it = it->next) {
        if (it->token == token && it->channel >= 0 && n < cap) {
            ids[n++] = it->channel;
        }
    }
    pthread_mutex_unlock(&ryzertc_lock);
    return n;
}
static void ryzertc_unregister_peer(uintptr_t token) {
    pthread_mutex_lock(&ryzertc_lock);
    ryzertc_owner **owner = &ryzertc_owners;
    while (*owner != NULL) {
        if ((*owner)->token == token) {
            ryzertc_owner *old = *owner;
            *owner = old->next;
            free(old);
        } else owner = &(*owner)->next;
    }
    ryzertc_event **ev = &ryzertc_first;
    while (*ev != NULL) {
        if ((*ev)->token == token) {
            ryzertc_event *old = *ev;
            *ev = old->next;
            ryzertc_queue_count--;
            ryzertc_queue_bytes -= old->data_len + old->detail_len;
            ryzertc_free_event(old);
        } else ev = &(*ev)->next;
    }
    ryzertc_last = NULL;
    for (ryzertc_event *p = ryzertc_first; p; p = p->next) ryzertc_last = p;
    pthread_mutex_unlock(&ryzertc_lock);
}
static void ryzertc_enqueue(uintptr_t token, int peer, int channel,
        const char *kind, const char *data, size_t len, const char *detail, size_t detail_len) {
    if (len > RYZERTC_MAX_MESSAGE || detail_len > RYZERTC_MAX_ICE_MID ||
        len > RYZERTC_MAX_QUEUE_BYTES - detail_len) return;
    ryzertc_event *ev = calloc(1, sizeof(*ev));
    if (ev == NULL) return;
    if (len) {
        ev->data = malloc(len);
        if (ev->data == NULL) { free(ev); return; }
        memcpy(ev->data, data, len);
    }
    if (detail_len) {
        ev->detail = malloc(detail_len);
        if (ev->detail == NULL) { ryzertc_free_event(ev); return; }
        memcpy(ev->detail, detail, detail_len);
    }
    ev->token = token; ev->peer = peer; ev->channel = channel;
    snprintf(ev->kind, sizeof(ev->kind), "%s", kind);
    ev->data_len = len; ev->detail_len = detail_len;
    pthread_mutex_lock(&ryzertc_lock);
    if (!ryzertc_has_owner_locked(token, -1) ||
        (channel >= 0 && !ryzertc_has_owner_locked(token, channel)) ||
        ryzertc_queue_count >= RYZERTC_MAX_EVENTS ||
        ryzertc_queue_bytes + len + detail_len > RYZERTC_MAX_QUEUE_BYTES) {
        pthread_mutex_unlock(&ryzertc_lock);
        ryzertc_free_event(ev);
        return;
    }
    if (ryzertc_last != NULL) ryzertc_last->next = ev;
    else ryzertc_first = ev;
    ryzertc_last = ev;
    ryzertc_queue_count++;
    ryzertc_queue_bytes += len + detail_len;
    pthread_mutex_unlock(&ryzertc_lock);
}
static size_t ryzertc_bounded_len(const char *text, size_t max) {
    if (text == NULL) return 0;
    size_t i = 0;
    while (i <= max && text[i] != '\0') ++i;
    return i;
}
static void ryzertc_description_callback(int pc, const char *sdp,
        const char *type, void *ptr) {
    size_t n = ryzertc_bounded_len(sdp, RYZERTC_MAX_SDP);
    size_t m = ryzertc_bounded_len(type, RYZERTC_MAX_ICE_MID);
    if (n <= RYZERTC_MAX_SDP && m <= RYZERTC_MAX_ICE_MID)
        ryzertc_enqueue((uintptr_t) ptr, pc, -1, "description", sdp, n, type, m);
}
static void ryzertc_candidate_callback(int pc, const char *candidate,
        const char *mid, void *ptr) {
    size_t n = ryzertc_bounded_len(candidate, RYZERTC_MAX_ICE_CANDIDATE);
    size_t m = ryzertc_bounded_len(mid, RYZERTC_MAX_ICE_MID);
    if (n <= RYZERTC_MAX_ICE_CANDIDATE && m <= RYZERTC_MAX_ICE_MID)
        ryzertc_enqueue((uintptr_t) ptr, pc, -1, "candidate", candidate, n, mid, m);
}
static void ryzertc_state_callback(int pc, rtcState state, void *ptr) {
    char value[24];
    int n = snprintf(value, sizeof(value), "%d", (int)state);
    if (n > 0) ryzertc_enqueue((uintptr_t)ptr, pc, -1, "state", value, (size_t)n, NULL, 0);
}
static void ryzertc_gathering_callback(int pc, rtcGatheringState state, void *ptr) {
    char value[24];
    int n = snprintf(value, sizeof(value), "%d", (int)state);
    if (n > 0) ryzertc_enqueue((uintptr_t)ptr, pc, -1, "gathering", value, (size_t)n, NULL, 0);
}
static void ryzertc_channel_open(int dc, void *ptr) {
    ryzertc_enqueue((uintptr_t)ptr, -1, dc, "channel_open", NULL, 0, NULL, 0);
}
static void ryzertc_channel_closed(int dc, void *ptr) {
    ryzertc_enqueue((uintptr_t)ptr, -1, dc, "channel_closed", NULL, 0, NULL, 0);
}
static void ryzertc_channel_message(int dc, const char *data, int size, void *ptr) {
    if (data == NULL) return;
    size_t len = size >= 0 ? (size_t)size :
        ryzertc_bounded_len(data, RYZERTC_MAX_MESSAGE);
    if (len <= RYZERTC_MAX_MESSAGE)
        ryzertc_enqueue((uintptr_t)ptr, -1, dc, "message", data, len, NULL, 0);
}
static bool ryzertc_attach_channel(uintptr_t token, int pc, int dc) {
    if (!ryzertc_register_channel(token, pc, dc)) return false;
    void *ptr = (void *)token;
    rtcSetUserPointer(dc, ptr);
    if (rtcSetOpenCallback(dc, ryzertc_channel_open) != RTC_ERR_SUCCESS ||
        rtcSetClosedCallback(dc, ryzertc_channel_closed) != RTC_ERR_SUCCESS ||
        rtcSetMessageCallback(dc, ryzertc_channel_message) != RTC_ERR_SUCCESS) return false;
    ryzertc_enqueue(token, pc, dc, "channel", NULL, 0, NULL, 0);
    return true;
}
static void ryzertc_channel_callback(int pc, int dc, void *ptr) {
    ryzertc_attach_channel((uintptr_t)ptr, pc, dc);
}
static bool ryzertc_attach_peer(uintptr_t token, int pc) {
    void *ptr = (void *)token;
    rtcSetUserPointer(pc, ptr);
    return rtcSetLocalDescriptionCallback(pc, ryzertc_description_callback) == RTC_ERR_SUCCESS &&
        rtcSetLocalCandidateCallback(pc, ryzertc_candidate_callback) == RTC_ERR_SUCCESS &&
        rtcSetStateChangeCallback(pc, ryzertc_state_callback) == RTC_ERR_SUCCESS &&
        rtcSetGatheringStateChangeCallback(pc, ryzertc_gathering_callback) == RTC_ERR_SUCCESS &&
        rtcSetDataChannelCallback(pc, ryzertc_channel_callback) == RTC_ERR_SUCCESS;
}

static zend_class_entry *runtime_ce;
static zend_class_entry *peer_ce;
static zend_object_handlers peer_handlers;

typedef struct {
    int handle;
    uintptr_t token;
    zend_object std;
} ryzertc_peer;

static inline ryzertc_peer *peer_from_zend(zend_object *obj) {
    return (ryzertc_peer *)((char *) obj - XtOffsetOf(ryzertc_peer, std));
}
#define Z_RYZERTC_PEER_P(zv) peer_from_zend(Z_OBJ_P(zv))

static void peer_native_close(ryzertc_peer *peer) {
    if (peer->handle >= 0) {
        int handle = peer->handle;
        peer->handle = -1;
        int channels[RYZERTC_MAX_CHANNELS_PER_PEER] = {0};
        size_t count = ryzertc_list_channels(peer->token, channels,
            RYZERTC_MAX_CHANNELS_PER_PEER);
        /* Revoke event ownership before touching native handles. */
        ryzertc_unregister_peer(peer->token);
        peer->token = 0;
        /* Delete SCTP channels before deleting the underlying connection.
         * libdatachannel keeps channel objects in its process-global C API
         * registry even after the peer handle is erased. */
        for (size_t i = 0; i < count; ++i) {
            rtcDeleteDataChannel(channels[i]);
        }
        rtcDeletePeerConnection(handle);
    }
}

static zend_object *peer_new(zend_class_entry *ce) {
    ryzertc_peer *peer = zend_object_alloc(sizeof(ryzertc_peer), ce);
    peer->handle = -1;
    peer->token = 0;
    zend_object_std_init(&peer->std, ce);
    object_properties_init(&peer->std, ce);
    peer->std.handlers = &peer_handlers;
    return &peer->std;
}

static void peer_free(zend_object *obj) {
    ryzertc_peer *peer = peer_from_zend(obj);
    peer_native_close(peer);
    zend_object_std_dtor(&peer->std);
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_runtime_version, 0, 0, IS_STRING, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_runtime_api, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_runtime_capabilities, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO_EX(arginfo_peer_construct, 0, 0, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, bindIp, IS_STRING, 1, "\"127.0.0.1\"")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, icePort, IS_LONG, 0, "0")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, udpMux, _IS_BOOL, 0, "false")
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_peer_offer, 0, 1, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, sdp, IS_STRING, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_peer_candidate, 0, 2, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, candidate, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, mid, IS_STRING, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_peer_void, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_peer_description, 0, 0, IS_STRING, 1)
ZEND_END_ARG_INFO()


ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_peer_channel_create, 0, 1, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, label, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, reliable, _IS_BOOL, 0, "true")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, ordered, _IS_BOOL, 0, "true")
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_peer_send, 0, 2, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, channelId, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, data, IS_STRING, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_peer_poll, 0, 0, IS_ARRAY, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, limit, IS_LONG, 0, "64")
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_peer_answer, 0, 1, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, sdp, IS_STRING, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Runtime, version) {
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_STRING(PHP_RYZERTC_VERSION);
}

PHP_METHOD(Runtime, apiVersion) {
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_LONG(PHP_RYZERTC_API_VERSION);
}

PHP_METHOD(Runtime, capabilities) {
    ZEND_PARSE_PARAMETERS_NONE();
    array_init(return_value);
    add_assoc_string(return_value, "backend", "libdatachannel");
    add_assoc_bool(return_value, "peerConnection", 1);
    add_assoc_bool(return_value, "iceUdpMux", 1);
    add_assoc_bool(return_value, "dataChannels", 1);
    add_assoc_bool(return_value, "asyncEventQueue", 1);
    add_assoc_bool(return_value, "sharedRakNetSocket", 0);
    add_assoc_bool(return_value, "bedrockIdentity", 0);
}

PHP_METHOD(PeerConnection, __construct) {
    zend_string *bind = NULL;
    zend_long port = 0;
    bool mux = false;
    ryzertc_peer *peer = Z_RYZERTC_PEER_P(ZEND_THIS);
    ZEND_PARSE_PARAMETERS_START(0, 3)
        Z_PARAM_OPTIONAL
        Z_PARAM_STR_OR_NULL(bind)
        Z_PARAM_LONG(port)
        Z_PARAM_BOOL(mux)
    ZEND_PARSE_PARAMETERS_END();
    if (peer->handle >= 0) {
        zend_throw_error(NULL, "RyzeRTC peer already initialized");
        RETURN_THROWS();
    }
    if (port < 0 || port > 65535 || (mux && port == 0)) {
        zend_value_error("ICE port must be 0..65535; UDP mux requires a fixed port");
        RETURN_THROWS();
    }
    const char *ip = bind != NULL ? ZSTR_VAL(bind) : "127.0.0.1";
    struct in_addr parsed;
    if (strlen(ip) > 63 || inet_pton(AF_INET, ip, &parsed) != 1) {
        zend_value_error("bindIp must be a numeric IPv4 address");
        RETURN_THROWS();
    }
    rtcConfiguration config = {0};
    config.bindAddress = ip;
    config.enableIceTcp = false;
    config.enableIceUdpMux = mux;
    /* RyzeRTC uses an explicit authenticated offer/answer lifecycle. */
    config.disableAutoNegotiation = true;
    config.portRangeBegin = (uint16_t) port;
    config.portRangeEnd = (uint16_t) port;
    config.maxMessageSize = 262144;
    int handle = rtcCreatePeerConnection(&config);
    if (handle < 0) {
        zend_throw_error(NULL, "Native WebRTC peer construction failed (%d)", handle);
        RETURN_THROWS();
    }
    peer->token = ryzertc_register_peer(handle);
    if (!peer->token || !ryzertc_attach_peer(peer->token, handle)) {
        if (peer->token) ryzertc_unregister_peer(peer->token);
        peer->token = 0;
        rtcDeletePeerConnection(handle);
        zend_throw_error(NULL, "RyzeRTC failed to install native callbacks");
        RETURN_THROWS();
    }
    peer->handle = handle;
}

static int peer_require_open(zval *zv) {
    int handle = Z_RYZERTC_PEER_P(zv)->handle;
    if (handle < 0) {
        zend_throw_error(NULL, "RyzeRTC peer is closed or uninitialized");
    }
    return handle;
}

PHP_METHOD(PeerConnection, setRemoteOffer) {
    zend_string *sdp;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(sdp)
    ZEND_PARSE_PARAMETERS_END();
    int handle = peer_require_open(ZEND_THIS);
    if (handle < 0) RETURN_THROWS();
    if (ZSTR_LEN(sdp) == 0 || ZSTR_LEN(sdp) > RYZERTC_MAX_SDP ||
        memchr(ZSTR_VAL(sdp), '\0', ZSTR_LEN(sdp)) != NULL) {
        zend_value_error("SDP must be nonempty, at most 64 KiB and contain no NUL");
        RETURN_THROWS();
    }
    int result = rtcSetRemoteDescription(handle, ZSTR_VAL(sdp), "offer");
    if (result != RTC_ERR_SUCCESS) {
        zend_throw_error(NULL, "Native remote SDP rejected (%d)", result);
        RETURN_THROWS();
    }
}

/**
 * ICE candidates are added only after verifying the incoming signaling
 * request; this API is transport-only and does not authenticate a user.
 */
PHP_METHOD(PeerConnection, addRemoteCandidate) {
    zend_string *candidate, *mid;
    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_STR(candidate)
        Z_PARAM_STR(mid)
    ZEND_PARSE_PARAMETERS_END();
    int handle = peer_require_open(ZEND_THIS);
    if (handle < 0) RETURN_THROWS();
    if (ZSTR_LEN(candidate) == 0 || ZSTR_LEN(candidate) > RYZERTC_MAX_ICE_CANDIDATE ||
        memchr(ZSTR_VAL(candidate), '\0', ZSTR_LEN(candidate)) != NULL ||
        memchr(ZSTR_VAL(candidate), '\n', ZSTR_LEN(candidate)) != NULL ||
        memchr(ZSTR_VAL(candidate), '\r', ZSTR_LEN(candidate)) != NULL) {
        zend_value_error("ICE candidate must be a single nonempty line of at most 8 KiB");
        RETURN_THROWS();
    }
    if (ZSTR_LEN(mid) == 0 || ZSTR_LEN(mid) > RYZERTC_MAX_ICE_MID ||
        memchr(ZSTR_VAL(mid), '\0', ZSTR_LEN(mid)) != NULL ||
        memchr(ZSTR_VAL(mid), '\n', ZSTR_LEN(mid)) != NULL ||
        memchr(ZSTR_VAL(mid), '\r', ZSTR_LEN(mid)) != NULL) {
        zend_value_error("ICE media ID must be a single nonempty line of at most 128 bytes");
        RETURN_THROWS();
    }
    int result = rtcAddRemoteCandidate(handle, ZSTR_VAL(candidate), ZSTR_VAL(mid));
    if (result != RTC_ERR_SUCCESS) {
        zend_throw_error(NULL, "Native ICE candidate rejected (%d)", result);
        RETURN_THROWS();
    }
}

PHP_METHOD(PeerConnection, beginAnswer) {
    ZEND_PARSE_PARAMETERS_NONE();
    int handle = peer_require_open(ZEND_THIS);
    if (handle < 0) RETURN_THROWS();
    int result = rtcSetLocalDescription(handle, "answer");
    if (result != RTC_ERR_SUCCESS) {
        zend_throw_error(NULL, "Native SDP answer could not be started (%d)", result);
        RETURN_THROWS();
    }
}

PHP_METHOD(PeerConnection, localDescription) {
    ZEND_PARSE_PARAMETERS_NONE();
    int handle = peer_require_open(ZEND_THIS);
    if (handle < 0) RETURN_THROWS();
    char buffer[RYZERTC_MAX_SDP + 1] = {0};
    int result = rtcGetLocalDescription(handle, buffer, sizeof(buffer));
    if (result == RTC_ERR_NOT_AVAIL) RETURN_NULL();
    if (result < 0) {
        zend_throw_error(NULL, "Native SDP read failed (%d)", result);
        RETURN_THROWS();
    }
    const char *end = memchr(buffer, '\0', sizeof(buffer));
    if (end == NULL) {
        zend_throw_error(NULL, "Native SDP is not NUL terminated");
        RETURN_THROWS();
    }
    RETURN_STRINGL(buffer, (size_t)(end - buffer));
}

PHP_METHOD(PeerConnection, remoteDescription) {
    ZEND_PARSE_PARAMETERS_NONE();
    int handle = peer_require_open(ZEND_THIS);
    if (handle < 0) RETURN_THROWS();
    char buffer[RYZERTC_MAX_SDP + 1] = {0};
    int result = rtcGetRemoteDescription(handle, buffer, sizeof(buffer));
    if (result == RTC_ERR_NOT_AVAIL) RETURN_NULL();
    if (result < 0) {
        zend_throw_error(NULL, "Native remote SDP read failed (%d)", result);
        RETURN_THROWS();
    }
    const char *end = memchr(buffer, '\0', sizeof(buffer));
    if (end == NULL) {
        zend_throw_error(NULL, "Native remote SDP is not NUL terminated");
        RETURN_THROWS();
    }
    RETURN_STRINGL(buffer, (size_t)(end - buffer));
}


/** Start an outbound WebRTC offer. The local SDP is reported by pollEvents(). */
PHP_METHOD(PeerConnection, beginOffer) {
    ZEND_PARSE_PARAMETERS_NONE();
    int pc = peer_require_open(ZEND_THIS);
    if (pc < 0) RETURN_THROWS();
    int result = rtcSetLocalDescription(pc, "offer");
    if (result != RTC_ERR_SUCCESS) {
        zend_throw_error(NULL, "RyzeRTC could not create offer (%d)", result);
        RETURN_THROWS();
    }
}
/** Complete negotiation initiated by beginOffer(). */
PHP_METHOD(PeerConnection, setRemoteAnswer) {
    zend_string *sdp;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(sdp)
    ZEND_PARSE_PARAMETERS_END();
    int pc = peer_require_open(ZEND_THIS);
    if (pc < 0) RETURN_THROWS();
    if (ZSTR_LEN(sdp) == 0 || ZSTR_LEN(sdp) > RYZERTC_MAX_SDP ||
        memchr(ZSTR_VAL(sdp), '\0', ZSTR_LEN(sdp)) != NULL) {
        zend_value_error("SDP must be nonempty, at most 64 KiB and contain no NUL");
        RETURN_THROWS();
    }
    int result = rtcSetRemoteDescription(pc, ZSTR_VAL(sdp), "answer");
    if (result != RTC_ERR_SUCCESS) {
        zend_throw_error(NULL, "RyzeRTC rejected remote answer (%d)", result);
        RETURN_THROWS();
    }
}
PHP_METHOD(PeerConnection, createDataChannel) {
    zend_string *label;
    bool reliable = true, ordered = true;
    ZEND_PARSE_PARAMETERS_START(1, 3)
        Z_PARAM_STR(label)
        Z_PARAM_OPTIONAL
        Z_PARAM_BOOL(reliable)
        Z_PARAM_BOOL(ordered)
    ZEND_PARSE_PARAMETERS_END();
    int pc = peer_require_open(ZEND_THIS);
    if (pc < 0) RETURN_THROWS();
    if (ZSTR_LEN(label) == 0 || ZSTR_LEN(label) > 128 ||
        memchr(ZSTR_VAL(label), '\0', ZSTR_LEN(label)) != NULL) {
        zend_value_error("DataChannel label must contain 1..128 bytes without NUL");
        RETURN_THROWS();
    }
    rtcDataChannelInit init = {0};
    init.reliability.unordered = !ordered;
    init.reliability.unreliable = !reliable;
    int dc = rtcCreateDataChannelEx(pc, ZSTR_VAL(label), &init);
    if (dc < 0) {
        zend_throw_error(NULL, "RyzeRTC could not create DataChannel (%d)", dc);
        RETURN_THROWS();
    }
    if (!ryzertc_attach_channel(Z_RYZERTC_PEER_P(ZEND_THIS)->token, pc, dc)) {
        rtcDeleteDataChannel(dc);
        zend_throw_error(NULL, "DataChannel registration limit or native callback failure");
        RETURN_THROWS();
    }
    RETURN_LONG(dc);
}
PHP_METHOD(PeerConnection, sendMessage) {
    zend_long channel;
    zend_string *data;
    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_LONG(channel)
        Z_PARAM_STR(data)
    ZEND_PARSE_PARAMETERS_END();
    int pc = peer_require_open(ZEND_THIS);
    if (pc < 0) RETURN_THROWS();
    uintptr_t token = Z_RYZERTC_PEER_P(ZEND_THIS)->token;
    if (channel < 0 || channel > INT32_MAX || !ryzertc_owns_channel(token, (int)channel)) {
        zend_value_error("DataChannel does not belong to this peer");
        RETURN_THROWS();
    }
    if (ZSTR_LEN(data) > RYZERTC_MAX_MESSAGE) {
        zend_value_error("DataChannel message exceeds 256 KiB");
        RETURN_THROWS();
    }
    int pending = rtcGetBufferedAmount((int)channel);
    if (pending < 0 || (size_t)pending + ZSTR_LEN(data) > RYZERTC_MAX_BUFFERED) {
        zend_throw_error(NULL, "DataChannel buffer is unavailable or saturated");
        RETURN_THROWS();
    }
    if (!rtcIsOpen((int)channel)) {
        zend_throw_error(NULL, "DataChannel is not open");
        RETURN_THROWS();
    }
    int result = rtcSendMessage((int)channel, ZSTR_VAL(data), (int)ZSTR_LEN(data));
    if (result != RTC_ERR_SUCCESS) {
        zend_throw_error(NULL, "DataChannel send failed (%d)", result);
        RETURN_THROWS();
    }
}
/**
 * Pulls a finite number of copied events. Each callback enqueues raw C bytes;
 * PHP values are constructed ONLY by this method on the calling Zend thread.
 */
PHP_METHOD(PeerConnection, pollEvents) {
    zend_long limit = 64;
    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(limit)
    ZEND_PARSE_PARAMETERS_END();
    if (limit < 1 || limit > 256) {
        zend_value_error("Poll limit must be between 1 and 256");
        RETURN_THROWS();
    }
    int pc = peer_require_open(ZEND_THIS);
    if (pc < 0) RETURN_THROWS();
    uintptr_t token = Z_RYZERTC_PEER_P(ZEND_THIS)->token;
    ryzertc_event *first = NULL, *last = NULL;
    pthread_mutex_lock(&ryzertc_lock);
    ryzertc_event **it = &ryzertc_first;
    zend_long count = 0;
    while (*it != NULL && count < limit) {
        if ((*it)->token == token) {
            ryzertc_event *ev = *it;
            *it = ev->next;
            ev->next = NULL;
            if (last) last->next = ev; else first = ev;
            last = ev; ++count;
            ryzertc_queue_count--;
            ryzertc_queue_bytes -= ev->data_len + ev->detail_len;
        } else it = &(*it)->next;
    }
    ryzertc_last = NULL;
    for (ryzertc_event *ev = ryzertc_first; ev; ev = ev->next) ryzertc_last = ev;
    pthread_mutex_unlock(&ryzertc_lock);
    array_init(return_value);
    while (first != NULL) {
        ryzertc_event *ev = first;
        first = ev->next;
        zval item;
        array_init(&item);
        add_assoc_string(&item, "type", ev->kind);
        add_assoc_long(&item, "channel", ev->channel);
        add_assoc_stringl(&item, "data", ev->data ? ev->data : "", ev->data_len);
        add_assoc_stringl(&item, "detail", ev->detail ? ev->detail : "", ev->detail_len);
        add_next_index_zval(return_value, &item);
        ryzertc_free_event(ev);
    }
}

PHP_METHOD(PeerConnection, close) {
    ZEND_PARSE_PARAMETERS_NONE();
    peer_native_close(Z_RYZERTC_PEER_P(ZEND_THIS));
}

static const zend_function_entry runtime_methods[] = {
    ZEND_ME(Runtime, version, arginfo_runtime_version, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    ZEND_ME(Runtime, apiVersion, arginfo_runtime_api, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    ZEND_ME(Runtime, capabilities, arginfo_runtime_capabilities, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    ZEND_FE_END
};
static const zend_function_entry peer_methods[] = {
    ZEND_ME(PeerConnection, __construct, arginfo_peer_construct, ZEND_ACC_PUBLIC)
    ZEND_ME(PeerConnection, setRemoteOffer, arginfo_peer_offer, ZEND_ACC_PUBLIC)
    ZEND_ME(PeerConnection, addRemoteCandidate, arginfo_peer_candidate, ZEND_ACC_PUBLIC)
    ZEND_ME(PeerConnection, remoteDescription, arginfo_peer_description, ZEND_ACC_PUBLIC)
    ZEND_ME(PeerConnection, beginOffer, arginfo_peer_void, ZEND_ACC_PUBLIC)
    ZEND_ME(PeerConnection, setRemoteAnswer, arginfo_peer_answer, ZEND_ACC_PUBLIC)
    ZEND_ME(PeerConnection, createDataChannel, arginfo_peer_channel_create, ZEND_ACC_PUBLIC)
    ZEND_ME(PeerConnection, sendMessage, arginfo_peer_send, ZEND_ACC_PUBLIC)
    ZEND_ME(PeerConnection, pollEvents, arginfo_peer_poll, ZEND_ACC_PUBLIC)
    ZEND_ME(PeerConnection, beginAnswer, arginfo_peer_void, ZEND_ACC_PUBLIC)
    ZEND_ME(PeerConnection, localDescription, arginfo_peer_description, ZEND_ACC_PUBLIC)
    ZEND_ME(PeerConnection, close, arginfo_peer_void, ZEND_ACC_PUBLIC)
    ZEND_FE_END
};

PHP_MINIT_FUNCTION(ryzertc) {
    zend_class_entry ce;
    INIT_NS_CLASS_ENTRY(ce, "RyzeRTC", "Runtime", runtime_methods);
    runtime_ce = zend_register_internal_class(&ce);
    runtime_ce->ce_flags |= ZEND_ACC_FINAL;
    INIT_NS_CLASS_ENTRY(ce, "RyzeRTC", "PeerConnection", peer_methods);
    peer_ce = zend_register_internal_class(&ce);
    peer_ce->create_object = peer_new;
    peer_ce->ce_flags |= ZEND_ACC_FINAL | ZEND_ACC_NOT_SERIALIZABLE;
    memcpy(&peer_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
    peer_handlers.offset = XtOffsetOf(ryzertc_peer, std);
    peer_handlers.free_obj = peer_free;
    peer_handlers.clone_obj = NULL;
    return SUCCESS;
}

/*
 * libdatachannel has process-global asynchronous workers. Releasing individual
 * peer handles does not synchronously drain those workers. Without rtcCleanup()
 * the loader may unmap their code / process-global locks on PHP exit, causing
 * a SIGSEGV in a background pthread. PHP MSHUTDOWN runs after request teardown
 * while this extension and its native dependencies are still resident.
 */
PHP_MSHUTDOWN_FUNCTION(ryzertc) {
    rtcCleanup(); /* Drains and joins native worker callbacks. */
    pthread_mutex_lock(&ryzertc_lock);
    while (ryzertc_first) {
        ryzertc_event *next = ryzertc_first->next;
        ryzertc_free_event(ryzertc_first);
        ryzertc_first = next;
    }
    while (ryzertc_owners) {
        ryzertc_owner *next = ryzertc_owners->next;
        free(ryzertc_owners);
        ryzertc_owners = next;
    }
    ryzertc_last = NULL;
    ryzertc_queue_count = ryzertc_queue_bytes = 0;
    pthread_mutex_unlock(&ryzertc_lock);
    return SUCCESS;
}

PHP_MINFO_FUNCTION(ryzertc) {
    php_info_print_table_start();
    php_info_print_table_header(2, "RyzeRTC support", "enabled");
    php_info_print_table_row(2, "RyzeRTC version", PHP_RYZERTC_VERSION);
    php_info_print_table_end();
}

zend_module_entry ryzertc_module_entry = {
    STANDARD_MODULE_HEADER,
    "ryzertc",
    NULL,
    PHP_MINIT(ryzertc),
    PHP_MSHUTDOWN(ryzertc),
    NULL,
    NULL,
    PHP_MINFO(ryzertc),
    PHP_RYZERTC_VERSION,
    STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_RYZERTC
#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
#endif
ZEND_GET_MODULE(ryzertc)
#endif
