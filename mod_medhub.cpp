#include <switch.h>
#include <fstream>
#include <math.h>

#define ASIO_STANDALONE 1

#define ENABLE_WSS 0
#define ENABLE_MEDHUB_PLAYBACK 0

#include <websocketpp/client.hpp>
#include <websocketpp/common/thread.hpp>
#include <websocketpp/config/asio_client.hpp>
#include <thread>

#include "nlohmann/json.hpp"

typedef struct {
    bool _debug;
    switch_atomic_t medhub_concurrent_cnt;
} medhub_global_t;

medhub_global_t *medhub_globals;

template<typename T>
class WebsocketClient;

#if ENABLE_WSS
typedef WebsocketClient<websocketpp::config::asio_tls_client> medhub_client;
#else
typedef WebsocketClient<websocketpp::config::asio_client> medhub_client;
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
    medhub_client         *client;

    //  asr fields
    int started;
    int stopped;
    int starting;
    switch_mutex_t          *mutex;
    switch_audio_resampler_t *re_sampler;
    char                    *medhub_url;
    asr_callback_t          *asr_callback;
#if ENABLE_MEDHUB_PLAYBACK
    // playback fields
    switch_codec_t          playback_codec;
    uint32_t                playback_rate;
    int32_t                 playback_channels;
    uint32_t                playback_timestamp;
    int                     current_stream_id;
    int                     last_playback_samples;
    bool                    last_playback_completed;
#endif
} medhub_context_t;

/**
 * 识别启动回调函数
 *
 * @param ctx
 */
void on_transcription_started(medhub_context_t *ctx, const nlohmann::json &hub_event);

/**
 * @brief 一句话开始回调函数
 *
 * @param ctx
 */
void on_sentence_begin(medhub_context_t *ctx, const nlohmann::json &hub_event);

/**
 * @brief 一句话结束回调函数
 *
 * @param ctx
 * @param text
 */
void on_sentence_end(medhub_context_t *ctx, const nlohmann::json &hub_event);

/**
 * @brief 识别结果变化回调函数
 *
 * @param ctx
 * @param text
 */
void on_transcription_result_changed(medhub_context_t *ctx, const nlohmann::json &hub_event);

/**
 * @brief 语音转写结束回调函数
 *
 * @param ctx
 */
void on_transcription_completed(medhub_context_t *ctx, const nlohmann::json &hub_event);

/**
 * @brief 异常识别回调函数
 *
 * @param ctx
 */
void on_task_failed(medhub_context_t *ctx);

/**
 * @brief 识别通道关闭回调函数
 *
 * @param ctx
 */
void on_channel_closed(medhub_context_t *ctx);

#if ENABLE_MEDHUB_PLAYBACK
void on_playback_start(medhub_context_t *ctx, const nlohmann::json &hub_event);
void on_playback_stop(medhub_context_t *ctx, const nlohmann::json &hub_event);
void on_playback_data(medhub_context_t *ctx, uint8_t *data, int32_t len);
#endif

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

    WebsocketClient(int is_ssl, medhub_context_t *asr_ctx)
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

    bool is_connected() {
        return m_open;
    }

    void on_message(websocketpp::connection_hdl hdl, message_ptr msg) {
        const std::string &payload = msg->get_payload();
        switch (msg->get_opcode()) {
            case websocketpp::frame::opcode::text: {
                nlohmann::json hubevent = nlohmann::json::parse(payload);
                std::string id_str = getThreadIdOfString(std::this_thread::get_id());
                if (medhub_globals->_debug) {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "thread: %s, on_message = %s\n",
                                      id_str.c_str(),
                                      payload.c_str());
                }

                if (hubevent["header"]["name"] == "TranscriptionStarted") {
                    on_transcription_started(m_asr_ctx, hubevent);
                } else if (hubevent["header"]["name"] == "TranscriptionCompleted") {
                    on_transcription_completed(m_asr_ctx, hubevent);
                    {
                        websocketpp::lib::error_code ec;
                        m_client.close(hdl, websocketpp::close::status::going_away, "", ec);
                        if (ec) {
                            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Error closing connection: %s\n",
                                              ec.message().c_str());
                        }
                    }
                } else if (hubevent["header"]["name"] == "SentenceBegin") {
                    on_sentence_begin(m_asr_ctx, hubevent);
                } else if (hubevent["header"]["name"] == "TranscriptionResultChanged") {
                    on_transcription_result_changed(m_asr_ctx, hubevent);
                } else if (hubevent["header"]["name"] == "SentenceEnd") {
                    on_sentence_end(m_asr_ctx, hubevent);
                }
#if ENABLE_MEDHUB_PLAYBACK
                else if (hubevent["header"]["name"] == "PlaybackStart") {
                    on_playback_start(m_asr_ctx, hubevent);
                } else if (hubevent["header"]["name"] == "PlaybackStop") {
                    on_playback_stop(m_asr_ctx, hubevent);
                }
#endif
            }
                break;
#if ENABLE_MEDHUB_PLAYBACK
            case websocketpp::frame::opcode::binary:
                on_playback_data(m_asr_ctx, (uint8_t *) payload.data(), (int32_t) payload.size());
                break;
#endif
            default:
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "un-handle opcode: %d\n", msg->get_opcode());
                break;
        }
    }

    // This method will block until the connection is complete
    int connect(const std::string &uri) {
        if (medhub_globals->_debug) {
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
        if (medhub_globals->_debug) {
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
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "medhub send begin msg failed: %s\n",
                              ec.message().c_str());
        } else {
            if (medhub_globals->_debug) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "medhub send begin msg success\n");
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
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "medhub send stop msg failed: %s\n",
                              ec.message().c_str());
        } else {
            if (medhub_globals->_debug) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "medhub send stop msg success\n");
            }
        }

        m_client.stop_perpetual();
        m_thread->join();

        on_channel_closed(m_asr_ctx);
    }

    // The open handler will signal that we are ready to start sending data
    void on_open(const websocketpp::connection_hdl &) {
        if (medhub_globals->_debug) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Connection opened, starting data!\n");
        }

        {
            scoped_lock guard(m_lock);
            m_open = true;
        }
    }

    // The close handler will signal that we should stop sending data
    void on_close(const websocketpp::connection_hdl &) {
        if (medhub_globals->_debug) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Connection closed, stopping data!\n");
        }

        {
            scoped_lock guard(m_lock);
            m_done = true;
        }
    }

    // The fail handler will signal that we should stop sending data
    void on_fail(const websocketpp::connection_hdl &) {
        if (medhub_globals->_debug) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Connection failed, stopping data!\n");
        }

        {
            scoped_lock guard(m_lock);
            m_done = true;
        }
        on_task_failed(m_asr_ctx);
    }

    void sendAudio(uint8_t *dp, size_t data_len, websocketpp::lib::error_code &ec) {
        m_client.send(m_hdl, dp, data_len, websocketpp::frame::opcode::binary, ec);
    }

