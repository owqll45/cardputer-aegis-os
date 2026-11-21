#include "app_micmeter.h"
#include <apps/utils/app_canvas.h>
#include <apps/utils/theme.h>
#include <mooncake_log.h>
#include <hal/hal.h>
#include <math.h>
#include <stdio.h>
#include <functional>
#include <esp_heap_caps.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const uint32_t kSampleRate = 16000;

// 1 = 把麦克风原始采样/寄存器打到串口（排查用），平时保持 0
#define MIC_DBG 0
static const int kNBand = 8;
static const float kBands[kNBand] = { 125, 250, 500, 1000, 2000, 3000, 4500, 6500 };

// 回放缓冲：优先 1.5s(48KB)，内存不够就退到 1s / 0.5s
static const int kRecWant[3] = { 24000, 16000, 8000 };

// ================= 按键队列（回调只入队，状态只在 app 任务里改）=================
#define MKQ_N 8
static std::function<void(const Keyboard::KeyEvent_t&)> s_key_handler;
static Keyboard::KeyEvent_t s_keyq[MKQ_N];
static volatile int s_q_head = 0;
static volatile int s_q_tail = 0;
static portMUX_TYPE s_q_mux = portMUX_INITIALIZER_UNLOCKED;

static void mkq_push(const Keyboard::KeyEvent_t& e)
{
    taskENTER_CRITICAL(&s_q_mux);
    int n = (s_q_head + 1) % MKQ_N;
    if (n != s_q_tail) {
        s_keyq[s_q_head] = e;
        s_q_head         = n;
    }
    taskEXIT_CRITICAL(&s_q_mux);
}

static bool mkq_pop(Keyboard::KeyEvent_t& e)
{
    bool ok = false;
    taskENTER_CRITICAL(&s_q_mux);
    if (s_q_tail != s_q_head) {
        e        = s_keyq[s_q_tail];
        s_q_tail = (s_q_tail + 1) % MKQ_N;
        ok       = true;
    }
    taskEXIT_CRITICAL(&s_q_mux);
    return ok;
}

// ================= ES8311 麦克风通路 =================

static void es8311_write(uint8_t reg, uint8_t v)
{
    M5.In_I2C.writeRegister(0x18, reg, &v, 1, 100000);
}

/*
 * 两个坑叠在一起，缺一不可：
 * 1) M5Unified 的 M5.begin() 为了 Capsule/Dial/DinMeter 的 power-hold，无条件把 GPIO46
 *    设成"输出高"。Cardputer ADV 的 GPIO46 恰好是 ES8311 的 ADC 数据输入，于是 I2S 读回来
 *    的采样全是 -1（数据线被自己钉死）。HalFacade::init() 里已经恢复成输入，这里兜底再改一次。
 * 2) 只写寄存器不够，ADC 需要 codec 整体断电再上电才真正开始采样；上电后补 PGA +18dB、
 *    ADC +12dB，底噪才会从 1~2 LSB 抬到几十 LSB（说话时上千）。
 */
static void mic_pin_fix()
{
    gpio_reset_pin(GPIO_NUM_46);
    gpio_set_direction(GPIO_NUM_46, GPIO_MODE_INPUT);
    gpio_pullup_dis(GPIO_NUM_46);
    gpio_pulldown_dis(GPIO_NUM_46);
}

static void mic_codec_start()
{
    es8311_write(0x00, 0x00);  // CSM power down
    vTaskDelay(pdMS_TO_TICKS(30));
    es8311_write(0x00, 0x80);  // CSM power on
    es8311_write(0x01, 0xBA);  // CLOCK: MCLK = BCLK
    es8311_write(0x02, 0x18);  // CLOCK: MULT_PRE
    es8311_write(0x0B, 0x00);
    es8311_write(0x0C, 0x00);
    es8311_write(0x0D, 0x01);  // 模拟电路上电
    es8311_write(0x0E, 0x02);  // 开模拟 PGA + ADC 调制器
    es8311_write(0x14, 0x10);  // 选 MIC1P-MIC1N 差分输入
    es8311_write(0x16, 0x04);  // MIC PGA +24dB
    es8311_write(0x17, 0xE7);  // ADC 数字音量 +20dB
    es8311_write(0x1B, 0x0A);
    es8311_write(0x1C, 0x6A);  // 旁路 EQ + 数字域去直流
    es8311_write(0x0A, 0x0C);  // ADC 串口输出：16bit I2S
    // 采音期间把喇叭 DAC 输出关掉：省电，也避免 DAC 输出串进麦克风
    es8311_write(0x13, 0x00);  // 关 DAC 输出驱动
    es8311_write(0x32, 0x00);  // DAC 音量拉到最小
    vTaskDelay(pdMS_TO_TICKS(150));
}

