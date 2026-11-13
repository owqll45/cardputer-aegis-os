#include "app_scientific_calculator.h"
#include <mooncake_log.h>
#include <hal/hal.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>

// ================= 表达式求值(递归下降) =================
namespace {

const double kPi = 3.14159265358979323846;
const double kE  = 2.71828182845904523536;

struct Parser {
    const char* s;
    bool deg;
    bool ok;
    const char* err;
    double ans;
    bool has_ans;
};

void perr(Parser& p, const char* m) { if (p.ok) { p.ok = false; p.err = m; } }
void skip_ws(Parser& p) { while (*p.s == ' ' || *p.s == '\t') p.s++; }

bool eat(Parser& p, char c)
{
    skip_ws(p);
    if (*p.s == c) { p.s++; return true; }
    return false;
}

double p_expr(Parser& p);
double p_unary(Parser& p);

double p_primary(Parser& p)
{
    skip_ws(p);
    if (!p.ok) return 0;
    if (*p.s == 0) { perr(p, "表达式不完整"); return 0; }

    if (*p.s == '(') {
        p.s++;
        double v = p_expr(p);
        if (!eat(p, ')')) { perr(p, "缺少 )"); return 0; }
        return v;
    }
    if ((*p.s >= '0' && *p.s <= '9') || *p.s == '.') {
        char* end = nullptr;
        double v  = strtod(p.s, &end);
        if (end == p.s) { perr(p, "数字错误"); return 0; }
        p.s = end;
        return v;
    }
    if (isalpha((unsigned char)*p.s)) {
        const char* st = p.s;
        while (isalnum((unsigned char)*p.s)) p.s++;
        char name[16];
        int n = (int)(p.s - st);
        if (n > 15) n = 15;
        for (int i = 0; i < n; i++) name[i] = (char)tolower((unsigned char)st[i]);
        name[n] = 0;

        if (strcmp(name, "pi") == 0) return kPi;
        if (strcmp(name, "e") == 0) return kE;
        if (strcmp(name, "ans") == 0) {
            if (!p.has_ans) { perr(p, "还没有 ANS"); return 0; }
            return p.ans;
        }

        if (!eat(p, '(')) { perr(p, "函数后要加 ("); return 0; }

        bool two = (strcmp(name, "min") == 0 || strcmp(name, "max") == 0 ||
                    strcmp(name, "pow") == 0 || strcmp(name, "mod") == 0 ||
                    strcmp(name, "hypot") == 0 || strcmp(name, "atan2") == 0);
        double a = p_expr(p);
        if (!p.ok) return 0;
        double b = 0;
        if (two) {
            if (!eat(p, ',')) { perr(p, "这里需要逗号"); return 0; }
            b = p_expr(p);
            if (!p.ok) return 0;
        }
        if (!eat(p, ')')) { perr(p, "缺少 )"); return 0; }

        double k = p.deg ? kPi / 180.0 : 1.0;
        if (strcmp(name, "sin") == 0) return sin(a * k);
        if (strcmp(name, "cos") == 0) return cos(a * k);
        if (strcmp(name, "tan") == 0) return tan(a * k);
        if (strcmp(name, "asin") == 0) {
            if (a < -1 || a > 1) { perr(p, "asin 只吃 -1~1"); return 0; }
            return asin(a) / k;
        }
        if (strcmp(name, "acos") == 0) {
            if (a < -1 || a > 1) { perr(p, "acos 只吃 -1~1"); return 0; }
            return acos(a) / k;
        }
        if (strcmp(name, "atan") == 0) return atan(a) / k;
        if (strcmp(name, "sinh") == 0) return sinh(a);
        if (strcmp(name, "cosh") == 0) return cosh(a);
        if (strcmp(name, "tanh") == 0) return tanh(a);
        if (strcmp(name, "log") == 0) {
            if (a <= 0) { perr(p, "log 只吃 >0"); return 0; }
            return log10(a);
        }
        if (strcmp(name, "ln") == 0) {
            if (a <= 0) { perr(p, "ln 只吃 >0"); return 0; }
            return log(a);
        }
        if (strcmp(name, "log2") == 0) {
            if (a <= 0) { perr(p, "log2 只吃 >0"); return 0; }
            return log2(a);
        }
        if (strcmp(name, "sqrt") == 0) {
            if (a < 0) { perr(p, "负数不能开方"); return 0; }
            return sqrt(a);
        }
        if (strcmp(name, "cbrt") == 0) return cbrt(a);
        if (strcmp(name, "abs") == 0) return fabs(a);
        if (strcmp(name, "exp") == 0) return exp(a);
        if (strcmp(name, "floor") == 0) return floor(a);
        if (strcmp(name, "ceil") == 0) return ceil(a);
        if (strcmp(name, "round") == 0) return round(a);
        if (strcmp(name, "trunc") == 0) return trunc(a);
        if (strcmp(name, "min") == 0) return fmin(a, b);
        if (strcmp(name, "max") == 0) return fmax(a, b);
        if (strcmp(name, "pow") == 0) return pow(a, b);
        if (strcmp(name, "mod") == 0) {
            if (b == 0) { perr(p, "取模不能是 0"); return 0; }
            return fmod(a, b);
        }
        if (strcmp(name, "hypot") == 0) return hypot(a, b);
        if (strcmp(name, "atan2") == 0) return atan2(a, b) / k;
        perr(p, "未知函数");
        return 0;
    }
    perr(p, "无法识别");
    return 0;
}

double p_postfix(Parser& p)
{
    double v = p_primary(p);
    while (p.ok) {
        skip_ws(p);
        if (*p.s != '!') break;
        p.s++;
        if (v < 0 || v != floor(v) || v > 170) { perr(p, "阶乘需 0~170 整数"); return 0; }
        double r = 1;
        for (int i = 2; i <= (int)v; i++) r *= i;
        v = r;
    }
    return v;
}

double p_power(Parser& p)
{
    double b = p_postfix(p);
    skip_ws(p);
    if (p.ok && *p.s == '^') {
        p.s++;
        double e = p_unary(p);
        return pow(b, e);
    }
    return b;
}

double p_unary(Parser& p)
{
    skip_ws(p);
    if (*p.s == '-') { p.s++; return -p_unary(p); }
    if (*p.s == '+') { p.s++; return p_unary(p); }
    return p_power(p);
}

double p_term(Parser& p)
{
    double v = p_unary(p);
    while (p.ok) {
        skip_ws(p);
        char c = *p.s;
        if (c == '*') {
            p.s++;
            v *= p_unary(p);
        } else if (c == '/') {
            p.s++;
            double d = p_unary(p);
            if (d == 0) { perr(p, "不能除以 0"); return 0; }
            v /= d;
        } else if (c == '%') {
            p.s++;
            double d = p_unary(p);
            if (d == 0) { perr(p, "取模不能是 0"); return 0; }
            v = fmod(v, d);
        } else {
            break;
        }
    }
    return v;
}

double p_expr(Parser& p)
{
    double v = p_term(p);
    while (p.ok) {
        skip_ws(p);
        char c = *p.s;
        if (c == '+') { p.s++; v += p_term(p); }
        else if (c == '-') { p.s++; v -= p_term(p); }
        else break;
    }
    return v;
}

bool calc_eval(const char* s, bool deg, double ans, bool has_ans, double* out, const char** err)
{
    Parser p{ s, deg, true, "", ans, has_ans };
    skip_ws(p);
    if (*p.s == 0) { *err = "空表达式"; return false; }
    double v = p_expr(p);
    if (p.ok) {
        skip_ws(p);
        if (*p.s) perr(p, "有多余内容");
    }
    if (!p.ok) { *err = p.err; return false; }
    if (std::isnan(v) || std::isinf(v)) { *err = "结果无效"; return false; }
    *out = v;
    return true;
}

std::string fmt_num(double v)
{
    if (v == 0) return "0";
    double a = fabs(v);
    char b[40];
    if (a >= 1e12 || a < 1e-6) snprintf(b, sizeof(b), "%.6e", v);
    else snprintf(b, sizeof(b), "%.10g", v);
    return std::string(b);
}

}  // namespace