#if ENABLE_MEDHUB_PLAYBACK
    void playback(const char *filename, const int stream_id, const int samples) {
        nlohmann::json json_playback = {
                {"header", {
                        // 当次消息请求ID，随机生成32位唯一ID。
                        //{"message_id", message_id},
                        // 整个实时语音合成的会话ID，整个请求中需要保持一致，32位唯一ID。
                        //{"task_id", m_task_id},
                        //{"namespace", "FlowingSpeechSynthesizer"},
                        {"name", "Playback"}
                        //{"appkey", m_appkey}
                }}
                /*,
                {"payload", {
                       {"file", filename}
                }} */
        };
        if (filename) {
            json_playback["payload"]["file"] = filename;
        }
        if (stream_id) {
            json_playback["payload"]["id"] = stream_id;
            json_playback["payload"]["samples"] = samples;
        }

        const std::string str_playback = json_playback.dump();
        if (medhub_globals->_debug) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "playback: send Playback command, detail: %s\n",
                              str_playback.c_str());
        }

        websocketpp::lib::error_code ec;
        m_client.send(m_hdl, str_playback, websocketpp::frame::opcode::text, ec);
        if (ec) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "medhub send playback msg failed: %s\n",
                              ec.message().c_str());
        } else {
            if (medhub_globals->_debug) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "medhub send playback msg success\n");
            }
        }
    }

    void playtts(const char *text) {
        const nlohmann::json json_playtts = {
                {"header", {
                                   // 当次消息请求ID，随机生成32位唯一ID。
                                   //{"message_id", message_id},
                                   // 整个实时语音合成的会话ID，整个请求中需要保持一致，32位唯一ID。
                                   //{"task_id", m_task_id},
                                   //{"namespace", "FlowingSpeechSynthesizer"},
                                   {"name", "PlayTTS"}
                                   //{"appkey", m_appkey}
                           }},
                {"payload", {
                                   {"text", text}
                           }}
        };

        const std::string str_playtts = json_playtts.dump();
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "playback: send Playback command, detail: %s\n",
                          str_playtts.c_str());

        websocketpp::lib::error_code ec;
        m_client.send(m_hdl, str_playtts, websocketpp::frame::opcode::text, ec);
        if (ec) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "medhub send playtts msg failed: %s\n",
                              ec.message().c_str());
        } else {
            if (medhub_globals->_debug) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "medhub send playtts msg success\n");
            }
        }
    }

    void stop_playback() {
        nlohmann::json json_stop_playback = {
                {"header", {
                                   // 当次消息请求ID，随机生成32位唯一ID。
                                   //{"message_id", message_id},
                                   // 整个实时语音合成的会话ID，整个请求中需要保持一致，32位唯一ID。
                                   //{"task_id", m_task_id},
                                   //{"namespace", "FlowingSpeechSynthesizer"},
                                   {"name", "StopPlayback"}
                                   //{"appkey", m_appkey}
                           }}
//                {"payload", {
//                                   {"file", filename}
//                           }}
        };

        const std::string str_stop_playback = json_stop_playback.dump();
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "playback: send StopPlayback command, detail: %s\n",
                          str_stop_playback.c_str());

        websocketpp::lib::error_code ec;
        m_client.send(m_hdl, str_stop_playback, websocketpp::frame::opcode::text, ec);
        if (ec) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "medhub send stop playback msg failed: %s\n",
                              ec.message().c_str());
        } else {
            if (medhub_globals->_debug) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "medhub send stop playback msg success\n");
            }
        }
    }
#endif

    websocketpp::client<T> m_client;
    websocketpp::lib::shared_ptr<websocketpp::lib::thread> m_thread;

private:

    medhub_context_t *m_asr_ctx;
    websocketpp::connection_hdl m_hdl;
    websocketpp::lib::mutex m_lock;
    bool m_open;
    bool m_done;
};

/**
 * 识别启动回调函数
 *
 * @param ctx
 */
void on_transcription_started(medhub_context_t *ctx, const nlohmann::json &hub_event) {
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
    if (medhub_globals->_debug) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "on_transcription_started: medhub\n");
    }
    switch_mutex_lock(ctx->mutex);
    ctx->started = 1;
    ctx->starting = 0;
    switch_mutex_unlock(ctx->mutex);

    if (ctx->asr_callback) {
        if (medhub_globals->_debug) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "on_transcription_started: call on_asr_started_func %p\n", ctx->asr_callback->on_asr_started_func);
        }
        ctx->asr_callback->on_asr_started_func(ctx->asr_callback->asr_caller);
    } else {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "on_transcription_started: ctx->asr_callback is null\n");
    }
}

/**
 * @brief 一句话开始回调函数
 *
 * @param ctx
 */
void on_sentence_begin(medhub_context_t *ctx, const nlohmann::json &hub_event) {
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
    if (medhub_globals->_debug) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "on_sentence_begin: medhub\n");
    }
    if (ctx->asr_callback) {
        ctx->asr_callback->on_asr_sentence_begin_func(ctx->asr_callback->asr_caller);
    }
#if ENABLE_MEDHUB_PLAYBACK
    if (ctx->current_stream_id) {
        // is playbacking
        ctx->client->stop_playback();
    }
#endif
}

/**
 * @brief 一句话结束回调函数
 *
 * @param ctx
 * @param text
 */
void on_sentence_end(medhub_context_t *ctx, const nlohmann::json &hub_event) {
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
#if ENABLE_MEDHUB_PLAYBACK
    if (ctx->current_stream_id) {
        // is playbacking
        ctx->client->playback(nullptr, ctx->current_stream_id, ctx->last_playback_samples);
    }
#endif
    std::string result = hub_event["payload"]["result"];
    asr_sentence_result_t asr_sentence_result = {
            hub_event["payload"]["index"],
            hub_event["payload"]["begin_time"],
            hub_event["payload"]["time"],
            hub_event["payload"]["confidence"],
            result.c_str()
    };
    if (medhub_globals->_debug) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "on_sentence_end: medhub\n");
    }
    if (ctx->asr_callback) {
        if (medhub_globals->_debug) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "on_sentence_end: call on_asr_sentence_end_func %p\n", ctx->asr_callback->on_asr_sentence_end_func);
        }
        ctx->asr_callback->on_asr_sentence_end_func(ctx->asr_callback->asr_caller, &asr_sentence_result);
    } else {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "on_sentence_end: ctx->asr_callback is null\n");
    }
}

