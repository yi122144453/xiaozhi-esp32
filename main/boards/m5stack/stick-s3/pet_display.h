/* ============================================================
 *  PetDisplay v2 · 把电子宠物画进小智 AI 语音固件
 * ------------------------------------------------------------
 *  继承 SpiLcdDisplay：
 *    - 保留小智原有的状态栏（顶部）和字幕（底部）
 *    - 把中间的"表情区"换成我们自己的宠物
 *    - SetEmotion() 不再显示 emoji，而是驱动宠物表情
 *    - SetChatMessage() 每来一轮回答，宠物就跟着动嘴、成长
 *
 *  v2 变更（更明显的养成感）：
 *    1. 三阶段造型差异明显：
 *       EGG  = 奶白色大蛋 + 斑点 + 左右摇晃 + 睡眼
 *       BABY = 小号粉圆团子 + 大腮红（无耳朵）
 *       TEEN = 更大的身体 + 竖起两只耳朵 + 深粉
 *    2. 整体体型缩小，不再顶满屏幕
 *    3. 成长加速：聊 3 轮孵化，12 轮长成少年
 *    4. 喂食/陪玩强反馈：吃相动画 + 爱心飘浮 + YUM!/HAHA! 飘字
 *    5. AI 情绪更明显：开心=爱心+小跳，兴奋=三颗爱心+大跳，
 *       难过=掉眼泪，思考=...；情绪持续 3 秒
 *    6. 主动提示：饿了喊 HUNGRY!、困了喊 SLEEPY..、
 *       睡着喊 Zzz...、升阶段白光闪烁 + LV UP!
 *
 *  放置位置：main/boards/m5stack/stick-s3/pet_display.h / .cc
 * ============================================================ */
#ifndef PET_DISPLAY_H
#define PET_DISPLAY_H

#include "display/lcd_display.h"
#include "settings.h"

#include <lvgl.h>
#include <cstdint>
#include <mutex>

class PetDisplay : public SpiLcdDisplay {
public:
    PetDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
               int width, int height, int offset_x, int offset_y,
               bool mirror_x, bool mirror_y, bool swap_xy);
    ~PetDisplay();

    // ---- 覆盖小智的显示接口 ----
    bool SetEmotion(const char* emotion) override;                 // 情绪 → 宠物表情
    void SetChatMessage(const char* role, const char* content) override;
    void SetupUI() override;                                       // 建我们的宠物层

    // ---- 给孩子用的动作（在 board 的按键回调里调用）----
    void FeedPet();        // 喂食（绑 B 键长按；睡着会被叫醒吃饭）
    void PlayWithPet();    // 陪玩（绑 A 键长按）
    void ToggleSleep();    // 睡觉/叫醒

private:
    // 表情特效
    enum Fx { FX_NONE = 0, FX_EAT, FX_PLAY, FX_HAPPY, FX_SAD, FX_THINK };

    void BuildPetUI();          // 创建 LVGL 对象（只在 UI 线程调）
    void Tick();                // 40ms 动画回调（LVGL 任务里跑）
    void ApplyStageLook();      // 按阶段重排造型（只在 UI 线程调）
    void SaveStats();
    void LoadStats();
    void SetMouth(int h, bool visible);
    void RefreshBars();

    // 跨线程投递（写端任意线程，读端只有 LVGL Tick）
    void PushFloatLocked(const char* txt, uint32_t color);  // 需持有 fx_mutex_
    void SpawnHeartsLocked(int count);                      // 需持有 fx_mutex_
    void SpawnFloatNow(const char* txt, uint32_t color);    // 仅 UI 线程
    void SpawnHeartsNow(int count);                         // 仅 UI 线程

    // ---- 数值 ----
    int hunger_ = 70;
    int mood_   = 70;
    int energy_ = 80;
    int stage_  = 0;            // 0 蛋 / 1 幼年 / 2 少年
    int rounds_ = 0;            // 累计对话轮数（用来长大）
    int fx_     = FX_NONE;
    uint32_t fx_until_ = 0;
    bool sleeping_ = false;
    bool talking_  = false;
    uint32_t talk_until_ = 0;
    uint32_t last_tick_ms_ = 0;
    uint32_t last_save_ms_ = 0;
    uint32_t flash_until_ = 0;  // 升阶段白光闪烁

    // ---- 当前阶段的造型基线（Tick 会加上呼吸/摇晃偏移）----
    int b_x_ = 41, b_y_ = 22, b_w_ = 53, b_h_ = 64;
    int eye_y_ = 48, eye_x_l_ = 58, eye_x_r_ = 72, eye_w_ = 8, eye_h_ = 3;
    int mouth_y_ = 62;
    int cheek_y_ = 56, cheek_x_l_ = 43, cheek_x_r_ = 85, cheek_s_ = 8;
    int ear_y_ = 12, ear_x_l_ = 38, ear_x_r_ = 80, ear_s_ = 17;
    int shadow_y_ = 88, shadow_w_ = 46;
    int body_col_ = 0xFFF1D6;

    // ---- 跨线程待办（fx_mutex_ 保护）----
    std::mutex fx_mutex_;
    int  pending_stage_  = -1;   // >=0 表示要升到该阶段
    bool bars_dirty_     = false;
    int  pending_hearts_ = 0;    // 要撒的爱心组数
    char pend_txt_[4][16];
    uint32_t pend_col_[4] = {0, 0, 0, 0};
    bool pend_used_[4]   = {false, false, false, false};
    int  pend_wr_        = 0;

    // ---- LVGL 对象 ----
    lv_obj_t* pet_root_ = nullptr;
    lv_obj_t* body_ = nullptr;
    lv_obj_t* shadow_ = nullptr;
    lv_obj_t* eye_l_ = nullptr;
    lv_obj_t* eye_r_ = nullptr;
    lv_obj_t* mouth_ = nullptr;
    lv_obj_t* cheek_l_ = nullptr;
    lv_obj_t* cheek_r_ = nullptr;
    lv_obj_t* ear_l_ = nullptr;
    lv_obj_t* ear_r_ = nullptr;
    lv_obj_t* tear_ = nullptr;
    lv_obj_t* flash_ = nullptr;             // 升阶段白光
    lv_obj_t* spot_[2] = {nullptr, nullptr};     // 蛋壳斑点
    lv_obj_t* float_label_[2] = {nullptr, nullptr};  // 飘字
    lv_obj_t* heart_[3][3] = {};             // 3 组像素爱心
    lv_obj_t* age_label_ = nullptr;
    lv_obj_t* bar_[3] = {nullptr, nullptr, nullptr};
    lv_obj_t* bar_label_[3] = {nullptr, nullptr, nullptr};
    uint32_t float_start_[2] = {0, 0};
    uint32_t heart_start_[3] = {0, 0, 0};
    lv_timer_t* timer_ = nullptr;
};

#endif  // PET_DISPLAY_H