// ================= App =================
AppScientificCalculator::AppScientificCalculator()
{
    setAppInfo().name = "Calculator";
}

AppScientificCalculator::~AppScientificCalculator() {}

void AppScientificCalculator::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "opened");

    // 使用 DisplayManager 共享画布（240x109），每次 pushCanvas() 推屏。
    // 不要自建 240x135 精灵：本机无 PSRAM，创建会失败并回退到 109 高，
    // 再用 pushSprite(0,0) 推屏会在屏幕底部留下一整条未刷新的长方形残影。
    _expr.clear();
    _caret     = 0;
    _result    = "0";
    _err.clear();
    _ok        = true;
    _val       = 0;
    _deg       = true;
    _ans       = 0;
    _has_ans   = false;
    _just_eval = false;
    _hist.clear();
    _hist_idx = -1;

    _key_event_slot_id = GetHAL().keyboard.onKeyEvent.connect([this](const Keyboard::KeyEvent_t& e) {
        if (!e.state || e.isModifier) return;

        if (e.keyCode == KEY_ENTER) { evaluate(); return; }
        if (e.keyCode == KEY_BACKSPACE) { backspace(); return; }
        if (e.keyCode == KEY_TAB) {
            _deg       = !_deg;
            _just_eval = false;
            update_preview();
            return;
        }
        if (e.keyCode == KEY_LEFT) {
            if (_caret > 0) _caret--;
            return;
        }
        if (e.keyCode == KEY_RIGHT) {
            if (_caret < (int)_expr.size()) _caret++;
            return;
        }
        if (e.keyCode == KEY_UP) { history_up(); return; }
        if (e.keyCode == KEY_DOWN) { history_down(); return; }

        // 可打印字符(shift 组合也算, 如 shift+9 → "(")
        if (e.keyName && e.keyName[0] && !e.keyName[1]) {
            char ch = e.keyName[0];
            if (ch >= 32 && ch <= 126) insert_char(ch);
        }
    });
}