/**
 * @brief 识别结果变化回调函数
 *
 * @param ctx
 * @param text
 */
void on_transcription_result_changed(medhub_context_t *ctx, const nlohmann::json &hub_event) {
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
    std::string result = hub_event["payload"]["result"];
    if (medhub_globals->_debug) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "on_transcription_result_changed: medhub\n");
    }
    if (ctx->asr_callback) {
        if (medhub_globals->_debug) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "on_transcription_result_changed: call on_asr_result_changed_func %p\n", ctx->asr_callback->on_asr_result_changed_func);
        }
        asr_sentence_result_t asr_sentence_result = {
                -1,
                -1,
                -1,
                0.0,
                result.c_str()
        };
        ctx->asr_callback->on_asr_result_changed_func(ctx->asr_callback->asr_caller, &asr_sentence_result);
    } else {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "on_transcription_result_changed: ctx->asr_callback is null\n");
    }
}

/**
 * @brief 语音转写结束回调函数
 *
 * @param ctx
 */
void on_transcription_completed(medhub_context_t *ctx, const nlohmann::json &hub_event) {
    /* TranscriptionCompleted 事件
    {
        "header": {
            "name": "TranscriptionCompleted",
        }
    } */
    if (medhub_globals->_debug) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "on_transcription_completed: medhub\n");
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
void on_task_failed(medhub_context_t *ctx) {
    if (medhub_globals->_debug) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "on_task_failed: medhub\n");
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
void on_channel_closed(medhub_context_t *ctx) {
    if (medhub_globals->_debug) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "on_channel_closed: medhub\n");
    }
    /*
    if (ctx->asr_callback) {
        if (medhub_globals->_debug) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "onChannelClosed: call on_asr_stopped_func %p\n", ctx->asr_callback->on_asr_stopped_func);
        }
        ctx->asr_callback->on_asr_stopped_func(ctx->asr_callback->asr_caller);
    } else {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "on_channel_closed: ctx->asr_callback is null\n");
    }
     */
}
#if ENABLE_MEDHUB_PLAYBACK
void on_playback_start(medhub_context_t *ctx, const nlohmann::json &hub_event) {
    /* PlaybackStart 事件
    {
        "header": {
            "name": "PlaybackStart",
        },
        "payload": {
            "stream_id": 4,
            "file": "...",
            "rate": 8000,
            "interval": 20,
            "channels": 1
        }
    } */
    const std::string filename = hub_event["payload"]["file"];
    const int stream_id = hub_event["payload"]["id"];
    const uint32_t rate = hub_event["payload"]["rate"];
    const int32_t interval = hub_event["payload"]["interval"];
    const int32_t channels = hub_event["payload"]["channels"];

    if (medhub_globals->_debug) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "recv PlaybackStart event: %s\n", filename.c_str());
    }

    // switch_core_session_write_lock(ctx->session);

    if (switch_core_codec_init(&ctx->playback_codec,
                               "L16",
                               NULL,
                               NULL,
                               rate,
                               interval,
                               channels,
                               SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE,
                               NULL,
                               switch_core_session_get_pool(ctx->session)) ==
        SWITCH_STATUS_SUCCESS) {
        if (medhub_globals->_debug) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(ctx->session), SWITCH_LOG_NOTICE,
                              "Codec Activated %s@%uhz %u channels %dms\n",
                              "L16", rate, channels, interval);
        }
    } else {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(ctx->session), SWITCH_LOG_WARNING,
                          "Raw Codec Activation Failed %s@%uhz %u channels %dms\n",
                          "L16", rate, channels, interval);
    }
    ctx->playback_rate = rate;
    ctx->playback_channels = channels;
    ctx->playback_timestamp = 0;
    ctx->current_stream_id = stream_id;

    switch_channel_t *channel = switch_core_session_get_channel(ctx->session);
    switch_channel_set_private(channel, "znc_playing", "1");

    // switch_core_session_rwunlock(ctx->session);
}

void on_playback_stop(medhub_context_t *ctx, const nlohmann::json &hub_event) {
    /* PlaybackStop 事件
    {
        "header": {
            "name": "PlaybackStop",
        },
        "payload": {
            "file": "...",
            "samples":38880,
            "completed":true
        }
    } */
    const std::string filename = hub_event["payload"]["file"];
    const int stream_id = hub_event["payload"]["id"];
    const int samples = hub_event["payload"]["samples"];
    const bool completed = hub_event["payload"]["completed"];

    ctx->last_playback_samples = samples;
    ctx->last_playback_completed = completed;

    if (medhub_globals->_debug) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                          "recv PlaybackStop event: file: %s/samples: %d/completed: %d, current stream id: %d/stop stream_id: %d\n",
                          filename.c_str(), samples, completed, ctx->current_stream_id, stream_id);
    }

    if (completed) {
        // playback completed, clear stream id
        ctx->current_stream_id = 0;
        // switch_core_session_write_lock(ctx->session);
        switch_channel_t *channel = switch_core_session_get_channel(ctx->session);
        switch_channel_set_private(channel, "znc_playing", nullptr);
        // switch_core_session_rwunlock(ctx->session);
    }
}

void on_playback_data(medhub_context_t *ctx, uint8_t *data, int32_t len) {
    switch_frame_t write_frame = { 0 };
    write_frame.codec = &ctx->playback_codec;
    write_frame.rate = ctx->playback_rate;
    write_frame.channels = ctx->playback_channels;
    write_frame.samples = len / 2;
    write_frame.timestamp = ctx->playback_timestamp;
    write_frame.data = data;
    write_frame.datalen = len;
    /*switch_status_t status =*/
    switch_core_session_write_frame(ctx->session, &write_frame, SWITCH_IO_FLAG_NONE, 0);
    ctx->playback_timestamp += write_frame.samples;
}
#endif

// typedef WebsocketClient<websocketpp::config::asio_tls_client> medhub_client;

#define MAX_FRAME_BUFFER_SIZE (1024*1024) //1MB
#define SAMPLE_RATE 8000

medhub_client *generateMediaHubClient(medhub_context_t *ctx) {
    auto *client = new medhub_client(1, ctx);
    if (!client) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "generateMediaHubClient failed.\n");
        return nullptr;
    }

#if ENABLE_WSS
    client->m_client.set_tls_init_handler(bind(&OnTlsInit, ::_1));
#endif

    if (medhub_globals->_debug) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "proxy url is:%s\n", ctx->medhub_url);
    }
    return client;
}

//======================================== fun asr end ===============

//======================================== freeswitch module start ===============
SWITCH_MODULE_LOAD_FUNCTION(mod_medhub_load);

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_medhub_shutdown);

