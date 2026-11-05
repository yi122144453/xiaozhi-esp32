/* ============================================================
 *  PetDisplay · 把电子宠物画进小智 AI 语音固件
 * ------------------------------------------------------------
 *  继承 SpiLcdDisplay：
 *    - 保留小智原有的状态栏（顶部）和字幕（底部）
 *    - 把中间的"表情区"换成我们自己的宠物（身体/眼睛/嘴/三根状态条）
 *    - SetEmotion() 不再显示 emoji，而是驱动宠物表情
 *    - SetChatMessage() 每来一轮回答，宠物就"吃一口饭"（成长）
 *
 *  放置位置：main/boards/m5stack/stick-s3/pet_display.h / .cc
 *  然后按 README 改 board 文件里 3 行即可。
 * ============================================================ */
#ifndef PET_DISPLAY_H
#define PET_DISPLAY_H

#include "display/lcd_display.h"
#include "settings.h"

#include <lvgl.h>
#include <cstdint>

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
    void FeedPet();        // 喂食（建议绑 B 键长按）
    void PlayWithPet();    // 陪玩（建议绑 B 键双击）
    void ToggleSleep();    // 睡觉/叫醒

private:
    // 表情
    enum Fx { FX_NONE = 0, FX_EAT, FX_PLAY, FX_HAPPY, FX_SAD, FX_THINK };

    void BuildPetUI();          // 创建 LVGL 对象
    void Tick();                // 40ms 动画回调（LVGL 任务里跑，天然线程安全）
    void ApplyStageLook();      // 长大后换造型
    void SaveStats();
    void LoadStats();
    void SetMouth(int h, bool visible);
    void RefreshBars();

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
    lv_obj_t* age_label_ = nullptr;
    lv_obj_t* bar_[3] = {nullptr, nullptr, nullptr};
    lv_obj_t* bar_label_[3] = {nullptr, nullptr, nullptr};
    lv_timer_t* timer_ = nullptr;
};

#endif  // PET_DISPLAY_H