void AppScientificCalculator::onRunning()
{
    if (GetHAL().goButton().wasClicked()) { close(); return; }

    static uint32_t last = 0;
    uint32_t now         = GetHAL().millis();
    if (now - last < 40) return;
    last = now;

    draw();
    GetHAL().pushCanvas();
}

void AppScientificCalculator::onClose()
{
    mclog::tagInfo(getAppInfo().name, "closed");
    if (_key_event_slot_id >= 0) GetHAL().keyboard.onKeyEvent.disconnect(_key_event_slot_id);
    _key_event_slot_id = -1;
}

void AppScientificCalculator::insert_char(char c)
{
    if (_just_eval) {
        _just_eval = false;
        if (_ok && strchr("+-*/^%", c)) {
            _expr  = _result;   // 接着结果继续算
            _caret = (int)_expr.size();
        } else {
            _expr.clear();
            _caret = 0;
            _err.clear();
        }
    }
    if (_expr.size() >= 48) return;
    _expr.insert(_expr.begin() + _caret, c);
    _caret++;
    _hist_idx = -1;
    update_preview();
}

void AppScientificCalculator::backspace()
{
    _just_eval = false;
    if (_caret > 0) {
        _expr.erase(_expr.begin() + (_caret - 1));
        _caret--;
    }
    _hist_idx = -1;
    update_preview();
}

void AppScientificCalculator::update_preview()
{
    if (_expr.empty()) {
        _result = "0";
        _err.clear();
        _ok  = true;
        _val = 0;
        return;
    }
    double v       = 0;
    const char* er = nullptr;
    if (calc_eval(_expr.c_str(), _deg, _ans, _has_ans, &v, &er)) {
        _val    = v;
        _result = fmt_num(v);
        _ok     = true;
        _err.clear();
    } else {
        _ok  = false;
        _err = er ? er : "错误";
    }
}

void AppScientificCalculator::evaluate()
{
    if (_expr.empty()) return;
    double v       = 0;
    const char* er = nullptr;
    if (!calc_eval(_expr.c_str(), _deg, _ans, _has_ans, &v, &er)) {
        _ok  = false;
        _err = er ? er : "错误";
        return;
    }
    _val    = v;
    _result = fmt_num(v);
    _ok     = true;
    _err.clear();
    _ans     = v;
    _has_ans = true;

    if (_hist.empty() || _hist.back() != _expr) {
        _hist.push_back(_expr);
        if (_hist.size() > 20) _hist.erase(_hist.begin());
    }
    _hist_idx  = (int)_hist.size() - 1;
    _just_eval = true;
}

