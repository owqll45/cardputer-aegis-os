#include "app_ai.h"
#include "app_ai_setup.h"
#include <mooncake_log.h>
#include <hal/hal.h>
#include "api_config.h"
#include "config_store.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include <nvs.h>
#include <apps/utils/app_canvas.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <utility>

static const char* TAG = "DeepSeek";

static portMUX_TYPE s_job_mux = portMUX_INITIALIZER_UNLOCKED;

/* API 密钥不入库: 放在同目录的 app_ai_secrets.h(已加入 .gitignore)。
 * 仓库里只有空默认值, 克隆后需自建该文件才能调用云端接口。 */
#if defined(__has_include)
#  if __has_include("app_ai_secrets.h")
#    include "app_ai_secrets.h"
#  endif
#endif
#ifndef AI_DEEPSEEK_KEY
#  define AI_DEEPSEEK_KEY ""
#endif
#ifndef AI_MIMO_KEY
#  define AI_MIMO_KEY ""
#endif

static const char* kDeepSeekKey   = AI_DEEPSEEK_KEY;
static const char* kDeepSeekBase  = "https://api.deepseek.com";
static const char* kDeepSeekModel = "deepseek-chat";

static const char* kMimoUrl    = "https://api.xiaomimimo.com/v1/chat/completions";
static const char* kMimoVoice  = "冰糖";
static const char* kMimoKeyDefault = AI_MIMO_KEY;

/* 给配置模块用：只回答"有没有兜底 Key"，不暴露内容。 */
bool ai_setup_fallback_available()
{
    return kDeepSeekKey && kDeepSeekKey[0] != 0;
}

// 樱花配色（16 位色，8bit 画布会自动量化）
static const uint16_t C_BG      = 0x1882;
static const uint16_t C_PANEL   = 0x2905;
static const uint16_t C_PINK    = 0xFDB8;
static const uint16_t C_PINK_L  = 0xFF3C;
static const uint16_t C_DIM     = 0x8C71;

struct TtsCtx {
    std::string b64;
    // 8 字节滑动窗口，用于匹配 "data":" 这个音频字段起点
    std::string win;
    bool in_audio = false;
    // 两块 PSRAM 缓冲轮流投递到同一个虚拟声道。
    // 之前用 4 个声道并行播放，导致不同时间点的音频同时混音（听起来像好几个声音叠在一起）。
    uint8_t* pcm[2] = {nullptr, nullptr};
    size_t len[2] = {0, 0};
    int cur = 0;
    volatile bool* stop = nullptr;
};

// 12288 字节 = 6144 个 16bit 采样，24kHz 下约 256ms 一段。
// 两段缓冲排队约 0.5 秒，用来吸收网络抖动，同时不至于占用太多内部 RAM。
static const size_t kTtsBufSize = 12288;
// 固定用一个虚拟声道，避免和界面音效争抢默认声道
static const uint8_t kTtsChannel = 3;

static uint8_t* tts_buf_alloc()
{
    uint8_t* p = (uint8_t*)heap_caps_malloc(kTtsBufSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = (uint8_t*)malloc(kTtsBufSize);
    return p;
}

static void truncate_utf8(std::string& s, size_t max_bytes)
{
    if (s.size() <= max_bytes) return;
    size_t n = max_bytes;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    s.resize(n);
}

static void trim_string(std::string& s)
{
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t p = 0;
    while (p < s.size() && (s[p] == '\n' || s[p] == '\r' || s[p] == ' ' || s[p] == '\t')) p++;
    if (p) s.erase(0, p);
}

static std::string strip_markdown(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        if (c == '*' || c == '`' || c == '#' || c == '_' || c == '|') continue;
        if (c == '\n' || c == '\r') { out += ' '; continue; }
        out += c;
    }
    return out;
}

static esp_err_t http_accum_handler(esp_http_client_event_t* evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data && evt->data_len > 0) {
        auto* s = static_cast<std::string*>(evt->user_data);
        if (s->size() < 8192) s->append((const char*)evt->data, evt->data_len);
    }
    return ESP_OK;
}

static int b64_value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int b64_decode4(const char* s, uint8_t* out)
{
    int v[4];
    for (int i = 0; i < 4; i++) {
        if (s[i] == '=') { v[i] = -2; continue; }
        v[i] = b64_value(s[i]);
        if (v[i] < 0) return 0;
    }
    out[0] = (uint8_t)((v[0] << 2) | (v[1] >> 4));
    int n = 1;
    if (v[2] != -2) { out[1] = (uint8_t)(((v[1] & 15) << 4) | (v[2] >> 2)); n = 2; }
    if (v[3] != -2) { out[2] = (uint8_t)(((v[2] & 3) << 6) | v[3]); n = 3; }
    return n;
}

static size_t b64_encode3(const uint8_t* in, size_t n, char* out)
{
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    out[0] = tbl[in[0] >> 2];
    out[1] = tbl[((in[0] & 3) << 4) | (n > 1 ? (in[1] >> 4) : 0)];
    out[2] = (n > 1) ? tbl[((in[1] & 15) << 2) | (n > 2 ? (in[2] >> 6) : 0)] : '=';
    out[3] = (n > 2) ? tbl[in[2] & 63] : '=';
    return 4;
}

static void make_wav_header(uint8_t* h, uint32_t pcm_bytes, uint32_t rate)
{
    memcpy(h + 0, "RIFF", 4);
    uint32_t chunk = 36 + pcm_bytes;
    memcpy(h + 4, &chunk, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    uint32_t fmtsize = 16;
    memcpy(h + 16, &fmtsize, 4);
    uint16_t fmt = 1;
    memcpy(h + 20, &fmt, 2);
    uint16_t ch = 1;
    memcpy(h + 22, &ch, 2);
    memcpy(h + 24, &rate, 4);
    uint32_t byte_rate = rate * 2;
    memcpy(h + 28, &byte_rate, 4);
    uint16_t block = 2;
    memcpy(h + 32, &block, 2);
    uint16_t bits = 16;
    memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &pcm_bytes, 4);
}

// ---------------- TTS 播放 ----------------

