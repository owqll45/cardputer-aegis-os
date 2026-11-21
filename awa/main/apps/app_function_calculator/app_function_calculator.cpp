#include "app_function_calculator.h"
#include <mooncake_log.h>
#include <hal/hal.h>
#include <apps/utils/app_canvas.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>

// ==CKCORE_BEGIN==
// ================= 函数计算核(纯函数, 宿主可测) =================
// 递归下降表达式解析, 支持变量 x; 另含一元二次求根、数值求根、求导、积分
namespace fccore {

const double kPi = 3.14159265358979323846;
const double kE  = 2.71828182845904523536;

struct FcP {
    const char* s;
    bool deg;
    bool ok;
    const char* err;
    double x;
    double ans;
    bool has_ans;
};

static void fc_err(FcP& p, const char* m)
{
    if (p.ok) {
        p.ok  = false;
        p.err = m;
    }
}

static void fc_ws(FcP& p)
{
    while (*p.s == ' ' || *p.s == '\t') p.s++;
}

static bool fc_eat(FcP& p, char c)
{
    fc_ws(p);
    if (*p.s == c) {
        p.s++;
        return true;
    }
    return false;
}

static double fc_expr(FcP& p);
static double fc_unary(FcP& p);

static long long fc_gcd_ll(long long a, long long b)
{
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b) {
        long long t = a % b;
        a = b;
        b = t;
    }
    return a;
}

static bool fc_to_int(double v, long long* out)
{
    if (v != floor(v)) return false;
    if (fabs(v) > 9e15) return false;
    *out = (long long)v;
    return true;
}

static double fc_primary(FcP& p)
{
    fc_ws(p);
    if (!p.ok) return 0;
    if (*p.s == 0) {
        fc_err(p, "表达式不完整");
        return 0;
    }

    if (*p.s == '(') {
        p.s++;
        double v = fc_expr(p);
        if (!fc_eat(p, ')')) {
            fc_err(p, "缺少 )");
            return 0;
        }
        return v;
    }
    if ((*p.s >= '0' && *p.s <= '9') || *p.s == '.') {
        char* end = nullptr;
        double v  = strtod(p.s, &end);
        if (end == p.s) {
            fc_err(p, "数字错误");
            return 0;
        }
        p.s = end;
        return v;
    }
    if (isalpha((unsigned char)*p.s) || *p.s == '_') {
        const char* st = p.s;
        while (isalnum((unsigned char)*p.s) || *p.s == '_') p.s++;
        char name[16];
        int n = (int)(p.s - st);
        if (n > 15) n = 15;
        for (int i = 0; i < n; i++) name[i] = (char)tolower((unsigned char)st[i]);
        name[n] = 0;

        if (strcmp(name, "pi") == 0) return kPi;
        if (strcmp(name, "e") == 0) return kE;
        if (strcmp(name, "x") == 0) return p.x;
        if (strcmp(name, "ans") == 0) {
            if (!p.has_ans) {
                fc_err(p, "还没有 ANS");
                return 0;
            }
            return p.ans;
        }

        if (!fc_eat(p, '(')) {
            fc_err(p, "函数后要加 (");
            return 0;
        }

        bool two = (strcmp(name, "min") == 0 || strcmp(name, "max") == 0 ||
                    strcmp(name, "pow") == 0 || strcmp(name, "mod") == 0 ||
                    strcmp(name, "hypot") == 0 || strcmp(name, "atan2") == 0 ||
                    strcmp(name, "gcd") == 0 || strcmp(name, "lcm") == 0 ||
                    strcmp(name, "ncr") == 0 || strcmp(name, "npr") == 0 ||
                    strcmp(name, "logb") == 0);
        double a = fc_expr(p);
        if (!p.ok) return 0;
        double b = 0;
        if (two) {
            if (!fc_eat(p, ',')) {
                fc_err(p, "这里需要逗号");
                return 0;
            }
            b = fc_expr(p);
            if (!p.ok) return 0;
        }
        if (!fc_eat(p, ')')) {
            fc_err(p, "缺少 )");
            return 0;
        }

        double k = p.deg ? kPi / 180.0 : 1.0;
        if (strcmp(name, "sin") == 0) return sin(a * k);
        if (strcmp(name, "cos") == 0) return cos(a * k);
        if (strcmp(name, "tan") == 0) return tan(a * k);
        if (strcmp(name, "sec") == 0) return 1.0 / cos(a * k);
        if (strcmp(name, "csc") == 0) return 1.0 / sin(a * k);
        if (strcmp(name, "cot") == 0) return 1.0 / tan(a * k);
        if (strcmp(name, "asin") == 0) {
            if (a < -1 || a > 1) {
                fc_err(p, "asin 只吃 -1~1");
                return 0;
            }
            return asin(a) / k;
        }
        if (strcmp(name, "acos") == 0) {
            if (a < -1 || a > 1) {
                fc_err(p, "acos 只吃 -1~1");
                return 0;
            }
            return acos(a) / k;
        }
        if (strcmp(name, "atan") == 0) return atan(a) / k;
        if (strcmp(name, "sinh") == 0) return sinh(a);
        if (strcmp(name, "cosh") == 0) return cosh(a);
        if (strcmp(name, "tanh") == 0) return tanh(a);
        if (strcmp(name, "asinh") == 0) return asinh(a);
        if (strcmp(name, "acosh") == 0) {
            if (a < 1) {
                fc_err(p, "acosh 只吃 >=1");
                return 0;
            }
            return acosh(a);
        }
        if (strcmp(name, "atanh") == 0) {
            if (a <= -1 || a >= 1) {
                fc_err(p, "atanh 只吃 (-1,1)");
                return 0;
            }
            return atanh(a);
        }
        if (strcmp(name, "log") == 0) {
            if (a <= 0) {
                fc_err(p, "log 只吃 >0");
                return 0;
            }
            return log10(a);
        }
        if (strcmp(name, "ln") == 0) {
            if (a <= 0) {
                fc_err(p, "ln 只吃 >0");
                return 0;
            }
            return log(a);
        }
        if (strcmp(name, "log2") == 0) {
            if (a <= 0) {
                fc_err(p, "log2 只吃 >0");
                return 0;
            }
            return log2(a);
        }
        if (strcmp(name, "logb") == 0) {
            if (a <= 0 || b <= 0 || b == 1) {
                fc_err(p, "logb(数,底) 需 >0 且底!=1");
                return 0;
            }
            return log(a) / log(b);
        }
        if (strcmp(name, "sqrt") == 0) {
            if (a < 0) {
                fc_err(p, "负数不能开方");
                return 0;
            }
            return sqrt(a);
        }
        if (strcmp(name, "cbrt") == 0) return cbrt(a);
        if (strcmp(name, "abs") == 0) return fabs(a);
        if (strcmp(name, "sign") == 0) return (a > 0) ? 1.0 : ((a < 0) ? -1.0 : 0.0);
        if (strcmp(name, "exp") == 0) return exp(a);
        if (strcmp(name, "floor") == 0) return floor(a);
        if (strcmp(name, "ceil") == 0) return ceil(a);
        if (strcmp(name, "round") == 0) return round(a);
        if (strcmp(name, "trunc") == 0) return trunc(a);
        if (strcmp(name, "min") == 0) return fmin(a, b);
        if (strcmp(name, "max") == 0) return fmax(a, b);
        if (strcmp(name, "pow") == 0) return pow(a, b);
        if (strcmp(name, "mod") == 0) {
            if (b == 0) {
                fc_err(p, "取模不能是 0");
                return 0;
            }
            return fmod(a, b);
        }
        if (strcmp(name, "hypot") == 0) return hypot(a, b);
        if (strcmp(name, "atan2") == 0) return atan2(a, b) / k;
        if (strcmp(name, "gcd") == 0) {
            long long ia, ib;
            if (!fc_to_int(a, &ia) || !fc_to_int(b, &ib)) {
                fc_err(p, "gcd 只吃整数");
                return 0;
            }
            return (double)fc_gcd_ll(ia, ib);
        }
        if (strcmp(name, "lcm") == 0) {
            long long ia, ib;
            if (!fc_to_int(a, &ia) || !fc_to_int(b, &ib)) {
                fc_err(p, "lcm 只吃整数");
                return 0;
            }
            long long g = fc_gcd_ll(ia, ib);
            if (g == 0) return 0;
            return (double)(ia / g * ib < 0 ? -(ia / g * ib) : ia / g * ib);
        }
        if (strcmp(name, "ncr") == 0 || strcmp(name, "npr") == 0) {
            long long ia, ib;
            if (!fc_to_int(a, &ia) || !fc_to_int(b, &ib)) {
                fc_err(p, "排列组合只吃整数");
                return 0;
            }
            if (ia < 0 || ib < 0 || ib > ia) {
                fc_err(p, "需要 n>=r>=0");
                return 0;
            }
            double r = 1;
            for (long long i = 0; i < ib; i++) r = r * (double)(ia - i);
            if (strcmp(name, "npr") == 0) return r;
            for (long long i = 2; i <= ib; i++) r = r / (double)i;
            return floor(r + 0.5);
        }
        fc_err(p, "未知函数");
        return 0;
    }
    fc_err(p, "无法识别");
    return 0;
}