static void mic_regs(const char* tag)
{
#if MIC_DBG
    static const uint8_t regs[] = { 0x01, 0x02, 0x0D, 0x0E, 0x14, 0x16, 0x17, 0x1C };
    char line[120];
    int o = snprintf(line, sizeof(line), "MICREG %s", tag);
    for (size_t i = 0; i < sizeof(regs); i++) {
        uint8_t v = 0;
        M5.In_I2C.readRegister(0x18, regs[i], &v, 1, 100000);
        o += snprintf(line + o, sizeof(line) - o, " %02X=%02X", regs[i], v);
    }
    printf("%s\n", line);
#else
    (void)tag;
#endif
}

// 启动麦克风：先让 M5 建好 I2S，再对 codec 做一次完整上电
static void mic_begin_ok(m5::Mic_Class& mic)
{
    if (mic.isEnabled()) mic.end();
    mic_pin_fix();
    auto cfg          = mic.config();
    cfg.sample_rate   = kSampleRate;
    cfg.magnification = 64;  // 软件增益 64/2 = 32 倍：codec 已带 +30dB，这里只补显示/回放幅度
    mic.config(cfg);
    mic.begin();
    vTaskDelay(pdMS_TO_TICKS(50));
    mic_codec_start();
}

AppMicMeter::AppMicMeter() { setAppInfo().name = "MicMeter"; }
AppMicMeter::~AppMicMeter() {}

void AppMicMeter::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "opened");

    _canvas = &GetHAL().canvas();
    appCanvasBegin(_canvas, getAppInfo().name);

    if (!_buf) {
        _buf = (int16_t*)heap_caps_malloc(sizeof(int16_t) * NSAMP, MALLOC_CAP_8BIT);
    }
    _have_data = false;
    _mode      = M_IDLE;
    _rec_pos   = 0;
    _rec_fail  = 0;
    _frames    = 0;
    _rms_db = _peak_db = _peak_hold = -90.0f;
    for (int i = 0; i < NBAND; i++) _band[i] = 0;
    for (int i = 0; i < NHIST; i++) _hist[i] = -90.0f;
    _hist_pos = 0;

    // 喇叭和麦克风共用 ES8311 和同一组 BCK/WS(41/43)，必须先让喇叭松手
    GetHAL().speaker.end();
    vTaskDelay(pdMS_TO_TICKS(30));
    mic_begin_ok(GetHAL().mic);
    _mic_started = true;
    _mic_ok      = GetHAL().mic.isEnabled();

    s_key_handler = [](const Keyboard::KeyEvent_t& k) {
        if (k.isModifier) return;
        mkq_push(k);
    };
    _key_slot = GetHAL().keyboard.onKeyEvent.connect(s_key_handler);

    mclog::tagInfo(getAppInfo().name, "mic ready");
    printf("MICHK ok=%d heap=%u largest=%u\n", (int)_mic_ok,
           (unsigned)esp_get_free_heap_size(),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
#if MIC_DBG
    mic_regs("boot");
#endif
}

