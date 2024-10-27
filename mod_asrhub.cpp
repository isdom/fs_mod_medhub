#include <switch.h>
#include <fstream>
#include <math.h>

#define ASIO_STANDALONE 1

#define ENABLE_WSS 0

#include <websocketpp/client.hpp>
#include <websocketpp/common/thread.hpp>
#include <websocketpp/config/asio_client.hpp>
#include <thread>

#include "nlohmann/json.hpp"

typedef struct {
    bool _debug;
    switch_atomic_t asrhub_concurrent_cnt;
} asrhub_global_t;

asrhub_global_t *asrhub_globals;

template<typename T>
class WebsocketClient;

#if ENABLE_WSS
typedef WebsocketClient<websocketpp::config::asio_tls_client> asrhub_client;
#else
typedef WebsocketClient<websocketpp::config::asio_client> asrhub_client;
#endif

// public declare

typedef struct {
    int32_t    sentence_index;
    int32_t    sentence_begin_time;
    int32_t    sentence_time;
    double     confidence;
    const char *sentence;
} asr_sentence_result_t;

typedef void (*on_asr_started_func_t) (void *);
typedef void (*on_asr_sentence_begin_func_t) (void *);
typedef void (*on_asr_sentence_end_func_t) (void *, asr_sentence_result_t *sentence_result);
typedef void (*on_asr_result_changed_func_t) (void *, asr_sentence_result_t *sentence_result);
typedef void (*on_asr_stopped_func_t) (void *);

typedef struct {
    void                            *asr_caller;
    on_asr_started_func_t           on_asr_started_func;
    on_asr_sentence_begin_func_t    on_asr_sentence_begin_func;
    on_asr_sentence_end_func_t      on_asr_sentence_end_func;
    on_asr_result_changed_func_t    on_asr_result_changed_func;
    on_asr_stopped_func_t           on_asr_stopped_func;
} asr_callback_t;

typedef void *(*asr_init_func_t) (switch_core_session_t *, const switch_codec_implementation_t *, const char *);
typedef bool (*asr_start_func_t) (void *asr_data, asr_callback_t *asr_callback);
typedef bool (*asr_send_audio_func_t) (void *asr_data, void *data, uint32_t data_len);
typedef void (*asr_stop_func_t) (void *asr_data);
typedef void (*asr_destroy_func_t) (void *asr_data);

typedef struct {
    asr_init_func_t asr_init_func;
    asr_start_func_t asr_start_func;
    asr_send_audio_func_t asr_send_audio_func;
    asr_stop_func_t asr_stop_func;
    asr_destroy_func_t asr_destroy_func;
} asr_provider_t;

// public declare end

//======================================== fun asr start ===============

typedef struct {
    switch_core_session_t   *session;
    asrhub_client         *client;
    int started;
    int stopped;
    int starting;
    switch_mutex_t          *mutex;
    switch_audio_resampler_t *re_sampler;
char                        *asrhub_url;
    asr_callback_t          *asr_callback;
} asrhub_context_t;

/**
 * 识别启动回调函数
 *
 * @param ctx
 */
void onTranscriptionStarted(asrhub_context_t *ctx) {
    if (asrhub_globals->_debug) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "onTranscriptionStarted: asrhub\n");
    }
    switch_mutex_lock(ctx->mutex);
    ctx->started = 1;
    ctx->starting = 0;
    switch_mutex_unlock(ctx->mutex);

    if (ctx->asr_callback) {
        if (asrhub_globals->_debug) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "onTranscriptionStarted: call on_asr_started_func %p\n", ctx->asr_callback->on_asr_started_func);
        }
        ctx->asr_callback->on_asr_started_func(ctx->asr_callback->asr_caller);
    } else {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "onTranscriptionStarted: ctx->asr_callback is null\n");
    }
}

/**
 * @brief 一句话开始回调函数
 *
 * @param ctx
 */
void onSentenceBegin(asrhub_context_t *ctx) {
    if (asrhub_globals->_debug) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "onSentenceBegin: asrhub\n");
    }
    if (ctx->asr_callback) {
        ctx->asr_callback->on_asr_sentence_begin_func(ctx->asr_callback->asr_caller);
    }
}