static double fc_postfix(FcP& p)
{
    double v = fc_primary(p);
    while (p.ok) {
        fc_ws(p);
        if (*p.s != '!') break;
        p.s++;
        if (v < 0 || v != floor(v) || v > 170) {
            fc_err(p, "阶乘需 0~170 整数");
            return 0;
        }
        double r = 1;
        for (int i = 2; i <= (int)v; i++) r *= i;
        v = r;
    }
    return v;
}

static double fc_power(FcP& p)
{
    double b = fc_postfix(p);
    fc_ws(p);
    if (p.ok && *p.s == '^') {
        p.s++;
        double e = fc_unary(p);
        return pow(b, e);
    }
    return b;
}

static double fc_unary(FcP& p)
{
    fc_ws(p);
    if (*p.s == '-') {
        p.s++;
        return -fc_unary(p);
    }
    if (*p.s == '+') {
        p.s++;
        return fc_unary(p);
    }
    return fc_power(p);
}

static double fc_term(FcP& p)
{
    double v = fc_unary(p);
    while (p.ok) {
        fc_ws(p);
        char c = *p.s;
        if (c == '*') {
            p.s++;
            v *= fc_unary(p);
        } else if (c == '/') {
            p.s++;
            double d = fc_unary(p);
            if (d == 0) {
                fc_err(p, "不能除以 0");
                return 0;
            }
            v /= d;
        } else if (c == '%') {
            p.s++;
            double d = fc_unary(p);
            if (d == 0) {
                fc_err(p, "取模不能是 0");
                return 0;
            }
            v = fmod(v, d);
        } else if (c == '(' || isalpha((unsigned char)c)) {
            // 隐式乘法: 2x, 2(3+4), (x+1)(x-1), 2sin(x)
            v *= fc_unary(p);
        } else {
            break;
        }
    }
    return v;
}

static double fc_expr(FcP& p)
{
    double v = fc_term(p);
    while (p.ok) {
        fc_ws(p);
        char c = *p.s;
        if (c == '+') {
            p.s++;
            v += fc_term(p);
        } else if (c == '-') {
            p.s++;
            v -= fc_term(p);
        } else {
            break;
        }
    }
    return v;
}

// 求值入口: 解析错误返回 false; 值可能是 inf/nan(由调用方判断)
static bool fc_eval2(const char* s, double x, bool deg, double ans, bool has_ans,
                     double* out, const char** err)
{
    FcP p;
    p.s       = s;
    p.deg     = deg;
    p.ok      = true;
    p.err     = "";
    p.x       = x;
    p.ans     = ans;
    p.has_ans = has_ans;
    fc_ws(p);
    if (*p.s == 0) {
        *err = "空表达式";
        return false;
    }
    double v = fc_expr(p);
    if (p.ok) {
        fc_ws(p);
        if (*p.s) fc_err(p, "有多余内容");
    }
    if (!p.ok) {
        *err = p.err;
        return false;
    }
    *out = v;
    return true;
}

// 兼容简化入口: 不带 ans
static bool fc_evalx(const char* s, double x, bool deg, double* out, const char** err)
{
    return fc_eval2(s, x, deg, 0, false, out, err);
}

// ---- 一元二次 ax^2+bx+c=0 ----
// 返回: 2=两实根 1=重根 0=复根(实部r1 虚部±i1) -1=退化为一次(r1) -2=无解 -3=无穷多解
static int fc_quad(double a, double b, double c, double* r1, double* i1, double* r2, double* i2)
{
    const double FQ_EPS = 1e-12;
    *i1 = 0;
    *i2 = 0;
    if (fabs(a) < FQ_EPS) {
        if (fabs(b) < FQ_EPS) {
            if (fabs(c) < FQ_EPS) return -3;
            return -2;
        }
        *r1 = -c / b;
        *r2 = *r1;
        return -1;
    }
    double d     = b * b - 4.0 * a * c;
    double scale = b * b + 4.0 * fabs(a * c);
    if (d > FQ_EPS * scale) {
        double sd = sqrt(d);
        double q  = -0.5 * (b + (b >= 0 ? sd : -sd));  // 数值稳定配方
        if (q != 0) {
            *r1 = q / a;
            *r2 = c / q;
        } else {
            *r1 = (-b + sd) / (2 * a);
            *r2 = (-b - sd) / (2 * a);
        }
        if (*r1 > *r2) {
            double t = *r1;
            *r1      = *r2;
            *r2      = t;
        }
        return 2;
    }
    if (d >= -FQ_EPS * scale) {
        *r1 = -b / (2 * a);
        *r2 = *r1;
        return 1;
    }
    *r1 = -b / (2 * a);
    *i1 = sqrt(-d) / (2 * fabs(a));
    *r2 = *r1;
    *i2 = -*i1;
    return 0;
}

