#ifndef SUPERMINI_DISPLAY_H
#define SUPERMINI_DISPLAY_H

#include "display/oled_display.h"
#include "assets/lang_config.h"
#include <esp_log.h>
#include <cstring>
#include <cstdio>

class SuperminiOledDisplay : public OledDisplay {
private:
    lv_obj_t* quota_container_ = nullptr;
    lv_obj_t* label_5h_ = nullptr;
    lv_obj_t* eta_5h_ = nullptr;
    lv_obj_t* bar_5h_ = nullptr;
    lv_obj_t* label_7d_ = nullptr;
    lv_obj_t* eta_7d_ = nullptr;
    lv_obj_t* bar_7d_ = nullptr;

    bool quota_visible_ = true;

public:
    SuperminiOledDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                         int width, int height, bool mirror_x, bool mirror_y)
        : OledDisplay(panel_io, panel, width, height, mirror_x, mirror_y) {}

    virtual void SetupUI() override {
        // First let base class setup top_bar_, status_bar_, content_, etc.
        OledDisplay::SetupUI();

        DisplayLockGuard lock(this);
        auto screen = lv_screen_active();

        // Create Quota container overlaying the content area (Y = 16 to 64, Height = 48)
        quota_container_ = lv_obj_create(screen);
        lv_obj_set_pos(quota_container_, 0, 16);
        lv_obj_set_size(quota_container_, LV_HOR_RES, LV_VER_RES - 16);
        lv_obj_set_style_pad_all(quota_container_, 0, 0);
        lv_obj_set_style_border_width(quota_container_, 0, 0);
        lv_obj_set_style_radius(quota_container_, 0, 0);
        lv_obj_set_style_bg_opa(quota_container_, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(quota_container_, lv_color_white(), 0);
        lv_obj_set_scrollbar_mode(quota_container_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_style_layout(quota_container_, LV_LAYOUT_NONE, 0);

        // --- Row 1: 5-Hour Text (Y = 1) ---
        label_5h_ = lv_label_create(quota_container_);
        lv_obj_set_pos(label_5h_, 2, 1);
        lv_obj_set_size(label_5h_, 62, 12);
        lv_label_set_text(label_5h_, "5H: --%");

        eta_5h_ = lv_label_create(quota_container_);
        lv_obj_set_pos(eta_5h_, 64, 1);
        lv_obj_set_size(eta_5h_, 62, 12);
        lv_obj_set_style_text_align(eta_5h_, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_text(eta_5h_, "rst:--");

        // --- Row 2: 5-Hour Progress Bar (Y = 14) ---
        bar_5h_ = lv_bar_create(quota_container_);
        lv_obj_set_pos(bar_5h_, 2, 14);
        lv_obj_set_size(bar_5h_, 124, 5);
        lv_bar_set_range(bar_5h_, 0, 100);
        lv_bar_set_value(bar_5h_, 0, LV_ANIM_OFF);
        lv_obj_set_style_radius(bar_5h_, 0, 0);
        lv_obj_set_style_radius(bar_5h_, 0, LV_PART_INDICATOR);
        lv_obj_set_style_border_width(bar_5h_, 1, 0);
        lv_obj_set_style_border_color(bar_5h_, lv_color_black(), 0);
        lv_obj_set_style_bg_color(bar_5h_, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(bar_5h_, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(bar_5h_, lv_color_black(), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(bar_5h_, LV_OPA_COVER, LV_PART_INDICATOR);

        // --- Row 3: 7-Day Text (Y = 23) ---
        label_7d_ = lv_label_create(quota_container_);
        lv_obj_set_pos(label_7d_, 2, 23);
        lv_obj_set_size(label_7d_, 62, 12);
        lv_label_set_text(label_7d_, "7D: --%");

        eta_7d_ = lv_label_create(quota_container_);
        lv_obj_set_pos(eta_7d_, 64, 23);
        lv_obj_set_size(eta_7d_, 62, 12);
        lv_obj_set_style_text_align(eta_7d_, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_text(eta_7d_, "rst:--");

        // --- Row 4: 7-Day Progress Bar (Y = 36) ---
        bar_7d_ = lv_bar_create(quota_container_);
        lv_obj_set_pos(bar_7d_, 2, 36);
        lv_obj_set_size(bar_7d_, 124, 5);
        lv_bar_set_range(bar_7d_, 0, 100);
        lv_bar_set_value(bar_7d_, 0, LV_ANIM_OFF);
        lv_obj_set_style_radius(bar_7d_, 0, 0);
        lv_obj_set_style_radius(bar_7d_, 0, LV_PART_INDICATOR);
        lv_obj_set_style_border_width(bar_7d_, 1, 0);
        lv_obj_set_style_border_color(bar_7d_, lv_color_black(), 0);
        lv_obj_set_style_bg_color(bar_7d_, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(bar_7d_, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(bar_7d_, lv_color_black(), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(bar_7d_, LV_OPA_COVER, LV_PART_INDICATOR);

        quota_visible_ = true;
    }

    void SetQuotaVisible(bool visible) {
        DisplayLockGuard lock(this);
        if (!quota_container_) return;
        if (visible == quota_visible_) return;
        quota_visible_ = visible;
        if (visible) {
            lv_obj_remove_flag(quota_container_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(quota_container_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    void UpdateQuota(float ctx_percent, float week_percent, const char* r5h, const char* rwk) {
        DisplayLockGuard lock(this);
        if (!quota_container_) return;

        char buf[32];
        snprintf(buf, sizeof(buf), "5H: %.0f%%", ctx_percent);
        lv_label_set_text(label_5h_, buf);

        if (r5h && r5h[0] && strcmp(r5h, "--") != 0) {
            snprintf(buf, sizeof(buf), "rst:%s", r5h);
            lv_label_set_text(eta_5h_, buf);
        } else {
            lv_label_set_text(eta_5h_, "rst:--");
        }
        lv_bar_set_value(bar_5h_, (int32_t)ctx_percent, LV_ANIM_OFF);

        snprintf(buf, sizeof(buf), "7D: %.0f%%", week_percent);
        lv_label_set_text(label_7d_, buf);

        if (rwk && rwk[0] && strcmp(rwk, "--") != 0) {
            snprintf(buf, sizeof(buf), "rst:%s", rwk);
            lv_label_set_text(eta_7d_, buf);
        } else {
            lv_label_set_text(eta_7d_, "rst:--");
        }
        lv_bar_set_value(bar_7d_, (int32_t)week_percent, LV_ANIM_OFF);
    }

    virtual void SetChatMessage(const char* role, const char* content) override {
        OledDisplay::SetChatMessage(role, content);
        if (content != nullptr && content[0] != '\0') {
            SetQuotaVisible(false);
        } else {
            SetQuotaVisible(true);
        }
    }

    virtual void ClearChatMessages() override {
        OledDisplay::ClearChatMessages();
        SetQuotaVisible(true);
    }

    virtual void SetEmotion(const char* emotion) override {
        OledDisplay::SetEmotion(emotion);
        if (emotion != nullptr && strcmp(emotion, "neutral") != 0 && strcmp(emotion, "standby") != 0) {
            SetQuotaVisible(false);
        }
    }

    virtual void SetStatus(const char* status) override {
        OledDisplay::SetStatus(status);
        if (status != nullptr) {
            if (strcmp(status, Lang::Strings::STANDBY) == 0) {
                SetQuotaVisible(true);
            } else if (strcmp(status, Lang::Strings::LISTENING) == 0 ||
                       strcmp(status, Lang::Strings::SPEAKING) == 0 ||
                       strcmp(status, Lang::Strings::CONNECTING) == 0) {
                SetQuotaVisible(false);
            }
        }
    }
};

#endif // SUPERMINI_DISPLAY_H
