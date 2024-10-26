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
    switch_atomic_t asrproxy_concurrent_cnt;
} asrproxy_global_t;

asrproxy_global_t *asrproxy_globals;

template<typename T>
class WebsocketClient;

#if ENABLE_WSS
typedef WebsocketClient<websocketpp::config::asio_tls_client> asrproxy_client;
#else
typedef WebsocketClient<websocketpp::config::asio_client> asrproxy_client;
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
    switch_core_session_t *session;
    asrproxy_client *client;
    int started;
    int stopped;
    int starting;
    switch_mutex_t *mutex;
    switch_audio_resampler_t *re_sampler;
    char *asrproxy_url;
    asr_callback_t *asr_callback;
} asrproxy_context_t;

/**
 * 识别启动回调函数
 *
 * @param ctx
 */
void onTranscriptionStarted(asrproxy_context_t *ctx) {
    if (asrproxy_globals->_debug) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "onTranscriptionStarted: asrproxy\n");
    }
    switch_mutex_lock(ctx->mutex);
    ctx->started = 1;
    ctx->starting = 0;
    switch_mutex_unlock(ctx->mutex);

    if (ctx->asr_callback) {
        if (asrproxy_globals->_debug) {
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
void onSentenceBegin(asrproxy_context_t *ctx) {
    if (asrproxy_globals->_debug) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "onSentenceBegin: asrproxy\n");
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
void onSentenceEnd(asrproxy_context_t *ctx, asr_sentence_result_t *asr_sentence_result) {
    if (asrproxy_globals->_debug) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "onSentenceEnd: asrproxy\n");
    }
    if (ctx->asr_callback) {
        if (asrproxy_globals->_debug) {
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
void onTranscriptionResultChanged(asrproxy_context_t *ctx, const std::string &text) {
    if (asrproxy_globals->_debug) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "onTranscriptionResultChanged: asrproxy\n");
    }
    if (ctx->asr_callback) {
        if (asrproxy_globals->_debug) {
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
void onTranscriptionCompleted(asrproxy_context_t *ctx) {
    if (asrproxy_globals->_debug) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "onTranscriptionCompleted: asrproxy\n");
    }
}

/**
 * @brief 异常识别回调函数
 *
 * @param ctx
 */
void onTaskFailed(asrproxy_context_t *ctx) {
    if (asrproxy_globals->_debug) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "onTaskFailed: asrproxy\n");
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
void onChannelClosed(asrproxy_context_t *ctx) {
    if (asrproxy_globals->_debug) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "onChannelClosed: asrproxy\n");
    }
    if (ctx->asr_callback) {
        if (asrproxy_globals->_debug) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "onChannelClosed: call on_asr_stopped_func %p\n", ctx->asr_callback->on_asr_stopped_func);
        }
        ctx->asr_callback->on_asr_stopped_func(ctx->asr_callback->asr_caller);
    } else {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "onChannelClosed: ctx->asr_callback is null\n");
    }
}

/**
 * Define a semi-cross platform helper method that waits/sleeps for a bit.
 */