void AppMicMeter::onRunning()
{
    if (_want_close) {
        close();
        return;
    }
    if (GetHAL().goButton().wasClicked()) {
        close();
        return;
    }

    auto& mic = GetHAL().mic;
    uint32_t now = GetHAL().millis();

    // ---- 按键 ----
    Keyboard::KeyEvent_t ke;
    while (mkq_pop(ke)) {
        if (!ke.state) continue;
        if (ke.keyCode == KEY_ESC) { _want_close = true; continue; }
        char c = ke.keyName[0];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c == ' ' && _mode == M_IDLE) startRecord();
    }

    // ---- 空闲：连续采集做电平/频谱 ----
    if (_mode == M_IDLE) {
        if (_buf && !mic.isRecording()) {
            if (_have_data) analyze(_buf, NSAMP);
            _have_data = mic.record(_buf, NSAMP, kSampleRate);
            if (!_have_data) {
                _rec_fail++;
                if (_rec_fail == 20) {
                    mclog::tagWarn(getAppInfo().name, "mic record failed");
                }
            } else {
                _rec_fail = 0;
            }
        }
        // 每秒打一次真实采样电平（诊断开关，平时关掉）
#if MIC_DBG
        if (now - _log_ms > 1000) {
            _log_ms = now;
            printf("MICLVL rms=%.1f pk=%d db=%.1f band=%.0f/%.0f/%.0f/%.0f/%.0f/%.0f/%.0f/%.0f\n",
                   32768.0f * powf(10.0f, _rms_db / 20.0f), _raw_peak, _rms_db, _band[0],
                   _band[1], _band[2], _band[3], _band[4], _band[5], _band[6], _band[7]);
        }
#endif
    }
    // ---- 录音 ----
    else if (_mode == M_REC) {
        if (!mic.isRecording()) {
            if (_rec_waiting) {
                analyze(_rec_buf + _rec_pos - _rec_chunk, _rec_chunk);
                _rec_waiting = false;
            }
            if (_rec_pos >= _rec_cap) {
                finishRecord();
            } else {
                int chunk = _rec_cap - _rec_pos;
                if (chunk > NSAMP) chunk = NSAMP;
                if (mic.record(_rec_buf + _rec_pos, chunk, kSampleRate)) {
                    _rec_pos += chunk;
                    _rec_chunk   = chunk;
                    _rec_waiting = true;
                } else {
                    finishRecord();
                }
            }
        }
    }
    // ---- 回放 ----
    else if (_mode == M_PLAY) {
        if (!GetHAL().speaker.isPlaying() && now - _mode_ms > 200) {
            finishPlay();
        }
    }

    // 峰值保持 800ms
    if (_peak_db >= _peak_hold) {
        _peak_hold    = _peak_db;
        _peak_hold_ms = now;
    } else if (now - _peak_hold_ms > 800) {
        _peak_hold -= 0.6f;
        if (_peak_hold < -90) _peak_hold = -90;
    }

    draw();
}

void AppMicMeter::onClose()
{
    if (_key_slot >= 0) {
        GetHAL().keyboard.onKeyEvent.disconnect(_key_slot);
        _key_slot = -1;
    }
    s_key_handler = nullptr;
    Keyboard::KeyEvent_t dump;
    while (mkq_pop(dump)) {
    }

    auto& mic = GetHAL().mic;
    if (mic.isEnabled()) mic.end();
    _mic_started = false;
    // 恢复喇叭 DAC 通路，否则退出后整机没声音
    GetHAL().speaker.begin();

    if (_buf) {
        heap_caps_free(_buf);
        _buf = nullptr;
    }
    if (_rec_buf) {
        heap_caps_free(_rec_buf);
        _rec_buf = nullptr;
        _rec_cap = 0;
    }
    appCanvasRestore(getAppInfo().name);
    _canvas = nullptr;
}

void AppMicMeter::startRecord()
{
    if (!_rec_buf) {
        for (int i = 0; i < 3 && !_rec_buf; i++) {
            _rec_buf = (int16_t*)heap_caps_malloc(sizeof(int16_t) * kRecWant[i], MALLOC_CAP_8BIT);
            if (_rec_buf) _rec_cap = kRecWant[i];
        }
    }
    if (!_rec_buf) {
        mclog::tagWarn(getAppInfo().name, "no mem for rec");
        printf("MICREC alloc_fail\n");
        return;
    }
    _rec_pos     = 0;
    _rec_waiting = false;
    _have_data   = false;
    _mode        = M_REC;
    _mode_ms     = GetHAL().millis();
    printf("MICREC start samples=%d\n", _rec_cap);
}