static void tts_put(TtsCtx* c, const uint8_t* data, size_t n)
{
    size_t off = 0;
    while (off < n) {
        if (c->stop && *c->stop) return;
        if (!c->pcm[c->cur]) return;  // 缓冲区分配失败则不播放
        if (c->len[c->cur] == 0) {
            // 同一个声道最多排 2 段，等有空位再填，确保缓冲不会被覆盖、也不会乱序
            while (GetHAL().speaker.isPlaying(kTtsChannel) >= 2) {
                if (c->stop && *c->stop) return;
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
        size_t space = kTtsBufSize - c->len[c->cur];
        size_t take = std::min(space, n - off);
        memcpy(c->pcm[c->cur] + c->len[c->cur], data + off, take);
        c->len[c->cur] += take;
        off += take;

        if (c->len[c->cur] == kTtsBufSize) {
            GetHAL().speaker.playRaw((const int16_t*)c->pcm[c->cur], c->len[c->cur] / 2,
                                     24000, false, 1, kTtsChannel, false);
            c->len[c->cur] = 0;
            c->cur ^= 1;
        }
    }
}

static esp_err_t tts_event_handler(esp_http_client_event_t* evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA || !evt->user_data || evt->data_len <= 0) return ESP_OK;
    auto* c = static_cast<TtsCtx*>(evt->user_data);

    // 逐字节流式解析，只保留 8 字节滑动窗口，绝不把整段 base64（可能几十 KB）存进内存
    const char* s = (const char*)evt->data;
    for (int i = 0; i < evt->data_len; i++) {
        char ch = s[i];
        if (!c->in_audio) {
            c->win.push_back(ch);
            if (c->win.size() > 8) c->win.erase(0, 1);
            if (c->win.size() == 8 && c->win == "\"data\":\"") {
                c->in_audio = true;
                c->win.clear();
                c->b64.clear();
            }
            continue;
        }
        // 音频 base64 字段内
        if (ch == '"') {
            // 正常块长度都是 4 的倍数，这里做一次收尾兜底
            if (c->b64.size() >= 4) {
                uint8_t tmp[3];
                int m = b64_decode4(c->b64.c_str(), tmp);
                if (m > 0) tts_put(c, tmp, (size_t)m);
            }
            c->b64.clear();
            c->in_audio = false;
            continue;
        }
        if (ch == '\n' || ch == '\r' || ch == ' ') continue;
        c->b64.push_back(ch);
        if (c->b64.size() >= 4) {
            uint8_t tmp[3];
            int m = b64_decode4(c->b64.c_str(), tmp);
            if (m > 0) tts_put(c, tmp, (size_t)m);
            c->b64.clear();
        }
    }
    return ESP_OK;
}

// ---------------- AppAI ----------------

AppAI::AppAI()
{
    setAppInfo().name = "DeepSeek";
    setAppInfo().icon = nullptr;
}

AppAI::~AppAI()
{
    _canvas = nullptr;
}

std::string AppAI::loadMimoKey()
{
    // 固定使用内置的最新 MiMo Key。
    // 旧版本可能把作废的 Key 写进过 NVS，读 NVS 会导致语音一直失败。
    return kMimoKeyDefault;
}

void AppAI::onOpen()
{
    mclog::tagInfo(TAG, "open");

    ApiConfig::getInstance().init();
    ConfigStore::getInstance().init();
    _mimo_key = loadMimoKey();

    GetHAL().display().fillScreen(TFT_BLACK);

    _canvas = &GetHAL().canvas();
    // 铺满整个屏幕（135 高），不再让底部留出系统栏的空白；失败自动降级 109
    appCanvasBegin(_canvas, getAppInfo().name);
    _W = _canvas->width();
    _H = _canvas->height();
    _canvas_ok = true;
    _state = ST_IDLE;
    _input.clear();
    _pending_chat.clear();
    _history.clear();
    _lines.clear();
    _lines_dirty = true;
    _job = nullptr;
    _v_rec = false;
    _toast.clear();
    _blink = true;
    _dots = 0;
    _wave = 0;
    uint32_t now = GetHAL().millis();
    _last_blink = _last_dots = _last_petals = _last_wave = now;

    for (int i = 0; i < 10; i++) {
        _petals[i].x = (float)(esp_random() % (_W + 20)) - 10;
        _petals[i].y = (float)(esp_random() % _H);
        _petals[i].vy = 0.35f + (esp_random() % 60) / 100.0f;
        _petals[i].phase = (esp_random() % 628) / 100.0f;
        _petals[i].size = 1 + (esp_random() % 2);
    }

    // 自动连接已保存的 WiFi（后台任务，不卡界面）
    ConfigStore::getInstance().init();
    // 首次使用且没有保存过 Key：自动进入配置模式（开热点），
    // 但先不连 STA，避免两个网络任务互相踩。
    bool first_run = !AiKeySetup::hasKey();
    _was_online = GetHAL().isWifiConnected();
    if (!first_run && !_was_online) {
        GetHAL().wifiInit();
        std::string ssid = ConfigStore::getInstance().getWifiSsid();
        if (!ssid.empty()) {
            startWifiJob(ssid, ConfigStore::getInstance().getWifiPassword());
        }
    }

    if (first_run) {
        // 热点启动放到 onRunning，避免阻塞 onOpen
        _state = ST_KEYCFG;
        _want_setup = true;
        _setup_auto = true;
        _setup_started = false;
        mclog::tagInfo(TAG, "no saved key, enter setup mode");
    }

    _key_slot = GetHAL().keyboard.onKeyEvent.connect(
        [this](const Keyboard::KeyEvent_t& e) { handleKey(e); });

    drawUI();
}

void AppAI::onRunning()
{
    if (GetHAL().goButton().wasClicked()) {
        close();
        return;
    }

    pollJob();

    // 录音请求（在应用任务里执行，保证和绘制画布串行）
    if (_want_record && !_job && _state == ST_IDLE) {
        _want_record = false;
        startRecording();
    }

    // 进入设置后在此任务里扫描（不阻塞按键回调）
    if (_need_scan) {
        _need_scan = false;
        doScan();
    }

    // 开热点 + 配置页也放到应用任务里，避免按键回调里长时间阻塞
    if (_want_setup && !_job) {
        _want_setup = false;
        startKeyConfig(_setup_auto);
    }

    uint32_t now = GetHAL().millis();

    // 网页保存/清空 Key 后，设备端状态同步刷新
    if (_state == ST_KEYCFG && _setup_started && now - _last_setup_check > 800) {
        _last_setup_check = now;
        bool has_key = AiKeySetup::hasKey();
        if (has_key != _setup_key_seen) {
            _setup_key_seen = has_key;
            _dirty = true;
        }
    }

    // 录音后画布可能因内存碎片只能恢复成 109 高（底部会露出旧画面）；
    // 等内存稳定后每秒重试一次，恢复到整屏 135 高。
    if (_canvas && _canvas_ok && _state != ST_RECORDING &&
        _canvas->height() < appCanvasTargetH() && now - _last_canvas_retry > 1000) {
        _last_canvas_retry = now;
        // 画布丢失（录音等场景），统一走画布收口重建；失败会自动降级 109
        appCanvasBegin(_canvas, getAppInfo().name);
        _W = _canvas->width();
        _H = _canvas->height();
        _dirty = true;
        _lines_dirty = true;
    }

    // 按 V 超过 300ms 才算“按住说话”，短按仍然输入字母 v
    if (_v_pending && !_v_rec && _state == ST_IDLE && now - _v_down_ms > 300) {
        _v_pending = false;
        _v_rec = true;
        _want_record = true;
    }

    bool online = GetHAL().isWifiConnected();
    if (online != _was_online) {
        _was_online = online;
        _dirty = true;
    }

    if (now - _last_blink > 500) {
        _last_blink = now;
        _blink = !_blink;
        _dirty = true;
    }

    if (now - _last_petals > 60) {
        _last_petals = now;
        updatePetals(true);
        _dirty = true;
    }

    if (_state == ST_THINKING && now - _last_dots > 400) {
        _last_dots = now;
        _dots = (_dots + 1) % 4;
        _lines_dirty = true;
        _dirty = true;
    }

    if (_state == ST_SPEAKING && now - _last_wave > 120) {
        _last_wave = now;
        _wave++;
        _dirty = true;
    }

    if (!_toast.empty() && (int32_t)(now - _toast_until) > 0) {
        _toast.clear();
        _dirty = true;
    }

    // 扫描 WiFi 时（键盘回调里阻塞）不要并发绘制同一块画布
    if (_dirty && !_scanning) {
        _dirty = false;
        drawUI();
    }
}

void AppAI::onClose()
{
    if (_key_slot >= 0) {
        GetHAL().keyboard.onKeyEvent.disconnect(_key_slot);
        _key_slot = -1;
    }

    if (_job) {
        bool done = false;
        portENTER_CRITICAL(&s_job_mux);
        _job->abandoned = true;
        _job->stop_rec = true;
        done = _job->done;
        portEXIT_CRITICAL(&s_job_mux);
        if (done) {
            for (auto* b : _job->wav_blocks) free(b);
            delete _job;
        }
        _job = nullptr;
        vTaskDelay(pdMS_TO_TICKS(150));  // 让后台任务自行收尾（麦克风/播放）
    }

    // 配置模式退出：先关网页服务再关热点
    if (_setup_started || AiKeySetup::isActive()) {
        AiKeySetup::stop();
    }
    _setup_started = false;
    _want_setup = false;

    GetHAL().speaker.stop();

    if (_canvas) {
        appCanvasRestore(getAppInfo().name);
        _canvas = nullptr;
    }
}

void AppAI::handleKey(const Keyboard::KeyEvent_t& ke)
{
    if (ke.isModifier) return;

    if (ke.state) {
        mclog::tagInfo(TAG, "key={} '{}' state={}", (int)ke.keyCode,
                       ke.keyName ? ke.keyName : "", (int)_state);
    }

    if (_state == ST_SETTINGS) {
        handleSettingsKey(ke);
        return;
    }

    if (_state == ST_KEYCFG) {
        handleKeyConfigKey(ke);
        return;
    }

    // 此键盘没有 Esc 键：用 Tab 或 Ctrl+S 打开 WiFi 设置
    bool ctrl_down = (GetHAL().keyboard.getModifierMask() & KEY_MOD_LCTRL) != 0;
    bool is_tab    = (ke.keyCode == KEY_TAB || ke.keyCode == 9 ||
                      (ke.keyName && strcmp(ke.keyName, "tab") == 0));
    if ((is_tab || (ctrl_down && ke.keyCode == KEY_S)) && ke.state && _state == ST_IDLE) {
        openSettings();
        return;
    }

    // 松开 V：无论当前状态如何都要复位，避免第二次按 V 失效
    if (ke.keyCode == KEY_V && !ke.state) {
        if (_v_rec) {
            _v_rec = false;
            _want_record = false;
            _v_pending = false;
            stopRecording();
        } else if (_v_pending) {
            // 短按（<300ms）= 输入字母 v，不是录音
            _v_pending = false;
            if ((_state == ST_IDLE || _state == ST_SPEAKING) && _input.size() < 100) {
                _input += 'v';
                _dirty = true;
            }
        }
        return;
    }

    // 长按 V 直接说话（不用再按 Ctrl）；短按仍然是字母 v
    if (!ctrl_down && ke.keyCode == KEY_V && ke.state && _input.empty() &&
        _state == ST_IDLE && !_v_rec) {
        _v_pending = true;
        _v_down_ms = GetHAL().millis();
        return;
    }

    // 按住 Ctrl+V 说话（避免打字时误触）
    // 这里只登记请求，真正开工在 onRunning 里：释放画布必须和绘制在同一个任务，
    // 否则 deleteSprite 会撞上正在推屏的 DMA，触发 Cache/MMU fault 重启。
    if (ctrl_down && ke.keyCode == KEY_V && ke.state && _input.empty() &&
        (_state == ST_IDLE || _state == ST_RECORDING)) {
        if (!_v_rec) {
            _v_rec = true;
            _want_record = true;
        }
        return;
    }

    if (!ke.state) return;

    // 播报中允许继续打字/回车发送；思考中、录音中仍然屏蔽
    if (_state == ST_THINKING || _state == ST_RECORDING) return;

    // Ctrl+Enter：把输入的拼音转成中文（回填输入框）
    if (ctrl_down && ke.keyCode == KEY_ENTER) {
        startPinyinConvert();
        return;
    }

    if (ke.keyCode == KEY_ENTER) {
        submitText();
        return;
    }

    if (ke.keyCode == KEY_BACKSPACE) {
        if (!_input.empty()) {
            _input.pop_back();
            _dirty = true;
        }
        return;
    }

    // Ctrl+T 开关语音播报（避免输入字母 t 时误触）
    if (ctrl_down && ke.keyCode == KEY_T) {
        _tts_on = !_tts_on;
        setToast(_tts_on ? "语音播报：开" : "语音播报：关");
        return;
    }

    // 按住 Ctrl 时不输入字符
    if (ctrl_down) return;

    if (ke.keyName && strlen(ke.keyName) == 1 && _input.size() < 100) {
        _v_pending = false;  // 期间输入了别的键，松开 V 就不再补字母 v
        _input += ke.keyName;
        _dirty = true;
    }
}

void AppAI::submitText()
{
    if (_input.empty()) return;

    // 播报中也能直接发送：消息立刻显示，同时打断当前播报。
    // 真正的请求等 TTS 任务收尾后在 pollJob 里发起，避免两个 HTTPS 任务抢内存。
    if (_state == ST_SPEAKING) {
        std::string text = _input;
        _input.clear();
        appendMsg(true, text);
        _pending_chat = text;
        _state = ST_THINKING;
        _dots = 0;
        _last_dots = GetHAL().millis();
        _dirty = true;
        return;
    }

    if (_job || _state != ST_IDLE) return;
    std::string text = _input;
    _input.clear();
    appendMsg(true, text);
    startChat(text);
}

void AppAI::startChat(const std::string& text)
{
    Job* j = new Job();
    j->kind = Job::JOB_CHAT;
    j->msgs = _history;

    _job = j;
    _state = ST_THINKING;
    _dots = 0;
    _last_dots = GetHAL().millis();
    _dirty = true;

    if (xTaskCreate(&AppAI::jobEntry, "ai_chat", 12288, j, 5, nullptr) != pdPASS) {
        delete j;
        _job = nullptr;
        _state = ST_IDLE;
        appendMsg(false, "[!] 内存不足，无法发送");
    }
}

void AppAI::startPinyinConvert()
{
    if (_job || _input.empty()) return;

    Job* j = new Job();
    j->kind     = Job::JOB_CHAT;
    j->to_input = true;

    Msg m;
    m.user = true;
    m.text = "把下面这串拼音转换成通顺的简体中文，只输出转换结果，不要解释、不要加引号：\n" + _input;
    j->msgs.push_back(m);

    _job          = j;
    _state        = ST_THINKING;
    _dots         = 0;
    _last_dots    = GetHAL().millis();
    _dirty        = true;

    if (xTaskCreate(&AppAI::jobEntry, "ai_pinyin", 12288, j, 5, nullptr) != pdPASS) {
        delete j;
        _job = nullptr;
        _state = ST_IDLE;
        setToast("内存不足，无法转换");
    }
}

void AppAI::releaseCanvasForAsr()
{
    if (!_canvas || !_canvas_ok) return;
    // 录音 + HTTPS 上传需要大块内存，整屏画布约 32KB，先还给系统
    _canvas->deleteSprite();
    _canvas_ok = false;
    mclog::tagInfo(TAG, "canvas released for ASR, free heap={}", (int)esp_get_free_heap_size());
}

void AppAI::restoreCanvas()
{
    if (!_canvas || _canvas_ok) return;
    appCanvasBegin(_canvas, getAppInfo().name);
    _W = _canvas->width();
    _H = _canvas->height();
    _canvas_ok = true;
    _dirty = true;
}

void AppAI::drawRecordingScreen()
{
    // 画布已释放，直接画到屏幕（录音进度条走满会自动发送）
    auto& d = GetHAL().display();
    const int W = d.width();
    d.fillScreen(0x0000);
    d.drawRect(0, 0, W, d.height(), C_PINK);
    d.setFont(&fonts::efontCN_12);
    d.setTextSize(1);

    uint32_t el = GetHAL().millis() - _rec_start;
    uint32_t maxms = _rec_max_ms ? _rec_max_ms : 1;
    int frac = (int)((uint64_t)el * 100 / maxms);
    if (frac > 100) frac = 100;

    d.setTextColor(C_PINK_L);
    const char* t1 = (_state == ST_RECORDING) ? "正在听…" : "正在识别…";
    int tw = d.textWidth(t1);
    d.setCursor((W - tw) / 2, 42);
    d.print(t1);

    if (_state == ST_RECORDING) {
        int bx = 10, by = 64, bw = W - 20, bh = 12;
        d.drawRoundRect(bx, by, bw, bh, 4, TFT_RED);
        if (frac > 0) d.fillRect(bx + 1, by + 1, (bw - 2) * frac / 100, bh - 2, TFT_RED);
        char b[48];
        snprintf(b, sizeof(b), "%lus / %lus  松开结束",
                 (unsigned long)(el / 1000), (unsigned long)(maxms / 1000));
        d.setTextColor(TFT_WHITE);
        int bw2 = d.textWidth(b);
        d.setCursor((W - bw2) / 2, by + 20);
        d.print(b);
    } else {
        const char* t2 = "请稍候…";
        int bw = d.textWidth(t2);
        d.setCursor((W - bw) / 2, 70);
        d.setTextColor(TFT_WHITE);
        d.print(t2);
    }
}

void AppAI::startRecording()
{
    if (_job) return;

    // 先释放画布，腾出 ~32KB 给录音缓冲和 HTTPS
    releaseCanvasForAsr();

    // 16kHz/16bit 单声道，和系统里已验证可用的录音代码保持一致
    Job* j = new Job();
    j->kind = Job::JOB_ASR;
    j->rate = 16000;

    auto add_block = [&](size_t sz) -> bool {
        uint8_t* b = (uint8_t*)heap_caps_malloc(sz, MALLOC_CAP_8BIT);
        if (!b) return false;
        j->wav_blocks.push_back(b);
        j->wav_cap.push_back(sz);
        j->wav_used.push_back(0);
        j->wav_cap_total += sz;
        return true;
    };

    // 用 12800 字节的小块申请（大块容易因碎片失败），
    // 每申请一块都保证账户里至少还剩 62KB 给 HTTPS 握手（实测握手约需 34KB）。
    for (int i = 0; i < 8; i++) {
        if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < 12800 + 62000) break;
        if (!add_block(12800)) break;
    }

    if (j->wav_blocks.empty()) {
        delete j;
        restoreCanvas();
        setToast("内存不足，无法录音");
        return;
    }

    _rec_start = GetHAL().millis();
    _rec_max_ms = (uint32_t)(j->wav_cap_total / (j->rate * 2) * 1000);
    mclog::tagInfo(TAG, "ASR start: blocks={} cap={} max_ms={} free={}",
                   (int)j->wav_blocks.size(), (int)j->wav_cap_total, (int)_rec_max_ms,
                   (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    _job = j;
    _state = ST_RECORDING;
    _dirty = true;

    if (xTaskCreate(&AppAI::jobEntry, "ai_asr", 10240, j, 5, nullptr) != pdPASS) {
        for (auto* b : j->wav_blocks) free(b);
        delete j;
        _job = nullptr;
        _state = ST_IDLE;
        restoreCanvas();
        setToast("无法启动录音");
    }
}

void AppAI::stopRecording()
{
    if (_job && _job->kind == Job::JOB_ASR) {
        _job->stop_rec = true;
        _state = ST_THINKING;
        _dots = 0;
        _last_dots = GetHAL().millis();
        _dirty = true;
    }
}

void AppAI::startTts(const std::string& text)
{
    if (text.empty()) {
        _state = ST_IDLE;
        return;
    }
    Job* j = new Job();
    j->kind = Job::JOB_TTS;
    j->extra = strip_markdown(text);
    truncate_utf8(j->extra, 600);

    _job = j;
    _state = ST_SPEAKING;
    _dirty = true;

    if (xTaskCreate(&AppAI::jobEntry, "ai_tts", 12288, j, 5, nullptr) != pdPASS) {
        delete j;
        _job = nullptr;
        _state = ST_IDLE;
        setToast("内存不足，无法播报");
    }
}

void AppAI::startWifiJob(const std::string& ssid, const std::string& pass)
{
    if (_job || ssid.empty()) return;

    Job* j = new Job();
    j->kind = Job::JOB_WIFI;
    j->extra = ssid;
    j->extra2 = pass;

    _job = j;
    _wifi_connecting = true;
    _dirty = true;

    if (xTaskCreate(&AppAI::jobEntry, "ai_wifi", 6144, j, 5, nullptr) != pdPASS) {
        delete j;
        _job = nullptr;
        _wifi_connecting = false;
        setToast("内存不足，无法连接WiFi");
    }
}

void AppAI::openSettings()
{
    mclog::tagInfo(TAG, "open wifi settings");
    _state = ST_SETTINGS;
    _set_phase = 0;
    _ap_sel = 0;
    _need_scan = true;  // 扫描放到应用任务里做，避免阻塞按键回调
    _dirty = true;
}

void AppAI::doScan()
{
    _scanning = true;
    _dirty = true;
    drawUI();  // 先显示"正在扫描"

    GetHAL().wifiInit();
    std::vector<HalFacade::ScanResult_t> res;
    GetHAL().wifiScan(res);
    _scanning = false;

    _ap_list.clear();
    // 按信号强度排序，去重，最多 5 个
    std::sort(res.begin(), res.end(), [](const HalFacade::ScanResult_t& a, const HalFacade::ScanResult_t& b) {
        return a.first > b.first;
    });
    for (auto& r : res) {
        if (r.second.empty()) continue;
        bool dup = false;
        for (auto& s : _ap_list) {
            if (s == r.second) {
                dup = true;
                break;
            }
        }
        if (dup) continue;
        _ap_list.push_back(r.second);
        if (_ap_list.size() >= 4) break;  // 屏幕只放得下 4 个 + 手动输入
    }
    _dirty = true;
}

void AppAI::handleSettingsKey(const Keyboard::KeyEvent_t& ke)
{
    if (!ke.state) return;

    bool ctrl_down = (GetHAL().keyboard.getModifierMask() & KEY_MOD_LCTRL) != 0;

    // Tab 返回上一级（这台键盘没有 Esc 键，统一用 Tab）
    bool is_tab = (ke.keyCode == KEY_TAB || ke.keyCode == 9 ||
                   (ke.keyName && strcmp(ke.keyName, "tab") == 0));
    if (is_tab) {
        _state = ST_IDLE;
        _dirty = true;
        return;
    }

    // Ctrl+; / Ctrl+. 在键盘驱动里会被转成上/下，可用于列表选择
    if (ke.keyCode == KEY_UP) {
        if (_set_phase == 0 && _ap_sel > 0) {
            _ap_sel--;
            _dirty = true;
        }
        return;
    }
    if (ke.keyCode == KEY_DOWN) {
        if (_set_phase == 0 && _ap_sel + 1 < (int)_ap_list.size() + 2) {
            _ap_sel++;
            _dirty = true;
        }
        return;
    }

    // 组合键不当作字符输入
    if (ctrl_down) return;

    if (_set_phase == 0) {
        if (ke.keyCode == KEY_E || ke.keyCode == KEY_SEMICOLON) {
            if (_ap_sel > 0) _ap_sel--;
            _dirty = true;
            return;
        }
        if (ke.keyCode == KEY_S || ke.keyCode == KEY_DOT) {
            if (_ap_sel + 1 < (int)_ap_list.size() + 2) _ap_sel++;
            _dirty = true;
            return;
        }
        if (ke.keyCode == KEY_ENTER || ke.keyCode == KEY_SLASH) {
            int n = (int)_ap_list.size();
            if (_ap_sel < n) {
                _ssid_buf = _ap_list[_ap_sel];
                _pass_buf.clear();
                _set_phase = 1;
            } else if (_ap_sel == n) {
                _ssid_buf.clear();
                _pass_buf.clear();
                _set_phase = 2;
            } else {
                // 配置 API Key：开热点 + 网页配置页
                _state = ST_KEYCFG;
                _want_setup = true;
                _setup_auto = false;
                _setup_started = false;
            }
            _dirty = true;
        }
        return;
    }

    if (_set_phase == 2) {
        if (ke.keyCode == KEY_BACKSPACE) {
            if (!_ssid_buf.empty()) _ssid_buf.pop_back();
            _dirty = true;
            return;
        }
        if (ke.keyCode == KEY_ENTER) {
            if (_ssid_buf.empty()) setToast("请输入 WiFi 名称");
            else {
                _set_phase = 1;
                _dirty = true;
            }
            return;
        }
        if (ke.keyName && strlen(ke.keyName) == 1 && _ssid_buf.size() < 32) {
            _ssid_buf += ke.keyName;
            _dirty = true;
        }
        return;
    }

    // 密码
    if (ke.keyCode == KEY_BACKSPACE) {
        if (!_pass_buf.empty()) _pass_buf.pop_back();
        _dirty = true;
        return;
    }
    if (ke.keyCode == KEY_ENTER) {
        mclog::tagInfo(TAG, "wifi connect request: ssid='{}' pass_len={}", _ssid_buf.c_str(),
                       (int)_pass_buf.size());
        ConfigStore::getInstance().init();
        ConfigStore::getInstance().setWifiSsid(_ssid_buf);
        ConfigStore::getInstance().setWifiPassword(_pass_buf);
        _state = ST_IDLE;
        startWifiJob(_ssid_buf, _pass_buf);
        return;
    }
    if (ke.keyName && strlen(ke.keyName) == 1 && _pass_buf.size() < 64) {
        _pass_buf += ke.keyName;
        _dirty = true;
    }
}

void AppAI::drawSettings()
{
    int W = _W;
    _canvas->setTextColor(C_PINK_L);
    _canvas->setCursor(6, 2);
    _canvas->print("WiFi 设置");

    _canvas->setTextColor(C_DIM);
    _canvas->setCursor(W - 48, 2);
    _canvas->print("TAB返回");

    _canvas->drawFastHLine(0, 17, W, C_PINK);

    if (_set_phase == 0) {
        if (_ap_list.empty()) {
            _canvas->setTextColor(TFT_WHITE);
            _canvas->setCursor(6, 24);
            _canvas->print(_scanning ? "正在扫描 WiFi..." : "未搜索到网络");
        }
        int y = _ap_list.empty() ? 40 : 20;
        for (int i = 0; i < (int)_ap_list.size(); i++) {
            bool sel = (i == _ap_sel);
            if (sel) _canvas->fillRoundRect(2, y - 1, W - 4, 14, 3, C_PINK);
            _canvas->setTextColor(sel ? 0x0000 : TFT_WHITE);
            _canvas->setCursor(6, y);
            _canvas->print(_ap_list[i].c_str());
            y += 14;
        }
        bool sel = (_ap_sel == (int)_ap_list.size());
        if (sel) _canvas->fillRoundRect(2, y - 1, W - 4, 14, 3, C_PINK);
        _canvas->setTextColor(sel ? 0x0000 : C_PINK_L);
        _canvas->setCursor(6, y);
        _canvas->print("手动输入网络");
        y += 14;

        bool sel_key = (_ap_sel == (int)_ap_list.size() + 1);
        if (sel_key) _canvas->fillRoundRect(2, y - 1, W - 4, 14, 3, C_PINK);
        _canvas->setTextColor(sel_key ? 0x0000 : C_PINK_L);
        _canvas->setCursor(6, y);
        _canvas->print("配置 API Key");
    } else if (_set_phase == 2) {
        _canvas->setTextColor(TFT_WHITE);
        _canvas->setCursor(6, 26);
        _canvas->print("WiFi 名称:");
        _canvas->drawRoundRect(2, 42, W - 4, 16, 4, C_PINK);
        _canvas->setTextColor(C_PINK_L);
        _canvas->setCursor(6, 44);
        _canvas->print(_ssid_buf.c_str());
        if (_blink) {
            int w = _canvas->textWidth(_ssid_buf.c_str());
            _canvas->fillRect(7 + w, 45, 2, 12, C_PINK_L);
        }
    } else {
        _canvas->setTextColor(TFT_WHITE);
        _canvas->setCursor(6, 24);
        _canvas->print(("网络: " + _ssid_buf).c_str());
        _canvas->drawRoundRect(2, 42, W - 4, 16, 4, C_PINK);
        _canvas->setTextColor(C_PINK_L);
        _canvas->setCursor(6, 44);
        std::string masked;
        masked.assign(_pass_buf.size(), '*');
        _canvas->print(masked.c_str());
        if (_blink) {
            int w = _canvas->textWidth(masked.c_str());
            _canvas->fillRect(7 + w, 45, 2, 12, C_PINK_L);
        }
    }

    _canvas->setFont(&fonts::efontCN_10);
    _canvas->setTextColor(C_DIM);
    const char* hint = (_set_phase == 0) ? "E/S或;/. 选择  Enter确认  Tab返回"
                                        : (1 == _set_phase ? "输入密码  Enter连接  Tab返回"
                                                           : "输入名称  Enter下一步  Tab返回");
    int hw = _canvas->textWidth(hint);
    _canvas->setCursor((W - hw) / 2, _H - 14);
    _canvas->print(hint);
    _canvas->setFont(&fonts::efontCN_12);

    if (!_toast.empty()) {
        int tw = _canvas->textWidth(_toast.c_str());
        int bx = (W - tw) / 2 - 6;
        if (bx < 2) bx = 2;
        _canvas->fillRoundRect(bx, _H - 36, tw + 12, 16, 5, C_PINK);
        _canvas->setTextColor(0x0000);
        _canvas->setCursor(bx + 6, _H - 34);
        _canvas->print(_toast.c_str());
    }
}

// ---------------- API Key 配置（热点 + 网页） ----------------

void AppAI::startKeyConfig(bool auto_enter)
{
    _setup_auto = auto_enter;
    _setup_started = false;
    _setup_ok = false;
    _setup_ssid.clear();
    _setup_pass.clear();
    _setup_url.clear();

    std::string ssid, pass, url;
    bool ok = AiKeySetup::start(ssid, pass, url);
    _setup_ssid = ssid;
    _setup_pass = pass;
    _setup_url = url;
    _setup_ok = ok;
    _setup_started = ok;
    _setup_key_seen = AiKeySetup::hasKey();
    _last_setup_check = GetHAL().millis();
    _state = ST_KEYCFG;
    _dirty = true;
    mclog::tagInfo(TAG, "key config start ok={} ssid='{}' url='{}'", ok, ssid.c_str(), url.c_str());
}

void AppAI::exitKeyConfig()
{
    if (_setup_started || AiKeySetup::isActive()) {
        AiKeySetup::stop();
    }
    _setup_started = false;
    _setup_ok = false;
    _want_setup = false;  // 防止刚进入就退出时，待启动的请求又把热点拉起来
    _state = ST_IDLE;
    _dirty = true;
    _lines_dirty = true;
    _was_online = GetHAL().isWifiConnected();

    if (AiKeySetup::hasKey()) {
        GetHAL().wifiInit();
        auto& store = ConfigStore::getInstance();
        if (!store.getWifiSsid().empty()) {
            startWifiJob(store.getWifiSsid(), store.getWifiPassword());
            setToast("Key 已保存，正在联网");
        } else {
            setToast("Key 已保存，请配置 WiFi");
        }
    } else {
        setToast("未配 Key，按 Tab 配置");
    }
    mclog::tagInfo(TAG, "key config exit, has_key={}", AiKeySetup::hasKey());
}

void AppAI::handleKeyConfigKey(const Keyboard::KeyEvent_t& ke)
{
    if (!ke.state) return;
    bool is_tab = (ke.keyCode == KEY_TAB || ke.keyCode == 9 ||
                   (ke.keyName && strcmp(ke.keyName, "tab") == 0));
    if (ke.keyCode == KEY_ENTER || is_tab) {
        exitKeyConfig();
    }
}

void AppAI::drawKeyConfig()
{
    int W = _W;
    _canvas->setTextColor(C_PINK_L);
    _canvas->setCursor(6, 2);
    _canvas->print("API Key 配置");

    _canvas->setTextColor(C_DIM);
    _canvas->setCursor(W - 48, 2);
    _canvas->print("TAB返回");

    _canvas->drawFastHLine(0, 17, W, C_PINK);

    std::string masked = AiKeySetup::maskedKey();
    _canvas->setCursor(6, 22);
    if (masked.empty()) {
        _canvas->setTextColor(TFT_ORANGE);
        _canvas->print("当前Key: 未保存");
    } else {
        _canvas->setTextColor(TFT_GREEN);
        _canvas->print(("当前Key: " + masked).c_str());
    }

    int y = 34;
    if (!_setup_started) {
        _canvas->setTextColor(C_PINK_L);
        _canvas->setCursor(6, y + 10);
        _canvas->print(_setup_ok ? "热点已启动" : "正在启动热点...");
    } else {
        _canvas->setTextColor(TFT_WHITE);
        _canvas->setCursor(6, y);
        _canvas->print(("热点: " + _setup_ssid).c_str());
        y += 14;
        _canvas->setCursor(6, y);
        _canvas->print(("密码: " + _setup_pass).c_str());
        y += 14;
        _canvas->setTextColor(C_DIM);
        _canvas->setCursor(6, y);
        _canvas->print("手机连上后浏览器打开:");
        y += 14;
        _canvas->setTextColor(C_PINK_L);
        _canvas->setCursor(6, y);
        _canvas->print(_setup_url.c_str());
    }

    _canvas->setFont(&fonts::efontCN_10);
    _canvas->setTextColor(C_DIM);
    const char* hint = (_setup_started || _setup_ok) ? "网页里填写并保存  Enter完成  Tab返回"
                                                    : "热点启动失败  Enter/Tab 返回";
    int hw = _canvas->textWidth(hint);
    _canvas->setCursor((W - hw) / 2, _H - 14);
    _canvas->print(hint);
    _canvas->setFont(&fonts::efontCN_12);

    if (!_toast.empty()) {
        int tw = _canvas->textWidth(_toast.c_str());
        int bx = (W - tw) / 2 - 6;
        if (bx < 2) bx = 2;
        _canvas->fillRoundRect(bx, _H - 36, tw + 12, 16, 5, C_PINK);
        _canvas->setTextColor(0x0000);
        _canvas->setCursor(bx + 6, _H - 34);
        _canvas->print(_toast.c_str());
    }
}

void AppAI::pollJob()
{
    // 播报中用户按了发送：先掐掉声音并通知 TTS 任务尽快收尾，
    // 真正的对话请求等 TTS 结束、内存释放后再发，避免两个 HTTPS 抢内存
    if (_job && _job->kind == Job::JOB_TTS && !_pending_chat.empty() && !_job->stop_rec) {
        _job->stop_rec = true;
        GetHAL().speaker.stop();
    }

    if (!_job) {
        // 播报恰好结束时用户才按下发送：这里补发，避免一直卡在“思考中”
        if (!_pending_chat.empty()) {
            std::string t = _pending_chat;
            _pending_chat.clear();
            startChat(t);
        }
        return;
    }

    bool done = false;
    bool ok = false;
    std::string text;

    portENTER_CRITICAL(&s_job_mux);
    done = _job->done;
    if (done) {
        text.swap(_job->result);
        ok = _job->ok;
    }
    portEXIT_CRITICAL(&s_job_mux);

    if (!done) return;

    Job::Kind kind = _job->kind;
    bool to_input  = _job->to_input;
    for (auto* b : _job->wav_blocks) free(b);
    _job->wav_blocks.clear();
    delete _job;
    _job = nullptr;

    if (kind == Job::JOB_WIFI) {
        _wifi_connecting = false;
        _was_online = GetHAL().isWifiConnected();
        if (!text.empty()) setToast(text);
        _dirty = true;
        _lines_dirty = true;
        return;
    }

    if (kind == Job::JOB_TTS) {
        _state = ST_IDLE;
        // 播报被打断后有等待发送的消息：马上开始新一轮对话
        if (!_pending_chat.empty()) {
            std::string t = _pending_chat;
            _pending_chat.clear();
            startChat(t);
            return;
        }
        if (!ok && !text.empty()) setToast(text);
        _dirty = true;
        return;
    }

    if (kind == Job::JOB_ASR) {
        // 录音期间释放的画布在这里恢复
        restoreCanvas();
        if (!ok || text.empty()) {
            _state = ST_IDLE;
            appendMsg(false, std::string("[!] ") + (text.empty() ? "没有识别到语音内容" : text));
            _dirty = true;
            return;
        }
        appendMsg(true, text);
        startChat(text);
        return;
    }

    // JOB_CHAT
    if (to_input) {
        // 拼音转中文：把结果回填到输入框，用户可再修改后按 Enter 发送
        _state = ST_IDLE;
        if (!ok || text.empty()) {
            setToast(text.empty() ? "拼音转换失败，请重试" : text);
        } else {
            truncate_utf8(text, 96);
            _input = text;
            setToast("已转成中文，Enter 发送");
        }
        _dirty = true;
        return;
    }

    if (!ok) {
        _state = ST_IDLE;
        appendMsg(false, "[!] " + text);
        // Key 无效/未配置/网络问题：给出可读指引，而不是停在思考中
        bool key_hint = text.find("HTTP 401") != std::string::npos ||
                        text.find("HTTP 403") != std::string::npos ||
                        text.find("API Key") != std::string::npos ||
                        text.find("Key") != std::string::npos;
        if (key_hint) {
            appendMsg(false, "[提示] 按 Tab 打开设置，选择「配置 API Key」重新填写。");
        }
        _dirty = true;
        return;
    }

    appendMsg(false, text);
    if (_tts_on && !_mimo_key.empty()) {
        startTts(text);
    } else {
        _state = ST_IDLE;
        _dirty = true;
    }
}

void AppAI::appendMsg(bool user, const std::string& text)
{
    Msg m;
    m.user = user;
    m.text = text;
    _history.push_back(m);
    if (_history.size() > 8) _history.erase(_history.begin());
    _lines_dirty = true;
}

void AppAI::setToast(const std::string& text)
{
    _toast = text;
    _toast_until = GetHAL().millis() + 1800;
    _dirty = true;
}

void AppAI::jobEntry(void* arg)
{
    Job* j = static_cast<Job*>(arg);
    std::string out;
    bool ok = false;

    if (j->kind == Job::JOB_CHAT) {
        ok = doChat(j->msgs, out);
    } else if (j->kind == Job::JOB_ASR) {
        size_t total = 0;
        size_t bi = 0;      // 当前写到第几块
        size_t used = 0;    // 当前块已写字节
        GetHAL().speaker.stop();
        // 喇叭和麦克风共用 ES8311 codec，且 BCK/WS 是同一组引脚(41/43)。
        // 只 stop() 不释放 I2S，麦克风会抢不到引脚（日志里 GPIO 41/43 not usable），
        // 录到的就全是噪音，ASR 自然识别不出内容。
        GetHAL().speaker.end();
        vTaskDelay(pdMS_TO_TICKS(20));

        // 诊断：依次试 右槽 / 左槽 / PDM 三种录音方式。
        // Cardputer ADV 的麦克风可能仍然是 GPIO46 上的 PDM 数字麦克风，
        // M5Unified 的 ADV 配置却按 ES8311 的 I2S 录音走，那样就一直是全零。
        auto capture = [&](int mode, size_t want_bytes) -> int {
            const char* name = (mode == 1) ? "left" : (mode == 2 ? "pdm" : "right");
            GetHAL().mic.end();
            {
                auto mic_cfg = M5.Mic.config();
                mic_cfg.sample_rate = j->rate;
                if (mode == 2) {
                    mic_cfg.pin_bck = -1;  // -1 = PDM 模式
                } else {
                    mic_cfg.pin_bck = GPIO_NUM_41;
                    mic_cfg.input_channel = (mode == 1) ? m5::input_only_left : m5::input_only_right;
                }
                M5.Mic.config(mic_cfg);
            }
            GetHAL().mic.begin();
            vTaskDelay(pdMS_TO_TICKS(200));
            if (mode != 2) {
                static const uint8_t seq[][2] = {
                    {0x01, 0xBA},  // CLOCK_MANAGER / MCLK=BCLK
                    {0x02, 0x18},  // CLOCK_MANAGER / MULT_PRE=3
                    {0x0D, 0x01},  // 模拟电路上电
                    {0x0E, 0x02},  // 使能 PGA + ADC 调制器
                    {0x14, 0x10},  // MIC1P-MIC1N
                    {0x17, 0xBF},  // ADC 数字音量 ±0dB
                    {0x1C, 0x6A},  // ADC EQ 旁路
                    {0x16, 0x01},  // MIC PGA +6dB（0=0dB 1=6 2=12 3=18 4=24）
                };
                for (auto& r : seq) {
                    uint8_t v = r[1];
                    M5.In_I2C.writeRegister(0x18, r[0], &v, 1, 100000);
                }
                vTaskDelay(pdMS_TO_TICKS(100));
            }

            size_t t = 0, b = 0, u = 0;
            while (t < want_bytes && b < j->wav_blocks.size()) {
                if (j->wav_cap[b] - u < 3200) { j->wav_used[b] = u; b++; u = 0; continue; }
                if (!GetHAL().mic.record((int16_t*)(j->wav_blocks[b] + u), 1600, j->rate)) break;
                u += 3200;
                t += 3200;
            }
            if (b < j->wav_blocks.size()) j->wav_used[b] = u;
            // record() 是异步的（只把请求丢给 mic_task 就返回），
            // 必须等它真正写完才能统计电平，否则读到的是旧内存里的残留数据。
            while (GetHAL().mic.isRecording()) vTaskDelay(pdMS_TO_TICKS(5));
            int32_t pk = 0;
            for (size_t k = 0; k < j->wav_blocks.size(); k++) {
                size_t bytes = (k < b) ? j->wav_used[k] : (k == b ? u : 0);
                const int16_t* p = (const int16_t*)j->wav_blocks[k];
                for (size_t i = 0; i < bytes / 2; i++) {
                    int32_t a = p[i] < 0 ? -p[i] : p[i];
                    if (a > pk) pk = a;
                }
            }
            bi = b; used = u; total = t;
            mclog::tagInfo(TAG, "capture {}: bytes={} peak={}", name, (int)t, (int)pk);
            return (int)pk;
        };

        // 依次尝试：右槽 → 左槽 → PDM，哪个有电平就用哪个
        if (capture(0, 32000) < 200) {
            if (capture(1, 32000) < 200) {
                capture(2, 32000);
            }
        }
        GetHAL().mic.end();
        // 记录完必须把喇叭重新开起来（重新打开 ES8311 的 DAC 通路），
        // 否则录音之后再播报会变成电流声或直接没声音。
        GetHAL().speaker.begin();
        j->wav_total = total;

        // 诊断：确认 I2S 出来的是连续单声道采样还是 L/R 交错（奇数位置是否恒为 0），
        // 以及统计录音电平（rms 接近 0 = 没录到）
        {
            int32_t peak = 0;
            int64_t sumsq = 0;
            size_t n = 0;
            int64_t sq_even = 0, sq_odd = 0;
            size_t n_even = 0, n_odd = 0;
            for (size_t k = 0; k < j->wav_blocks.size(); k++) {
                size_t bytes = (k < bi) ? j->wav_used[k] : (k == bi ? used : 0);
                const int16_t* p = (const int16_t*)j->wav_blocks[k];
                int32_t bpeak = 0;
                int64_t bsumsq = 0;
                for (size_t i = 0; i < bytes / 2; i++) {
                    int32_t v = p[i];
                    int32_t a = v < 0 ? -v : v;
                    if (a > peak) peak = a;
                    if (a > bpeak) bpeak = a;
                    sumsq += (int64_t)v * v;
                    bsumsq += (int64_t)v * v;
                    n++;
                    if (i & 1) { sq_odd += (int64_t)v * v; n_odd++; }
                    else       { sq_even += (int64_t)v * v; n_even++; }
                }
                // 每块约 0.4 秒，用来判断电平是否随说话起伏（起伏=确实录到了人声）
                int brms = (bytes / 2) ? (int)sqrt((double)bsumsq / (double)(bytes / 2)) : 0;
                mclog::tagInfo(TAG, "ASR block {}: bytes={} rms={} peak={}", (int)k, (int)bytes, brms, (int)bpeak);
                // 再按 0.1 秒（3200 字节）细分打印，用来看 ADC 是从第几秒才真正开始出数据
                for (size_t off = 0; off + 3200 <= bytes; off += 3200) {
                    int32_t sp = 0;
                    int64_t ss = 0;
                    for (size_t i = 0; i < 1600; i++) {
                        int32_t v = p[off / 2 + i];
                        int32_t a = v < 0 ? -v : v;
                        if (a > sp) sp = a;
                        ss += (int64_t)v * v;
                    }
                    mclog::tagInfo(TAG, "  seg {}.{}s: rms={} peak={} first={}", (int)k,
                                   (int)(off / 3200) + 1, (int)sqrt((double)ss / 1600.0), (int)sp,
                                   (int)p[off / 2]);
                }
            }
            int rms = n ? (int)sqrt((double)sumsq / (double)n) : 0;
            int rms_even = n_even ? (int)sqrt((double)sq_even / (double)n_even) : 0;
            int rms_odd = n_odd ? (int)sqrt((double)sq_odd / (double)n_odd) : 0;
            const int16_t* q = (const int16_t*)j->wav_blocks[0];
            mclog::tagInfo(TAG, "ASR mic raw: peak={} rms={} rms_even={} rms_odd={} samples={} head={},{},{},{},{},{},{}",
                           (int)peak, rms, rms_even, rms_odd, (int)n,
                           (int)q[0], (int)q[1], (int)q[2], (int)q[3], (int)q[4], (int)q[5], (int)q[6]);

            // 录音电平实在太低时用软件增益兜底，否则服务端只会当成没人说话
            int sg = 1;
            if (rms < 150) sg = 16;
            else if (rms < 400) sg = 8;
            else if (rms < 1000) sg = 4;
            else if (rms < 2500) sg = 2;
            if (sg > 1) {
                for (size_t k = 0; k < j->wav_blocks.size(); k++) {
                    size_t bytes = (k < bi) ? j->wav_used[k] : (k == bi ? used : 0);
                    int16_t* p = (int16_t*)j->wav_blocks[k];
                    for (size_t i = 0; i < bytes / 2; i++) {
                        int32_t v = (int32_t)p[i] * sg;
                        if (v > 32767) v = 32767;
                        else if (v < -32768) v = -32768;
                        p[i] = (int16_t)v;
                    }
                }
            }
            mclog::tagInfo(TAG, "ASR mic level: gain=x{}", sg);
        }

        // 临时诊断：把刚录到（并做完增益）的 PCM 原样播一遍，
        // 用来判断麦克风到底录到了什么：正常人声 / 很轻 / 变调 / 全是噪音 / 无声。
        if (total >= 3200) {
            GetHAL().speaker.setVolume(200);
            mclog::tagInfo(TAG, "ASR playback start: pcm={} rate={}", (int)total, (int)j->rate);
            for (size_t k = 0; k < j->wav_blocks.size(); k++) {
                size_t bytes = (k < bi) ? j->wav_used[k] : (k == bi ? used : 0);
                if (!bytes) continue;
                GetHAL().speaker.playRaw((const int16_t*)j->wav_blocks[k], bytes / 2,
                                         j->rate, false, 1, 0, true);
                while (GetHAL().speaker.isPlaying(0)) vTaskDelay(pdMS_TO_TICKS(10));
            }
            mclog::tagInfo(TAG, "ASR playback done");
        }

        if (total < 3200) {
            out = "录音太短，请按住 V 说话";
        } else if (!j->abandoned) {
            ok = doAsr(j->wav_blocks, j->wav_used, total, j->rate, out);
        }
    } else if (j->kind == Job::JOB_TTS) {
        std::string terr;
        // stop_rec 同时承担"打断播报"的停止信号（onClose 时则由 abandoned 处理）
        ok = doTts(j->extra, &j->stop_rec, terr);
        if (!ok) out = terr;
    } else if (j->kind == Job::JOB_WIFI) {
        // 必须先初始化 WiFi；wifiConnect 内部直接用 esp_wifi_set_config
        GetHAL().wifiInit();
        ok = GetHAL().wifiConnect(j->extra, j->extra2);
        out = ok ? "WiFi 已连接" : "WiFi 连接失败，请检查密码或信号";
    }

    portENTER_CRITICAL(&s_job_mux);
    j->result.swap(out);
    j->ok = ok;
    j->done = true;
    bool abandoned = j->abandoned;
    portEXIT_CRITICAL(&s_job_mux);

    if (abandoned) {
        for (auto* b : j->wav_blocks) free(b);
        delete j;
    }
    vTaskDelete(nullptr);
}

// ---------------- 网络请求 ----------------

static bool ensure_wifi(std::string& err)
{
    if (GetHAL().isWifiConnected()) return true;

    GetHAL().wifiInit();
    if (GetHAL().isWifiConnected()) return true;

    auto& store = ConfigStore::getInstance();
    std::string ssid = store.getWifiSsid();
    std::string pwd = store.getWifiPassword();
    if (ssid.empty()) {
        err = "网络未连接：请先进入 Set WiFi 配置网络";
        return false;
    }
    if (!GetHAL().wifiConnect(ssid, pwd)) {
        err = "WiFi 连接失败，请检查网络设置";
        return false;
    }
    return true;
}

bool AppAI::doChat(const std::vector<Msg>& msgs, std::string& out)
{
    std::string err;
    if (!ensure_wifi(err)) {
        out = err;
        return false;
    }

    auto& store = ConfigStore::getInstance();
    auto& api = ApiConfig::getInstance();

    std::string key, base, model, provider = "deepseek";
    if (store.isConfigured()) {
        key = store.getApiKey();
        base = store.getApiUrl();
        provider = store.getProvider();
        model = store.getAssistantConfig().model_id;
    } else if (api.isConfigured()) {
        key = api.getApiKey();
        base = api.getBaseUrl();
        model = api.getDefaultModel();
        provider = (api.getProvider() == ApiProvider::OPENAI) ? "openai" : "deepseek";
    }
    if (key.empty()) key = kDeepSeekKey;
    if (base.empty()) base = kDeepSeekBase;
    if (model.empty()) model = kDeepSeekModel;

    // 既没有保存过 Key，也没有内置兜底：直接给出可操作的提示，不发起无效请求
    if (key.empty()) {
        out = "尚未配置 API Key：按 Tab 打开设置，选择「配置 API Key」";
        return false;
    }

    while (!base.empty() && base.back() == '/') base.pop_back();

    std::string url;
    if (base.find("/chat/completions") != std::string::npos) {
        url = base;
    } else if (base.size() >= 3 && base.compare(base.size() - 3, 3, "/v1") == 0) {
        url = base + "/chat/completions";
    } else if (provider == "deepseek") {
        url = base + "/chat/completions";
    } else {
        url = base + "/v1/chat/completions";
    }

    cJSON* root = cJSON_CreateObject();
    cJSON* arr = cJSON_CreateArray();

    cJSON* sys = cJSON_CreateObject();
    cJSON_AddStringToObject(sys, "role", "system");
    cJSON_AddStringToObject(sys, "content",
        "你是运行在 M5Stack Cardputer ADV 掌上电脑上的 AI 助手 DeepSeek。"
        "请用简体中文回答，直接给出结论，尽量控制在 120 字以内，"
        "不要使用 Markdown 表格、代码块或长列表。");
    cJSON_AddItemToArray(arr, sys);

    for (auto& m : msgs) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "role", m.user ? "user" : "assistant");
        cJSON_AddStringToObject(item, "content", m.text.c_str());
        cJSON_AddItemToArray(arr, item);
    }

    cJSON_AddStringToObject(root, "model", model.c_str());
    cJSON_AddItemToObject(root, "messages", arr);
    cJSON_AddNumberToObject(root, "max_tokens", 700);
    cJSON_AddNumberToObject(root, "temperature", 0.7);
    cJSON_AddBoolToObject(root, "stream", false);

    char* body = cJSON_PrintUnformatted(root);
    if (!body) {
        cJSON_Delete(root);
        out = "请求数据构造失败";
        return false;
    }

    std::string raw;
    raw.reserve(4096);

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 30000;
    config.buffer_size = 1024;
    config.event_handler = http_accum_handler;
    config.user_data = &raw;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(body);
        cJSON_Delete(root);
        out = "HTTP 客户端初始化失败";
        return false;
    }

    std::string auth = "Bearer " + key;
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth.c_str());
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err2 = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(body);
    cJSON_Delete(root);

    if (err2 != ESP_OK) {
        out = std::string("网络请求失败：") + esp_err_to_name(err2);
        return false;
    }

    if (status != 200) {
        std::string reason;
        cJSON* ej = cJSON_Parse(raw.c_str());
        if (ej) {
            cJSON* e = cJSON_GetObjectItem(ej, "error");
            if (e) {
                cJSON* em = cJSON_GetObjectItem(e, "message");
                if (em && em->valuestring) reason = em->valuestring;
            }
            cJSON_Delete(ej);
        }
        if (status == 401 || status == 403) {
            out = "API Key 无效或已过期，请重新配置";
            if (!reason.empty()) out += "：" + reason;
            if (out.size() > 160) out = out.substr(0, 160) + "...";
            return false;
        }
        char head[48];
        snprintf(head, sizeof(head), "请求失败 (HTTP %d)", status);
        out = head;
        if (!reason.empty()) out += "：" + reason;
        if (out.size() > 160) out = out.substr(0, 160) + "...";
        return false;
    }

    cJSON* json = cJSON_Parse(raw.c_str());
    if (!json) {
        out = "返回数据解析失败，请重试";
        return false;
    }

    std::string content;
    cJSON* choices = cJSON_GetObjectItem(json, "choices");
    if (choices && cJSON_GetArraySize(choices) > 0) {
        cJSON* message = cJSON_GetObjectItem(cJSON_GetArrayItem(choices, 0), "message");
        cJSON* c = message ? cJSON_GetObjectItem(message, "content") : nullptr;
        if (c && c->valuestring) content = c->valuestring;
    }
    cJSON_Delete(json);

    trim_string(content);
    if (content.empty()) {
        out = "AI 没有返回内容，请重试";
        return false;
    }
    if (content.size() > 1200) content = content.substr(0, 1200) + "...";

    out = content;
    return true;
}