void WaitABit() {
#ifdef WIN32
    Sleep(1000);
#else
    usleep(1000);
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

    WebsocketClient(int is_ssl, asrproxy_context_t *asr_ctx) : m_open(false), m_done(false) {
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
                if (asrproxy_globals->_debug) {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "thread: %s, on_message = %s\n",
                                      id_str.c_str(),
                                      payload.c_str());
                }

                if (asr_result["mode"] == "2pass-online") {
                    // sample response:
                    // {"is_final":false,"mode":"2pass-online","text":"可","wav_name":"asr"}
                    // {"is_final":false,"mode":"2pass-online","text":"我现在","wav_name":"asr"}
                    // {"is_final":false,"mode":"2pass-online","text":"不","wav_name":"asr"}
                    //  {"is_final":false,"mode":"2pass-online","text":"没必","wav_name":"asr"}
                    onTranscriptionResultChanged(m_asr_ctx, asr_result["text"]);
                } else if (asr_result["mode"] == "2pass-offline") {
                    // sample response:
                    //  {"is_final":false,"mode":"2pass-offline",
                    //  "stamp_sents":[{"end":3485,"punc":"","start":2970,"text_seg":"可 以","ts_list":[[2970,3130],[3130,3485]]}],
                    //  "text":"可以","timestamp":"[[2970,3130],[3130,3485]]","wav_name":"asr"}
                    //
                    //  {"is_final":false,"mode":"2pass-offline",
                    //  "stamp_sents":[{"end":-1,"punc":"，","start":-1,"text_seg":"","ts_list":[]},{"end":14485,"punc":"","start":13500,"text_seg":"我 现 在 不 用",
                    //          "ts_list":[[13500,13720],[13720,13840],[13840,14000],[14000,14100],[14100,14485]]}],
                    //  "text":"，我现在不用","timestamp":"[[13500,13720],[13720,13840],[13840,14000],[14000,14100],[14100,14485]]","wav_name":"asr"}
                    //
                    //  {"is_final":false,"mode":"2pass-offline",
                    //  "stamp_sents":[{"end":-1,"punc":"，","start":-1,"text_seg":"","ts_list":[]},{"end":22195,"punc":"","start":21460,"text_seg":"没 必 要",
                    //          "ts_list":[[21460,21700],[21700,21860],[21860,22195]]}],
                    //  "text":"，没必要","timestamp":"[[21460,21700],[21700,21860],[21860,22195]]","wav_name":"asr"}
                    const std::string &tm_str = asr_result["timestamp"];
                    const std::string &text = asr_result["text"];

                    if (asrproxy_globals->_debug) {
                        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "timestamp: %s\n",
                                          tm_str.c_str());
                    }
                    int32_t sentence_begin_time = -1, sentence_time = -1;
                    {
                        const size_t first_begin_pos = tm_str.find("[[");
                        if (std::string::npos != first_begin_pos) {
                            const size_t first_end_pos = tm_str.find(',');
                            if (std::string::npos != first_end_pos) {
                                sentence_begin_time = (int32_t) strtol(
                                        tm_str.substr(first_begin_pos + 2, first_end_pos - first_begin_pos - 2).c_str(),
                                        nullptr, 10);
                            }
                        }
                    }
                    {
                        const size_t last_begin_pos = tm_str.find_last_of(',');
                        if (std::string::npos != last_begin_pos) {
                            const size_t last_end_pos = tm_str.find_last_of("]]");
                            if (std::string::npos != last_end_pos) {
                                sentence_time = (int32_t)strtol(
                                        tm_str.substr(last_begin_pos + 1, last_end_pos - last_begin_pos - 1).c_str(),
                                        nullptr, 10);
                            }
                        }
                    }

                    asr_sentence_result_t asr_sentence_result = {
                            -1,
                            sentence_begin_time,
                            sentence_time,
                            0.0,
                            text.c_str()
                    };
                    onSentenceEnd(m_asr_ctx, &asr_sentence_result);
                }

                if (asr_result["is_final"] == true) {
                    websocketpp::lib::error_code ec;

                    m_client.close(hdl, websocketpp::close::status::going_away, "", ec);

                    if (ec) {
                        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Error closing connection: %s\n",
                                          ec.message().c_str());
                    }
                }
            }
                break;
            default:
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "un-handle opcode: %d\n", msg->get_opcode());
                break;
        }
    }

    // This method will block until the connection is complete
    int start(const std::string &uri, const std::string &asr_mode, std::vector<int> chunk_vector) {
        if (asrproxy_globals->_debug) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "start wsc with: %s mode: %s\n", uri.c_str(),
                              asr_mode.c_str());
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

        if (asrproxy_globals->_debug) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "start send wsc first msg\n");
        }
        // first message
        bool wait = false;
        while (true) {
            {
                scoped_lock guard(m_lock);
                // If the connection has been closed, stop generating data
                if (m_done) {
//              break;
                    return -1;
                }
                // If the connection hasn't been opened yet wait a bit and retry
                if (!m_open) {
                    wait = true;
                } else {
                    break;
                }
            }

            if (wait) {
                // LOG(INFO) << "wait.." << m_open;
                WaitABit();
                continue;
            }
        }

        {
            nlohmann::json json_begin;
            nlohmann::json chunk_size = nlohmann::json::array();
            chunk_size.push_back(chunk_vector[0]);
            chunk_size.push_back(chunk_vector[1]);
            chunk_size.push_back(chunk_vector[2]);
            json_begin["mode"] = asr_mode;
            json_begin["chunk_size"] = chunk_size;
            json_begin["wav_name"] = "asr";
            json_begin["wav_format"] = "pcm";
            json_begin["is_speaking"] = true;

            websocketpp::lib::error_code ec;
            m_client.send(m_hdl, json_begin.dump(), websocketpp::frame::opcode::text, ec);
            if (ec) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "asrproxy send begin msg failed: %s\n",
                                  ec.message().c_str());
            } else {
                if (asrproxy_globals->_debug) {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "asrproxy send begin msg success\n");
                }
            }
        }

        return 0;
    }

    void stop() {
        {
            nlohmann::json json_end;
            json_end["is_speaking"] = false;
            websocketpp::lib::error_code ec;
            m_client.send(m_hdl, json_end.dump(), websocketpp::frame::opcode::text, ec);
            if (ec) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "asrproxy send end msg failed: %s\n",
                                  ec.message().c_str());
            } else {
                if (asrproxy_globals->_debug) {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "asrproxy send end msg success\n");
                }
            }
        }

        m_client.stop_perpetual();
        m_thread->join();

        onChannelClosed(m_asr_ctx);
    }

    // The open handler will signal that we are ready to start sending data
    void on_open(const websocketpp::connection_hdl &) {
        if (asrproxy_globals->_debug) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Connection opened, starting data!\n");
        }

        {
            scoped_lock guard(m_lock);
            m_open = true;
        }
        onTranscriptionStarted(m_asr_ctx);
    }

    // The close handler will signal that we should stop sending data
    void on_close(const websocketpp::connection_hdl &) {
        if (asrproxy_globals->_debug) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Connection closed, stopping data!\n");
        }

        {
            scoped_lock guard(m_lock);
            m_done = true;
        }
        onTranscriptionCompleted(m_asr_ctx);
    }

    // The fail handler will signal that we should stop sending data
    void on_fail(const websocketpp::connection_hdl &) {
        if (asrproxy_globals->_debug) {
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

    asrproxy_context_t *m_asr_ctx;
    websocketpp::connection_hdl m_hdl;
    websocketpp::lib::mutex m_lock;
    bool m_open;
    bool m_done;
};

// typedef WebsocketClient<websocketpp::config::asio_tls_client> asrproxy_client;

#define MAX_FRAME_BUFFER_SIZE (1024*1024) //1MB
#define SAMPLE_RATE 8000

asrproxy_client *generateAsrClient(asrproxy_context_t *pvt) {
    auto *client = new asrproxy_client(1, pvt);
    if (!client) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "generateAsrClient failed.\n");
        return nullptr;
    }