extern "C"
{
SWITCH_MODULE_DEFINITION(mod_medhub, mod_medhub_load, mod_medhub_shutdown, nullptr);
};

static void *init_medhub(switch_core_session_t *session, const switch_codec_implementation_t *read_impl, const char *cmd);

static bool start_medhub(medhub_context_t *ctx, asr_callback_t *asr_callback);

static bool send_audio_to_medhub(medhub_context_t *ctx, void *data, uint32_t data_len);

static void stop_medhub(medhub_context_t *ctx);

static void destroy_medhub(medhub_context_t *ctx);

static const asr_provider_t medhub_funcs = {
        init_medhub,
        reinterpret_cast<asr_start_func_t>(start_medhub),
        reinterpret_cast<asr_send_audio_func_t>(send_audio_to_medhub),
        reinterpret_cast<asr_stop_func_t>(stop_medhub),
        reinterpret_cast<asr_destroy_func_t>(destroy_medhub)
};

static switch_status_t attach_medhub_provider_on_channel_init(switch_core_session_t *session) {
    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_channel_set_private(channel, "medhub", &medhub_funcs);
    return SWITCH_STATUS_SUCCESS;
}

switch_state_handler_table_t medhub_cs_handlers = {
        /*! executed when the state changes to init */
        // switch_state_handler_t on_init;
        attach_medhub_provider_on_channel_init,
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

// params: <uuid> proxyurl=<uri>
#define MAX_API_ARGC 20

static void *init_medhub(switch_core_session_t *session, const switch_codec_implementation_t *read_impl, const char *cmd) {
    switch_channel_t *channel = switch_core_session_get_channel(session);
    if (medhub_globals->_debug) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "init_medhub:%s\n", switch_channel_get_name(channel));
    }

    auto *ctx = (medhub_context_t *)switch_channel_get_private(channel, "_medhub_ctx");
    if (!ctx) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "init_medhub failed, can't found medhub ctx by %s\n",
                          switch_channel_get_name(channel));
        return nullptr;
    }

    if (read_impl->actual_samples_per_second != SAMPLE_RATE) {
        if (switch_resample_create(&ctx->re_sampler,
                                   read_impl->actual_samples_per_second,
                                   SAMPLE_RATE,
                                   16 * (read_impl->microseconds_per_packet / 1000) * 2,
                                   SWITCH_RESAMPLE_QUALITY,
                                   1) != SWITCH_STATUS_SUCCESS) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Unable to allocate re_sampler\n");
            return nullptr;
        }
        if (medhub_globals->_debug) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
                              "create re-sampler bcs of media sampler/s is %d but fun asr support: %d, while ms/p: %d\n",
                              read_impl->actual_samples_per_second, SAMPLE_RATE, read_impl->microseconds_per_packet);
        }
    }
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "init_medhub's read_impl: samples_per_packet: %d/samples_per_second: %d/actual_samples_per_second: %d/microseconds_per_packet: %d\n",
                      read_impl->samples_per_packet, read_impl->samples_per_second, read_impl->actual_samples_per_second,
                      read_impl->microseconds_per_packet);

    return ctx;
}

static bool start_medhub(medhub_context_t *ctx, asr_callback_t *asr_callback) {
    bool  ret_val = false;
    if (ctx->stopped == 1) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "start_medhub: ctx->stopped\n");
        return ret_val;
    }
    switch_mutex_lock(ctx->mutex);
    ctx->asr_callback = asr_callback;
    if (ctx->client) {
        if (ctx->client->is_connected()) {
            if (medhub_globals->_debug) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Starting Transaction \n");
            }
            ctx->client->startTranscription();
        } else {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "media hub not connect, ignore \n");
        }
    }
    switch_mutex_unlock(ctx->mutex);

    ret_val = true;
    return ret_val;
}

static bool send_audio_to_medhub(medhub_context_t *ctx, void *data, uint32_t data_len) {
    bool  ret_val = false;
    // send audio to asr
    switch_mutex_lock(ctx->mutex);

    if (ctx->client) {
        if (ctx->re_sampler) {
            //====== resample ==== ///
            switch_resample_process(ctx->re_sampler, (int16_t *) data, (int) data_len / 2 / 1);
            memcpy(data, ctx->re_sampler->to, ctx->re_sampler->to_len * 2 * 1);
            data_len = ctx->re_sampler->to_len * 2 * 1;
            if (medhub_globals->_debug) {
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
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "send_audio_to_medhub: connecting, ignore send audio\n");
        }
        ret_val = true;
        if (medhub_globals->_debug) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "send_audio_to_medhub: send audio %d\n",
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

static void stop_medhub(medhub_context_t *ctx) {
    switch_mutex_lock(ctx->mutex);
    switch_channel_t *channel = switch_core_session_get_channel(ctx->session);
    if (ctx->client) {
        if (medhub_globals->_debug) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "try to stop medhub on channel: %s\n",
                              switch_channel_get_name(channel));
        }
        ctx->client->stop();
        //7: 识别结束, 释放fac对象
        delete ctx->client;
        ctx->client = nullptr;
        if (medhub_globals->_debug) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "stop medhub and client is released on channel: %s\n",
                              switch_channel_get_name(channel));
        }
    } else {
        if (medhub_globals->_debug) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
                              "medhub has already stopped and released on channel:%s\n",
                              switch_channel_get_name(channel));
        }
    }
    switch_mutex_unlock(ctx->mutex);
}

static void destroy_medhub(medhub_context_t *ctx) {
    switch_core_session_t *session = ctx->session;
    switch_channel_t *channel = switch_core_session_get_channel(session);
    auto *medhub_ctx = (medhub_context_t *)switch_channel_get_private(channel, "_medhub_ctx");
    if (!medhub_ctx) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "destroy_medhub: [%s]'s medhub_context is nullptr, destroy_medhub called before",
                          switch_core_session_get_uuid(session));
        return;
    }
    switch_channel_set_private(channel, "_medhub_ctx", nullptr); // clear channel's private data for medhub_context

    if (medhub_globals->_debug) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(ctx->session), SWITCH_LOG_NOTICE,
                          "destroy_medhub: release all resource for session -> on channel: %s\n",
                          switch_channel_get_name(channel));
    }
    stop_medhub(ctx);
    if (medhub_globals->_debug) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE,
                          "destroy_medhub: stop_medhub -> channel: %s\n",
                          switch_channel_get_name(channel));
    }

    // decrement medhub concurrent count
    switch_atomic_dec(&medhub_globals->medhub_concurrent_cnt);

    if (ctx->re_sampler) {
        switch_resample_destroy(&ctx->re_sampler);
        if (medhub_globals->_debug) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE,
                              "destroy_medhub: switch_resample_destroy -> on channel: %s\n",
                              switch_channel_get_name(channel));
        }
    }
    switch_mutex_destroy(ctx->mutex);
    if (medhub_globals->_debug) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE,
                          "destroy_medhub: switch_mutex_destroy -> on channel: %s\n",
                          switch_channel_get_name(channel));
    }
}