bool AppAI::doAsr(const std::vector<uint8_t*>& blocks, const std::vector<size_t>& used,
                  size_t total, uint32_t rate, std::string& out)
{
    std::string key = loadMimoKey();
    if (key.empty()) {
        out = "未配置 MiMo Key（按 S 设置）";
        return false;
    }

    std::string err;
    if (!ensure_wifi(err)) {
        out = err;
        return false;
    }

    const std::string prefix =
        "{\"model\":\"mimo-v2.5-asr\",\"messages\":[{\"role\":\"user\",\"content\":[{"
        "\"type\":\"input_audio\",\"input_audio\":{\"data\":\"data:audio/wav;base64,";
    const std::string suffix =
        "\",\"format\":\"wav\"}}]}],\"asr_options\":{\"language\":\"zh\"}}";

    uint8_t hdr[44];
    make_wav_header(hdr, (uint32_t)total, rate);

    size_t wav_len = 44 + total;
    size_t b64len = ((wav_len + 2) / 3) * 4;
    size_t content_len = prefix.size() + b64len + suffix.size();

    esp_http_client_config_t cfg = {};
    cfg.url = kMimoUrl;
    cfg.timeout_ms = 30000;
    cfg.buffer_size = 1024;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        out = "HTTP 客户端初始化失败";
        return false;
    }

    esp_http_client_set_method(cli, HTTP_METHOD_POST);
    esp_http_client_set_header(cli, "Content-Type", "application/json");
    esp_http_client_set_header(cli, "api-key", key.c_str());

    bool ok = false;
    std::string raw;

    mclog::tagInfo(TAG, "ASR connecting: content={} heap={} largest={}", (int)content_len,
                   (int)esp_get_free_heap_size(),
                   (int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    // 注意：esp_http_client_open 返回 esp_err_t，ESP_OK(0) 才是成功。
    // 之前写成 if (esp_http_client_open(...)) 判断反了，握手成功也会被当成失败。
    esp_err_t open_err = esp_http_client_open(cli, (int)content_len);
    if (open_err == ESP_OK) {
        mclog::tagInfo(TAG, "ASR open ok, heap={}", (int)esp_get_free_heap_size());
        bool write_ok = esp_http_client_write(cli, prefix.c_str(), prefix.size()) == (int)prefix.size();

        // 把 base64 先攒到 1KB 缓冲再一次写出。
        // 之前每 4 个字符写一次，等于每个 4 字节就发一个 TLS 记录，慢到超时。
        char obuf[1024];
        size_t on = 0;
        auto flush_out = [&]() -> bool {
            if (on == 0) return true;
            bool r = esp_http_client_write(cli, obuf, (int)on) == (int)on;
            on = 0;
            return r;
        };
        auto put4 = [&](const char* e) -> bool {
            if (on + 4 > sizeof(obuf)) {
                if (!flush_out()) return false;
            }
            memcpy(obuf + on, e, 4);
            on += 4;
            return true;
        };

        // 分块数据按 3 字节对齐做 base64，跨块时保留余数，保证编码连续
        uint8_t carry[3];
        int cn = 0;
        auto feed = [&](const uint8_t* p, size_t n) {
            if (!write_ok) return;
            char enc[4];
            for (size_t i = 0; i < n; i++) {
                carry[cn++] = p[i];
                if (cn == 3) {
                    b64_encode3(carry, 3, enc);
                    if (!put4(enc)) { write_ok = false; return; }
                    cn = 0;
                }
            }
        };

        feed(hdr, sizeof(hdr));
        for (size_t i = 0; i < blocks.size() && write_ok; i++) {
            size_t n = (i < used.size()) ? used[i] : 0;
            if (n) feed(blocks[i], n);
        }
        if (write_ok && cn > 0) {
            char enc[4];
            b64_encode3(carry, (size_t)cn, enc);
            if (!put4(enc)) write_ok = false;
        }
        if (write_ok && !flush_out()) write_ok = false;

        if (write_ok) {
            write_ok = esp_http_client_write(cli, suffix.c_str(), suffix.size()) == (int)suffix.size();
        }

        if (write_ok) {
            mclog::tagInfo(TAG, "ASR upload done: pcm={} content={}", (int)total, (int)content_len);
            esp_http_client_fetch_headers(cli);
            int status = esp_http_client_get_status_code(cli);
            char buf[512];
            int n;
            while ((n = esp_http_client_read(cli, buf, sizeof(buf))) > 0) {
                if (raw.size() < 8192) raw.append(buf, n);
            }
            mclog::tagInfo(TAG, "ASR resp: status={} len={}", status, (int)raw.size());

            if (status == 200) {
                cJSON* json = cJSON_Parse(raw.c_str());
                if (json) {
                    cJSON* choices = cJSON_GetObjectItem(json, "choices");
                    if (choices && cJSON_GetArraySize(choices) > 0) {
                        cJSON* message = cJSON_GetObjectItem(cJSON_GetArrayItem(choices, 0), "message");
                        cJSON* c = message ? cJSON_GetObjectItem(message, "content") : nullptr;
                        if (c && c->valuestring) out = c->valuestring;
                        ok = !out.empty();
                    }
                    cJSON_Delete(json);
                }
                if (out.empty()) {
                    // 200 但内容为空 = 服务端认为这段音频里没有可识别的话，打印原始响应用于排查
                    mclog::tagWarn(TAG, "ASR empty content, body={}", raw.c_str());
                    out = "没有识别到语音内容";
                }
            } else {
                if (status == 402) {
                    out = "小米账户余额不足，语音识别不可用";
                } else {
                    char head[48];
                    snprintf(head, sizeof(head), "语音识别失败 (HTTP %d)", status);
                    out = head;
                }
                mclog::tagError(TAG, "ASR failed status={} body={}", status, raw.c_str());
            }
        } else {
            mclog::tagError(TAG, "ASR write failed: pcm={} content={}", (int)total, (int)content_len);
            out = "音频上传失败";
        }
    } else {
        mclog::tagError(TAG, "ASR open failed: {} heap={} largest={}", esp_err_to_name(open_err),
                        (int)esp_get_free_heap_size(),
                        (int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        out = "无法连接语音服务";
    }

    esp_http_client_cleanup(cli);
    trim_string(out);
    return ok;
}

bool AppAI::doTts(const std::string& text, volatile bool* stop, std::string& err)
{
    std::string key = loadMimoKey();
    if (key.empty()) {
        err = "未配置语音 Key";
        return false;
    }

    std::string say = text;
    truncate_utf8(say, 600);
    if (say.empty()) {
        err = "没有可播报的内容";
        return false;
    }

    // 内存不足时直接跳过播报，避免 TLS/字符串扩容时 new 失败导致重启
    if (esp_get_free_heap_size() < 60000 ||
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 24000) {
        err = "内存不足，本次跳过语音播报";
        mclog::tagError(TAG, "TTS skipped, heap={} largest={}", (int)esp_get_free_heap_size(),
                        (int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        return false;
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", "mimo-v2.5-tts");

    cJSON* arr = cJSON_CreateArray();
    cJSON* msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "assistant");
    cJSON_AddStringToObject(msg, "content", say.c_str());
    cJSON_AddItemToArray(arr, msg);
    cJSON_AddItemToObject(root, "messages", arr);

    cJSON* audio = cJSON_CreateObject();
    cJSON_AddStringToObject(audio, "format", "pcm16");
    cJSON_AddStringToObject(audio, "voice", kMimoVoice);
    cJSON_AddItemToObject(root, "audio", audio);
    cJSON_AddBoolToObject(root, "stream", true);

    char* body = cJSON_PrintUnformatted(root);
    if (!body) {
        cJSON_Delete(root);
        err = "请求数据构造失败";
        return false;
    }

    TtsCtx* ctx = new TtsCtx();
    ctx->stop = stop;
    for (int i = 0; i < 2; i++) ctx->pcm[i] = tts_buf_alloc();

    mclog::tagInfo(TAG, "TTS start: len={} heap={} largest={}", (int)say.size(),
                   (int)esp_get_free_heap_size(), (int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    esp_http_client_config_t cfg = {};
    cfg.url = kMimoUrl;
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 60000;
    cfg.buffer_size = 1024;
    cfg.event_handler = tts_event_handler;
    cfg.user_data = ctx;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (cli) {
        GetHAL().speaker.setVolume(120);
        esp_http_client_set_header(cli, "Content-Type", "application/json");
        esp_http_client_set_header(cli, "api-key", key.c_str());
        esp_http_client_set_post_field(cli, body, strlen(body));
        esp_err_t r = esp_http_client_perform(cli);
        int status = esp_http_client_get_status_code(cli);
        if (r != ESP_OK || status != 200) {
            char b[48];
            snprintf(b, sizeof(b), "语音播报失败 (HTTP %d)", status);
            err = b;
            mclog::tagError(TAG, "TTS failed: {} status={}", esp_err_to_name(r), status);
        }
        esp_http_client_cleanup(cli);
        mclog::tagInfo(TAG, "TTS done: status={} heap={} largest_internal={}", status,
                       (int)esp_get_free_heap_size(),
                       (int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    } else {
        err = "语音播报连接失败";
    }

    bool stopped = (stop && *stop);

    // 播放最后不足一包的数据
    if (!stopped && ctx->len[ctx->cur] > 0) {
        GetHAL().speaker.playRaw((const int16_t*)ctx->pcm[ctx->cur], ctx->len[ctx->cur] / 2,
                                 24000, false, 1, kTtsChannel, false);
        ctx->len[ctx->cur] = 0;
    }

    // 等这个声道播完，保证状态与声音同步（30 秒足够排空队列里的两段缓冲）
    uint32_t t0 = GetHAL().millis();
    while (!stopped && GetHAL().speaker.isPlaying(kTtsChannel) && GetHAL().millis() - t0 < 30000) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    for (int i = 0; i < 2; i++) free(ctx->pcm[i]);
    delete ctx;
    free(body);
    cJSON_Delete(root);
    return err.empty();
}

// ---------------- 绘制 ----------------

void AppAI::updatePetals(bool move)
{
    if (move) {
        for (auto& p : _petals) {
            p.y += p.vy;
            p.phase += 0.06f;
            p.x += sinf(p.phase) * 0.5f;
            if (p.y > _H + 4) {
                p.y = -4;
                p.x = (float)(esp_random() % (_W + 20)) - 10;
            }
        }
    }
}

void AppAI::drawSakura(int cx, int cy, int r)
{
    for (int i = 0; i < 5; i++) {
        float a = (float)i * 1.2566f - 1.5708f;
        int px = cx + (int)(cosf(a) * (float)r * 0.62f);
        int py = cy + (int)(sinf(a) * (float)r * 0.62f);
        _canvas->fillCircle(px, py, (int)((float)r * 0.46f), C_PINK);
    }
    _canvas->fillCircle(cx, cy, std::max(1, r / 4), 0xFE80);
}

std::vector<std::string> AppAI::wrapText(const std::string& text, int maxWidth)
{
    std::vector<std::string> lines;
    std::string line;
    size_t i = 0;

    while (i < text.size()) {
        unsigned char ch = (unsigned char)text[i];
        if (ch == '\r') { i++; continue; }
        if (ch == '\n') {
            lines.push_back(line);
            line.clear();
            i++;
            continue;
        }

        size_t n = 1;
        if ((ch & 0xE0) == 0xC0) n = 2;
        else if ((ch & 0xF0) == 0xE0) n = 3;
        else if ((ch & 0xF8) == 0xF0) n = 4;
        if (i + n > text.size()) n = 1;

        std::string cand = line + text.substr(i, n);
        if (_canvas->textWidth(cand.c_str()) > maxWidth && !line.empty()) {
            lines.push_back(line);
            line = text.substr(i, n);
        } else {
            line = cand;
        }
        i += n;
    }

    if (!line.empty()) lines.push_back(line);
    return lines;
}

void AppAI::rebuildLines()
{
    _canvas->setFont(&fonts::efontCN_12);
    _canvas->setTextSize(1);

    _lines.clear();

    if (_history.empty()) {
        _lines.push_back("樱花模式就绪");
        _lines.push_back("Enter 发送 · Ctrl+V 说话");
        _lines.push_back("Ctrl+Enter 拼音转中文");
        _lines.push_back("Tab 打开 WiFi 设置");
    }

    int maxW = _W - 10;
    for (auto& m : _history) {
        std::string prefix = m.user ? "你: " : "DeepSeek: ";
        auto wrapped = wrapText(prefix + m.text, maxW);
        for (auto& l : wrapped) _lines.push_back(l);
    }

    if (_state == ST_THINKING) {
        std::string dots(_dots, '.');
        bool asr = _job && _job->kind == Job::JOB_ASR;
        _lines.push_back(std::string(asr ? "识别中" : "思考中") + dots);
    }

    _lines_dirty = false;
}

void AppAI::drawUI()
{
    if (!_canvas_ok) {
        drawRecordingScreen();
        return;
    }

    const int W = _W;

    if (_lines_dirty) rebuildLines();

    _canvas->setFont(&fonts::efontCN_12);
    _canvas->setTextSize(1);
    _canvas->fillScreen(C_BG);

    // 飘落的樱花
    for (auto& p : _petals) {
        int w = p.size;
        _canvas->fillEllipse((int)p.x, (int)p.y, w, w + 1, C_PINK);
    }

    if (_state == ST_SETTINGS) {
        drawSettings();
        // 设置页也要推到屏幕，否则按 Tab 后只会"卡一下"看不到任何变化
        appCanvasPush(_canvas);
        if (_H < appCanvasTargetH()) {
            GetHAL().display().fillRect(0, _H, GetHAL().display().width(),
                                        GetHAL().display().height() - _H, C_BG);
        }
        return;
    }

    if (_state == ST_KEYCFG) {
        drawKeyConfig();
        appCanvasPush(_canvas);
        if (_H < appCanvasTargetH()) {
            GetHAL().display().fillRect(0, _H, GetHAL().display().width(),
                                        GetHAL().display().height() - _H, C_BG);
        }
        return;
    }

    // 顶栏
    _canvas->drawFastHLine(0, 17, W, C_PINK);
    drawSakura(10, 9, 7);

    _canvas->setTextColor(TFT_WHITE);
    _canvas->setCursor(21, 2);
    _canvas->print("DeepSeek");

    const char* st = "";
    uint16_t st_col = C_PINK_L;
    if (_state == ST_RECORDING) {
        st = "录音";
        st_col = TFT_RED;
    } else if (_state == ST_THINKING) {
        st = (_job && _job->kind == Job::JOB_ASR) ? "识别" : "思考";
        st_col = TFT_YELLOW;
    } else if (_state == ST_SPEAKING) {
        st = "";
    } else if (_wifi_connecting) {
        st = "连WiFi";
        st_col = TFT_YELLOW;
    } else if (_was_online) {
        st = "online";
        st_col = TFT_GREEN;
    } else {
        st = "offline";
        st_col = TFT_ORANGE;
    }

    if (_state == ST_SPEAKING) {
        int base = W - 8;
        for (int i = 0; i < 6; i++) {
            int h = 3 + (int)((sinf((float)(_wave + i) * 0.9f) + 1.0f) * 4.0f);
            _canvas->fillRect(base - i * 5, 9 - h / 2, 3, h, C_PINK_L);
        }
    } else if (st[0]) {
        int tw = _canvas->textWidth(st);
        _canvas->setTextColor(st_col);
        _canvas->setCursor(W - tw - 5, 2);
        _canvas->print(st);
    }

    // 对话区：无底板，文字直接浮在樱花背景上
    const int line_h = 12;
    const int max_lines = 6;
    int start = (int)_lines.size() > max_lines ? (int)_lines.size() - max_lines : 0;
    int y = 20;
    for (int i = start; i < (int)_lines.size(); i++) {
        bool ai = _lines[i].rfind("DeepSeek:", 0) == 0;
        _canvas->setTextColor(ai ? C_PINK_L : TFT_WHITE);
        _canvas->setCursor(6, y);
        _canvas->print(_lines[i].c_str());
        y += line_h;
    }

    // 输入区（只描边，不挡樱花）
    int input_y = _H - 32;

    if (_state == ST_RECORDING) {
        // 录音进度条：表盘走到头就代表缓冲写满，会自动发送这段语音
        uint32_t el = GetHAL().millis() - _rec_start;
        uint32_t maxms = _rec_max_ms ? _rec_max_ms : 1;
        int frac = (int)((uint64_t)el * 100 / maxms);
        if (frac > 100) frac = 100;
        _canvas->drawRoundRect(2, input_y, W - 4, 14, 4, TFT_RED);
        if (frac > 0) _canvas->fillRect(3, input_y + 1, (W - 6) * frac / 100, 12, TFT_RED);
        char b[64];
        snprintf(b, sizeof(b), "正在听 %lus / %lus  松开结束",
                 (unsigned long)(el / 1000), (unsigned long)(maxms / 1000));
        int tw = _canvas->textWidth(b);
        _canvas->setTextColor(TFT_WHITE);
        _canvas->setCursor((W - tw) / 2, input_y + 1);
        _canvas->print(b);
    } else {
        _canvas->drawRoundRect(2, input_y, W - 4, 14, 4, C_PINK);

        std::string shown = "> " + _input;
        if (_blink) shown += "_";

        while (_canvas->textWidth(shown.c_str()) > W - 14 && shown.size() > 2) {
            shown.erase(2, 1);
        }

        _canvas->setTextColor(C_PINK_L);
        _canvas->setCursor(6, input_y + 1);
        _canvas->print(shown.c_str());
    }

    // 底部提示
    _canvas->setFont(&fonts::efontCN_10);
    _canvas->setTextColor(C_DIM);
    const char* hint = "Enter发送 Tab设置 GO退出";
    int hw = _canvas->textWidth(hint);
    _canvas->setCursor((W - hw) / 2, _H - 14);
    _canvas->print(hint);
    _canvas->setFont(&fonts::efontCN_12);

    // Toast
    if (!_toast.empty()) {
        int tw = _canvas->textWidth(_toast.c_str());
        int bx = (W - tw) / 2 - 6;
        if (bx < 2) bx = 2;
        _canvas->fillRoundRect(bx, _H - 50, tw + 12, 16, 5, C_PINK);
        _canvas->setTextColor(0x0000);
        _canvas->setCursor(bx + 6, _H - 48);
        _canvas->print(_toast.c_str());
    }

    appCanvasPush(_canvas);
    if (_H < appCanvasTargetH()) {
        // 画布只恢复到 109 高时，把底部剩余区域刷成背景，避免露出上一屏残留
        GetHAL().display().fillRect(0, _H, GetHAL().display().width(),
                                    GetHAL().display().height() - _H, C_BG);
    }
}
