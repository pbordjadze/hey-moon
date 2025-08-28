#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "watch.h"
#include "watch_utility.h"
#include "movement.h"
#include "moon_alarm_face.h"
#include <stdio.h>

// --- date helpers (Sensor Watch stores year as 0..99 meaning 2000..2099) ---
static inline bool _is_leap(uint16_t y_full)
{
    // works for 2000..2099 range used by the watch
    return (y_full % 4) == 0;
}

static inline uint8_t _days_in_month(uint16_t y_full, uint8_t m)
{
    static const uint8_t dim[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2)
        return dim[1] + (_is_leap(y_full) ? 1 : 0);
    return dim[m - 1];
}

static inline void _decrement_one_day(watch_date_time_t *dt)
{
    uint16_t y_full = (uint16_t)(dt->unit.year + 2000);
    if (dt->unit.day > 1)
    {
        dt->unit.day--;
        return;
    }
    // day == 1 → roll back month (and maybe year)
    if (dt->unit.month > 1)
    {
        dt->unit.month--;
    }
    else
    {
        dt->unit.month = 12;
        if (dt->unit.year == 0)
        {
            dt->unit.year = 63; // wrap to 2099 if ever needed
            y_full = 2063;
        }
        else
        {
            dt->unit.year--;
            y_full--;
        }
    }
    dt->unit.day = _days_in_month(y_full, dt->unit.month);
}

/* ---------- helpers ---------- */

static inline uint16_t minutes_since_midnight(uint8_t hh, uint8_t mm)
{
    return (uint16_t)hh * 60u + (uint16_t)mm;
}

static inline void minutes_to_hhmm(uint16_t mins, uint8_t *hh, uint8_t *mm)
{
    *hh = (uint8_t)(mins / 60u);
    *mm = (uint8_t)(mins % 60u);
}