SWITCH_STANDARD_API(medhub_concurrent_cnt_function) {
    const uint32_t concurrent_cnt = switch_atomic_read (&medhub_globals->medhub_concurrent_cnt);
    stream->write_function(stream, "%d\n", concurrent_cnt);
    return SWITCH_STATUS_SUCCESS;
}

#define ASRHUB_DEBUG_SYNTAX "<on|off>"
SWITCH_STANDARD_API(mod_medhub_debug) {
    if (zstr(cmd)) {
        stream->write_function(stream, "-USAGE: %s\n", ASRHUB_DEBUG_SYNTAX);
    } else {
        if (!strcasecmp(cmd, "on")) {
            medhub_globals->_debug = true;
            stream->write_function(stream, "medhub Debug: on\n");
        } else if (!strcasecmp(cmd, "off")) {
            medhub_globals->_debug = false;
            stream->write_function(stream, "medhub Debug: off\n");
        } else {
            stream->write_function(stream, "-USAGE: %s\n", ASRHUB_DEBUG_SYNTAX);
        }
    }
    return SWITCH_STATUS_SUCCESS;
}

static switch_status_t medhub_cleanup_on_channel_destroy(switch_core_session_t *session) {
    if (medhub_globals->_debug) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
                          "medhub_cleanup_on_channel_destroy: try to cleanup medhub_context on session [%s] destroy",
                          switch_core_session_get_uuid(session));
    }
    switch_core_session_write_lock(session);
    switch_channel_t *channel = switch_core_session_get_channel(session);
    auto *medhub_ctx = (medhub_context_t *)switch_channel_get_private(channel, "_medhub_ctx");
    if (!medhub_ctx) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "medhub_cleanup_on_channel_destroy: [%s]'s medhub_context is nullptr",
                          switch_core_session_get_uuid(session));
        goto unlock;
    }
    destroy_medhub(medhub_ctx);

    unlock:
    switch_core_session_rwunlock(session);
    return SWITCH_STATUS_SUCCESS;
}

const static switch_state_handler_table_t session_medhub_handlers = {
        /*! executed when the state changes to init */
        // switch_state_handler_t on_init;
        nullptr,
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
        medhub_cleanup_on_channel_destroy,
        // int flags;
        0
};

static medhub_context_t *init_medhub_ctx_for(const char *url, const char* uuid) {
    switch_core_session_t *session = switch_core_session_force_locate(uuid);
    if (!session) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                          "init_medhub_ctx_for failed, can't found session by %s\n", uuid);
        return nullptr;
    }

    medhub_context_t *ctx;
    if (!(ctx = (medhub_context_t *) switch_core_session_alloc(session, sizeof(medhub_context_t)))) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                          "init_medhub_ctx_for failed, can't alloc session ctx for %s\n", uuid);
        goto unlock;
    }

    ctx->started = 0;
    ctx->stopped = 0;
    ctx->starting = 0;
    ctx->session = session;
    ctx->medhub_url = switch_core_session_strdup(session, url);
    switch_mutex_init(&ctx->mutex, SWITCH_MUTEX_NESTED, switch_core_session_get_pool(session));

    switch_channel_set_private(switch_core_session_get_channel(session), "_medhub_ctx", ctx);

    // increment medhub concurrent count
    switch_atomic_inc(&medhub_globals->medhub_concurrent_cnt);

    unlock:
    // add rwunlock for BUG: un-released channel, ref: https://blog.csdn.net/xxm524/article/details/125821116
    //  We meet : ... Locked, Waiting on external entities
    switch_core_session_rwunlock(session);
    return ctx;
}

static void connect_medhub(medhub_context_t *ctx) {
    if (!ctx) {
        return;
    }
    if (SWITCH_STATUS_SUCCESS == switch_core_session_read_lock(ctx->session)) {
        switch_mutex_lock(ctx->mutex);
        if (ctx->started == 0) {
            if (ctx->starting == 0) {
                ctx->starting = 1;
                if (medhub_globals->_debug) {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Starting Connecting to Media Hub %s \n", ctx->medhub_url);
                }
                switch_channel_t *channel = switch_core_session_get_channel(ctx->session);
                medhub_client *client = generateMediaHubClient(ctx);
                ctx->client = client;
                if (medhub_globals->_debug) {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Init Media Hub Client.%s\n",
                                      switch_channel_get_name(channel));
                }

                if (ctx->client->connect(std::string(ctx->medhub_url)) < 0) {
                    ctx->stopped = 1;
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
                                      "start() failed. may be can not connect media hub server(%s). please check network or firewalld:%s\n",
                                      ctx->medhub_url, switch_channel_get_name(channel));
                    ctx->client->stop();
                    delete ctx->client;
                    ctx->client = nullptr;
                    // start()失败，释放request对象
                }

                if (switch_channel_add_state_handler(channel, &session_medhub_handlers) < 0) {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "connect_medhub: session [%s] hook channel state change failed!\n",
                                      switch_channel_get_name(channel));
                } else {
                    if (medhub_globals->_debug) {
                        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "connect_medhub: session [%s] hook channel state change success!\n",
                                          switch_channel_get_name(channel));
                    }
                }
            }
        }
        switch_mutex_unlock(ctx->mutex);
        switch_core_session_rwunlock(ctx->session);
    }
}

// uuid_connect_medhub <uuid> url=<uri>
SWITCH_STANDARD_API(uuid_connect_medhub_function) {
    if (zstr(cmd)) {
        stream->write_function(stream, "uuid_connect_medhub: parameter missing.\n");
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "uuid_connect_medhub: parameter missing.\n");
        return SWITCH_STATUS_SUCCESS;
    }

    switch_status_t status = SWITCH_STATUS_SUCCESS;
    const char *_hub_url = nullptr;

    switch_memory_pool_t *pool;
    switch_core_new_memory_pool(&pool);
    char *my_cmd = switch_core_strdup(pool, cmd);

    char *argv[MAX_API_ARGC];
    memset(argv, 0, sizeof(char *) * MAX_API_ARGC);

    int argc = switch_split(my_cmd, ' ', argv);
    if (medhub_globals->_debug) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "uuid_connect_medhub: cmd[%s], argc[%d]\n", my_cmd,
                          argc);
    }

    if (argc < 1) {
        stream->write_function(stream, "uuid is required.\n");
        switch_goto_status(SWITCH_STATUS_SUCCESS, end);
    }

    for (int idx = 1; idx < MAX_API_ARGC; idx++) {
        if (argv[idx]) {
            char *ss[2] = {nullptr, nullptr};
            int cnt = switch_split(argv[idx], '=', ss);
            if (cnt == 2) {
                char *var = ss[0];
                char *val = ss[1];
                if (medhub_globals->_debug) {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
                                      "uuid_connect_medhub: process arg[%s = %s]\n", var, val);
                }
                if (!strcasecmp(var, "url")) {
                    _hub_url = val;
                    continue;
                }
            }
        }
    }

    if (!_hub_url) {
        stream->write_function(stream, "url is required.\n");
        switch_goto_status(SWITCH_STATUS_SUCCESS, end);
    }

    connect_medhub(init_medhub_ctx_for(_hub_url, argv[0]));