// ---- 数值求导: 中心差分 + Richardson 外推 ----
static bool fc_derivative(const char* f, double x0, bool deg, double* out, const char** err)
{
    double ax = fabs(x0);
    double h  = 1e-4 * (ax > 1.0 ? ax : 1.0);
    double fp, fm;
    if (!fc_evalx(f, x0 + h, deg, &fp, err)) return false;
    if (!fc_evalx(f, x0 - h, deg, &fm, err)) return false;
    double d1 = (fp - fm) / (2 * h);
    h *= 0.5;
    if (!fc_evalx(f, x0 + h, deg, &fp, err)) return false;
    if (!fc_evalx(f, x0 - h, deg, &fm, err)) return false;
    double d2 = (fp - fm) / (2 * h);
    double r  = (4.0 * d2 - d1) / 3.0;
    if (std::isnan(r) || std::isinf(r)) {
        *err = "该点不可求导";
        return false;
    }
    *out = r;
    return true;
}

// ---- 定积分: Simpson 辛普森法 ----
static bool fc_integral(const char* f, double a, double b, bool deg, double* out, const char** err)
{
    if (a == b) {
        *out = 0;
        return true;
    }
    double sign = 1.0;
    if (a > b) {
        double t = a;
        a        = b;
        b        = t;
        sign     = -1.0;
    }
    const int n = 1024;  // 偶数
    double h    = (b - a) / n;
    double y0, yn;
    if (!fc_evalx(f, a, deg, &y0, err)) {
        *err = "区间端点不可计算";
        return false;
    }
    if (!fc_evalx(f, b, deg, &yn, err)) {
        *err = "区间端点不可计算";
        return false;
    }
    double s = y0 + yn;
    for (int i = 1; i < n; i++) {
        double y;
        if (!fc_evalx(f, a + h * i, deg, &y, err)) {
            *err = "积分区间内有不可计算点";
            return false;
        }
        if (std::isnan(y) || std::isinf(y)) {
            *err = "积分区间内有发散点";
            return false;
        }
        s += (i & 1) ? 4.0 * y : 2.0 * y;
    }
    double v = s * h / 3.0 * sign;
    if (std::isnan(v) || std::isinf(v)) {
        *err = "积分发散";
        return false;
    }
    *out = v;
    return true;
}

// ---- 任意方程 f(x)=0: 区间扫描符号变化 + 二分; 自动排除极点 ----
static int fc_find_roots(const char* f, double lo, double hi, bool deg,
                         double* roots, int cap)
{
    if (lo > hi) {
        double t = lo;
        lo       = hi;
        hi       = t;
    }
    if (lo == hi || cap <= 0) return 0;
    const int N = 1200;
    const char* err;
    double step = (hi - lo) / N;
    double px   = lo, py = 0;
    bool pok    = fc_evalx(f, px, deg, &py, &err) && !std::isnan(py) && !std::isinf(py);
    int cnt     = 0;

    for (int i = 1; i <= N && cnt < cap; i++) {
        double x = lo + step * i;
        double y = 0;
        bool ok  = fc_evalx(f, x, deg, &y, &err) && !std::isnan(y) && !std::isinf(y);
        if (ok && pok) {
            bool crossed = (py == 0) || ((py < 0) != (y < 0));
            if (crossed) {
                double a = px, b = x, fa = py;
                for (int it = 0; it < 80; it++) {
                    double m  = 0.5 * (a + b);
                    double fm = 0;
                    if (!fc_evalx(f, m, deg, &fm, &err) || std::isnan(fm) || std::isinf(fm)) break;
                    if (fm == 0) {
                        a = m;
                        b = m;
                        break;
                    }
                    if ((fa < 0) != (fm < 0)) {
                        b = m;
                    } else {
                        a  = m;
                        fa = fm;
                    }
                }
                double r = 0.5 * (a + b);
                double fr = 0;
                // 排除极点(如 1/x 在 0 处): 根处 |f| 必须足够小
                if (fc_evalx(f, r, deg, &fr, &err) && fabs(fr) < 1e-6) {
                    bool dup = false;
                    for (int k = 0; k < cnt; k++) {
                        double tol = 1e-7 * (fabs(roots[k]) > 1 ? fabs(roots[k]) : 1);
                        if (fabs(roots[k] - r) < tol) {
                            dup = true;
                            break;
                        }
                    }
                    if (!dup) roots[cnt++] = r;
                }
            }
        }
        px  = x;
        py  = y;
        pok = ok;
    }
    // 排序(冒泡, 数量小)
    for (int i = 0; i < cnt; i++) {
        for (int j = i + 1; j < cnt; j++) {
            if (roots[j] < roots[i]) {
                double t = roots[i];
                roots[i] = roots[j];
                roots[j] = t;
            }
        }
    }
    return cnt;
}

// ---- 数字格式化 ----
static void fc_fmt(double v, char* buf, int bufsz)
{
    if (v == 0) {
        snprintf(buf, bufsz, "0");
        return;
    }
    double a = fabs(v);
    if (a >= 1e12 || a < 1e-6) snprintf(buf, bufsz, "%.6e", v);
    else snprintf(buf, bufsz, "%.10g", v);
}

}  // namespace fccore
// ==CKCORE_END==

using fccore::fc_eval2;
using fccore::fc_evalx;
using fccore::fc_quad;
using fccore::fc_derivative;
using fccore::fc_integral;
using fccore::fc_find_roots;
using fccore::fc_fmt;

// ================= App =================
AppFunctionCalculator::AppFunctionCalculator()
{
    setAppInfo().name = "FuncCalc";
}

AppFunctionCalculator::~AppFunctionCalculator() {}

