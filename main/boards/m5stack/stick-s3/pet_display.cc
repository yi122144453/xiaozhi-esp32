/* ============================================================
 *  PetDisplay v2 · 电子宠物 × 小智语音 合体实现
 *  屏幕：135 x 240（竖屏，SpiLcdDisplay 原生方向）
 *  布局：顶部状态栏（小智原生） / 中间宠物区 / 底部字幕（小智原生）
 *
 *  线程模型：
 *    - 所有 LVGL 对象操作只发生在 SetupUI() 和 Tick()（LVGL 任务）
 *    - SetEmotion/SetChatMessage/按键回调 在其他线程，只改普通
 *      int/bool 并把"要做的事"放进 fx_mutex_ 保护的待办队列，
 *      由 Tick 统一消费 —— 不碰显示锁，不会死锁
 * ============================================================ */
#include "pet_display.h"

#include <esp_log.h>
#include <cstring>
#include <cstdio>
#include <cmath>

#define TAG "PetDisplay"

// ---------------- 配色 ----------------
static const uint32_t C_BG      = 0x0E101A;
static const uint32_t C_SHADOW  = 0x181A26;
static const uint32_t C_EYE     = 0x1A1C2A;
static const uint32_t C_TEAR    = 0x60BEFA;
static const uint32_t C_DIM     = 0x7880A0;
static const uint32_t C_BAR_FD  = 0x68D696;
static const uint32_t C_BAR_MD  = 0xFAC45C;
static const uint32_t C_BAR_EN  = 0x60BEFA;

// 三阶段身体颜色：蛋=奶白 / 幼年=浅粉 / 少年=深粉
static const uint32_t C_EGG_BODY  = 0xFFF1D6;
static const uint32_t C_BABY_BODY = 0xFF9AA8;
static const uint32_t C_TEEN_BODY = 0xFF8496;
static const uint32_t C_SPOT      = 0xE8CFA0;   // 蛋壳斑点
static const uint32_t C_HEART     = 0xFF5A7A;   // 爱心
static const uint32_t C_CHEEK     = 0xFFA8B4;

// 宠物区几何（相对 pet_root_，尺寸 135x150）
static const int PET_W = 135;
static const int PET_H = 150;
static const int MOUTH_W = 16;
static const int MOUTH_X = (PET_W - MOUTH_W) / 2;
static const int BAR_X = 26, BAR_W = 100, BAR_H = 9;
static const int BAR_Y[3] = {98, 114, 130};

// 爱心组 x 基线（组内：圆+圆+方）
static const int HEART_X[3] = {40, 62, 84};
static const int HEART_Y = 70;

static inline int clamp100(int v) { return v < 0 ? 0 : (v > 100 ? 100 : v); }

// ---------------- 小工具 ----------------
static lv_obj_t* make_box(lv_obj_t* parent, int x, int y, int w, int h,
                          uint32_t color, int radius) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_border_width(o, 0, 0);
    return o;
}

// ============================================================
//  构造 / 析构
// ============================================================
PetDisplay::PetDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                       int width, int height, int offset_x, int offset_y,
                       bool mirror_x, bool mirror_y, bool swap_xy)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y,
                    mirror_x, mirror_y, swap_xy) {
    LoadStats();
    ESP_LOGI(TAG, "PetDisplay v2 created %dx%d, stage=%d rounds=%d", width, height, stage_, rounds_);
}

PetDisplay::~PetDisplay() {
    if (timer_) {
        lv_timer_delete(timer_);
        timer_ = nullptr;
    }
}

// ============================================================
//  存档（复用小智自带的 Settings，底层就是 NVS）
// ============================================================
void PetDisplay::LoadStats() {
    Settings s("pet", true);
    hunger_ = s.GetInt("hunger", 70);
    mood_   = s.GetInt("mood", 70);
    energy_ = s.GetInt("energy", 80);
    stage_  = s.GetInt("stage", 0);
    rounds_ = s.GetInt("rounds", 0);
}