/**
 * @brief 一句话结束回调函数
 *
 * @param ctx
 * @param text
 */
void onSentenceEnd(asrhub_context_t *ctx, asr_sentence_result_t *asr_sentence_result) {
    if (asrhub_globals->_debug) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "onSentenceEnd: asrhub\n");
    }
    if (ctx->asr_callback) {
        if (asrhub_globals->_debug) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "onSentenceEnd: call on_asr_sentence_end_func %p\n", ctx->asr_callback->on_asr_sentence_end_func);
        }
        ctx->asr_callback->on_asr_sentence_end_func(ctx->asr_callback->asr_caller, asr_sentence_result);
    } else {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "onSentenceEnd: ctx->asr_callback is null\n");
    }
}

/**
 * @brief 识别结果变化回调函数
 *
 * @param ctx
 * @param text
 */
void onTranscriptionResultChanged(asrhub_context_t *ctx, const std::string &text) {
    if (asrhub_globals->_debug) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "onTranscriptionResultChanged: asrhub\n");
    }
    if (ctx->asr_callback) {
        if (asrhub_globals->_debug) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "onTranscriptionResultChanged: call on_asr_result_changed_func %p\n", ctx->asr_callback->on_asr_result_changed_func);
        }
        asr_sentence_result_t asr_sentence_result = {
                -1,
                -1,
                -1,
                0.0,
                text.c_str()
        };
        ctx->asr_callback->on_asr_result_changed_func(ctx->asr_callback->asr_caller, &asr_sentence_result);
    } else {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "onTranscriptionResultChanged: ctx->asr_callback is null\n");
    }
}

/**
 * @brief 语音转写结束回调函数
 *
 * @param ctx
 */
void onTranscriptionCompleted(asrhub_context_t *ctx) {
    if (asrhub_globals->_debug) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "onTranscriptionCompleted: asrhub\n");
    }
    if (ctx->asr_callback) {
        ctx->asr_callback->on_asr_stopped_func(ctx->asr_callback->asr_caller);
    }
}

/**
 * @brief 异常识别回调函数
 *
 * @param ctx
 */
void onTaskFailed(asrhub_context_t *ctx) {
    if (asrhub_globals->_debug) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "onTaskFailed: asrhub\n");
    }
    switch_mutex_lock(ctx->mutex);
    ctx->started = 0;
    switch_mutex_unlock(ctx->mutex);
}

/**
 * @brief 识别通道关闭回调函数
 *
 * @param ctx
 */
void onChannelClosed(asrhub_context_t *ctx) {
    if (asrhub_globals->_debug) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "onChannelClosed: asrhub\n");
    }
    /*
    if (ctx->asr_callback) {
        if (asrhub_globals->_debug) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "onChannelClosed: call on_asr_stopped_func %p\n", ctx->asr_callback->on_asr_stopped_func);
        }
        ctx->asr_callback->on_asr_stopped_func(ctx->asr_callback->asr_caller);
    } else {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "onChannelClosed: ctx->asr_callback is null\n");
    }
     */
}

/**
 * Define a semi-cross platform helper method that waits/sleeps for a bit.
 */
void WaitABit(long milliseconds) {
#ifdef WIN32
    Sleep(1000);
#else
    usleep(1000 * milliseconds);
#endif
}

typedef websocketpp::config::asio_client::message_type::ptr message_ptr;
typedef websocketpp::lib::shared_ptr<websocketpp::lib::asio::ssl::context> context_ptr;

using websocketpp::lib::bind;
using websocketpp::lib::placeholders::_1;
using websocketpp::lib::placeholders::_2;

context_ptr OnTlsInit(const websocketpp::connection_hdl &) {
    context_ptr ctx = websocketpp::lib::make_shared<asio::ssl::context>(asio::ssl::context::sslv23);

    try {
        ctx->set_options(
                asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 |
                asio::ssl::context::no_sslv3 | asio::ssl::context::single_dh_use);

    } catch (std::exception &e) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                          "OnTlsInit asio::ssl::context::set_options exception: %s\n", e.what());
    }
    return ctx;
}

