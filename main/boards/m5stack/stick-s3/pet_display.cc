/* ============================================================
 *  PetDisplay · 电子宠物 × 小智语音 合体实现
 *  屏幕：135 x 240（竖屏，SpiLcdDisplay 原生方向）
 *  布局：顶部状态栏（小智原生） / 中间宠物区 / 底部字幕（小智原生）
 * ============================================================ */
#include "pet_display.h"

#include <esp_log.h>
#include <cstring>
#include <cstdio>
#include <cmath>

#define TAG "PetDisplay"

// ---------------- 配色 ----------------
static const uint32_t C_BG      = 0x0E101A;
static const uint32_t C_BODY    = 0xFF929E;
static const uint32_t C_SHADOW  = 0x181A26;
static const uint32_t C_EYE     = 0x1A1C2A;
static const uint32_t C_CHEEK   = 0xFFA8B4;
static const uint32_t C_TEAR    = 0x60BEFA;
static const uint32_t C_DIM     = 0x7880A0;
static const uint32_t C_BAR_FD  = 0x68D696;
static const uint32_t C_BAR_MD  = 0xFAC45C;
static const uint32_t C_BAR_EN  = 0x60BEFA;

// 宠物区几何（相对 pet_root_，尺寸 135x150）
static const int PET_W = 135;
static const int PET_H = 150;
static const int BODY_X = 29, BODY_Y = 14, BODY_W = 76, BODY_H = 72;
static const int EYE_Y = 42, EYE_L_X = 49, EYE_R_X = 75, EYE_D = 11;
static const int MOUTH_Y = 66, MOUTH_W = 16;
static const int CHEEK_Y = 60;
static const int BAR_X = 26, BAR_W = 100, BAR_H = 9;
static const int BAR_Y[3] = {98, 114, 130};

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
    ESP_LOGI(TAG, "PetDisplay created %dx%d, stage=%d", width, height, stage_);
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

    // 3) 宠物层：整宽、只占中间一条，避免盖住顶部状态栏和底部字幕
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

    // 4) 40ms 定时器跑动画（回调在 LVGL 任务里执行，天然线程安全）
    timer_ = lv_timer_create([](lv_timer_t* t) {
        static_cast<PetDisplay*>(lv_timer_get_user_data(t))->Tick();
    }, 40, this);

    ApplyStageLook();
    RefreshBars();
    ESP_LOGI(TAG, "pet UI built");
}

void PetDisplay::BuildPetUI() {
    // 耳朵先建，身体后建，这样耳朵下半截会被身体盖住
    ear_l_ = make_box(pet_root_, 34, 4, 18, 18, C_BODY, LV_RADIUS_CIRCLE);
    ear_r_ = make_box(pet_root_, 83, 4, 18, 18, C_BODY, LV_RADIUS_CIRCLE);
    lv_obj_add_flag(ear_l_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ear_r_, LV_OBJ_FLAG_HIDDEN);

    shadow_ = make_box(pet_root_, 38, 84, 58, 8, C_SHADOW, 4);
    body_   = make_box(pet_root_, BODY_X, BODY_Y, BODY_W, BODY_H, C_BODY, 26);

    eye_l_ = make_box(pet_root_, EYE_L_X, EYE_Y, EYE_D, EYE_D, C_EYE, LV_RADIUS_CIRCLE);
    eye_r_ = make_box(pet_root_, EYE_R_X, EYE_Y, EYE_D, EYE_D, C_EYE, LV_RADIUS_CIRCLE);

    cheek_l_ = make_box(pet_root_, 40, CHEEK_Y, 7, 7, C_CHEEK, LV_RADIUS_CIRCLE);
    cheek_r_ = make_box(pet_root_, 88, CHEEK_Y, 7, 7, C_CHEEK, LV_RADIUS_CIRCLE);

    mouth_ = make_box(pet_root_, (PET_W - MOUTH_W) / 2, MOUTH_Y, MOUTH_W, 5, C_EYE, 3);

    tear_ = make_box(pet_root_, EYE_L_X, EYE_Y + 14, 5, 5, C_TEAR, LV_RADIUS_CIRCLE);
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
}

void PetDisplay::ApplyStageLook() {
    if (!body_) return;
    if (stage_ >= 2) {                     // 少年期：露出两只耳朵
        lv_obj_clear_flag(ear_l_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ear_r_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ear_l_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ear_r_, LV_OBJ_FLAG_HIDDEN);
    }
    char buf[24];
    const char* nm = (stage_ == 0) ? "EGG" : (stage_ == 1 ? "BABY" : "TEEN");
    snprintf(buf, sizeof(buf), "%s %d", nm, rounds_);
    lv_label_set_text(age_label_, buf);
}