end:
    switch_core_destroy_memory_pool(&pool);
    return status;
}

static void fire_report_ai_speak(const char *uuid,
                                 const char *content_id,
                                 const char *ccs_call_id,
                                 const char *record_start_timestamp,
                                 const char *playback_start_timestamp,
                                 const char *playback_stop_timestamp,
                                 const char *playback_ms) {
    switch_event_t *event = nullptr;
    if (switch_event_create(&event, SWITCH_EVENT_CUSTOM) == SWITCH_STATUS_SUCCESS) {
        switch_event_set_subclass_name(event, "znc_report_ai_speak");

        switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Unique-ID", uuid);
        switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "ccs_call_id", ccs_call_id);
        switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "content_id", content_id);
        if (record_start_timestamp) {
            switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Record-Start-Timestamp", record_start_timestamp);
        }
        if (playback_start_timestamp) {
            switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Ai-Start-Timestamp", playback_start_timestamp);
        }

        switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Ai-Stop-Timestamp", playback_stop_timestamp);
        switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Ai-Play-Time-Ms", playback_ms);
        switch_event_fire(&event);
    }
}

static void stop_current_playing_for(switch_core_session_t *session) {
    // ref: https://github.com/signalwire/freeswitch/blob/98f164d2bff57c70aa84d71d5ead921ebbd33e22/src/switch_ivr_play_say.c#L1675
    // switch_channel_set_flag_value(switch_core_session_get_channel(session), CF_BREAK, 2);
    switch_file_handle_t *fhp = nullptr;
    if (switch_ivr_get_file_handle(session, &fhp) == SWITCH_STATUS_SUCCESS) {
        switch_set_flag_locked(fhp, SWITCH_FILE_DONE);
        switch_ivr_release_file_handle(session, &fhp);
    }
}

static void pause_current_playing_for(switch_core_session_t *session) {
    // https://github.com/signalwire/freeswitch/blob/98f164d2bff57c70aa84d71d5ead921ebbd33e22/src/switch_ivr.c#L4106
    switch_file_handle_t *fhp = nullptr;
    if (switch_ivr_get_file_handle(session, &fhp) == SWITCH_STATUS_SUCCESS) {
        if (!switch_test_flag(fhp, SWITCH_FILE_PAUSE)) {
            switch_set_flag_locked(fhp, SWITCH_FILE_PAUSE);
            switch_core_file_command(fhp, SCFC_PAUSE_READ);
        }
        switch_channel_t *channel = switch_core_session_get_channel(session);
        const char *str_ccs_call_id = switch_channel_get_variable(channel, "ccs_call_id");
        const char *str_record_start_timestamp = switch_channel_get_variable(channel, "record_start_timestamp");
        const char *str_playback_start_timestamp = switch_channel_get_variable(channel, "playback_start_timestamp");
        const char *str_current_ai_content_id = switch_channel_get_variable(channel, "current_ai_content_id");
        const char *str_playback_ms = nullptr;

        if (fhp->native_rate >= 1000) {
            uint32_t last_playback_ms = 0;
            const char *str_last_playback_ms = switch_channel_get_variable(channel, "last_playback_ms");
            if (!str_last_playback_ms) {
                last_playback_ms = fhp->samples_in / (fhp->native_rate / 1000);
                switch_channel_set_variable_printf(channel, "last_playback_ms", "%d", last_playback_ms);
                str_playback_ms = switch_channel_get_variable(channel, "last_playback_ms");
            }
            else {
                last_playback_ms = fhp->samples_in / (fhp->native_rate / 1000);
                str_playback_ms = switch_core_session_sprintf(session, "%d", last_playback_ms - switch_safe_atol(str_last_playback_ms, 0));
                switch_channel_set_variable_printf(channel, "last_playback_ms", "%d", last_playback_ms);
            }
        }
        switch_ivr_release_file_handle(session, &fhp);
        fire_report_ai_speak(switch_core_session_get_uuid(session),
                             str_current_ai_content_id,
                             str_ccs_call_id,
                             str_record_start_timestamp,
                             str_playback_start_timestamp,
                             switch_core_session_sprintf(session, "%ld", switch_micro_time_now()),
                             str_playback_ms);

    }
}

static void resume_current_playing_for(switch_core_session_t *session) {
    // https://github.com/signalwire/freeswitch/blob/98f164d2bff57c70aa84d71d5ead921ebbd33e22/src/switch_ivr.c#L4106
    switch_file_handle_t *fhp = nullptr;
    if (switch_ivr_get_file_handle(session, &fhp) == SWITCH_STATUS_SUCCESS) {
        if (switch_test_flag(fhp, SWITCH_FILE_PAUSE)) {
            switch_clear_flag_locked(fhp, SWITCH_FILE_PAUSE);
            switch_core_file_command(fhp, SCFC_PAUSE_READ);
        }
        switch_ivr_release_file_handle(session, &fhp);

        switch_channel_t *channel = switch_core_session_get_channel(session);
        switch_channel_set_variable_printf(channel, "playback_start_timestamp", "%ld", switch_micro_time_now());
    }
}