// template for tls or not config
template<typename T>
class WebsocketClient {
public:
    // typedef websocketpp::client<T> client;
    // typedef websocketpp::client<websocketpp::config::asio_tls_client>
    // wss_client;
    typedef websocketpp::lib::lock_guard<websocketpp::lib::mutex> scoped_lock;

    WebsocketClient(int is_ssl, asrhub_context_t *asr_ctx)
    : m_open(false), m_done(false) {
        m_asr_ctx = asr_ctx;

        // set up access channels to only log interesting things
        m_client.clear_access_channels(websocketpp::log::alevel::all);
        m_client.set_access_channels(websocketpp::log::alevel::connect);
        m_client.set_access_channels(websocketpp::log::alevel::disconnect);
        m_client.set_access_channels(websocketpp::log::alevel::app);

        // Initialize the Asio transport policy
        m_client.init_asio();
        m_client.start_perpetual();

        // Bind the handlers we are using
        using websocketpp::lib::bind;
        using websocketpp::lib::placeholders::_1;
        m_client.set_open_handler(bind(&WebsocketClient::on_open, this, _1));
        m_client.set_close_handler(bind(&WebsocketClient::on_close, this, _1));

        m_client.set_message_handler(
                [this](websocketpp::connection_hdl hdl, message_ptr msg) {
                    on_message(hdl, msg);
                });

        m_client.set_fail_handler(bind(&WebsocketClient::on_fail, this, _1));
        m_client.clear_access_channels(websocketpp::log::alevel::all);
    }

    std::string getThreadIdOfString(const std::thread::id &id) {
        std::stringstream sin;
        sin << id;
        return sin.str();
    }