void AppFunctionCalculator::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "opened");

    // 共享画布可能带着上一个 app 的 textDatum / 色深 / clipRect, 先走统一入口复位,
    // 否则光标定位整体偏移, 看起来像"计算器用不了"。
    GetHAL().display().fillScreen(TFT_BLACK);
    _canvas = &GetHAL().canvas();
    appCanvasBegin(_canvas, getAppInfo().name);
    _canvas->setTextDatum(top_left);

    _mode      = 0;
    _deg       = false;  // 默认弧度: 图形页 ±10 范围内 sin(x) 才能看出波形; 需要角度按 ` 切换
    _ans       = 0;
    _has_ans   = false;
    _cf.t.clear();
    _cf.c      = 0;
    _cres      = "0";
    _cok       = true;
    _just_eval = false;
    _hist.clear();
    _hist_idx = -1;

    _gf.t   = "sin(x)";
    _gf.c   = (int)_gf.t.size();
    _g_edit = false;
    _gcx = _gcy = 0;
    _gsc        = 12;
    _trace      = false;
    _tcol       = 120;

    _sub = 0;
    _qa.t = "1"; _qa.c = 1;
    _qb.t = "-3"; _qb.c = 2;
    _qc.t = "2"; _qc.c = 1;
    _sf.t = "x^3-x"; _sf.c = (int)_sf.t.size();
    _slo.t = "-10"; _slo.c = 3;
    _shi.t = "10"; _shi.c = 2;
    _s_sel = 0;
    _s_res.clear();
    _s_ok = true;

    _df.t  = "x^2"; _df.c = 3;
    _dx0.t = "3"; _dx0.c = 1;
    _da.t  = "0"; _da.c = 1;
    _db.t  = "3"; _db.c = 1;
    _d_sel = 0;
    _d_res.clear();
    _d_ok = true;

    _key_event_slot_id = GetHAL().keyboard.onKeyEvent.connect([this](const Keyboard::KeyEvent_t& e) {
        if (!e.state || e.isModifier) return;

        if (e.keyCode == KEY_ESC) { on_esc(); return; }
        if (e.keyCode == KEY_ENTER) { on_enter(); return; }
        if (e.keyCode == KEY_BACKSPACE) { on_backspace(); return; }
        if (e.keyCode == KEY_TAB) { on_tab(); return; }
        if (e.keyCode == KEY_LEFT) { on_arrow(0); return; }
        if (e.keyCode == KEY_RIGHT) { on_arrow(1); return; }
        if (e.keyCode == KEY_UP) { on_arrow(2); return; }
        if (e.keyCode == KEY_DOWN) { on_arrow(3); return; }

        if (e.keyName && e.keyName[0] && !e.keyName[1]) {
            char ch = e.keyName[0];
            if (ch >= 32 && ch <= 126) on_char(ch);
        }
    });
}

void AppFunctionCalculator::onRunning()
{
    if (_want_close) { _want_close = false; close(); return; }
    if (GetHAL().goButton().wasClicked()) {
        // 图形编辑中：G0 只取消编辑，不退出应用
        if (_mode == 1 && _g_edit) { _g_edit = false; return; }
        close();
        return;
    }

    static uint32_t last = 0;
    uint32_t now         = GetHAL().millis();
    if (now - last < 40) return;
    last = now;

    draw();
    appCanvasPush(_canvas);
}

void AppFunctionCalculator::onClose()
{
    mclog::tagInfo(getAppInfo().name, "closed");
    if (_key_event_slot_id >= 0) GetHAL().keyboard.onKeyEvent.disconnect(_key_event_slot_id);
    _key_event_slot_id = -1;
    appCanvasRestore(getAppInfo().name);
    _canvas = nullptr;
}

// ================= 字段编辑 =================
void AppFunctionCalculator::field_insert(Field& f, char ch)
{
    if (f.t.size() >= 44) return;
    f.t.insert(f.t.begin() + f.c, ch);
    f.c++;
}

void AppFunctionCalculator::field_bs(Field& f)
{
    if (f.c > 0) {
        f.t.erase(f.t.begin() + (f.c - 1));
        f.c--;
    }
}

void AppFunctionCalculator::field_caret(Field& f, int d)
{
    f.c += d;
    if (f.c < 0) f.c = 0;
    if (f.c > (int)f.t.size()) f.c = (int)f.t.size();
}

// ================= 按键路由 =================
void AppFunctionCalculator::on_char(char ch)
{
    if (ch == '`') {
        _deg = !_deg;
        if (_mode == 0) calc_preview();
        return;
    }
    if (_mode == 0) {
        if (_just_eval) {
            _just_eval = false;
            if (_cok && strchr("+-*/^%!", ch)) {
                char b[40];
                fc_fmt(_ans, b, sizeof(b));
                _cf.t = b;
                _cf.c = (int)_cf.t.size();
            } else {
                _cf.t.clear();
                _cf.c = 0;
                _cerr.clear();
            }
        }
        field_insert(_cf, ch);
        _hist_idx = -1;
        calc_preview();
        return;
    }
    if (_mode == 1) {
        if (!_g_edit) {
            if (ch == '+' || ch == '=') {
                _gsc *= 1.5;
                if (_gsc > 3000) _gsc = 3000;
                return;
            }
            if (ch == '-' || ch == '_') {
                _gsc /= 1.5;
                if (_gsc < 0.5) _gsc = 0.5;
                return;
            }
            if (ch == '0') {
                _gcx = _gcy = 0;
                _gsc        = 12;
                return;
            }
            if (ch == 't' || ch == 'T') {
                _trace = !_trace;
                return;
            }
            return;  // 非编辑态忽略其它字符
        }
        field_insert(_gf, ch);
        return;
    }
    if (_mode == 2) {
        if (_s_sel == 0) return;  // 类型选择器不吃字符
        Field* f = solve_field(_s_sel);
        if (f) field_insert(*f, ch);
        return;
    }
    if (_mode == 3) {
        Field* f = calcu_field(_d_sel);
        if (f) field_insert(*f, ch);
        return;
    }
}

void AppFunctionCalculator::on_enter()
{
    if (_mode == 0) {
        calc_eval_now();
        return;
    }
    if (_mode == 1) {
        _g_edit = !_g_edit;  // Enter 进入/确认编辑
        return;
    }
    if (_mode == 2) {
        solve_now();
        return;
    }
    if (_mode == 3) {
        calcu_now();
        return;
    }
}

void AppFunctionCalculator::on_backspace()
{
    if (_mode == 0) {
        _just_eval = false;
        field_bs(_cf);
        _hist_idx = -1;
        calc_preview();
        return;
    }
    if (_mode == 1) {
        if (_g_edit) field_bs(_gf);
        return;
    }
    if (_mode == 2) {
        Field* f = solve_field(_s_sel);
        if (f) field_bs(*f);
        return;
    }
    if (_mode == 3) {
        Field* f = calcu_field(_d_sel);
        if (f) field_bs(*f);
        return;
    }
}

