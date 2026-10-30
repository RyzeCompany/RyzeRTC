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

static zend_class_entry *runtime_ce;
static zend_class_entry *peer_ce;
static zend_object_handlers peer_handlers;

typedef struct {
    int handle;
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
        rtcClosePeerConnection(handle);
        rtcDeletePeerConnection(handle);
    }
}

static zend_object *peer_new(zend_class_entry *ce) {
    ryzertc_peer *peer = zend_object_alloc(sizeof(ryzertc_peer), ce);
    peer->handle = -1;
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
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_peer_void, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_peer_description, 0, 0, IS_STRING, 1)
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
    add_assoc_bool(return_value, "dataChannels", 0);
    add_assoc_bool(return_value, "asyncEventQueue", 0);
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
    config.portRangeBegin = (uint16_t) port;
    config.portRangeEnd = (uint16_t) port;
    config.maxMessageSize = 262144;
    int handle = rtcCreatePeerConnection(&config);
    if (handle < 0) {
        zend_throw_error(NULL, "Native WebRTC peer construction failed (%d)", handle);
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
    NULL,
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