    void on_message(websocketpp::connection_hdl hdl, message_ptr msg) {
        const std::string &payload = msg->get_payload();
        switch (msg->get_opcode()) {
            case websocketpp::frame::opcode::text: {
                nlohmann::json asr_result = nlohmann::json::parse(payload);
                std::string id_str = getThreadIdOfString(std::this_thread::get_id());
                if (asrhub_globals->_debug) {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "thread: %s, on_message = %s\n",
                                      id_str.c_str(),
                                      payload.c_str());
                }

                if (asr_result["header"]["name"] == "TranscriptionStarted") {
                    /* TranscriptionStarted 事件
                    {
                        "header": {
                            "message_id": "05450bf69c53413f8d88aed1ee60****",
                            "task_id": "640bc797bb684bd6960185651307****",
                            "namespace": "FlowingSpeechSynthesizer",
                            "name": "TranscriptionStarted",
                            "status": 20000000,
                            "status_message": "GATEWAY|SUCCESS|Success."
                        },
                        "payload": {
                            "session_id": "1231231dfdf****"
                        }
                    } */
                    onTranscriptionStarted(m_asr_ctx);
                } else if (asr_result["header"]["name"] == "TranscriptionCompleted") {
                    /* TranscriptionCompleted 事件
                    {
                        "header": {
                            "name": "TranscriptionCompleted",
                        }
                    } */
                    onTranscriptionCompleted(m_asr_ctx);
                    {
                        websocketpp::lib::error_code ec;
                        m_client.close(hdl, websocketpp::close::status::going_away, "", ec);
                        if (ec) {
                            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Error closing connection: %s\n",
                                              ec.message().c_str());
                        }
                    }
                } else if (asr_result["header"]["name"] == "SentenceBegin") {
                    /* SentenceBegin 事件
                    {
                        "header": {
                            "name": "SentenceBegin",
                        },
                        "payload": {
                            "index": 1,
                            "time": 320
                        }
                    } */
                    onSentenceBegin(m_asr_ctx);
                } else if (asr_result["header"]["name"] == "TranscriptionResultChanged") {
                    /* TranscriptionResultChanged 事件
                    {
                        "header": {
                            "name": "TranscriptionResultChanged",
                        },
                        "payload": {
                            "index":1,
                            "time":1800,
                            "result":"今年双十一"
                        }
                    } */
                    std::string result = asr_result["payload"]["result"];
                    onTranscriptionResultChanged(m_asr_ctx, result);
                } else if (asr_result["header"]["name"] == "SentenceEnd") {
                    /* SentenceEnd 事件
                    {
                        "header": {
                            "name": "SentenceEnd",
                        },
                        "payload": {
                            "index": 1,
                            "time": 3260,
                            "begin_time": 1800,
                            "result": "今年双十一我要买电视"
                        }
                    } */
                    std::string result = asr_result["payload"]["result"];
                    asr_sentence_result_t asr_sentence_result = {
                            asr_result["payload"]["index"],
                            asr_result["payload"]["begin_time"],
                            asr_result["payload"]["time"],
                            asr_result["payload"]["confidence"],
                            result.c_str()
                    };
                    onSentenceEnd(m_asr_ctx, &asr_sentence_result);
                }
            }
                break;
            default:
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "un-handle opcode: %d\n", msg->get_opcode());
                break;
        }
    }

    // This method will block until the connection is complete
    int connect(const std::string &uri) {
        if (asrhub_globals->_debug) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "connect to: %s \n", uri.c_str());
        }

        {
            // Create a new connection to the given URI
            websocketpp::lib::error_code ec;
            typename websocketpp::client<T>::connection_ptr con = m_client.get_connection(uri, ec);
            if (ec) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Get Connection Error: %s\n",
                                  ec.message().c_str());
                return -1;
            }
            // Grab a handle for this connection so we can talk to it in a thread
            // safe manor after the event loop starts.
            m_hdl = con->get_handle();

            // Queue the connection. No DNS queries or network connections will be
            // made until the io_service event loop is run.
            m_client.connect(con);
        }

        // Create a thread to run the ASIO io_service event loop
        m_thread.reset(new websocketpp::lib::thread(&websocketpp::client<T>::run, &m_client));
        return 0;
    }

    int startTranscription() {
        if (asrhub_globals->_debug) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "send StartTranscription command\n");
        }

        nlohmann::json json_startTranscription = {
                {"header", {
                                   // 当次消息请求ID，随机生成32位唯一ID。
                                   //{"message_id", message_id},
                                   // 整个实时语音合成的会话ID，整个请求中需要保持一致，32位唯一ID。
                                   //{"task_id", m_task_id},
                                   //{"namespace", "FlowingSpeechSynthesizer"},
                                   {"name", "StartTranscription"}
                                   //{"appkey", m_appkey}
                           }} //,
                //{"payload", {
                //                   {"format", "pcm"},
                //                   {"sample_rate", 16000},
                //                   {"enable_subtitle", true}
                //           }}
        };

        const std::string str_startTranscription = json_startTranscription.dump();
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "startTranscription: send StartTranscription command, detail: %s\n",
                          str_startTranscription.c_str());

        websocketpp::lib::error_code ec;
        m_client.send(m_hdl, str_startTranscription, websocketpp::frame::opcode::text, ec);
        if (ec) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "asrhub send begin msg failed: %s\n",
                              ec.message().c_str());
        } else {
            if (asrhub_globals->_debug) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "asrhub send begin msg success\n");
            }
        }

        return 0;
    }

    void stop() {
        nlohmann::json json_stopTranscription = {
                {"header", {
                        // 当次消息请求ID，随机生成32位唯一ID。
                        //{"message_id", message_id},
                        // 整个实时语音合成的会话ID，整个请求中需要保持一致，32位唯一ID。
                        //{"task_id", m_task_id},
                        //{"namespace", "FlowingSpeechSynthesizer"},
                        {"name", "StopTranscription"}
                        //{"appkey", m_appkey}
                }} //,
                //{"payload", {
                //                   {"format", "pcm"},
                //                   {"sample_rate", 16000},
                //                   {"enable_subtitle", true}
                //           }}
        };

        const std::string str_stopTranscription = json_stopTranscription.dump();
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "stop: send StopTranscription command, detail: %s\n",
                          str_stopTranscription.c_str());

        websocketpp::lib::error_code ec;
        m_client.send(m_hdl, str_stopTranscription, websocketpp::frame::opcode::text, ec);
        if (ec) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "asrhub send stop msg failed: %s\n",
                              ec.message().c_str());
        } else {
            if (asrhub_globals->_debug) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "asrhub send stop msg success\n");
            }
        }

        m_client.stop_perpetual();
        m_thread->join();

        onChannelClosed(m_asr_ctx);
    }

    // The open handler will signal that we are ready to start sending data
    void on_open(const websocketpp::connection_hdl &) {
        if (asrhub_globals->_debug) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Connection opened, starting data!\n");
        }

        {
            scoped_lock guard(m_lock);
            m_open = true;
        }
        // onTranscriptionStarted(m_asr_ctx);
        startTranscription();
    }

    // The close handler will signal that we should stop sending data
    void on_close(const websocketpp::connection_hdl &) {
        if (asrhub_globals->_debug) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Connection closed, stopping data!\n");
        }

        {
            scoped_lock guard(m_lock);
            m_done = true;
        }
        // onTranscriptionCompleted(m_asr_ctx);
    }

    // The fail handler will signal that we should stop sending data
    void on_fail(const websocketpp::connection_hdl &) {
        if (asrhub_globals->_debug) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Connection failed, stopping data!\n");
        }

        {
            scoped_lock guard(m_lock);
            m_done = true;
        }
        onTaskFailed(m_asr_ctx);
    }

    void sendAudio(uint8_t *dp, size_t data_len, websocketpp::lib::error_code &ec) {
        m_client.send(m_hdl, dp, data_len, websocketpp::frame::opcode::binary, ec);
    }

    websocketpp::client<T> m_client;
    websocketpp::lib::shared_ptr<websocketpp::lib::thread> m_thread;