#if ENABLE_WSS
    client->m_client.set_tls_init_handler(bind(&OnTlsInit, ::_1));
#endif

    if (asrproxy_globals->_debug) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "proxy url is:%s\n", pvt->asrproxy_url);
    }
    return client;
}

//======================================== fun asr end ===============

//======================================== freeswitch module start ===============
SWITCH_MODULE_LOAD_FUNCTION(mod_asrproxy_load);

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_asrproxy_shutdown);

extern "C"
{
SWITCH_MODULE_DEFINITION(mod_asrproxy, mod_asrproxy_load, mod_asrproxy_shutdown, nullptr);
};

static void *init_asrproxy(switch_core_session_t *session, const switch_codec_implementation_t *read_impl, const char *cmd);

static bool start_asrproxy(asrproxy_context_t *pvt, asr_callback_t *asr_callback);

static bool send_audio_to_asrproxy(asrproxy_context_t *pvt, void *data, uint32_t data_len);

static void stop_asrproxy(asrproxy_context_t *pvt);

static void destroy_asrproxy(asrproxy_context_t *pvt);

static const asr_provider_t asrproxy_funcs = {
        init_asrproxy,
        reinterpret_cast<asr_start_func_t>(start_asrproxy),
        reinterpret_cast<asr_send_audio_func_t>(send_audio_to_asrproxy),
        reinterpret_cast<asr_stop_func_t>(stop_asrproxy),
        reinterpret_cast<asr_destroy_func_t>(destroy_asrproxy)
};