// hub_uuid_play <uuid> file=<filename> cancel_on_speak=[1|0] pause_on_speak=[1|0] content_id=<number>
SWITCH_STANDARD_API(hub_uuid_play_function) {
    if (zstr(cmd)) {
        stream->write_function(stream, "hub_uuid_play: parameter missing.\n");
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "hub_uuid_play: parameter missing.\n");
        return SWITCH_STATUS_SUCCESS;
    }

    switch_status_t status = SWITCH_STATUS_SUCCESS;
    switch_core_session_t *session4play = nullptr;
    char *_file = nullptr, *_cancel_on_speak = nullptr, *_pause_on_speak = nullptr, *_content_id = nullptr;

    switch_memory_pool_t *pool;
    switch_core_new_memory_pool(&pool);
    char *my_cmd = switch_core_strdup(pool, cmd);

    char *argv[MAX_API_ARGC];
    memset(argv, 0, sizeof(char *) * MAX_API_ARGC);

    int argc = switch_split(my_cmd, ' ', argv);
    if (medhub_globals->_debug) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "hub_uuid_play: cmd[%s], argc[%d]\n", my_cmd, argc);
    }

    if (argc < 1) {
        stream->write_function(stream, "uuid is required.\n");
        switch_goto_status(SWITCH_STATUS_SUCCESS, end);
    }

    for (int idx = 1; idx < MAX_API_ARGC; idx++) {
        if (argv[idx]) {
            char *ss[2] = {nullptr, nullptr};
            int cnt = switch_split(argv[idx], '=', ss);
            if (cnt == 2) {
                char *var = ss[0];
                char *val = ss[1];
                if (medhub_globals->_debug) {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "hub_uuid_play: process arg[%s = %s]\n", var, val);
                }
                if (!strcasecmp(var, "file")) {
                    _file = val;
                    continue;
                }
                if (!strcasecmp(var, "cancel_on_speak")) {
                    _cancel_on_speak = val;
                    continue;
                }
                if (!strcasecmp(var, "pause_on_speak")) {
                    _pause_on_speak = val;
                    continue;
                }
                if (!strcasecmp(var, "content_id")) {
                    _content_id = val;
                    continue;
                }
            }
        }
    }

    if (!_file) {
        stream->write_function(stream, "file is required.\n");
        switch_goto_status(SWITCH_STATUS_SUCCESS, end);
    }

    session4play = switch_core_session_force_locate(argv[0]);
    if (!session4play) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "hub_uuid_play failed, can't found session by %s\n",
                          argv[0]);
    } else {
        switch_channel_t *channel = switch_core_session_get_channel(session4play);
        if (medhub_globals->_debug) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "hub_uuid_play:%s\n", switch_channel_get_name(channel));
        }

        auto *ctx = (medhub_context_t *)switch_channel_get_private(channel, "_medhub_ctx");
        if (!ctx) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "hub_uuid_play failed, can't found medhub ctx by %s\n",
                              argv[0]);
        }
        else {
            switch_ivr_broadcast(switch_channel_get_uuid(channel), _file, (SMF_NONE | SMF_ECHO_ALEG | SMF_ECHO_BLEG));
        }

        // add rwunlock for BUG: un-released channel, ref: https://blog.csdn.net/xxm524/article/details/125821116
        //  We meet : ... Locked, Waiting on external entities
        switch_core_session_rwunlock(session4play);
    }

    end:
    switch_core_destroy_memory_pool(&pool);
    return status;
}

#if ENABLE_MEDHUB_PLAYBACK
// hub_uuid_play <uuid> file=<filename> cancel_on_speak=[1|0] pause_on_speak=[1|0] content_id=<number>
SWITCH_STANDARD_API(hub_uuid_play_function) {
    if (zstr(cmd)) {
        stream->write_function(stream, "hub_uuid_play: parameter missing.\n");
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "hub_uuid_play: parameter missing.\n");
        return SWITCH_STATUS_SUCCESS;
    }

    switch_status_t status = SWITCH_STATUS_SUCCESS;
    switch_core_session_t *session4play = nullptr;
    char *_file = nullptr, *_cancel_on_speak = nullptr, *_pause_on_speak = nullptr, *_content_id = nullptr;

    switch_memory_pool_t *pool;
    switch_core_new_memory_pool(&pool);
    char *my_cmd = switch_core_strdup(pool, cmd);

    char *argv[MAX_API_ARGC];
    memset(argv, 0, sizeof(char *) * MAX_API_ARGC);

    int argc = switch_split(my_cmd, ' ', argv);
    if (medhub_globals->_debug) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "hub_uuid_play: cmd[%s], argc[%d]\n", my_cmd, argc);
    }

    if (argc < 1) {
        stream->write_function(stream, "uuid is required.\n");
        switch_goto_status(SWITCH_STATUS_SUCCESS, end);
    }

    for (int idx = 1; idx < MAX_API_ARGC; idx++) {
        if (argv[idx]) {
            char *ss[2] = {nullptr, nullptr};
            int cnt = switch_split(argv[idx], '=', ss);
            if (cnt == 2) {
                char *var = ss[0];
                char *val = ss[1];
                if (medhub_globals->_debug) {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "hub_uuid_play: process arg[%s = %s]\n", var, val);
                }
                if (!strcasecmp(var, "file")) {
                    _file = val;
                    continue;
                }
                if (!strcasecmp(var, "cancel_on_speak")) {
                    _cancel_on_speak = val;
                    continue;
                }
                if (!strcasecmp(var, "pause_on_speak")) {
                    _pause_on_speak = val;
                    continue;
                }
                if (!strcasecmp(var, "content_id")) {
                    _content_id = val;
                    continue;
                }
            }
        }
    }

    if (!_file) {
        stream->write_function(stream, "file is required.\n");
        switch_goto_status(SWITCH_STATUS_SUCCESS, end);
    }

    session4play = switch_core_session_force_locate(argv[0]);
    if (!session4play) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "hub_uuid_play failed, can't found session by %s\n",
                          argv[0]);
    } else {
        switch_channel_t *channel = switch_core_session_get_channel(session4play);
        if (medhub_globals->_debug) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "hub_uuid_play:%s\n", switch_channel_get_name(channel));
        }

        auto *ctx = (medhub_context_t *)switch_channel_get_private(channel, "_medhub_ctx");
        if (!ctx) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "hub_uuid_play failed, can't found medhub ctx by %s\n",
                              argv[0]);
        }
        else {
            if (ctx->current_stream_id == 0) {
                ctx->current_stream_id = 0;   // clear current stream id
                ctx->client->playback(_file, 0, 0);
            }
        }

        // add rwunlock for BUG: un-released channel, ref: https://blog.csdn.net/xxm524/article/details/125821116
        //  We meet : ... Locked, Waiting on external entities
        switch_core_session_rwunlock(session4play);
    }

    end:
    switch_core_destroy_memory_pool(&pool);
    return status;
}

std::string to_utf8(uint32_t cp) {
    /*
    if using C++11 or later, you can do this:

    std::wstring_convert<std::codecvt_utf8<char32_t>, char32_t> conv;
    return conv.to_bytes( (char32_t)cp );

    Otherwise...
    */

    std::string result;

    int count;
    if (cp <= 0x007F) {
        count = 1;
    }
    else if (cp <= 0x07FF) {
        count = 2;
    }
    else if (cp <= 0xFFFF) {
        count = 3;
    }
    else if (cp <= 0x10FFFF) {
        count = 4;
    }
    else {
        return result; // or throw an exception
    }

    result.resize(count);

    if (count > 1)
    {
        for (int i = count-1; i > 0; --i)
        {
            result[i] = (char) (0x80 | (cp & 0x3F));
            cp >>= 6;
        }

        for (int i = 0; i < count; ++i)
            cp |= (1 << (7-i));
    }

    result[0] = (char) cp;

    return result;
}