void AppScientificCalculator::history_up()
{
    if (_hist.empty()) return;
    if (_hist_idx < 0) _hist_idx = (int)_hist.size();
    if (_hist_idx > 0) {
        _hist_idx--;
        _expr  = _hist[_hist_idx];
        _caret = (int)_expr.size();
        _just_eval = false;
        update_preview();
    }
}

void AppScientificCalculator::history_down()
{
    if (_hist.empty() || _hist_idx < 0) return;
    if (_hist_idx < (int)_hist.size() - 1) {
        _hist_idx++;
        _expr = _hist[_hist_idx];
    } else {
        _hist_idx = (int)_hist.size();
        _expr.clear();
    }
    _caret     = (int)_expr.size();
    _just_eval = false;
    update_preview();
}

void AppScientificCalculator::draw()
{
    auto& cv  = GetHAL().canvas();
    auto* c   = &cv;
    int w     = c->width();
    int h     = c->height();

    c->fillScreen(TFT_BLACK);

    // ---- 状态栏 ----
    c->setFont(&fonts::efontCN_10_b);
    c->setTextColor(_deg ? TFT_CYAN : TFT_ORANGE);
    c->setCursor(3, 1);
    c->print(_deg ? "DEG" : "RAD");
    if (_has_ans) {
        std::string s = "ANS=" + fmt_num(_ans);
        c->setTextColor(TFT_DARKGREY);
        c->setCursor(w - 3 - c->textWidth(s.c_str()), 1);
        c->print(s.c_str());
    }
    c->drawFastHLine(0, 13, w, 0x4208);

    // ---- 表达式(横向滚动, 光标可见) ----
    c->setFont(&fonts::efontCN_12);
    const int padx = 3;
    const int avail = w - padx * 2;
    int start = 0;
    while (start < _caret && c->textWidth(_expr.substr(start, _caret - start).c_str()) > avail) start++;
    std::string view = _expr.substr(start);
    while (!view.empty() && c->textWidth(view.c_str()) > avail) view.pop_back();

    if (_expr.empty()) {
        c->setTextColor(TFT_DARKGREY);
        c->setCursor(padx, 18);
        c->print("输入表达式");
    } else {
        c->setTextColor(TFT_WHITE);
        c->setCursor(padx, 18);
        c->print(view.c_str());
        if ((GetHAL().millis() / 500) % 2 == 0) {
            int cx = padx + c->textWidth(_expr.substr(start, _caret - start).c_str());
            if (cx > w - 2) cx = w - 2;
            c->fillRect(cx, 17, 1, 14, TFT_CYAN);
        }
    }

    // ---- 结果(右对齐) ----
    std::string rs = _expr.empty() ? std::string("0") : (_ok ? _result : _err);
    c->setFont(&fonts::efontCN_14);
    int rw = c->textWidth(rs.c_str());
    if (rw > w - 6) {
        c->setFont(&fonts::efontCN_10_b);
        rw = c->textWidth(rs.c_str());
    }
    int rx = w - 3 - rw;
    if (rx < 3) rx = 3;
    c->setTextColor(_expr.empty() ? TFT_GREEN : (_ok ? TFT_GREEN : TFT_RED));
    c->setCursor(rx, 36);
    c->print(rs.c_str());
    c->drawFastHLine(0, 56, w, 0x4208);

    // ---- 历史 ----
    c->setFont(&fonts::efontCN_10_b);
    c->setTextColor(TFT_DARKGREY);
    int hy    = 58;
    int first = (int)_hist.size() - 3;
    if (first < 0) first = 0;
    for (int i = first; i < (int)_hist.size(); i++) {
        std::string line = _hist[i];
        while (!line.empty() && c->textWidth(line.c_str()) > w - 6) line.pop_back();
        c->setCursor(3, hy);
        c->print(line.c_str());
        hy += 13;
    }

    // ---- 底部提示 ----
    c->setCursor(3, h - 13);
    c->print("Enter=算 Tab=角度 ↑↓=历史 G0=退");
}