void AppMicMeter::finishRecord()
{
    auto& mic = GetHAL().mic;
    while (mic.isRecording()) vTaskDelay(pdMS_TO_TICKS(5));
    mic.end();

    // 串口证据：录到的真实采样峰值/前 16 个原始值；顺便做归一化，回放才听得清
    {
        int pk = 0;
        double sum = 0;
        for (int i = 0; i < _rec_pos; i++) {
            int a = _rec_buf[i] < 0 ? -_rec_buf[i] : _rec_buf[i];
            if (a > pk) pk = a;
            sum += (double)_rec_buf[i] * _rec_buf[i];
        }
        float gain = 1.0f;
        if (pk > 0) {
            gain = 20000.0f / pk;
            if (gain > 32.0f) gain = 32.0f;
        }
#if MIC_DBG
        printf("MICREC done n=%d pk=%d rms=%.1f gain=%.1f\n", _rec_pos, pk,
               _rec_pos ? sqrtf((float)(sum / _rec_pos)) : 0.0f, gain);
        printf("MICREC samp");
        for (int i = 0; i < 16 && i < _rec_pos; i++) printf(" %d", _rec_buf[i]);
        printf("\n");
#else
        (void)sum;
#endif
        if (gain > 1.01f) {
            for (int i = 0; i < _rec_pos; i++) {
                int v = (int)(_rec_buf[i] * gain);
                if (v > 32767) v = 32767;
                if (v < -32768) v = -32768;
                _rec_buf[i] = (int16_t)v;
            }
        }
    }

    GetHAL().speaker.begin();
    vTaskDelay(pdMS_TO_TICKS(60));
    GetHAL().speaker.setVolume(200);
    bool ok = GetHAL().speaker.playRaw(_rec_buf, _rec_pos, kSampleRate, false);
#if MIC_DBG
    printf("MICPLAY samples=%d ok=%d\n", _rec_pos, (int)ok);
#endif
    _mode    = M_PLAY;
    _mode_ms = GetHAL().millis();
    if (!ok) finishPlay();
}

void AppMicMeter::finishPlay()
{
    GetHAL().speaker.end();
    vTaskDelay(pdMS_TO_TICKS(30));
    mic_begin_ok(GetHAL().mic);
    _mic_started = true;
    _mic_ok      = GetHAL().mic.isEnabled();
    _have_data   = false;
    _rec_pos     = 0;
    _mode        = M_IDLE;
    mic_regs("after_play");
}

