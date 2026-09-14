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

    // Column 1: 5-Hour Quota
    lv_obj_t* title_5h_ = nullptr;
    lv_obj_t* label_5h_ = nullptr;
    lv_obj_t* bar_5h_ = nullptr;
    lv_obj_t* eta_5h_ = nullptr;

    // Column 2: Weekly Quota
    lv_obj_t* title_7d_ = nullptr;
    lv_obj_t* label_7d_ = nullptr;
    lv_obj_t* bar_7d_ = nullptr;
    lv_obj_t* eta_7d_ = nullptr;

    bool quota_visible_ = true;

public:
    SuperminiOledDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                         int width, int height, bool mirror_x, bool mirror_y)
        : OledDisplay(panel_io, panel, width, height, mirror_x, mirror_y) {}

    virtual void SetupUI() override {
        // Let base class setup top_bar_, status_bar_, content_, etc.
        OledDisplay::SetupUI();

        DisplayLockGuard lock(this);
        auto screen = lv_screen_active();

        // Quota container occupying the lower 48 pixels (Y = 16 to 64)
        quota_container_ = lv_obj_create(screen);
        lv_obj_set_pos(quota_container_, 0, 16);
        lv_obj_set_size(quota_container_, LV_HOR_RES, LV_VER_RES - 16);
        lv_obj_set_style_pad_all(quota_container_, 0, 0);
        lv_obj_set_style_border_width(quota_container_, 0, 0);
        lv_obj_set_style_radius(quota_container_, 0, 0);
        lv_obj_set_style_bg_opa(quota_container_, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(quota_container_, lv_color_white(), 0);
        lv_obj_set_scrollbar_mode(quota_container_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_remove_flag(quota_container_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_layout(quota_container_, LV_LAYOUT_NONE, 0);

        // ==================== COLUMN 1: 5-HOUR (X: 4 to 60) ====================
        // Row 1: Header (Y = 1)
        title_5h_ = lv_label_create(quota_container_);
        lv_obj_set_pos(title_5h_, 4, 1);
        lv_obj_set_width(title_5h_, 56);
        lv_obj_set_height(title_5h_, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(title_5h_, 0, 0);
        lv_obj_set_style_border_width(title_5h_, 0, 0);
        lv_obj_set_style_text_align(title_5h_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_remove_flag(title_5h_, LV_OBJ_FLAG_SCROLLABLE);
        lv_label_set_long_mode(title_5h_, LV_LABEL_LONG_CLIP);
        lv_label_set_text(title_5h_, "5-HOUR");

        // Row 2: Percentage Text (Y = 14)
        label_5h_ = lv_label_create(quota_container_);
        lv_obj_set_pos(label_5h_, 4, 14);
        lv_obj_set_width(label_5h_, 56);
        lv_obj_set_height(label_5h_, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(label_5h_, 0, 0);
        lv_obj_set_style_border_width(label_5h_, 0, 0);
        lv_obj_set_style_text_align(label_5h_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_remove_flag(label_5h_, LV_OBJ_FLAG_SCROLLABLE);
        lv_label_set_long_mode(label_5h_, LV_LABEL_LONG_CLIP);
        lv_label_set_text(label_5h_, "--%");

        // Row 3: Progress Bar (Y = 28, Height = 4px)
        bar_5h_ = lv_bar_create(quota_container_);
        lv_obj_set_pos(bar_5h_, 4, 28);
        lv_obj_set_size(bar_5h_, 56, 4);
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

        // Row 4: Reset Countdown (Y = 34)
        eta_5h_ = lv_label_create(quota_container_);
        lv_obj_set_pos(eta_5h_, 4, 34);
        lv_obj_set_width(eta_5h_, 56);
        lv_obj_set_height(eta_5h_, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(eta_5h_, 0, 0);
        lv_obj_set_style_border_width(eta_5h_, 0, 0);
        lv_obj_set_style_text_align(eta_5h_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_remove_flag(eta_5h_, LV_OBJ_FLAG_SCROLLABLE);
        lv_label_set_long_mode(eta_5h_, LV_LABEL_LONG_CLIP);
        lv_label_set_text(eta_5h_, "--");

        // ==================== COLUMN 2: WEEKLY (X: 68 to 124) ====================
        // Row 1: Header (Y = 1)
        title_7d_ = lv_label_create(quota_container_);
        lv_obj_set_pos(title_7d_, 68, 1);
        lv_obj_set_width(title_7d_, 56);
        lv_obj_set_height(title_7d_, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(title_7d_, 0, 0);
        lv_obj_set_style_border_width(title_7d_, 0, 0);
        lv_obj_set_style_text_align(title_7d_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_remove_flag(title_7d_, LV_OBJ_FLAG_SCROLLABLE);
        lv_label_set_long_mode(title_7d_, LV_LABEL_LONG_CLIP);
        lv_label_set_text(title_7d_, "WEEKLY");

        // Row 2: Percentage Text (Y = 14)
        label_7d_ = lv_label_create(quota_container_);
        lv_obj_set_pos(label_7d_, 68, 14);
        lv_obj_set_width(label_7d_, 56);
        lv_obj_set_height(label_7d_, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(label_7d_, 0, 0);
        lv_obj_set_style_border_width(label_7d_, 0, 0);
        lv_obj_set_style_text_align(label_7d_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_remove_flag(label_7d_, LV_OBJ_FLAG_SCROLLABLE);
        lv_label_set_long_mode(label_7d_, LV_LABEL_LONG_CLIP);
        lv_label_set_text(label_7d_, "--%");

        // Row 3: Progress Bar (Y = 28, Height = 4px)
        bar_7d_ = lv_bar_create(quota_container_);
        lv_obj_set_pos(bar_7d_, 68, 28);
        lv_obj_set_size(bar_7d_, 56, 4);
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

        // Row 4: Reset Countdown (Y = 34)
        eta_7d_ = lv_label_create(quota_container_);
        lv_obj_set_pos(eta_7d_, 68, 34);
        lv_obj_set_width(eta_7d_, 56);
        lv_obj_set_height(eta_7d_, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(eta_7d_, 0, 0);
        lv_obj_set_style_border_width(eta_7d_, 0, 0);
        lv_obj_set_style_text_align(eta_7d_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_remove_flag(eta_7d_, LV_OBJ_FLAG_SCROLLABLE);
        lv_label_set_long_mode(eta_7d_, LV_LABEL_LONG_CLIP);
        lv_label_set_text(eta_7d_, "--");

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
        snprintf(buf, sizeof(buf), "%.0f%%", ctx_percent);
        lv_label_set_text(label_5h_, buf);

        if (r5h && r5h[0] && strcmp(r5h, "--") != 0) {
            snprintf(buf, sizeof(buf), "%s", r5h);
            lv_label_set_text(eta_5h_, buf);
        } else {
            lv_label_set_text(eta_5h_, "--");
        }
        lv_bar_set_value(bar_5h_, (int32_t)ctx_percent, LV_ANIM_OFF);

        snprintf(buf, sizeof(buf), "%.0f%%", week_percent);
        lv_label_set_text(label_7d_, buf);

        if (rwk && rwk[0] && strcmp(rwk, "--") != 0) {
            snprintf(buf, sizeof(buf), "%s", rwk);
            lv_label_set_text(eta_7d_, buf);
        } else {
            lv_label_set_text(eta_7d_, "--");
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