private:

    asrhub_context_t *m_asr_ctx;
    websocketpp::connection_hdl m_hdl;
    websocketpp::lib::mutex m_lock;
    bool m_open;
    bool m_done;
};

// typedef WebsocketClient<websocketpp::config::asio_tls_client> asrhub_client;

#define MAX_FRAME_BUFFER_SIZE (1024*1024) //1MB
#define SAMPLE_RATE 8000

asrhub_client *generateAsrClient(asrhub_context_t *ctx) {
    auto *client = new asrhub_client(1, ctx);
    if (!client) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "generateAsrClient failed.\n");
        return nullptr;
    }

#if ENABLE_WSS
    client->m_client.set_tls_init_handler(bind(&OnTlsInit, ::_1));
#endif

    if (asrhub_globals->_debug) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "proxy url is:%s\n", ctx->asrhub_url);
    }
    return client;
}

//======================================== fun asr end ===============

//======================================== freeswitch module start ===============
SWITCH_MODULE_LOAD_FUNCTION(mod_asrhub_load);

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_asrhub_shutdown);

extern "C"
{
SWITCH_MODULE_DEFINITION(mod_asrhub, mod_asrhub_load, mod_asrhub_shutdown, nullptr);
};

static void *init_asrhub(switch_core_session_t *session, const switch_codec_implementation_t *read_impl, const char *cmd);

static bool start_asrhub(asrhub_context_t *ctx, asr_callback_t *asr_callback);

static bool send_audio_to_asrhub(asrhub_context_t *ctx, void *data, uint32_t data_len);

static void stop_asrhub(asrhub_context_t *ctx);

static void destroy_asrhub(asrhub_context_t *ctx);

static const asr_provider_t asrhub_funcs = {
        init_asrhub,
        reinterpret_cast<asr_start_func_t>(start_asrhub),
        reinterpret_cast<asr_send_audio_func_t>(send_audio_to_asrhub),
        reinterpret_cast<asr_stop_func_t>(stop_asrhub),
        reinterpret_cast<asr_destroy_func_t>(destroy_asrhub)
};

static switch_status_t attach_asrhub_provider_on_channel_init(switch_core_session_t *session) {
    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_channel_set_private(channel, "asrhub", &asrhub_funcs);
    return SWITCH_STATUS_SUCCESS;
}