static switch_status_t attach_asrproxy_provider_on_channel_init(switch_core_session_t *session) {
    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_channel_set_private(channel, "asrproxy", &asrproxy_funcs);
    return SWITCH_STATUS_SUCCESS;
}

switch_state_handler_table_t asrproxy_cs_handlers = {
        /*! executed when the state changes to init */
        // switch_state_handler_t on_init;
        attach_asrproxy_provider_on_channel_init,
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

static void *init_asrproxy(switch_core_session_t *session, const switch_codec_implementation_t *read_impl, const char *cmd) {
    char *_proxy_url = nullptr;

    switch_memory_pool_t *pool;
    switch_core_new_memory_pool(&pool);
    char *my_cmd = switch_core_strdup(pool, cmd);

    char *argv[MAX_API_ARGC];
    memset(argv, 0, sizeof(char *) * MAX_API_ARGC);

    int argc = switch_split(my_cmd, ' ', argv);
    if (asrproxy_globals->_debug) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "cmd:%s, args count: %d\n", my_cmd, argc);
    }

    for (int idx = 1; idx < MAX_API_ARGC; idx++) {
        if (argv[idx]) {
            char *ss[2] = {nullptr, nullptr};
            int cnt = switch_split(argv[idx], '=', ss);
            if (cnt == 2) {
                char *var = ss[0];
                char *val = ss[1];
                if (asrproxy_globals->_debug) {
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

    asrproxy_context_t *pvt;
    if (!(pvt = (asrproxy_context_t *) switch_core_session_alloc(session, sizeof(asrproxy_context_t)))) {
        goto end;
    }
    pvt->started = 0;
    pvt->stopped = 0;
    pvt->starting = 0;
    pvt->session = session;
    pvt->asrproxy_url = switch_core_session_strdup(session, _proxy_url);
    switch_mutex_init(&pvt->mutex, SWITCH_MUTEX_NESTED, switch_core_session_get_pool(session));

    if (read_impl->actual_samples_per_second != SAMPLE_RATE) {
        if (switch_resample_create(&pvt->re_sampler,
                                   read_impl->actual_samples_per_second,
                                   SAMPLE_RATE,
                                   16 * (read_impl->microseconds_per_packet / 1000) * 2,
                                   SWITCH_RESAMPLE_QUALITY,
                                   1) != SWITCH_STATUS_SUCCESS) {
            // release all resource alloc before
            switch_mutex_destroy(pvt->mutex);

            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Unable to allocate re_sampler\n");
            pvt = nullptr;
            goto end;
        }
        if (asrproxy_globals->_debug) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
                              "create re-sampler bcs of media sampler/s is %d but fun asr support: %d, while ms/p: %d\n",
                              read_impl->actual_samples_per_second, SAMPLE_RATE, read_impl->microseconds_per_packet);
        }
    }

    // increment asrproxy concurrent count
    switch_atomic_inc(&asrproxy_globals->asrproxy_concurrent_cnt);

end:
    switch_core_destroy_memory_pool(&pool);
    return pvt;
}

static bool start_asrproxy(asrproxy_context_t *pvt, asr_callback_t *asr_callback) {
    bool  ret_val = false;
    if (pvt->stopped == 1) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "start_asrproxy: pvt->stopped\n");
        return ret_val;
    }

    switch_mutex_lock(pvt->mutex);
    if (pvt->started == 0) {
        if (pvt->starting == 0) {
            pvt->starting = 1;
            if (asrproxy_globals->_debug) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Starting Transaction \n");
            }
            switch_channel_t *channel = switch_core_session_get_channel(pvt->session);
            pvt->asr_callback = asr_callback;
            asrproxy_client *fac = generateAsrClient(pvt);
            if (!fac) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Asr Client init failed.%s\n",
                                  switch_channel_get_name(channel));
                ret_val = false;
                goto unlock;
            }
            pvt->client = fac;
            if (asrproxy_globals->_debug) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Init Fun Asr Client.%s\n",
                                  switch_channel_get_name(channel));
            }

            std::vector<int> chunk_size;
            chunk_size.push_back(5);
            chunk_size.push_back(10);
            chunk_size.push_back(5);

            if (pvt->client->start(std::string(pvt->asrproxy_url), "2pass", chunk_size) < 0) {
                pvt->stopped = 1;
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
                                  "start() failed. may be can not connect server(%s). please check network or firewalld:%s\n",
                                  pvt->asrproxy_url, switch_channel_get_name(channel));
                pvt->client->stop();
                delete pvt->client;
                pvt->client = nullptr;
                // start()失败，释放request对象
                ret_val = false;
                goto unlock;
            }
            ret_val = true;
        }
    }

    unlock:
    switch_mutex_unlock(pvt->mutex);
    return ret_val;
}