void PetDisplay::RefreshBars() {
    if (!bar_[0]) return;
    lv_bar_set_value(bar_[0], hunger_, LV_ANIM_OFF);
    lv_bar_set_value(bar_[1], mood_, LV_ANIM_OFF);
    lv_bar_set_value(bar_[2], energy_, LV_ANIM_OFF);

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
//  动画心跳：40ms 一次
// ============================================================
void PetDisplay::Tick() {
    if (!pet_root_) return;

    uint32_t now = lv_tick_get();
    float t = now / 1000.0f;

    // ---- 数值衰减：每 1 秒结算一次 ----
    if (now - last_tick_ms_ >= 1000) {
        last_tick_ms_ = now;
        if (!sleeping_) {
            if ((now / 1000) % 10 == 0) hunger_ = clamp100(hunger_ - 1);
            if ((now / 1000) % 15 == 0) energy_ = clamp100(energy_ - 1);
            if ((now / 1000) % 20 == 0) mood_ = clamp100(mood_ - (hunger_ < 20 ? 2 : 1));
            if (energy_ <= 0) sleeping_ = true;
        } else {
            energy_ = clamp100(energy_ + 1);
            hunger_ = clamp100(hunger_ - 1);
        }
        RefreshBars();
    }

    // ---- 30 秒存一次盘 ----
    if (now - last_save_ms_ > 30000) {
        last_save_ms_ = now;
        SaveStats();
    }

    // ---- 呼吸 / 跳跃 ----
    int bob = sleeping_ ? (int)(sinf(t * 1.0f) * 1.0f)
                        : (int)(sinf(t * 1.8f) * 2.0f);
    if (fx_ == FX_PLAY) bob = -(int)(fabsf(sinf(t * 9.0f)) * 9.0f);
    if (fx_ == FX_HAPPY) bob = -(int)(fabsf(sinf(t * 5.0f)) * 5.0f);

    lv_obj_set_y(body_, BODY_Y + bob);
    lv_obj_set_y(eye_l_, EYE_Y + bob);
    lv_obj_set_y(eye_r_, EYE_Y + bob);
    lv_obj_set_y(cheek_l_, CHEEK_Y + bob);
    lv_obj_set_y(cheek_r_, CHEEK_Y + bob);
    lv_obj_set_y(mouth_, MOUTH_Y + bob);
    lv_obj_set_y(tear_, EYE_Y + 14 + bob);
    lv_obj_set_y(ear_l_, 4 + bob);
    lv_obj_set_y(ear_r_, 4 + bob);

    // ---- 眨眼：每 4 秒闭 120ms ----
    bool blink = sleeping_ || ((now % 4000) < 120);
    int eh = blink ? 3 : EYE_D;
    lv_obj_set_height(eye_l_, eh);
    lv_obj_set_height(eye_r_, eh);
    lv_obj_set_y(eye_l_, EYE_Y + bob + (blink ? 4 : 0));
    lv_obj_set_y(eye_r_, EYE_Y + bob + (blink ? 4 : 0));

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

    // ---- 特效到期 ----
    if (fx_ != FX_NONE && (int32_t)(now - fx_until_) > 0) {
        fx_ = FX_NONE;
    }
}

// ============================================================
//  小智的显示接口
// ============================================================
bool PetDisplay::SetEmotion(const char* emotion) {
    if (!emotion) return false;

    // 大模型返回的情绪词，映射到我们的表情
    if (!strcmp(emotion, "happy") || !strcmp(emotion, "laughing") ||
        !strcmp(emotion, "loving") || !strcmp(emotion, "kissy")) {
        fx_ = FX_HAPPY;
        mood_ = clamp100(mood_ + 3);
    } else if (!strcmp(emotion, "excited") || !strcmp(emotion, "surprised") ||
               !strcmp(emotion, "confident") || !strcmp(emotion, "cool")) {
        fx_ = FX_PLAY;
        mood_ = clamp100(mood_ + 5);
    } else if (!strcmp(emotion, "sad") || !strcmp(emotion, "crying") ||
               !strcmp(emotion, "angry") || !strcmp(emotion, "confused")) {
        fx_ = FX_SAD;
    } else if (!strcmp(emotion, "thinking")) {
        fx_ = FX_THINK;
    } else {
        fx_ = FX_NONE;                 // neutral 等
    }
    fx_until_ = lv_tick_get() + 1800;
    ESP_LOGI(TAG, "emotion=%s -> fx=%d", emotion, fx_);
    return true;                        // 告诉我们"已处理"，小智就不会再找 emoji 资源
}

void PetDisplay::SetChatMessage(const char* role, const char* content) {
    SpiLcdDisplay::SetChatMessage(role, content);   // 字幕继续交给父类（自带中文字体）

    // 助手说一句话 = 一次"互动"，宠物跟着动嘴、消耗一点体力、累计成长
    if (role && !strcmp(role, "assistant")) {
        int len = content ? (int)strlen(content) : 0;
        talking_ = true;
        talk_until_ = lv_tick_get() + (len > 0 ? len * 150 : 1500);
        rounds_++;
        hunger_ = clamp100(hunger_ - 2);
        mood_   = clamp100(mood_ + 2);

        // 成长：聊够 8 轮孵化，30 轮长大
        int want = (rounds_ >= 30) ? 2 : (rounds_ >= 8 ? 1 : 0);
        if (want > stage_) {
            stage_ = want;
            ESP_LOGI(TAG, "stage up -> %d", stage_);
        }
    }
}

// ============================================================
//  孩子能触发的动作
// ============================================================
void PetDisplay::FeedPet() {
    if (sleeping_) return;
    hunger_ = clamp100(hunger_ + 15);
    mood_   = clamp100(mood_ + 4);
    fx_ = FX_EAT;
    fx_until_ = lv_tick_get() + 1200;
    RefreshBars();
    SaveStats();
    ESP_LOGI(TAG, "feed -> hunger=%d", hunger_);
}

void PetDisplay::PlayWithPet() {
    if (sleeping_) return;
    if (energy_ < 8) { fx_ = FX_SAD; fx_until_ = lv_tick_get() + 1200; return; }
    mood_   = clamp100(mood_ + 12);
    energy_ = clamp100(energy_ - 6);
    fx_ = FX_PLAY;
    fx_until_ = lv_tick_get() + 1500;
    RefreshBars();
    SaveStats();
}

void PetDisplay::ToggleSleep() {
    sleeping_ = !sleeping_;
    fx_ = FX_NONE;
    SaveStats();
}