void ues_to_utf8(std::string &ues) {
    std::string::size_type startIdx = 0;
    do {
        startIdx = ues.find("\\u", startIdx);
        if (startIdx == std::string::npos) break;

        std::string::size_type endIdx = ues.find_first_not_of("0123456789abcdefABCDEF", startIdx+2);
        if (endIdx == std::string::npos) {
            endIdx = ues.length() + 1;
        }

        std::string tmpStr = ues.substr(startIdx+2, endIdx-(startIdx+2));
        std::istringstream iss(tmpStr);

        uint32_t cp;
        if (iss >> std::hex >> cp)
        {
            std::string utf8 = to_utf8(cp);
            ues.replace(startIdx, 2+tmpStr.length(), utf8);
            startIdx += utf8.length();
        }
        else {
            startIdx += 2;
        }
    }
    while (true);
}

// hub_uuid_tts <uuid> text=<text> cancel_on_speak=[1|0] pause_on_speak=[1|0] content_id=<number>
SWITCH_STANDARD_API(hub_uuid_tts_function) {
    if (zstr(cmd)) {
        stream->write_function(stream, "hub_uuid_tts: parameter missing.\n");
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "hub_uuid_tts: parameter missing.\n");
        return SWITCH_STATUS_SUCCESS;
    }

    switch_status_t status = SWITCH_STATUS_SUCCESS;
    switch_core_session_t *session4play = nullptr;
    char *_text = nullptr, *_cancel_on_speak = nullptr, *_pause_on_speak = nullptr, *_content_id = nullptr;

    switch_memory_pool_t *pool;
    switch_core_new_memory_pool(&pool);
    char *my_cmd = switch_core_strdup(pool, cmd);

    char *argv[MAX_API_ARGC];
    memset(argv, 0, sizeof(char *) * MAX_API_ARGC);

    int argc = switch_split(my_cmd, ' ', argv);
    if (medhub_globals->_debug) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "hub_uuid_play: cmd[%s], argc[%d]\n", my_cmd, argc);
    }

    if (argc < 1) {
        stream->write_function(stream, "uuid is required.\n");
        switch_goto_status(SWITCH_STATUS_SUCCESS, end);
    }

    for (int idx = 1; idx < MAX_API_ARGC; idx++) {
        if (argv[idx]) {
            char *ss[2] = {nullptr, nullptr};
            int cnt = switch_split(argv[idx], '=', ss);
            if (cnt == 2) {
                char *var = ss[0];
                char *val = ss[1];
                if (medhub_globals->_debug) {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "hub_uuid_play: process arg[%s = %s]\n", var, val);
                }
                if (!strcasecmp(var, "text")) {
                    std::string ues(val);
                    ues_to_utf8(ues);
                    _text = switch_core_strdup(pool, ues.c_str());
                    continue;
                }
                if (!strcasecmp(var, "cancel_on_speak")) {
                    _cancel_on_speak = val;
                    continue;
                }
                if (!strcasecmp(var, "pause_on_speak")) {
                    _pause_on_speak = val;
                    continue;
                }
                if (!strcasecmp(var, "content_id")) {
                    _content_id = val;
                    continue;
                }
            }
        }
    }

    if (!_text) {
        stream->write_function(stream, "text is required.\n");
        switch_goto_status(SWITCH_STATUS_SUCCESS, end);
    }

    session4play = switch_core_session_force_locate(argv[0]);
    if (!session4play) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "hub_uuid_play failed, can't found session by %s\n",
                          argv[0]);
    } else {
        switch_channel_t *channel = switch_core_session_get_channel(session4play);
        if (medhub_globals->_debug) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "hub_uuid_play:%s\n", switch_channel_get_name(channel));
        }

        medhub_context_t *ctx = (medhub_context_t *)switch_channel_get_private(channel, "_medhub_ctx");
        if (!ctx) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "hub_uuid_play failed, can't found medhub ctx by %s\n",
                              argv[0]);
        }
        else {
            ctx->client->playtts(_text);
        }

        // add rwunlock for BUG: un-released channel, ref: https://blog.csdn.net/xxm524/article/details/125821116
        //  We meet : ... Locked, Waiting on external entities
        switch_core_session_rwunlock(session4play);
    }

    end:
    switch_core_destroy_memory_pool(&pool);
    return status;
}
#endif

/**
 *  定义load函数，加载时运行
 */
SWITCH_MODULE_LOAD_FUNCTION(mod_medhub_load) {
    switch_api_interface_t *api_interface = nullptr;
    *module_interface = switch_loadable_module_create_module_interface(pool, modname);

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_medhub load starting\n");

    medhub_globals = (medhub_global_t *)switch_core_alloc(pool, sizeof(medhub_global_t));

    medhub_globals->_debug = false;

    // register global state handlers
    switch_core_add_state_handler(&medhub_cs_handlers);

    SWITCH_ADD_API(api_interface,
                   "uuid_connect_medhub",
                   "uuid_connect_medhub api",
                   uuid_connect_medhub_function,
                   "<cmd><args>");

    SWITCH_ADD_API(api_interface,
                   "hub_uuid_play",
                   "hub_uuid_play api",
                   hub_uuid_play_function,
                   "<cmd><args>");

#if ENABLE_MEDHUB_PLAYBACK
    SWITCH_ADD_API(api_interface,
                   "hub_uuid_tts",
                   "hub_uuid_tts api",
                   hub_uuid_tts_function,
                   "<cmd><args>");
#endif

    SWITCH_ADD_API(api_interface,
                   "medhub_concurrent_cnt",
                   "medhub_concurrent_cnt api",
                   medhub_concurrent_cnt_function,
                   "<cmd><args>");

    SWITCH_ADD_API(api_interface,
                   "medhub_debug",
                   "Set medhub debug",
                   mod_medhub_debug,
                   ASRHUB_DEBUG_SYNTAX);

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_medhub loaded\n");

    return SWITCH_STATUS_SUCCESS;
}

/**
 *  定义shutdown函数，关闭时运行
 */
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_medhub_shutdown) {
    // unregister global state handlers
    switch_core_remove_state_handler(&medhub_cs_handlers);

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, " mod_medhub shutdown called\n");
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, " mod_medhub unload\n");
    return SWITCH_STATUS_SUCCESS;
}