void AppFunctionCalculator::on_tab()
{
    _mode   = (_mode + 1) % 4;
    _g_edit = false;
}

void AppFunctionCalculator::on_esc()
{
    if (_mode == 1 && _g_edit) {
        _g_edit = false;
        return;
    }
    // 按键回调在 desktop_task 里, 直接 close() 会和 mooncake 的异步关闭打架;
    // 置标记, 由 onRunning(本 app 任务上下文) 退出。
    _want_close = true;
}

void AppFunctionCalculator::on_arrow(int dir)
{
    if (_mode == 0) {
        if (dir == 0) field_caret(_cf, -1);
        else if (dir == 1) field_caret(_cf, 1);
        else if (dir == 2) hist_move(-1);
        else hist_move(1);
        return;
    }
    if (_mode == 1) {
        if (_g_edit) {
            if (dir == 0) field_caret(_gf, -1);
            else if (dir == 1) field_caret(_gf, 1);
            return;
        }
        if (_trace && (dir == 0 || dir == 1)) {
            _tcol += (dir == 0) ? -1 : 1;
            if (_tcol < 0) _tcol = 0;
            if (_tcol > 239) _tcol = 239;
            return;
        }
        double pan = 8.0 / _gsc;  // 8 像素的数学距离
        if (dir == 0) _gcx -= pan;
        else if (dir == 1) _gcx += pan;
        else if (dir == 2) _gcy += pan;
        else _gcy -= pan;
        return;
    }
    if (_mode == 2) {
        if (_s_sel == 0) {
            if (dir == 0 || dir == 1) {
                _sub   = 1 - _sub;
                _s_res.clear();
                _s_ok  = true;
            }
            if (dir == 3) _s_sel = 1;
            return;
        }
        if (dir == 2 || dir == 3) {
            _s_sel += (dir == 2) ? -1 : 1;
            int mx = 3;
            if (_s_sel < 0) _s_sel = 0;
            if (_s_sel > mx) _s_sel = mx;
            return;
        }
        Field* f = solve_field(_s_sel);
        if (f) field_caret(*f, (dir == 0) ? -1 : 1);
        return;
    }
    if (_mode == 3) {
        if (dir == 2 || dir == 3) {
            _d_sel += (dir == 2) ? -1 : 1;
            if (_d_sel < 0) _d_sel = 0;
            if (_d_sel > 3) _d_sel = 3;
            return;
        }
        Field* f = calcu_field(_d_sel);
        if (f) field_caret(*f, (dir == 0) ? -1 : 1);
        return;
    }
}

// ================= 计算模式 =================
void AppFunctionCalculator::calc_preview()
{
    if (_cf.t.empty()) {
        _cres = "0";
        _cok  = true;
        _cerr.clear();
        return;
    }
    double v;
    const char* err = nullptr;
    if (fc_eval2(_cf.t.c_str(), 0, _deg, _ans, _has_ans, &v, &err)) {
        if (std::isnan(v) || std::isinf(v)) {
            _cok  = false;
            _cerr = "结果无效";
        } else {
            _cok = true;
            char b[40];
            fc_fmt(v, b, sizeof(b));
            _cres = b;
            _cerr.clear();
        }
    } else {
        _cok  = false;
        _cerr = err ? err : "错误";
    }
}

void AppFunctionCalculator::calc_eval_now()
{
    if (_cf.t.empty()) return;
    double v;
    const char* err = nullptr;
    if (fc_eval2(_cf.t.c_str(), 0, _deg, _ans, _has_ans, &v, &err)) {
        if (std::isnan(v) || std::isinf(v)) {
            _cok  = false;
            _cerr = "结果无效";
        } else {
            _cok     = true;
            _ans     = v;
            _has_ans = true;
            char b[40];
            fc_fmt(v, b, sizeof(b));
            _cres = b;
            _cerr.clear();
        }
    } else {
        _cok  = false;
        _cerr = err ? err : "错误";
    }
    if (_hist.empty() || _hist.back() != _cf.t) {
        _hist.push_back(_cf.t);
        if (_hist.size() > 20) _hist.erase(_hist.begin());
    }
    _hist_idx  = (int)_hist.size() - 1;
    _just_eval = true;
}

void AppFunctionCalculator::hist_move(int dir)
{
    if (_hist.empty()) return;
    if (dir < 0) {
        if (_hist_idx < 0) _hist_idx = (int)_hist.size();
        if (_hist_idx > 0) _hist_idx--;
    } else {
        if (_hist_idx < 0) return;
        if (_hist_idx < (int)_hist.size() - 1) _hist_idx++;
        else {
            _hist_idx = (int)_hist.size();
            _cf.t.clear();
            _cf.c      = 0;
            _just_eval = false;
            calc_preview();
            return;
        }
    }
    _cf.t      = _hist[_hist_idx];
    _cf.c      = (int)_cf.t.size();
    _just_eval = false;
    calc_preview();
}

// ================= 图像模式 =================
double AppFunctionCalculator::graph_eval(double x, bool* ok)
{
    double v;
    const char* err = nullptr;
    if (!fc_evalx(_gf.t.c_str(), x, _deg, &v, &err) || std::isnan(v) || std::isinf(v)) {
        *ok = false;
        return 0;
    }
    *ok = true;
    return v;
}

// ================= 方程模式 =================
AppFunctionCalculator::Field* AppFunctionCalculator::solve_field(int idx)
{
    if (_sub == 0) {
        if (idx == 1) return &_qa;
        if (idx == 2) return &_qb;
        if (idx == 3) return &_qc;
    } else {
        if (idx == 1) return &_sf;
        if (idx == 2) return &_slo;
        if (idx == 3) return &_shi;
    }
    return nullptr;
}