// FNV-1a 32-bit over a tiny struct (YYYY,MM,DD, salt)
static uint32_t fnv1a_u32(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++)
    {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

// Map 32-bit hash uniformly into [0, span). Use multiply-high to avoid modulo bias.
static uint32_t map_u32_to_span(uint32_t h, uint32_t span)
{
    // span must be > 0
    return (uint32_t)(((uint64_t)h * (uint64_t)span) >> 32);
}

/*
Anchor rule for “the day”:
- If window does NOT cross midnight (start <= end): the day is the calendar day of `start`.
- If window crosses midnight (start > end): we treat the whole window as anchored to the
  date on which the START occurs. Practically:
    If now >= start → anchor = today.
    Else           → anchor = yesterday (because we’re in the after-midnight part).
This way, everybody with the same local window lands on the same hash key.
*/
static void derive_active_window_anchor(const moon_alarm_state_t *s, watch_date_time_t now,
                                        watch_date_time_t *anchor_out,
                                        uint16_t *win_start_min_out,
                                        uint16_t *win_span_min_out)
{

    uint16_t smin = minutes_since_midnight(s->start_hour, s->start_min);
    uint16_t emin = minutes_since_midnight(s->end_hour, s->end_min);

    *win_start_min_out = smin;
    uint16_t span;
    if (smin <= emin)
        span = (uint16_t)(emin - smin); // same-day window
    else
        span = (uint16_t)(1440 - smin + emin); // overnight window

    if (span == 0)
        span = 1; // avoid zero-length window; treat as 1 minute

    *win_span_min_out = span;

    watch_date_time_t anchor = now;

    if (smin <= emin)
    {
        // Same-day window: anchor is today's date at start time.
    }
    else
    {
        // Overnight window: if current time is before start, we’re in the after-midnight part
        uint16_t nowmin = minutes_since_midnight(now.unit.hour, now.unit.minute);
        if (nowmin < smin)
        {
            anchor = now;                // start from "now"
            _decrement_one_day(&anchor); // anchor is yesterday’s date
        }
    }

    // normalize anchor time to 00:00 (date only)
    anchor.unit.hour = 0;
    anchor.unit.minute = 0;
    anchor.unit.second = 0;

    *anchor_out = anchor;
}

// Compute the deterministic target minute-of-day (0..1439) for the *active* window covering “now”.
// Returns true and sets out_minute if the window exists (span >= 1).
static bool compute_target_minute_today(const moon_alarm_state_t *s, watch_date_time_t now, uint16_t *out_minute)
{
    watch_date_time_t anchor;
    uint16_t win_start_min, win_span_min;
    derive_active_window_anchor(s, now, &anchor, &win_start_min, &win_span_min);

    // Hash key: (YYYY,MM,DD,salt) from the ANCHOR date
    struct
    {
        uint16_t y;
        uint8_t m, d;
        uint16_t salt;
    } key = {.y = (uint16_t)(anchor.unit.year + 2000),
             .m = anchor.unit.month,
             .d = anchor.unit.day,
             .salt = s->salt};

    uint32_t h = fnv1a_u32(&key, sizeof(key));
    uint32_t offset = map_u32_to_span(h, win_span_min); // 0..span-1

    uint32_t target = (uint32_t)win_start_min + offset;
    if (target >= 1440)
        target -= 1440; // wrap if window started late-day and offset spilled over

    *out_minute = (uint16_t)target;
    return true;
}

static inline void button_beep()
{
    if (movement_button_should_sound())
        watch_buzzer_play_note_with_volume(BUZZER_NOTE_C7, 50, movement_button_volume());
}

/* ---------- UI helpers (re-use your existing display code) ---------- */

static void display_window(const moon_alarm_state_t *s, moon_alarm_setting_mode_t mode)
{
    // Pick label and which time to show
    const char *tag = "ALM";
    uint8_t hh = s->start_hour, mm = s->start_min;

    if (mode == MOON_ALARM_MODE_SET_START_HOUR || mode == MOON_ALARM_MODE_SET_START_MIN)
    {
        tag = "ST";
        hh = s->start_hour;
        mm = s->start_min;
    }
    else if (mode == MOON_ALARM_MODE_SET_END_HOUR || mode == MOON_ALARM_MODE_SET_END_MIN)
    {
        tag = "EN";
        hh = s->end_hour;
        mm = s->end_min;
    }
    else
    {
        // Normal mode: show START by default (simple + unambiguous).
        tag = "AL";
        hh = s->start_hour;
        mm = s->start_min;
    }

    watch_display_text_with_fallback(WATCH_POSITION_TOP_LEFT, tag, "AL");

    // Indicators
    if (s->alarm_is_on)
        watch_set_indicator(WATCH_INDICATOR_SIGNAL);
    watch_set_colon();
    watch_clear_indicator(WATCH_INDICATOR_PM);
    watch_clear_indicator(WATCH_INDICATOR_24H);

    // Format chosen time on bottom row, 12h/24h aware
    char buf[7];
    if (!movement_clock_mode_24h())
    {
        if (hh >= 12)
            watch_set_indicator(WATCH_INDICATOR_PM);
        uint8_t h12 = hh % 12 ? hh % 12 : 12;
        sprintf(buf, "%2d%02d", h12, mm);
    }
    else
    {
        watch_set_indicator(WATCH_INDICATOR_24H);
        sprintf(buf, "%02d%02d", hh, mm);
    }
    watch_display_text(WATCH_POSITION_BOTTOM, buf);

    // Clear other regions so nothing “lingers”
    watch_display_text(WATCH_POSITION_TOP_RIGHT, "    ");
}

/* ---------- face API ---------- */

void moon_alarm_face_setup(uint8_t idx, void **ctx_ptr)
{
    (void)idx;
    if (*ctx_ptr)
        return;
    *ctx_ptr = malloc(sizeof(moon_alarm_state_t));
    moon_alarm_state_t *st = (moon_alarm_state_t *)*ctx_ptr;
    memset(st, 0, sizeof(*st));

    // Defaults: 08:00 → 10:00
    st->start_hour = 8;
    st->start_min = 0;
    st->end_hour = 10;
    st->end_min = 0;
    st->salt = 0; // keep 0 unless you want “channelized” randomness
}

void moon_alarm_face_activate(void *context)
{
    moon_alarm_state_t *st = (moon_alarm_state_t *)context;
    st->mode = MOON_ALARM_MODE_NONE;
}

void moon_alarm_face_resign(void *context)
{
    (void)context;
}

bool moon_alarm_face_loop(movement_event_t event, void *context)
{
    moon_alarm_state_t *st = (moon_alarm_state_t *)context;

    switch (event.event_type)
    {
    case EVENT_ACTIVATE:
        display_window(st, st->mode);
        break;

    case EVENT_TICK:
        if (st->mode == MOON_ALARM_MODE_NONE)
            break;

        display_window(st, st->mode);

        if ((event.subsecond % 2) == 0)
        {
            switch (st->mode)
            {
            case MOON_ALARM_MODE_SET_START_HOUR:
            case MOON_ALARM_MODE_SET_END_HOUR:
                watch_display_text(WATCH_POSITION_HOURS, "  ");
                break;
            case MOON_ALARM_MODE_SET_START_MIN:
            case MOON_ALARM_MODE_SET_END_MIN:
                watch_display_text(WATCH_POSITION_MINUTES, "  ");
                break;
            default:
                break;
            }
        }
        break;

    case EVENT_LIGHT_BUTTON_DOWN:
        switch (st->mode)
        {
        case MOON_ALARM_MODE_NONE:
            movement_illuminate_led();
            break;
        case MOON_ALARM_MODE_SET_START_HOUR:
            st->mode = MOON_ALARM_MODE_SET_START_MIN;
            break;
        case MOON_ALARM_MODE_SET_START_MIN:
            st->mode = MOON_ALARM_MODE_SET_END_HOUR;
            break;
        case MOON_ALARM_MODE_SET_END_HOUR:
            st->mode = MOON_ALARM_MODE_SET_END_MIN;
            break;
        case MOON_ALARM_MODE_SET_END_MIN:
            st->mode = MOON_ALARM_MODE_NONE;
            movement_request_tick_frequency(1);
            st->alarm_is_on = 1;
            movement_set_alarm_enabled(true);
            watch_set_indicator(WATCH_INDICATOR_SIGNAL);
            button_beep();
            printf("[moon_alarm] ST=%02d:%02d  END=%02d:%02d  on=%d\n",
                   st->start_hour, st->start_min, st->end_hour, st->end_min, st->alarm_is_on);

            display_window(st, st->mode);
            break;
        }
        break;

    case EVENT_ALARM_BUTTON_UP:
        if (st->mode == MOON_ALARM_MODE_NONE)
        {
            st->alarm_is_on ^= 1;
            if (st->alarm_is_on)
            {
                movement_set_alarm_enabled(true);
                watch_set_indicator(WATCH_INDICATOR_SIGNAL);
            }
            else
            {
                movement_set_alarm_enabled(false);
                watch_clear_indicator(WATCH_INDICATOR_SIGNAL);
            }
        }
        break;

    case EVENT_ALARM_BUTTON_DOWN:
        switch (st->mode)
        {
        case MOON_ALARM_MODE_NONE:
            // handled on LONG_PRESS to enter settings
            break;
        case MOON_ALARM_MODE_SET_START_HOUR:
            st->start_hour = (st->start_hour + 1) % 24;
            break;
        case MOON_ALARM_MODE_SET_START_MIN:
            st->start_min = (st->start_min + 1) % 60;
            break;
        case MOON_ALARM_MODE_SET_END_HOUR:
            st->end_hour = (st->end_hour + 1) % 24;
            break;
        case MOON_ALARM_MODE_SET_END_MIN:
            st->end_min = (st->end_min + 1) % 60;
            break;
        }
        display_window(st, st->mode);
        break;

    case EVENT_ALARM_LONG_PRESS:
        if (st->mode == MOON_ALARM_MODE_NONE)
        {
            st->mode = MOON_ALARM_MODE_SET_START_HOUR;
            movement_request_tick_frequency(4);
            button_beep();
            display_window(st, st->mode);
        }
        break;

    case EVENT_BACKGROUND_TASK:
        movement_play_alarm(); // ring when scheduled (see advise)
        break;

    case EVENT_TIMEOUT:
        movement_move_to_face(0);
        break;

    default:
        movement_default_loop_handler(event);
        break;
    }

    return true;
}

movement_watch_face_advisory_t moon_alarm_face_advise(void *context)
{
    moon_alarm_state_t *st = (moon_alarm_state_t *)context;
    movement_watch_face_advisory_t adv = {0};

    if (!st->alarm_is_on)
        return adv;

    watch_date_time_t now = movement_get_local_date_time();
    uint16_t target_min;
    if (!compute_target_minute_today(st, now, &target_min))
        return adv;

    uint16_t now_min = minutes_since_midnight(now.unit.hour, now.unit.minute);
    adv.wants_background_task = (now_min == target_min);

    return adv;
}