void PetDisplay::SaveStats() {
    Settings s("pet", true);
    s.SetInt("hunger", hunger_);
    s.SetInt("mood", mood_);
    s.SetInt("energy", energy_);
    s.SetInt("stage", stage_);
    s.SetInt("rounds", rounds_);
}

// ============================================================
//  建界面
// ============================================================
void PetDisplay::SetupUI() {
    SpiLcdDisplay::SetupUI();          // 先让小智把自己的状态栏/字幕建好
    if (pet_root_) return;             // 只建一次

    DisplayLockGuard lock(this);
    if (!lock) {
        ESP_LOGE(TAG, "lock failed in SetupUI");
        return;
    }

    // 1) 底色改成我们的深色
    if (container_) {
        lv_obj_set_style_bg_color(container_, lv_color_hex(C_BG), 0);
    }

    // 2) 藏掉小智原生的大表情区（我们不显示 emoji 了）
    if (emoji_box_)   lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    if (emoji_label_) lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    if (emoji_image_) lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);

    // 3) 宠物层：整宽、只占中间一条
    pet_root_ = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(pet_root_);
    lv_obj_clear_flag(pet_root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(pet_root_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(pet_root_, 0, 42);
    lv_obj_set_size(pet_root_, PET_W, PET_H);
    lv_obj_set_style_bg_opa(pet_root_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pet_root_, 0, 0);

    BuildPetUI();
    lv_obj_move_foreground(pet_root_);

    // 4) 40ms 定时器跑动画
    timer_ = lv_timer_create([](lv_timer_t* t) {
        static_cast<PetDisplay*>(lv_timer_get_user_data(t))->Tick();
    }, 40, this);

    ApplyStageLook();
    RefreshBars();
    ESP_LOGI(TAG, "pet UI built");
}

void PetDisplay::BuildPetUI() {
    // 耳朵先建，身体后建：耳朵下半截会被身体盖住
    ear_l_ = make_box(pet_root_, 38, 12, 17, 17, C_TEEN_BODY, LV_RADIUS_CIRCLE);
    ear_r_ = make_box(pet_root_, 80, 12, 17, 17, C_TEEN_BODY, LV_RADIUS_CIRCLE);
    lv_obj_add_flag(ear_l_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ear_r_, LV_OBJ_FLAG_HIDDEN);

    shadow_ = make_box(pet_root_, 45, 88, 46, 8, C_SHADOW, 4);
    body_   = make_box(pet_root_, 41, 22, 53, 64, C_EGG_BODY, 26);

    // 蛋壳斑点（只在蛋阶段显示）
    spot_[0] = make_box(pet_root_, 51, 38, 9, 9, C_SPOT, LV_RADIUS_CIRCLE);
    spot_[1] = make_box(pet_root_, 75, 60, 7, 7, C_SPOT, LV_RADIUS_CIRCLE);
    lv_obj_add_flag(spot_[0], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(spot_[1], LV_OBJ_FLAG_HIDDEN);

    eye_l_ = make_box(pet_root_, 58, 48, 8, 3, C_EYE, 2);
    eye_r_ = make_box(pet_root_, 72, 48, 8, 3, C_EYE, 2);

    cheek_l_ = make_box(pet_root_, 43, 56, 8, 8, C_CHEEK, LV_RADIUS_CIRCLE);
    cheek_r_ = make_box(pet_root_, 85, 56, 8, 8, C_CHEEK, LV_RADIUS_CIRCLE);
    lv_obj_add_flag(cheek_l_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(cheek_r_, LV_OBJ_FLAG_HIDDEN);

    mouth_ = make_box(pet_root_, MOUTH_X, 62, MOUTH_W, 5, C_EYE, 3);

    tear_ = make_box(pet_root_, 58, 62, 5, 5, C_TEAR, LV_RADIUS_CIRCLE);
    lv_obj_add_flag(tear_, LV_OBJ_FLAG_HIDDEN);

    // 左上角：阶段 + 轮数（纯 ASCII，不依赖中文字体）
    age_label_ = lv_label_create(pet_root_);
    lv_obj_set_pos(age_label_, 4, 0);
    lv_obj_set_style_text_color(age_label_, lv_color_hex(C_DIM), 0);
    lv_label_set_text(age_label_, "EGG 0");

    // 三根状态条
    const char* names[3] = {"FD", "MD", "EN"};
    for (int i = 0; i < 3; i++) {
        bar_label_[i] = lv_label_create(pet_root_);
        lv_obj_set_pos(bar_label_[i], 4, BAR_Y[i] - 2);
        lv_obj_set_style_text_color(bar_label_[i], lv_color_hex(C_DIM), 0);
        lv_label_set_text(bar_label_[i], names[i]);

        bar_[i] = lv_bar_create(pet_root_);
        lv_obj_remove_style_all(bar_[i]);
        lv_obj_set_pos(bar_[i], BAR_X, BAR_Y[i]);
        lv_obj_set_size(bar_[i], BAR_W, BAR_H);
        lv_bar_set_range(bar_[i], 0, 100);
        lv_obj_set_style_radius(bar_[i], 3, 0);
        lv_obj_set_style_bg_opa(bar_[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(bar_[i], lv_color_hex(C_SHADOW), 0);
        lv_obj_set_style_radius(bar_[i], 3, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(bar_[i], LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(bar_[i], lv_color_hex(C_BAR_FD), LV_PART_INDICATOR);
    }

    // 升阶段白光（盖住整个宠物区，平时隐藏）
    flash_ = make_box(pet_root_, 0, 0, PET_W, PET_H, 0xFFFFFF, 8);
    lv_obj_set_style_opa(flash_, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(flash_, LV_OBJ_FLAG_HIDDEN);

    // 飘字 x2
    for (int i = 0; i < 2; i++) {
        float_label_[i] = lv_label_create(pet_root_);
        lv_label_set_text(float_label_[i], "");
        lv_obj_set_style_text_color(float_label_[i], lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_pos(float_label_[i], 46, 58);
        lv_obj_add_flag(float_label_[i], LV_OBJ_FLAG_HIDDEN);
    }

    // 3 组像素爱心（圆+圆+方），平时隐藏
    for (int g = 0; g < 3; g++) {
        int hx = HEART_X[g];
        heart_[g][0] = make_box(pet_root_, hx,     HEART_Y,     9, 9, C_HEART, LV_RADIUS_CIRCLE);
        heart_[g][1] = make_box(pet_root_, hx + 9, HEART_Y,     9, 9, C_HEART, LV_RADIUS_CIRCLE);
        heart_[g][2] = make_box(pet_root_, hx + 4, HEART_Y + 5, 9, 9, C_HEART, 2);
        for (int j = 0; j < 3; j++) lv_obj_add_flag(heart_[g][j], LV_OBJ_FLAG_HIDDEN);
    }
}

// ============================================================
//  三阶段造型：把所有部件挪到该阶段的基线位置
//  （只在 UI 线程调用）
// ============================================================
void PetDisplay::ApplyStageLook() {
    if (!body_) return;
    bool show_ears = false, show_cheeks = false, show_spots = false;

    if (stage_ <= 0) {                     // EGG：奶白大蛋 + 斑点 + 睡眼
        b_x_ = 41; b_y_ = 22; b_w_ = 53; b_h_ = 64;
        eye_y_ = 48; eye_x_l_ = 58; eye_x_r_ = 72; eye_w_ = 8; eye_h_ = 3;
        mouth_y_ = 62;
        cheek_y_ = 56; cheek_x_l_ = 43; cheek_x_r_ = 85; cheek_s_ = 8;
        shadow_y_ = 88; shadow_w_ = 46;
        body_col_ = C_EGG_BODY;
        show_spots = true;
    } else if (stage_ == 1) {              // BABY：小圆团子 + 大腮红
        b_x_ = 36; b_y_ = 26; b_w_ = 63; b_h_ = 56;
        eye_y_ = 46; eye_x_l_ = 55; eye_x_r_ = 74; eye_w_ = 9; eye_h_ = 9;
        mouth_y_ = 64;
        cheek_y_ = 56; cheek_x_l_ = 43; cheek_x_r_ = 85; cheek_s_ = 9;
        shadow_y_ = 84; shadow_w_ = 52;
        body_col_ = C_BABY_BODY;
        show_cheeks = true;
    } else {                               // TEEN：大身体 + 两只耳朵
        b_x_ = 32; b_y_ = 24; b_w_ = 71; b_h_ = 62;
        eye_y_ = 44; eye_x_l_ = 51; eye_x_r_ = 76; eye_w_ = 11; eye_h_ = 11;
        mouth_y_ = 66;
        cheek_y_ = 58; cheek_x_l_ = 40; cheek_x_r_ = 88; cheek_s_ = 7;
        ear_y_ = 12; ear_x_l_ = 38; ear_x_r_ = 80; ear_s_ = 17;
        shadow_y_ = 88; shadow_w_ = 58;
        body_col_ = C_TEEN_BODY;
        show_ears = true; show_cheeks = true;
    }

    // 身体
    lv_obj_set_pos(body_, b_x_, b_y_);
    lv_obj_set_size(body_, b_w_, b_h_);
    lv_obj_set_style_radius(body_, (b_w_ < b_h_ ? b_w_ : b_h_) / 2, 0);
    lv_obj_set_style_bg_color(body_, lv_color_hex(body_col_), 0);
    // 耳朵颜色跟随身体
    lv_obj_set_style_bg_color(ear_l_, lv_color_hex(body_col_), 0);
    lv_obj_set_style_bg_color(ear_r_, lv_color_hex(body_col_), 0);

    // 影子
    lv_obj_set_pos(shadow_, (PET_W - shadow_w_) / 2, shadow_y_);
    lv_obj_set_size(shadow_, shadow_w_, 8);

    // 眼睛（蛋阶段是细缝）
    lv_obj_set_size(eye_l_, eye_w_, eye_h_);
    lv_obj_set_size(eye_r_, eye_w_, eye_h_);
    lv_obj_set_style_radius(eye_l_, (eye_h_ > 5) ? LV_RADIUS_CIRCLE : 2, 0);
    lv_obj_set_style_radius(eye_r_, (eye_h_ > 5) ? LV_RADIUS_CIRCLE : 2, 0);

    // 耳朵 / 腮红 / 斑点 位置 + 显隐
    lv_obj_set_pos(ear_l_, ear_x_l_, ear_y_);
    lv_obj_set_pos(ear_r_, ear_x_r_, ear_y_);
    lv_obj_set_size(ear_l_, ear_s_, ear_s_);
    lv_obj_set_size(ear_r_, ear_s_, ear_s_);
    lv_obj_set_pos(cheek_l_, cheek_x_l_, cheek_y_);
    lv_obj_set_pos(cheek_r_, cheek_x_r_, cheek_y_);
    lv_obj_set_size(cheek_l_, cheek_s_, cheek_s_);
    lv_obj_set_size(cheek_r_, cheek_s_, cheek_s_);
    lv_obj_set_pos(spot_[0], b_x_ + 10, b_y_ + 16);
    lv_obj_set_pos(spot_[1], b_x_ + 34, b_y_ + 38);

    auto set_vis = [&](lv_obj_t* o, bool v) {
        if (!o) return;
        if (v) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
        else   lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    };
    set_vis(ear_l_, show_ears);  set_vis(ear_r_, show_ears);
    set_vis(cheek_l_, show_cheeks); set_vis(cheek_r_, show_cheeks);
    set_vis(spot_[0], show_spots); set_vis(spot_[1], show_spots);

    // 名牌
    char buf[24];
    const char* nm = (stage_ == 0) ? "EGG" : (stage_ == 1 ? "BABY" : "TEEN");
    snprintf(buf, sizeof(buf), "%s %d", nm, rounds_);
    lv_label_set_text(age_label_, buf);
}

void PetDisplay::RefreshBars() {
    if (!bar_[0]) return;
    lv_bar_set_value(bar_[0], hunger_, LV_ANIM_ON);
    lv_bar_set_value(bar_[1], mood_, LV_ANIM_ON);
    lv_bar_set_value(bar_[2], energy_, LV_ANIM_ON);

    lv_obj_set_style_bg_color(bar_[0], lv_color_hex(hunger_ < 20 ? 0xF87070 : C_BAR_FD), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar_[1], lv_color_hex(mood_ < 20 ? 0xF87070 : C_BAR_MD), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar_[2], lv_color_hex(energy_ < 20 ? 0xF87070 : C_BAR_EN), LV_PART_INDICATOR);
}

void PetDisplay::SetMouth(int h, bool visible) {
    if (!mouth_) return;
    if (!visible) {
        lv_obj_add_flag(mouth_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(mouth_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_height(mouth_, h);
    lv_obj_set_style_radius(mouth_, (h > 8) ? LV_RADIUS_CIRCLE : 3, 0);
}

// ============================================================
//  特效投递
// ============================================================
void PetDisplay::PushFloatLocked(const char* txt, uint32_t color) {
    snprintf(pend_txt_[pend_wr_], sizeof(pend_txt_[0]), "%s", txt ? txt : "");
    pend_col_[pend_wr_] = color;
    pend_used_[pend_wr_] = true;
    pend_wr_ = (pend_wr_ + 1) % 4;
}

void PetDisplay::SpawnHeartsLocked(int count) {
    if (count > 3) count = 3;
    pending_hearts_ = count;
}

void PetDisplay::SpawnFloatNow(const char* txt, uint32_t color) {
    if (!float_label_[0] || !float_label_[1]) return;
    static int next = 0;
    int s = next; next ^= 1;
    lv_label_set_text(float_label_[s], txt);
    lv_obj_set_style_text_color(float_label_[s], lv_color_hex(color), 0);
    lv_obj_clear_flag(float_label_[s], LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(float_label_[s], LV_OPA_COVER, 0);
    float_start_[s] = lv_tick_get();
}

void PetDisplay::SpawnHeartsNow(int count) {
    if (!heart_[0][0]) return;
    if (count > 3) count = 3;
    uint32_t now = lv_tick_get();
    for (int g = 0; g < count; g++) {
        heart_start_[g] = now + g * 230;   // 一组接一组跳出来
    }
}

// ============================================================
//  动画心跳：40ms 一次（只在 LVGL 任务里跑）
// ============================================================
void PetDisplay::Tick() {
    if (!pet_root_) return;

    uint32_t now = lv_tick_get();
    float t = now / 1000.0f;

    // ---- 消费其他线程投递的待办 ----
    {
        std::lock_guard<std::mutex> lk(fx_mutex_);
        if (pending_stage_ >= 0) {
            stage_ = pending_stage_;
            pending_stage_ = -1;
            ApplyStageLook();
            flash_until_ = now + 900;
            SpawnFloatNow("LV UP!", 0xFFFFFF);
            SpawnHeartsNow(3);
            fx_ = FX_HAPPY;
            fx_until_ = now + 2500;
            SaveStats();
            ESP_LOGI(TAG, "stage up -> %d", stage_);
        }
        if (bars_dirty_) { bars_dirty_ = false; RefreshBars(); }
        if (pending_hearts_ > 0) { SpawnHeartsNow(pending_hearts_); pending_hearts_ = 0; }
        for (int i = 0; i < 4; i++) {
            if (pend_used_[i]) {
                SpawnFloatNow(pend_txt_[i], pend_col_[i]);
                pend_used_[i] = false;
            }
        }
    }

    // ---- 数值衰减：每 1 秒结算一次 ----
    if (now - last_tick_ms_ >= 1000) {
        last_tick_ms_ = now;
        if (!sleeping_) {
            if ((now / 1000) % 10 == 0) hunger_ = clamp100(hunger_ - 1);
            if ((now / 1000) % 15 == 0) energy_ = clamp100(energy_ - 1);
            if ((now / 1000) % 20 == 0) mood_ = clamp100(mood_ - (hunger_ < 20 ? 2 : 1));
            if (energy_ <= 0) {
                sleeping_ = true;
                SpawnFloatNow("Zzz...", 0x9AA8C8);
            }
            // 主动提醒：饿了/困了，每 15 秒喊一次
            if (!sleeping_ && (now / 1000) % 15 == 7) {
                if (hunger_ < 20)       SpawnFloatNow("HUNGRY!", 0xF87070);
                else if (energy_ < 15)  SpawnFloatNow("SLEEPY..", 0x9AA8C8);
            }
        } else {
            energy_ = clamp100(energy_ + 1);
            hunger_ = clamp100(hunger_ - 1);
            if (energy_ >= 100) {
                sleeping_ = false;
                SpawnFloatNow("MORNING!", 0x68D696);
            }
        }
        RefreshBars();
    }

    // ---- 30 秒存一次盘 ----
    if (now - last_save_ms_ > 30000) {
        last_save_ms_ = now;
        SaveStats();
    }

    // ---- 呼吸 / 跳跃 / 蛋的摇晃 ----
    int bob = sleeping_ ? (int)(sinf(t * 1.0f) * 1.0f)
                        : (int)(sinf(t * 1.8f) * 2.0f);
    if (fx_ == FX_PLAY) bob = -(int)(fabsf(sinf(t * 9.0f)) * 9.0f);
    if (fx_ == FX_HAPPY) bob = -(int)(fabsf(sinf(t * 5.0f)) * 5.0f);
    int ox = (stage_ == 0 && !sleeping_) ? (int)(sinf(t * 3.0f) * 2.0f) : 0;

    // ---- 眨眼：每 4 秒闭 120ms（蛋阶段眼睛本来就是缝，不眨）----
    bool blink = !sleeping_ && (eye_h_ > 5) && ((now % 4000) < 120);
    int eh = (sleeping_ || blink) ? 3 : eye_h_;

    lv_obj_set_x(body_, b_x_ + ox);
    lv_obj_set_x(eye_l_, eye_x_l_ + ox);
    lv_obj_set_x(eye_r_, eye_x_r_ + ox);
    lv_obj_set_x(cheek_l_, cheek_x_l_ + ox);
    lv_obj_set_x(cheek_r_, cheek_x_r_ + ox);
    lv_obj_set_x(mouth_, MOUTH_X + ox);
    lv_obj_set_x(ear_l_, ear_x_l_ + ox);
    lv_obj_set_x(ear_r_, ear_x_r_ + ox);
    lv_obj_set_x(tear_, eye_x_l_ + ox);
    lv_obj_set_x(spot_[0], b_x_ + 10 + ox);
    lv_obj_set_x(spot_[1], b_x_ + 34 + ox);

    lv_obj_set_y(body_, b_y_ + bob);
    lv_obj_set_y(eye_l_, eye_y_ + bob + (blink ? 4 : 0));
    lv_obj_set_y(eye_r_, eye_y_ + bob + (blink ? 4 : 0));
    lv_obj_set_y(cheek_l_, cheek_y_ + bob);
    lv_obj_set_y(cheek_r_, cheek_y_ + bob);
    lv_obj_set_y(mouth_, mouth_y_ + bob);
    lv_obj_set_y(tear_, eye_y_ + 14 + bob);
    lv_obj_set_y(ear_l_, ear_y_ + bob);
    lv_obj_set_y(ear_r_, ear_y_ + bob);
    lv_obj_set_y(spot_[0], b_y_ + 16 + bob);
    lv_obj_set_y(spot_[1], b_y_ + 38 + bob);

    lv_obj_set_height(eye_l_, eh);
    lv_obj_set_height(eye_r_, eh);

    // ---- 嘴型随状态 ----
    bool talking = talking_ && (int32_t)(talk_until_ - now) > 0;
    if (sleeping_) {
        SetMouth(3, true);
    } else if (fx_ == FX_SAD) {
        SetMouth(4, false);
        lv_obj_clear_flag(tear_, LV_OBJ_FLAG_HIDDEN);
    } else if (fx_ == FX_EAT || talking) {
        int h = 8 + (int)(fabsf(sinf(t * 12.0f)) * 5.0f);   // 一开一合
        SetMouth(h, true);
    } else {
        SetMouth(5, true);
    }
    if (fx_ != FX_SAD) lv_obj_add_flag(tear_, LV_OBJ_FLAG_HIDDEN);

    // ---- 飘字动画：上浮 + 渐隐，1.3 秒 ----
    for (int i = 0; i < 2; i++) {
        if (!float_label_[i] || float_start_[i] == 0) continue;
        uint32_t el = now - float_start_[i];
        if (el > 1300) {
            lv_obj_add_flag(float_label_[i], LV_OBJ_FLAG_HIDDEN);
            float_start_[i] = 0;
            continue;
        }
        lv_obj_set_y(float_label_[i], 58 - (int)(el / 40));
        lv_obj_set_style_opa(float_label_[i], 255 - (int)(255 * el / 1300), 0);
    }

    // ---- 爱心动画：上浮 + 渐隐，1.1 秒 ----
    for (int g = 0; g < 3; g++) {
        if (!heart_[g][0]) continue;
        int32_t el = (int32_t)(now - heart_start_[g]);
        if (el < 0) continue;                       // 还没轮到这组
        if (el > 1100) {
            if (heart_start_[g] == 0 || heart_start_[g] > now) continue;
            for (int j = 0; j < 3; j++) lv_obj_add_flag(heart_[g][j], LV_OBJ_FLAG_HIDDEN);
            heart_start_[g] = 0;
            continue;
        }
        int dy = (int)(el / 40);
        int o = 255 - (int)(255 * el / 1100);
        for (int j = 0; j < 3; j++) {
            lv_obj_clear_flag(heart_[g][j], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_opa(heart_[g][j], o, 0);
        }
        lv_obj_set_y(heart_[g][0], HEART_Y - dy);
        lv_obj_set_y(heart_[g][1], HEART_Y - dy);
        lv_obj_set_y(heart_[g][2], HEART_Y + 5 - dy);
    }

    // ---- 升阶段白光闪烁 ----
    if (flash_) {
        if ((int32_t)(flash_until_ - now) > 0) {
            int ph = ((flash_until_ - now) / 150) % 2;
            lv_obj_clear_flag(flash_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_opa(flash_, ph ? 150 : 40, 0);
        } else {
            lv_obj_add_flag(flash_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // ---- 特效到期 ----
    if (fx_ != FX_NONE && (int32_t)(now - fx_until_) > 0) {
        fx_ = FX_NONE;
    }
}

// ============================================================
//  小智的显示接口（其他线程调用：只改数值 + 投递待办）
// ============================================================
bool PetDisplay::SetEmotion(const char* emotion) {
    if (!emotion) return false;

    // 大模型返回的情绪词，映射到宠物动画
    if (!strcmp(emotion, "happy") || !strcmp(emotion, "laughing") ||
        !strcmp(emotion, "loving") || !strcmp(emotion, "kissy") ||
        !strcmp(emotion, "winking") || !strcmp(emotion, "delicious")) {
        fx_ = FX_HAPPY;
        mood_ = clamp100(mood_ + 3);
        std::lock_guard<std::mutex> lk(fx_mutex_);
        pending_hearts_ = 2;
        PushFloatLocked("LOVE!", C_HEART);
    } else if (!strcmp(emotion, "excited") || !strcmp(emotion, "surprised") ||
               !strcmp(emotion, "confident") || !strcmp(emotion, "cool") ||
               !strcmp(emotion, "funny") || !strcmp(emotion, "silly")) {
        fx_ = FX_PLAY;
        mood_ = clamp100(mood_ + 5);
        std::lock_guard<std::mutex> lk(fx_mutex_);
        pending_hearts_ = 3;
        PushFloatLocked("YAY!", 0xFAC45C);
    } else if (!strcmp(emotion, "sad") || !strcmp(emotion, "crying") ||
               !strcmp(emotion, "angry") || !strcmp(emotion, "confused")) {
        fx_ = FX_SAD;
        std::lock_guard<std::mutex> lk(fx_mutex_);
        PushFloatLocked("sob..", C_TEAR);
    } else if (!strcmp(emotion, "thinking")) {
        fx_ = FX_THINK;
        std::lock_guard<std::mutex> lk(fx_mutex_);
        PushFloatLocked("...", C_DIM);
    } else {
        fx_ = FX_NONE;                 // neutral 等
    }
    fx_until_ = lv_tick_get() + 3000;  // v1 是 1.8 秒，太短看不出
    ESP_LOGI(TAG, "emotion=%s -> fx=%d", emotion, fx_);
    return true;                        // 告诉小智"已处理"，不再找 emoji 资源
}

void PetDisplay::SetChatMessage(const char* role, const char* content) {
    SpiLcdDisplay::SetChatMessage(role, content);   // 字幕继续交给父类（自带中文字体）

    // 助手说一句话 = 一次"互动"：动嘴、消耗体力、累计成长
    if (role && !strcmp(role, "assistant")) {
        int len = content ? (int)strlen(content) : 0;
        talking_ = true;
        talk_until_ = lv_tick_get() + (len > 0 ? len * 150 : 1500);
        rounds_++;
        hunger_ = clamp100(hunger_ - 2);
        mood_   = clamp100(mood_ + 2);

        // 成长加速版：3 轮孵化，12 轮长成少年
        int want = (rounds_ >= 12) ? 2 : (rounds_ >= 3 ? 1 : 0);
        std::lock_guard<std::mutex> lk(fx_mutex_);
        bars_dirty_ = true;
        if (want > stage_) pending_stage_ = want;
    }
}

// ============================================================
//  孩子能触发的动作（按键回调线程调用）
// ============================================================
void PetDisplay::FeedPet() {
    if (sleeping_) {
        sleeping_ = false;                 // 睡着也会被叫醒吃饭
        std::lock_guard<std::mutex> lk(fx_mutex_);
        PushFloatLocked("WAKE!", 0xFFFFFF);
    }
    hunger_ = clamp100(hunger_ + 18);
    mood_   = clamp100(mood_ + 5);
    fx_ = FX_EAT;
    fx_until_ = lv_tick_get() + 1600;
    {
        std::lock_guard<std::mutex> lk(fx_mutex_);
        bars_dirty_ = true;
        pending_hearts_ = 2;
        PushFloatLocked("YUM!", 0x68D696);
    }
    SaveStats();
    ESP_LOGI(TAG, "feed -> hunger=%d", hunger_);
}

void PetDisplay::PlayWithPet() {
    if (sleeping_) {
        std::lock_guard<std::mutex> lk(fx_mutex_);
        PushFloatLocked("Zzz...", 0x9AA8C8);
        return;
    }
    if (energy_ < 8) {
        fx_ = FX_SAD;
        fx_until_ = lv_tick_get() + 1200;
        std::lock_guard<std::mutex> lk(fx_mutex_);
        PushFloatLocked("TIRED!", 0xF87070);
        return;
    }
    mood_   = clamp100(mood_ + 14);
    energy_ = clamp100(energy_ - 6);
    fx_ = FX_PLAY;
    fx_until_ = lv_tick_get() + 2000;
    {
        std::lock_guard<std::mutex> lk(fx_mutex_);
        bars_dirty_ = true;
        pending_hearts_ = 3;
        PushFloatLocked("HAHA!", 0xFAC45C);
    }
    SaveStats();
}

void PetDisplay::ToggleSleep() {
    sleeping_ = !sleeping_;
    fx_ = FX_NONE;
    SaveStats();
    std::lock_guard<std::mutex> lk(fx_mutex_);
    PushFloatLocked(sleeping_ ? "Zzz..." : "HI!", sleeping_ ? 0x9AA8C8 : 0xFFFFFF);
}
