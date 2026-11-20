// SPDX-License-Identifier: MIT
// Open-Meteo field selection and bounds adapted from Weather, copyright (c) 2026 Weather project.
#include "forecast.h"

#include <string.h>

static bool metric(const gadget_json_api_t *json, const gadget_json_t *value, int min, int max, int *out)
{
    float number;
    if (!json->number(value, &number) || number < min || number > max) {
        return false;
    }
    *out = (int)(number + (number < 0 ? -0.5f : 0.5f));
    return true;
}

static bool iso_date(const char *text)
{
    if (text == NULL) {
        return false;
    }
    for (unsigned i = 0; i < 10; ++i) {
        const char ch = text[i];
        if ((i == 4 || i == 7) ? ch != '-' : (ch < '0' || ch > '9')) {
            return false;
        }
    }
    return text[10] == '\0';
}

bool weather_forecast(const gadget_json_api_t *json, const gadget_json_t *document, forecast_t *forecast)
{
    const gadget_json_t *current = json->member(document, "current");
    const gadget_json_t *daily = json->member(document, "daily");
    const gadget_json_t *dates = json->member(daily, "time");
    const gadget_json_t *high = json->member(daily, "temperature_2m_max");
    const gadget_json_t *low = json->member(daily, "temperature_2m_min");
    const gadget_json_t *codes = json->member(daily, "weather_code");

    forecast_t result = {.humidity = -1, .wind = -1, .code = -1};
    if (!metric(json, json->member(current, "temperature_2m"), -100, 70, &result.temperature)) {
        return false;
    }
    metric(json, json->member(current, "relative_humidity_2m"), 0, 100, &result.humidity);
    metric(json, json->member(current, "wind_speed_10m"), 0, 540, &result.wind);
    metric(json, json->member(current, "weather_code"), 0, 99, &result.code);

    for (unsigned i = 0; i < FORECAST_DAYS; ++i) {
        forecast_day_t *day = &result.days[i];
        day->code = -1;
        const char *date = json->string(json->at(dates, i));
        if (iso_date(date) && metric(json, json->at(high, i), -100, 70, &day->high) &&
            metric(json, json->at(low, i), -100, 70, &day->low) && day->low <= day->high) {
            memcpy(day->date, date, sizeof(day->date));
            metric(json, json->at(codes, i), 0, 99, &day->code);
        }
    }
    *forecast = result;
    return true;
}

const char *weather_condition(int code)
{
    if (code == 0) return "Clear";
    if (code == 1) return "Mostly clear";
    if (code == 2) return "Partly cloudy";
    if (code == 3) return "Overcast";
    if (code == 45 || code == 48) return "Fog";
    if (code >= 51 && code <= 57) return "Drizzle";
    if (code >= 61 && code <= 67) return "Rain";
    if (code >= 71 && code <= 77) return "Snow";
    if (code >= 80 && code <= 82) return "Rain showers";
    if (code == 85 || code == 86) return "Snow showers";
    if (code >= 95 && code <= 99) return "Thunderstorms";
    return "Condition unavailable";
}