switch_state_handler_table_t asrhub_cs_handlers = {
        /*! executed when the state changes to init */
        // switch_state_handler_t on_init;
        attach_asrhub_provider_on_channel_init,
        /*! executed when the state changes to routing */
        // switch_state_handler_t on_routing;
        nullptr,
        /*! executed when the state changes to execute */
        // switch_state_handler_t on_execute;
        nullptr,
        /*! executed when the state changes to hangup */
        // switch_state_handler_t on_hangup;
        nullptr,
        /*! executed when the state changes to exchange_media */
        // switch_state_handler_t on_exchange_media;
        nullptr,
        /*! executed when the state changes to soft_execute */
        // switch_state_handler_t on_soft_execute;
        nullptr,
        /*! executed when the state changes to consume_media */
        // switch_state_handler_t on_consume_media;
        nullptr,
        /*! executed when the state changes to hibernate */
        // switch_state_handler_t on_hibernate;
        nullptr,
        /*! executed when the state changes to reset */
        // switch_state_handler_t on_reset;
        nullptr,
        /*! executed when the state changes to park */
        // switch_state_handler_t on_park;
        nullptr,
        /*! executed when the state changes to reporting */
        // switch_state_handler_t on_reporting;
        nullptr,
        /*! executed when the state changes to destroy */
        // switch_state_handler_t on_destroy;
        nullptr,
        // int flags;
        0
};

void adjustVolume(int16_t *pcm, size_t pcm_len, float vol_multiplier) {
    int32_t pcm_val;
    for (size_t ctr = 0; ctr < pcm_len; ctr++) {
        pcm_val = (int32_t)((float)pcm[ctr] * vol_multiplier);
        if (pcm_val < 32767 && pcm_val > -32768) {
            pcm[ctr] = (int16_t)pcm_val;
        } else if (pcm_val > 32767) {
            pcm[ctr] = 32767;
        } else if (pcm_val < -32768) {
            pcm[ctr] = -32768;
        }
    }
}

// params: <uuid> proxyurl=<uri>
#define MAX_API_ARGC 20

static void *init_asrhub(switch_core_session_t *session, const switch_codec_implementation_t *read_impl, const char *cmd) {
    char *_proxy_url = nullptr;

    switch_memory_pool_t *pool;
    switch_core_new_memory_pool(&pool);
    char *my_cmd = switch_core_strdup(pool, cmd);

    char *argv[MAX_API_ARGC];
    memset(argv, 0, sizeof(char *) * MAX_API_ARGC);

    int argc = switch_split(my_cmd, ' ', argv);
    if (asrhub_globals->_debug) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "cmd:%s, args count: %d\n", my_cmd, argc);
    }

    for (int idx = 1; idx < MAX_API_ARGC; idx++) {
        if (argv[idx]) {
            char *ss[2] = {nullptr, nullptr};
            int cnt = switch_split(argv[idx], '=', ss);
            if (cnt == 2) {
                char *var = ss[0];
                char *val = ss[1];
                if (asrhub_globals->_debug) {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "process arg: %s = %s\n", var, val);
                }
                if (!strcasecmp(var, "proxyurl")) {
                    _proxy_url = val;
                    continue;
                }
            }
        }
    }

    if (!_proxy_url) {
        switch_core_destroy_memory_pool(&pool);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "proxyurl is required.\n");
        return nullptr;
    }

    asrhub_context_t *ctx;
    if (!(ctx = (asrhub_context_t *) switch_core_session_alloc(session, sizeof(asrhub_context_t)))) {
        goto end;
    }
    ctx->started = 0;
    ctx->stopped = 0;
    ctx->starting = 0;
    ctx->session = session;
    ctx->asrhub_url = switch_core_session_strdup(session, _proxy_url);
    switch_mutex_init(&ctx->mutex, SWITCH_MUTEX_NESTED, switch_core_session_get_pool(session));

    if (read_impl->actual_samples_per_second != SAMPLE_RATE) {
        if (switch_resample_create(&ctx->re_sampler,
                                   read_impl->actual_samples_per_second,
                                   SAMPLE_RATE,
                                   16 * (read_impl->microseconds_per_packet / 1000) * 2,
                                   SWITCH_RESAMPLE_QUALITY,
                                   1) != SWITCH_STATUS_SUCCESS) {
            // release all resource alloc before
            switch_mutex_destroy(ctx->mutex);

            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Unable to allocate re_sampler\n");
            ctx = nullptr;
            goto end;
        }
        if (asrhub_globals->_debug) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
                              "create re-sampler bcs of media sampler/s is %d but fun asr support: %d, while ms/p: %d\n",
                              read_impl->actual_samples_per_second, SAMPLE_RATE, read_impl->microseconds_per_packet);
        }
    }

    // increment asrhub concurrent count
    switch_atomic_inc(&asrhub_globals->asrhub_concurrent_cnt);