void AppFunctionCalculator::solve_now()
{
    _s_res.clear();
    _s_ok = true;
    const char* err = nullptr;
    double v;
    char b1[40], b2[40];

    if (_sub == 0) {
        double a, b, c;
        if (!fc_evalx(_qa.t.c_str(), 0, _deg, &a, &err)) {
            _s_ok = false;
            _s_res.push_back(std::string("a 错误: ") + (err ? err : ""));
            return;
        }
        if (!fc_evalx(_qb.t.c_str(), 0, _deg, &b, &err)) {
            _s_ok = false;
            _s_res.push_back(std::string("b 错误: ") + (err ? err : ""));
            return;
        }
        if (!fc_evalx(_qc.t.c_str(), 0, _deg, &c, &err)) {
            _s_ok = false;
            _s_res.push_back(std::string("c 错误: ") + (err ? err : ""));
            return;
        }
        double r1, i1, r2, i2;
        int k = fc_quad(a, b, c, &r1, &i1, &r2, &i2);
        if (k == -3) {
            _s_res.push_back("0=0: 无穷多解");
        } else if (k == -2) {
            _s_res.push_back("矛盾: 无解");
        } else if (k == -1) {
            fc_fmt(r1, b1, sizeof(b1));
            _s_res.push_back("退化为一次方程");
            _s_res.push_back(std::string("x = ") + b1);
        } else {
            double d = b * b - 4 * a * c;
            fc_fmt(d, b2, sizeof(b2));
            _s_res.push_back(std::string("Δ = b^2-4ac = ") + b2);
            if (k == 2) {
                fc_fmt(r1, b1, sizeof(b1));
                _s_res.push_back(std::string("x1 = ") + b1);
                fc_fmt(r2, b1, sizeof(b1));
                _s_res.push_back(std::string("x2 = ") + b1);
            } else if (k == 1) {
                fc_fmt(r1, b1, sizeof(b1));
                _s_res.push_back(std::string("x1 = x2 = ") + b1);
            } else {
                fc_fmt(r1, b1, sizeof(b1));
                fc_fmt(i1, b2, sizeof(b2));
                _s_res.push_back(std::string("x = ") + b1 + " ± " + b2 + " i");
            }
        }
        return;
    }

    // f(x)=0
    double lo, hi;
    if (!fc_evalx(_slo.t.c_str(), 0, _deg, &lo, &err)) {
        _s_ok = false;
        _s_res.push_back(std::string("下限错误: ") + (err ? err : ""));
        return;
    }
    if (!fc_evalx(_shi.t.c_str(), 0, _deg, &hi, &err)) {
        _s_ok = false;
        _s_res.push_back(std::string("上限错误: ") + (err ? err : ""));
        return;
    }
    // 先验证函数本身可解析
    if (!fc_evalx(_sf.t.c_str(), (lo + hi) / 2, _deg, &v, &err)) {
        _s_ok = false;
        _s_res.push_back(std::string("f(x) 错误: ") + (err ? err : ""));
        return;
    }
    double roots[8];
    int n = fc_find_roots(_sf.t.c_str(), lo, hi, _deg, roots, 8);
    if (n == 0) {
        _s_res.push_back("区间内未发现变号根");
        _s_res.push_back("(相切根如x^2=0不在此列)");
        return;
    }
    for (int i = 0; i < n; i++) {
        fc_fmt(roots[i], b1, sizeof(b1));
        char ln[56];
        snprintf(ln, sizeof(ln), "x%d = %s", i + 1, b1);
        _s_res.push_back(ln);
    }
}

// ================= 微积分模式 =================
AppFunctionCalculator::Field* AppFunctionCalculator::calcu_field(int idx)
{
    if (idx == 0) return &_df;
    if (idx == 1) return &_dx0;
    if (idx == 2) return &_da;
    return &_db;
}

void AppFunctionCalculator::calcu_now()
{
    _d_res.clear();
    _d_ok = true;
    const char* err = nullptr;
    double x0, a, b, v;
    char b1[40], b2[40];

    if (!fc_evalx(_df.t.c_str(), 0, _deg, &v, &err)) {
        _d_ok = false;
        _d_res.push_back(std::string("f(x) 错误: ") + (err ? err : ""));
        return;
    }
    if (!fc_evalx(_dx0.t.c_str(), 0, _deg, &x0, &err)) {
        _d_ok = false;
        _d_res.push_back(std::string("x0 错误: ") + (err ? err : ""));
        return;
    }
    if (!fc_evalx(_da.t.c_str(), 0, _deg, &a, &err)) {
        _d_ok = false;
        _d_res.push_back(std::string("a 错误: ") + (err ? err : ""));
        return;
    }
    if (!fc_evalx(_db.t.c_str(), 0, _deg, &b, &err)) {
        _d_ok = false;
        _d_res.push_back(std::string("b 错误: ") + (err ? err : ""));
        return;
    }

    double dv;
    if (fc_derivative(_df.t.c_str(), x0, _deg, &dv, &err)) {
        fc_fmt(x0, b1, sizeof(b1));
        fc_fmt(dv, b2, sizeof(b2));
        _d_res.push_back(std::string("f'(") + b1 + ") = " + b2);
    } else {
        _d_ok = false;
        _d_res.push_back(std::string("求导失败: ") + (err ? err : ""));
    }

    double iv;
    if (fc_integral(_df.t.c_str(), a, b, _deg, &iv, &err)) {
        fc_fmt(a, b1, sizeof(b1));
        fc_fmt(b, b2, sizeof(b2));
        std::string line = std::string("S[") + b1 + "," + b2 + "] f dx = ";
        fc_fmt(iv, b1, sizeof(b1));
        line += b1;
        _d_res.push_back(line);
    } else {
        _d_ok = false;
        _d_res.push_back(std::string("积分失败: ") + (err ? err : ""));
    }
    _d_res.push_back("(中心差分+外推 / Simpson)");
}

// ================= 绘制 =================
void AppFunctionCalculator::draw()
{
    auto& cv = GetHAL().canvas();
    cv.setTextDatum(top_left);
    cv.fillScreen(TFT_BLACK);
    if (_mode == 0) draw_calc();
    else if (_mode == 1) draw_graph();
    else if (_mode == 2) draw_solve();
    else draw_calcu();
}

void AppFunctionCalculator::draw_topbar(const char* mode_name)
{
    auto& cv = GetHAL().canvas();
    int w    = cv.width();
    cv.setFont(&fonts::efontCN_10);
    cv.setTextColor(TFT_YELLOW);
    cv.setCursor(3, 1);
    char mt[32];
    snprintf(mt, sizeof(mt), "%s %d/4", mode_name, _mode + 1);
    cv.print(mt);
    cv.setTextColor(_deg ? TFT_CYAN : TFT_ORANGE);
    const char* ds = _deg ? "DEG" : "RAD";
    cv.setCursor(w - 3 - cv.textWidth(ds), 1);
    cv.print(ds);
    cv.drawFastHLine(0, 13, w, 0x4208);
}

void AppFunctionCalculator::draw_hint(const char* s)
{
    auto& cv = GetHAL().canvas();
    cv.setFont(&fonts::efontCN_10);
    cv.setTextColor(TFT_DARKGREY);
    cv.setCursor(3, cv.height() - 12);
    cv.print(s);
}

