#pragma once

#include "gadget_module.h"

#define FORECAST_DAYS 3

typedef struct {
    char date[11];
    int low, high, code;
} forecast_day_t;

typedef struct {
    int temperature, humidity, wind, code;
    forecast_day_t days[FORECAST_DAYS];
} forecast_t;

bool weather_forecast(const gadget_json_api_t *json, const gadget_json_t *document, forecast_t *forecast);
const char *weather_condition(int code);
