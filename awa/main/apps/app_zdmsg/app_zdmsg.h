/*
 * LoRa 消息终端(ZD v1)
 *
 * 与 C3 节点(c3msg M1)通过 ESP-NOW channel 1 互通:
 *   Cardputer 负责键盘输入 + 多页终端显示, C3 负责收空口/中继。
 * 帧格式见 apps/utils/glink/zdlink.h(与 d3/c3msg/main/main.c 完全一致)。
 *
 * 布局: 顶栏画进系统栏画布(240x26), 内容区用 240x109, 合计占满 240x135。
 * 页面: 消息列表(首页) / 写消息 / 节点 / 设置 / 关于, 数字键 1-5 直达;
 *       列表页键入任意字母即进入写消息(G0 逐级返回, 列表页 G0 退出)。
 * 键盘回调只入队, 所有处理/绘制都在 onRunning(app 主任务)里完成。
 */
#pragma once
#include <mooncake.h>
#include <hal/hal.h>
#include <string>

/* 串口取证用: 让面板画一页字号对比(在 app 任务里绘制, 不跨任务画屏) */
void zdmsg_font_demo(bool on);

class AppZdMsg : public mooncake::AppAbility {
public:
    AppZdMsg();
    ~AppZdMsg();

    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    enum Page : uint8_t { PG_LIST, PG_WRITE, PG_NODES, PG_SET, PG_ABOUT };

    /* 时间来源: 0 未同步 1 手动 2 LoRa 3 GPS 4 网络(NTP) */
    enum : uint8_t { TS_NONE = 0, TS_MANUAL, TS_LORA, TS_GPS, TS_NET };

    int _key_slot = -1;
    std::string _input;

    uint16_t _last_msgid = 0;
    uint8_t _last_via = 1;   /* ZD_VIA_*: 最近一次发送走的路径 */
    uint32_t _bridge_id = 0; /* 选中的 C3 桥 NodeNum, 0=自动 */
    uint32_t _last_peer = 0; /* 上次有成功往来的对端(NodeNum), 存 NVS */
    uint32_t _last_draw_ms = 0;
    uint32_t _cursor_ms = 0;
    bool _cursor_on = true;
    int _bat_level = 0;
    uint32_t _bat_ms = 0;
    uint16_t _unread = 0;

    uint8_t _time_src = TS_NONE;
    bool _time_edit = false;
    int _edit_h = 0;
    int _edit_m = 0;
    int _tz = 8;

    char _lnack_txt[48] = {0};
    uint32_t _lnack_ms = 0;

    Page _page = PG_LIST;
    int _list_scroll = 0;
    int _node_scroll = 0;
    int _set_sel = 0;

    uint8_t _target = 0;      /* 0 = ESP-NOW 本地, 1 = 经 C3 → LoRa */
    bool _relay_on = true;    /* 是否参与中继转发 */
    bool _mute = false;
    /* 自动优先：ESP-NOW 发出后等 ACK，超时改走 LoRa 桥（同一文本，接收端去重） */
    bool _auto_pending = false;
    uint16_t _auto_msgid = 0;
    uint32_t _auto_deadline = 0;
    char _auto_text[97] = {0};
    int _vol = 128;
    int _bright = 100;

    uint32_t _toast_ms = 0;
    char _toast[48] = {0};

    void load_settings();
    void handle_keys();
    void handle_key(const Keyboard::KeyEvent_t& e);
    void toggle_target();
    void send_input();
    void drain_events();
    void note_peer(uint32_t id);
    void set_toast(const char* s);
    int node_online_count() const;

    void goto_page(Page p);
    void clock_hm(char* out, size_t n) const;
    const char* time_src_name() const;
    void time_start_edit();
    void time_apply_manual();
    /* 协议 v1.2 §1.2 采纳规则: 未同步收任意可信源; 手动只被 GPS 覆盖;
     * GPS 不被手动覆盖, 可被另一条 GPS 微调。返回是否真的采纳。 */
    bool time_adopt(uint32_t utc, uint8_t src, uint8_t acc);

    void draw();
    void draw_topbar();
    void draw_footer(const char* hint);
    void draw_list();
    void draw_write();
    void draw_nodes();
    void draw_settings();
    void draw_about();
};
