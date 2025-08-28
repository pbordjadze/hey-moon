
#pragma once

#include "movement.h"

// Modes for editing start/end times
typedef enum
{
    MOON_ALARM_MODE_NONE = 0,
    MOON_ALARM_MODE_SET_START_HOUR,
    MOON_ALARM_MODE_SET_START_MIN,
    MOON_ALARM_MODE_SET_END_HOUR,
    MOON_ALARM_MODE_SET_END_MIN
} moon_alarm_setting_mode_t;

typedef struct
{
    uint8_t start_hour;
    uint8_t start_min;
    uint8_t end_hour;
    uint8_t end_min;
    uint8_t alarm_is_on;
    uint16_t salt; // optional: allows multiple “channels” of sync
    moon_alarm_setting_mode_t mode;
} moon_alarm_state_t;

void moon_alarm_face_setup(uint8_t watch_face_index, void **context_ptr);
void moon_alarm_face_activate(void *context);
bool moon_alarm_face_loop(movement_event_t event, void *context);
void moon_alarm_face_resign(void *context);
movement_watch_face_advisory_t moon_alarm_face_advise(void *context);

#define moon_alarm_face ((const watch_face_t){ \
    moon_alarm_face_setup,                     \
    moon_alarm_face_activate,                  \
    moon_alarm_face_loop,                      \
    moon_alarm_face_resign,                    \
    moon_alarm_face_advise})