void AppFunctionCalculator::draw_field(int x, int y, const char* label, Field& f, bool sel, int maxw)
{
    auto& cv = GetHAL().canvas();
    cv.setFont(&fonts::efontCN_12);
    cv.setTextColor(sel ? TFT_CYAN : TFT_DARKGREY);
    cv.setCursor(x, y);
    cv.print(label);
    int lx = x + cv.textWidth(label) + 2;

    int avail = x + maxw - lx;
    if (avail < 10) avail = 10;
    int start = 0;
    if (sel) {
        while (start < f.c &&
               cv.textWidth(f.t.substr(start, f.c - start).c_str()) > avail) {
            start++;
        }
    }
    std::string view = f.t.substr(start);
    while (!view.empty() && cv.textWidth(view.c_str()) > avail) view.pop_back();

    if (f.t.empty()) {
        cv.setTextColor(0x4208);
        cv.setCursor(lx, y);
        cv.print("...");
    } else {
        cv.setTextColor(sel ? TFT_WHITE : TFT_LIGHTGREY);
        cv.setCursor(lx, y);
        cv.print(view.c_str());
    }
    if (sel && (GetHAL().millis() / 500) % 2 == 0) {
        int cx = lx + cv.textWidth(f.t.substr(start, f.c - start).c_str());
        int mx = x + maxw - 2;
        if (cx > mx) cx = mx;
        cv.fillRect(cx, y, 1, 12, TFT_CYAN);
    }
    if (sel) {
        cv.drawRect(x - 2, y - 2, maxw, 16, 0x2104);
    }
}

void AppFunctionCalculator::draw_calc()
{
    auto& cv = GetHAL().canvas();
    int w    = cv.width();
    draw_topbar("计算");

    // ANS 指示
    if (_has_ans) {
        char b[40];
        fc_fmt(_ans, b, sizeof(b));
        std::string s = std::string("ANS=") + b;
        cv.setFont(&fonts::efontCN_10);
        cv.setTextColor(TFT_DARKGREY);
        cv.setCursor(w - 3 - cv.textWidth(s.c_str()) - 34, 1);
        cv.print(s.c_str());
    }

    // 表达式(横向滚动)
    cv.setFont(&fonts::efontCN_12);
    const int padx  = 3;
    const int avail = w - padx * 2;
    int start       = 0;
    while (start < _cf.c &&
           cv.textWidth(_cf.t.substr(start, _cf.c - start).c_str()) > avail) {
        start++;
    }
    std::string view = _cf.t.substr(start);
    while (!view.empty() && cv.textWidth(view.c_str()) > avail) view.pop_back();

    if (_cf.t.empty()) {
        cv.setTextColor(TFT_DARKGREY);
        cv.setCursor(padx, 18);
        cv.print("输入表达式, 如 2x 或 (1+2)*3");
    } else {
        cv.setTextColor(TFT_WHITE);
        cv.setCursor(padx, 18);
        cv.print(view.c_str());
        if ((GetHAL().millis() / 500) % 2 == 0) {
            int cx = padx + cv.textWidth(_cf.t.substr(start, _cf.c - start).c_str());
            if (cx > w - 2) cx = w - 2;
            cv.fillRect(cx, 17, 1, 14, TFT_CYAN);
        }
    }

    // 结果
    std::string rs = _cf.t.empty() ? std::string("0") : (_cok ? _cres : _cerr);
    cv.setFont(&fonts::efontCN_14);
    int rw = cv.textWidth(rs.c_str());
    if (rw > w - 6) {
        cv.setFont(&fonts::efontCN_10);
        rw = cv.textWidth(rs.c_str());
    }
    int rx = w - 3 - rw;
    if (rx < 3) rx = 3;
    cv.setTextColor(_cf.t.empty() ? TFT_GREEN : (_cok ? TFT_GREEN : TFT_RED));
    cv.setCursor(rx, 36);
    cv.print(rs.c_str());
    cv.drawFastHLine(0, 56, w, 0x4208);

    // 历史
    cv.setFont(&fonts::efontCN_10);
    cv.setTextColor(TFT_DARKGREY);
    int hy    = 60;
    int first = (int)_hist.size() - 3;
    if (first < 0) first = 0;
    for (int i = first; i < (int)_hist.size(); i++) {
        std::string line = _hist[i];
        while (!line.empty() && cv.textWidth(line.c_str()) > w - 6) line.pop_back();
        cv.setCursor(3, hy);
        cv.print(line.c_str());
        hy += 13;
    }

    draw_hint("Tab=模式 `=度/弧 Enter=计算 ↑↓历史 Esc/G0退");
}