static bool send_audio_to_asrproxy(asrproxy_context_t *pvt, void *data, uint32_t data_len) {
    bool  ret_val = false;
    // send audio to asr
    switch_mutex_lock(pvt->mutex);

    if (pvt->client) {
        if (pvt->re_sampler) {
            //====== resample ==== ///
            switch_resample_process(pvt->re_sampler, (int16_t *) data, (int) data_len / 2 / 1);
            memcpy(data, pvt->re_sampler->to, pvt->re_sampler->to_len * 2 * 1);
            data_len = pvt->re_sampler->to_len * 2 * 1;
            if (asrproxy_globals->_debug) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "ASR new samples:%d\n",
                                  pvt->re_sampler->to_len);
            }
        }

        if (pvt->started) {
            websocketpp::lib::error_code ec;
            pvt->client->sendAudio((uint8_t *) data, (size_t) data_len, ec);

            if (ec) {
                pvt->stopped = 1;
                switch_channel_t *channel = switch_core_session_get_channel(pvt->session);
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "send audio failed: %s -> on channel: %s\n",
                                  ec.message().c_str(), switch_channel_get_name(channel));
                pvt->client->stop();
                delete pvt->client;
                pvt->client = nullptr;
                ret_val = false;
                goto unlock;
            }
        } else {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "send_audio_to_asrproxy: connecting, ignore send audio\n");
        }
        ret_val = true;
        if (asrproxy_globals->_debug) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "send_audio_to_asrproxy: send audio %d\n",
                              data_len);
        }
    } else {
        switch_channel_t *channel = switch_core_session_get_channel(pvt->session);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "try send audio but client has been released -> on channel: %s\n",
                          switch_channel_get_name(channel));
        ret_val = false;
    }

    unlock:
    switch_mutex_unlock(pvt->mutex);
    return ret_val;
}

static void stop_asrproxy(asrproxy_context_t *pvt) {
    switch_mutex_lock(pvt->mutex);
    switch_channel_t *channel = switch_core_session_get_channel(pvt->session);
    if (pvt->client) {
        if (asrproxy_globals->_debug) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "try to stop fun asr on channel: %s\n",
                              switch_channel_get_name(channel));
        }
        pvt->client->stop();
        //7: 识别结束, 释放fac对象
        delete pvt->client;
        pvt->client = nullptr;
        if (asrproxy_globals->_debug) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "stop fun asr and client is released on channel: %s\n",
                              switch_channel_get_name(channel));
        }
    } else {
        if (asrproxy_globals->_debug) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
                              "fun asr has already stopped and released on channel:%s\n",
                              switch_channel_get_name(channel));
        }
    }
    switch_mutex_unlock(pvt->mutex);
}

