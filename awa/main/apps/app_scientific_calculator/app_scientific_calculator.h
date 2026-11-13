#pragma once
#include <mooncake.h>
#include <string>
#include <vector>

/*
 * 科学计算器(表达式输入)
 * 直接敲键盘写表达式, 回车求值; 实时预览结果
 * 支持: + - * / % ^ ( )  !   以及 sin cos tan asin acos atan
 *       sinh cosh tanh log ln log2 sqrt cbrt abs exp floor ceil round trunc
 *       min(a,b) max(a,b) pow(a,b) mod(a,b) hypot(a,b) atan2(a,b)
 *       常量 pi e ans;  Tab 切换 DEG/RAD
 */
class AppScientificCalculator : public mooncake::AppAbility {
public:
    AppScientificCalculator();
    ~AppScientificCalculator();
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    int _key_event_slot_id = -1;

    std::string _expr;
    int _caret = 0;
    std::string _result = "0";
    std::string _err;
    bool _ok = true;
    double _val = 0;

    bool _deg = true;
    double _ans = 0;
    bool _has_ans = false;
    bool _just_eval = false;

    std::vector<std::string> _hist;
    int _hist_idx = -1;

    void insert_char(char c);
    void backspace();
    void evaluate();
    void update_preview();
    void history_up();
    void history_down();
    void draw();
};