end:
    switch_core_destroy_memory_pool(&pool);
    return ctx;
}

static bool start_asrhub(asrhub_context_t *ctx, asr_callback_t *asr_callback) {
    bool  ret_val = false;
    if (ctx->stopped == 1) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "start_asrhub: ctx->stopped\n");
        return ret_val;
    }

    switch_mutex_lock(ctx->mutex);
    if (ctx->started == 0) {
        if (ctx->starting == 0) {
            ctx->starting = 1;
            if (asrhub_globals->_debug) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Starting Transaction \n");
            }
            switch_channel_t *channel = switch_core_session_get_channel(ctx->session);
            ctx->asr_callback = asr_callback;
            asrhub_client *client = generateAsrClient(ctx);
            if (!client) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Asr Client init failed.%s\n",
                                  switch_channel_get_name(channel));
                ret_val = false;
                goto unlock;
            }
            ctx->client = client;
            if (asrhub_globals->_debug) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Init Fun Asr Client.%s\n",
                                  switch_channel_get_name(channel));
            }

            if (ctx->client->connect(std::string(ctx->asrhub_url)) < 0) {
                ctx->stopped = 1;
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
                                  "start() failed. may be can not connect server(%s). please check network or firewalld:%s\n",
                                  ctx->asrhub_url, switch_channel_get_name(channel));
                ctx->client->stop();
                delete ctx->client;
                ctx->client = nullptr;
                // start()失败，释放request对象
                ret_val = false;
                goto unlock;
            }
            ret_val = true;
        }
    }

    unlock:
    switch_mutex_unlock(ctx->mutex);
    return ret_val;
}

static bool send_audio_to_asrhub(asrhub_context_t *ctx, void *data, uint32_t data_len) {
    bool  ret_val = false;
    // send audio to asr
    switch_mutex_lock(ctx->mutex);

    if (ctx->client) {
        if (ctx->re_sampler) {
            //====== resample ==== ///
            switch_resample_process(ctx->re_sampler, (int16_t *) data, (int) data_len / 2 / 1);
            memcpy(data, ctx->re_sampler->to, ctx->re_sampler->to_len * 2 * 1);
            data_len = ctx->re_sampler->to_len * 2 * 1;
            if (asrhub_globals->_debug) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "ASR new samples:%d\n",
                                  ctx->re_sampler->to_len);
            }
        }

        if (ctx->started) {
            websocketpp::lib::error_code ec;
            ctx->client->sendAudio((uint8_t *) data, (size_t) data_len, ec);

            if (ec) {
                ctx->stopped = 1;
                switch_channel_t *channel = switch_core_session_get_channel(ctx->session);
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "send audio failed: %s -> on channel: %s\n",
                                  ec.message().c_str(), switch_channel_get_name(channel));
                ctx->client->stop();
                delete ctx->client;
                ctx->client = nullptr;
                ret_val = false;
                goto unlock;
            }
        } else {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "send_audio_to_asrhub: connecting, ignore send audio\n");
        }
        ret_val = true;
        if (asrhub_globals->_debug) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "send_audio_to_asrhub: send audio %d\n",
                              data_len);
        }
    } else {
        switch_channel_t *channel = switch_core_session_get_channel(ctx->session);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "try send audio but client has been released -> on channel: %s\n",
                          switch_channel_get_name(channel));
        ret_val = false;
    }

    unlock:
    switch_mutex_unlock(ctx->mutex);
    return ret_val;
}

static void stop_asrhub(asrhub_context_t *ctx) {
    switch_mutex_lock(ctx->mutex);
    switch_channel_t *channel = switch_core_session_get_channel(ctx->session);
    if (ctx->client) {
        if (asrhub_globals->_debug) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "try to stop asrhub on channel: %s\n",
                              switch_channel_get_name(channel));
        }
        ctx->client->stop();
        //7: 识别结束, 释放fac对象
        delete ctx->client;
        ctx->client = nullptr;
        if (asrhub_globals->_debug) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "stop asrhub and client is released on channel: %s\n",
                              switch_channel_get_name(channel));
        }
    } else {
        if (asrhub_globals->_debug) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
                              "asrhub has already stopped and released on channel:%s\n",
                              switch_channel_get_name(channel));
        }
    }
    switch_mutex_unlock(ctx->mutex);
}