static void destroy_asrproxy(asrproxy_context_t *pvt) {
    switch_core_session_t *session = pvt->session;
    switch_channel_t *channel = switch_core_session_get_channel(session);
    if (asrproxy_globals->_debug) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(pvt->session), SWITCH_LOG_NOTICE,
                          "destroy_asrproxy: release all resource for session -> on channel: %s\n",
                          switch_channel_get_name(channel));
    }
    stop_asrproxy(pvt);
    if (asrproxy_globals->_debug) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE,
                          "destroy_asrproxy: stop_asrproxy -> channel: %s\n",
                          switch_channel_get_name(channel));
    }

    // decrement asrproxy concurrent count
    switch_atomic_dec(&asrproxy_globals->asrproxy_concurrent_cnt);

    if (pvt->re_sampler) {
        switch_resample_destroy(&pvt->re_sampler);
        if (asrproxy_globals->_debug) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE,
                              "destroy_asrproxy: switch_resample_destroy -> on channel: %s\n",
                              switch_channel_get_name(channel));
        }
    }
    switch_mutex_destroy(pvt->mutex);
    if (asrproxy_globals->_debug) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE,
                          "destroy_asrproxy: switch_mutex_destroy -> on channel: %s\n",
                          switch_channel_get_name(channel));
    }
}

SWITCH_STANDARD_API(asrproxy_concurrent_cnt_function) {
    const uint32_t concurrent_cnt = switch_atomic_read (&asrproxy_globals->asrproxy_concurrent_cnt);
    stream->write_function(stream, "%d\n", concurrent_cnt);
    return SWITCH_STATUS_SUCCESS;
}

#define ASRPROXY_DEBUG_SYNTAX "<on|off>"
SWITCH_STANDARD_API(mod_asrproxy_debug) {
    if (zstr(cmd)) {
        stream->write_function(stream, "-USAGE: %s\n", ASRPROXY_DEBUG_SYNTAX);
    } else {
        if (!strcasecmp(cmd, "on")) {
            asrproxy_globals->_debug = true;
            stream->write_function(stream, "asrproxy Debug: on\n");
        } else if (!strcasecmp(cmd, "off")) {
            asrproxy_globals->_debug = false;
            stream->write_function(stream, "asrproxy Debug: off\n");
        } else {
            stream->write_function(stream, "-USAGE: %s\n", ASRPROXY_DEBUG_SYNTAX);
        }
    }
    return SWITCH_STATUS_SUCCESS;
}

/**
 *  定义load函数，加载时运行
 */
SWITCH_MODULE_LOAD_FUNCTION(mod_asrproxy_load) {
    switch_api_interface_t *api_interface = nullptr;
    *module_interface = switch_loadable_module_create_module_interface(pool, modname);

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_asrproxy load starting\n");

    asrproxy_globals = (asrproxy_global_t *)switch_core_alloc(pool, sizeof(asrproxy_global_t));

    asrproxy_globals->_debug = false;

    // register global state handlers
    switch_core_add_state_handler(&asrproxy_cs_handlers);

    SWITCH_ADD_API(api_interface,
                   "asrproxy_concurrent_cnt",
                   "asrproxy_concurrent_cnt api",
                   asrproxy_concurrent_cnt_function,
                   "<cmd><args>");

    SWITCH_ADD_API(api_interface, "asrproxy_debug", "Set asrproxy debug", mod_asrproxy_debug, ASRPROXY_DEBUG_SYNTAX);

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_asrproxy loaded\n");

    return SWITCH_STATUS_SUCCESS;
}

/**
 *  定义shutdown函数，关闭时运行
 */
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_asrproxy_shutdown) {
    // unregister global state handlers
    switch_core_remove_state_handler(&asrproxy_cs_handlers);

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, " mod_asrproxy shutdown called\n");
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, " mod_asrproxy unload\n");
    return SWITCH_STATUS_SUCCESS;
}