void AppMicMeter::analyze(const int16_t* data, int n)
{
    if (!data || n <= 0) return;

    double sum = 0;
    int peak = 0;
    for (int i = 0; i < n; i++) {
        int v = data[i];
        sum += (double)v * v;
        int a = v < 0 ? -v : v;
        if (a > peak) peak = a;
    }
    float rms = sqrtf((float)(sum / n));
    if (rms < 1.0f) rms = 1.0f;
    if (peak < 1) peak = 1;

    _rms_db  = 20.0f * log10f(rms / 32768.0f);
    _peak_db = 20.0f * log10f((float)peak / 32768.0f);
    if (_rms_db < -90) _rms_db = -90;
    if (_peak_db < -90) _peak_db = -90;
    _raw_peak = peak;

    // 8 段 Goertzel（比整段 FFT 省很多）
    for (int b = 0; b < NBAND; b++) {
        float f = kBands[b];
        int k = (int)(0.5f + (n * f) / kSampleRate);
        float w = 2.0f * (float)M_PI * k / n;
        float coeff = 2.0f * cosf(w);
        float s0 = 0, s1 = 0, s2 = 0;
        for (int i = 0; i < n; i++) {
            s0 = (float)data[i] + coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        float power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
        if (power < 0) power = 0;
        float mag = sqrtf(power) / n;
        float db  = 20.0f * log10f(mag / 16384.0f + 1e-6f);
        if (db < -70) db = -70;
        if (db > 0) db = 0;
        _band[b] = db;
    }

    _hist[_hist_pos] = _rms_db;
    _hist_pos        = (_hist_pos + 1) % NHIST;
    _frames++;
}

void AppMicMeter::draw()
{
    if (!_canvas) return;
    auto& cv = *_canvas;

    cv.fillScreen(0x0841);

    // 画布可能是 240x135 全屏，也可能降级成 240x109；一律按实际高度排版，避免底部被裁掉
    const int W = cv.width();
    const int H = cv.height();

    // 顶部：标题 + 采样状态
    cv.fillRect(0, 0, W, 13, 0x1082);
    cv.setFont(&fonts::efontCN_10);
    cv.setTextColor(TFT_WHITE);
    cv.setCursor(4, 3);
    cv.printf("MIC %u", (unsigned)(_frames % 10));  // 数字跳动 = 正在采音

    bool live = _mic_ok && _rec_fail < 20;
    cv.setTextColor(live ? TFT_GREEN : TFT_RED);
    cv.setCursor(72, 3);
    if (_mode == M_REC) {
        cv.setTextColor(TFT_RED);
        cv.print("● 录音中");
    } else if (_mode == M_PLAY) {
        cv.setTextColor(TFT_CYAN);
        cv.print("▶ 回放中");
    } else {
        cv.print(live ? "LIVE 采音中" : "无麦克风");
    }
    cv.setTextColor(0x7BEF);
    cv.setCursor(202, 3);
    cv.print("Esc/G0");

    // dB 数值
    char buf[64];
    int rms_t = (int)(_rms_db * 10.0f);
    cv.setFont(&fonts::Font2);
    cv.setTextColor(TFT_WHITE);
    cv.setCursor(4, 16);
    snprintf(buf, sizeof(buf), "%d.%d", rms_t / 10, (rms_t < 0 ? -rms_t : rms_t) % 10);
    cv.print(buf);
    cv.setFont(&fonts::efontCN_10);
    cv.setTextColor(0x7BEF);
    cv.print(" dB");

    cv.setCursor(150, 18);
    cv.setTextColor(TFT_YELLOW);
    snprintf(buf, sizeof(buf), "PK %d", _raw_peak);
    cv.print(buf);

    // 电平条（-70..-10 dB 映射满量程）
    const int barX = 4, barY = 36, barW = 232, barH = 11;
    cv.drawRect(barX, barY, barW, barH, 0x4A69);
    float t = (_rms_db + 70.0f) / 60.0f;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    int w = (int)(t * (barW - 2));
    uint16_t col = TFT_GREEN;
    if (t > 0.85f) col = TFT_RED;
    else if (t > 0.65f) col = TFT_YELLOW;
    if (w > 0) cv.fillRect(barX + 1, barY + 1, w, barH - 2, col);

    float tp = (_peak_hold + 70.0f) / 60.0f;
    if (tp < 0) tp = 0;
    if (tp > 1) tp = 1;
    int px = barX + 1 + (int)(tp * (barW - 2));
    cv.drawFastVLine(px, barY - 2, barH + 4, TFT_WHITE);

    // 录音进度（盖在电平条上）
    if (_mode == M_REC && _rec_cap > 0) {
        int pw = (int)((float)_rec_pos / _rec_cap * (barW - 2));
        cv.fillRect(barX + 1, barY + 1, pw, barH - 2, TFT_RED);
    }

    // 音量波形（60 帧滚动）
    const int hintY = H - 12;
    const int specH = 15;
    const int specY = hintY - 3 - specH;
    const int wfY   = 50;
    int wfH         = specY - 3 - wfY;
    if (wfH < 12) wfH = 12;
    cv.drawRect(4, wfY, 232, wfH, 0x2945);
    for (int i = 0; i < NHIST - 1; i++) {
        int i0 = (_hist_pos + i) % NHIST;
        int i1 = (_hist_pos + i + 1) % NHIST;
        float v0 = (_hist[i0] + 70.0f) / 60.0f;
        float v1 = (_hist[i1] + 70.0f) / 60.0f;
        if (v0 < 0) v0 = 0;
        if (v0 > 1) v0 = 1;
        if (v1 < 0) v1 = 0;
        if (v1 > 1) v1 = 1;
        int x0 = 5 + i * 230 / (NHIST - 1);
        int x1 = 5 + (i + 1) * 230 / (NHIST - 1);
        int y0 = wfY + wfH - 1 - (int)(v0 * (wfH - 2));
        int y1 = wfY + wfH - 1 - (int)(v1 * (wfH - 2));
        cv.drawLine(x0, y0, x1, y1, TFT_GREEN);
    }

    // 8 段频谱柱
    int bw = 26, gap = 3;
    for (int i = 0; i < NBAND; i++) {
        int x = 5 + i * (bw + gap);
        float v = (_band[i] + 70.0f) / 60.0f;
        if (v < 0) v = 0;
        if (v > 1) v = 1;
        int h = (int)(v * specH);
        cv.drawRect(x, specY, bw, specH, 0x39E7);
        uint16_t bc = 0x07E0;
        if (v > 0.8f) bc = 0xF800;
        else if (v > 0.6f) bc = 0xFFE0;
        else if (v > 0.4f) bc = 0x07FF;
        if (h > 0) cv.fillRect(x + 1, specY + specH - h, bw - 2, h, bc);
    }

    cv.setFont(&fonts::efontCN_10);
    cv.setTextColor(0x7BEF);
    cv.setCursor(4, hintY);
    cv.print("SPACE 录音回放  Esc/G0 返回");

    appCanvasPush(&cv);
}