void AppFunctionCalculator::draw_graph()
{
    auto& cv  = GetHAL().canvas();
    int w     = cv.width();
    int h     = cv.height();
    const int top = 14;
    const int bot = h - 14;  // 绘图区 [top, bot]

    // 视野 -> 屏幕映射
    // sx = w/2 + (x-cx)*sc ; sy = (top+bot)/2 - (y-cy)*sc
    const double cx0 = (top + bot) / 2.0;

    // ---- 网格与坐标轴 ----
    double raw   = 30.0 / _gsc;             // 每 30px 的数学距离
    double base  = pow(10.0, floor(log10(raw)));
    double step  = base * 10.0;
    double cands[4] = { base, base * 2, base * 5, base * 10 };
    for (int i = 0; i < 4; i++) {
        if (cands[i] >= raw * 0.95) {
            step = cands[i];
            break;
        }
    }
    double x_min = _gcx - (w / 2.0) / _gsc;
    double x_max = _gcx + (w / 2.0) / _gsc;
    double y_min = _gcy - (cx0 - top) / _gsc;
    double y_max = _gcy + (cx0 - top) / _gsc;

    cv.setFont(&fonts::efontCN_10);
    // 竖直网格线
    for (double gx = ceil(x_min / step) * step; gx <= x_max; gx += step) {
        int sx = (int)(w / 2.0 + (gx - _gcx) * _gsc + 0.5);
        if (sx < 0 || sx >= w) continue;
        bool axis = fabs(gx) < step * 1e-6;
        cv.drawFastVLine(sx, top, bot - top + 1, axis ? 0x630C : 0x1883);
        if (!axis) {
            char lb[24];
            fc_fmt(gx, lb, sizeof(lb));
            cv.setTextColor(0x52AA);
            cv.setCursor(sx + 1, bot - 10);
            cv.print(lb);
        }
    }
    // 水平网格线
    for (double gy = ceil(y_min / step) * step; gy <= y_max; gy += step) {
        int sy = (int)(cx0 - (gy - _gcy) * _gsc + 0.5);
        if (sy < top || sy > bot) continue;
        bool axis = fabs(gy) < step * 1e-6;
        cv.drawFastHLine(0, sy, w, axis ? 0x630C : 0x1883);
        if (!axis) {
            char lb[24];
            fc_fmt(gy, lb, sizeof(lb));
            cv.setTextColor(0x52AA);
            cv.setCursor(2, sy - 10 < top ? top : sy - 10);
            cv.print(lb);
        }
    }

    // ---- 曲线 ----
    int py_prev = 0;
    bool has_prev = false;
    for (int sx = 0; sx < w; sx++) {
        double x = _gcx + (sx - w / 2.0) / _gsc;
        bool ok;
        double y = graph_eval(x, &ok);
        if (!ok) {
            has_prev = false;
            continue;
        }
        double syf = cx0 - (y - _gcy) * _gsc;
        if (syf < top - 200 || syf > bot + 200) {
            has_prev = false;  // 渐近线断点
            continue;
        }
        int sy = (int)(syf + 0.5);
        if (has_prev && abs(sy - py_prev) <= (bot - top)) {
            cv.drawLine(sx - 1, py_prev, sx, sy, TFT_GREEN);
        } else if (sy >= top && sy <= bot) {
            cv.drawPixel(sx, sy, TFT_GREEN);
        }
        py_prev  = sy;
        has_prev = true;
    }

    // ---- 描点 ----
    if (_trace && !_g_edit) {
        double x = _gcx + (_tcol - w / 2.0) / _gsc;
        bool ok;
        double y = graph_eval(x, &ok);
        cv.drawFastVLine(_tcol, top, bot - top + 1, 0x4208);
        if (ok) {
            int sy = (int)(cx0 - (y - _gcy) * _gsc + 0.5);
            if (sy >= top && sy <= bot) {
                cv.fillCircle(_tcol, sy, 2, TFT_RED);
            }
            char bx[56], b1[24], b2[24];
            fc_fmt(x, b1, sizeof(b1));
            fc_fmt(y, b2, sizeof(b2));
            snprintf(bx, sizeof(bx), "x=%s y=%s", b1, b2);
            cv.setFont(&fonts::efontCN_10);
            int bw = cv.textWidth(bx);
            int bx0 = _tcol + 4 + bw > w ? _tcol - 4 - bw : _tcol + 4;
            cv.setTextColor(TFT_YELLOW);
            cv.setCursor(bx0, top + 2);
            cv.print(bx);
        }
    }

    // ---- 顶部栏 ----
    cv.fillRect(0, 0, w, 13, TFT_BLACK);
    draw_topbar("图像");

    // ---- 底部: f(x) 或编辑 ----
    cv.fillRect(0, bot + 1, w, h - bot - 1, TFT_BLACK);
    cv.setFont(&fonts::efontCN_10);
    if (_g_edit) {
        cv.setTextColor(TFT_CYAN);
        cv.setCursor(3, bot + 2);
        std::string s = "f(x)=" + _gf.t;
        while (!s.empty() && cv.textWidth(s.c_str()) > w - 6) s.pop_back();
        cv.print(s.c_str());
        if ((GetHAL().millis() / 500) % 2 == 0) {
            int cx = 3 + cv.textWidth(s.c_str());
            if (cx > w - 2) cx = w - 2;
            cv.fillRect(cx, bot + 1, 1, 11, TFT_CYAN);
        }
    } else {
        cv.setTextColor(TFT_DARKGREY);
        cv.setCursor(3, bot + 2);
        std::string s = "f(x)=" + _gf.t;
        while (!s.empty() && cv.textWidth(s.c_str()) > w - 6) s.pop_back();
        cv.print(s.c_str());
    }
    // 编辑/操作提示放顶部右侧小字
    cv.setFont(&fonts::efontCN_10);
    cv.setTextColor(TFT_DARKGREY);
    const char* gh = _g_edit ? "Enter确认 Esc/G0退" : "Enter改f(x) 方向平移 +-缩放 0复位 t描点";
    int ghw = cv.textWidth(gh);
    if (ghw < w - 60) {
        cv.setCursor(w - 3 - ghw, 1);
        cv.print(gh);
    }
}

void AppFunctionCalculator::draw_solve()
{
    auto& cv = GetHAL().canvas();
    int w    = cv.width();
    draw_topbar("方程");

    // 类型选择器
    cv.setFont(&fonts::efontCN_12);
    cv.setTextColor(_s_sel == 0 ? TFT_CYAN : TFT_DARKGREY);
    cv.setCursor(3, 17);
    cv.print(_sub == 0 ? "类型:一元二次 ax^2+bx+c=0" : "类型:f(x)=0 区间求根");
    if (_s_sel == 0) cv.drawRect(1, 15, w - 2, 16, 0x2104);

    if (_sub == 0) {
        draw_field(5, 36, "a=", _qa, _s_sel == 1, w - 10);
        draw_field(5, 54, "b=", _qb, _s_sel == 2, w - 10);
        draw_field(5, 72, "c=", _qc, _s_sel == 3, w - 10);
    } else {
        draw_field(5, 36, "f(x)=", _sf, _s_sel == 1, w - 10);
        draw_field(5, 54, "lo=", _slo, _s_sel == 2, 100);
        draw_field(120, 54, "hi=", _shi, _s_sel == 3, 100);
    }

    // 结果
    cv.setFont(&fonts::efontCN_10);
    int ry = 92;
    for (size_t i = 0; i < _s_res.size() && i < 3; i++) {
        cv.setTextColor(_s_ok ? TFT_GREEN : TFT_RED);
        cv.setCursor(5, ry);
        cv.print(_s_res[i].c_str());
        ry += 11;
    }
    if (_s_res.size() > 3) {
        cv.setTextColor(TFT_DARKGREY);
        cv.setCursor(5, ry);
        char more[24];
        snprintf(more, sizeof(more), "...共 %d 条", (int)_s_res.size());
        cv.print(more);
    }

    draw_hint("上下选框 左右改/换类型 Enter=求解 Esc/G0退");
}

void AppFunctionCalculator::draw_calcu()
{
    auto& cv = GetHAL().canvas();
    int w    = cv.width();
    draw_topbar("微积分");

    draw_field(5, 18, "f(x)=", _df, _d_sel == 0, w - 10);
    draw_field(5, 36, "x0=", _dx0, _d_sel == 1, w - 10);
    draw_field(5, 54, "a=", _da, _d_sel == 2, w - 10);
    draw_field(5, 72, "b=", _db, _d_sel == 3, w - 10);

    cv.setFont(&fonts::efontCN_10);
    int ry = 92;
    for (size_t i = 0; i < _d_res.size() && i < 3; i++) {
        cv.setTextColor(i == 2 ? TFT_DARKGREY : (_d_ok ? TFT_GREEN : TFT_RED));
        cv.setCursor(5, ry);
        cv.print(_d_res[i].c_str());
        ry += 11;
    }

    draw_hint("上下选框 Enter=求导+积分 Tab=模式 Esc/G0退");
}
