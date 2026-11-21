#pragma once
#include <mooncake.h>
#include <M5GFX.h>
#include <string>
#include <vector>

/*
 * 函数计算器(覆盖小学到大学)
 * Tab 循环四大模式:
 *  [计算]   表达式求值: 四则/乘方/开方/三角/反三角/双曲/对数/指数/阶乘/
 *           排列组合 ncr npr/gcd/lcm/min max/mod/hypot, 常量 pi e ans,
 *           支持隐式乘法(如 2x, 2(3+4)); 上下键翻历史
 *  [图像]   y=f(x) 函数图像: Enter 改函数, 方向键平移, +/- 缩放,
 *           0 复位视野, t 描点模式(左右键沿曲线读坐标)
 *  [方程]   类型一: 一元二次 ax^2+bx+c=0 (求根公式, 判别式, 复数根)
 *           类型二: 任意 f(x)=0 区间扫描+二分法求全部变号根(自动排极点)
 *  [微积分] 数值求导 f'(x0) (中心差分+Richardson 外推)
 *           定积分 ∫[a,b] f(x)dx (Simpson 辛普森法)
 * ` 键(反引号)全局切换 DEG/RAD; Esc 退出(图像编辑态先取消编辑)
 */
class AppFunctionCalculator : public mooncake::AppAbility {
public:
    AppFunctionCalculator();
    ~AppFunctionCalculator();
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    struct Field {
        std::string t;
        int c = 0;
    };

    int _key_event_slot_id = -1;
    LGFX_Sprite* _canvas = nullptr;
    bool _want_close = false;
    int _mode = 0;  // 0 计算 1 图像 2 方程 3 微积分

    // ---- 计算模式 ----
    Field _cf;
    std::string _cres = "0";
    std::string _cerr;
    bool _cok       = true;
    bool _just_eval = false;
    std::vector<std::string> _hist;
    int _hist_idx = -1;

    // ---- 图像模式 ----
    Field _gf;
    bool _g_edit  = false;
    double _gcx   = 0;    // 视野中心(数学坐标)
    double _gcy   = 0;
    double _gsc   = 12;   // 每单位像素数
    bool _trace   = false;
    int _tcol     = 120;  // 描点列(屏幕坐标)

    // ---- 方程模式 ----
    int _sub = 0;  // 0 一元二次 1 f(x)=0
    Field _qa, _qb, _qc;
    Field _sf, _slo, _shi;
    int _s_sel = 0;
    std::vector<std::string> _s_res;
    bool _s_ok = true;

    // ---- 微积分模式 ----
    Field _df, _dx0, _da, _db;
    int _d_sel = 0;
    std::vector<std::string> _d_res;
    bool _d_ok = true;

    // ---- 全局 ----
    bool _deg     = true;
    double _ans   = 0;
    bool _has_ans = false;

    // 按键路由
    void on_char(char ch);
    void on_enter();
    void on_backspace();
    void on_tab();
    void on_esc();
    void on_arrow(int dir);  // 0左 1右 2上 3下

    // 字段编辑
    static void field_insert(Field& f, char ch);
    static void field_bs(Field& f);
    static void field_caret(Field& f, int d);

    // 计算模式
    void calc_preview();
    void calc_eval_now();
    void hist_move(int dir);

    // 图像模式
    double graph_eval(double x, bool* ok);

    // 方程模式
    void solve_now();
    Field* solve_field(int idx);

    // 微积分模式
    void calcu_now();
    Field* calcu_field(int idx);

    // 绘制
    void draw();
    void draw_calc();
    void draw_graph();
    void draw_solve();
    void draw_calcu();
    void draw_field(int x, int y, const char* label, Field& f, bool sel, int maxw);
    void draw_topbar(const char* mode_name);
    void draw_hint(const char* s);
};