static void destroy_asrhub(asrhub_context_t *ctx) {
    switch_core_session_t *session = ctx->session;
    switch_channel_t *channel = switch_core_session_get_channel(session);
    if (asrhub_globals->_debug) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(ctx->session), SWITCH_LOG_NOTICE,
                          "destroy_asrhub: release all resource for session -> on channel: %s\n",
                          switch_channel_get_name(channel));
    }
    stop_asrhub(ctx);
    if (asrhub_globals->_debug) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE,
                          "destroy_asrhub: stop_asrhub -> channel: %s\n",
                          switch_channel_get_name(channel));
    }

    // decrement asrhub concurrent count
    switch_atomic_dec(&asrhub_globals->asrhub_concurrent_cnt);

    if (ctx->re_sampler) {
        switch_resample_destroy(&ctx->re_sampler);
        if (asrhub_globals->_debug) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE,
                              "destroy_asrhub: switch_resample_destroy -> on channel: %s\n",
                              switch_channel_get_name(channel));
        }
    }
    switch_mutex_destroy(ctx->mutex);
    if (asrhub_globals->_debug) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE,
                          "destroy_asrhub: switch_mutex_destroy -> on channel: %s\n",
                          switch_channel_get_name(channel));
    }
}

SWITCH_STANDARD_API(asrhub_concurrent_cnt_function) {
    const uint32_t concurrent_cnt = switch_atomic_read (&asrhub_globals->asrhub_concurrent_cnt);
    stream->write_function(stream, "%d\n", concurrent_cnt);
    return SWITCH_STATUS_SUCCESS;
}

#define ASRPROXY_DEBUG_SYNTAX "<on|off>"
SWITCH_STANDARD_API(mod_asrhub_debug) {
    if (zstr(cmd)) {
        stream->write_function(stream, "-USAGE: %s\n", ASRPROXY_DEBUG_SYNTAX);
    } else {
        if (!strcasecmp(cmd, "on")) {
            asrhub_globals->_debug = true;
            stream->write_function(stream, "asrhub Debug: on\n");
        } else if (!strcasecmp(cmd, "off")) {
            asrhub_globals->_debug = false;
            stream->write_function(stream, "asrhub Debug: off\n");
        } else {
            stream->write_function(stream, "-USAGE: %s\n", ASRPROXY_DEBUG_SYNTAX);
        }
    }
    return SWITCH_STATUS_SUCCESS;
}

/**
 *  定义load函数，加载时运行
 */
SWITCH_MODULE_LOAD_FUNCTION(mod_asrhub_load) {
    switch_api_interface_t *api_interface = nullptr;
    *module_interface = switch_loadable_module_create_module_interface(pool, modname);

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_asrhub load starting\n");

    asrhub_globals = (asrhub_global_t *)switch_core_alloc(pool, sizeof(asrhub_global_t));

    asrhub_globals->_debug = false;

    // register global state handlers
    switch_core_add_state_handler(&asrhub_cs_handlers);

    SWITCH_ADD_API(api_interface,
                   "asrhub_concurrent_cnt",
                   "asrhub_concurrent_cnt api",
                   asrhub_concurrent_cnt_function,
                   "<cmd><args>");

    SWITCH_ADD_API(api_interface, "asrhub_debug", "Set asrhub debug", mod_asrhub_debug, ASRPROXY_DEBUG_SYNTAX);

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_asrhub loaded\n");

    return SWITCH_STATUS_SUCCESS;
}

/**
 *  定义shutdown函数，关闭时运行
 */
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_asrhub_shutdown) {
    // unregister global state handlers
    switch_core_remove_state_handler(&asrhub_cs_handlers);

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, " mod_asrhub shutdown called\n");
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, " mod_asrhub unload\n");
    return SWITCH_STATUS_SUCCESS;
}