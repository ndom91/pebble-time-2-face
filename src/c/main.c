// Poster — a Pebble Time 2 (emery, 200x228) watchface.
// Staggered monospace digits over a halftone block, a vertical date rail and a
// black status strip with weather, heart rate, steps and battery.
#include <pebble.h>
#include "assets.h"

// ---- layout (px) ----------------------------------------------------------

#define ACCENT GColorOrange          // #FF5500
#define HOURS_X 44
#define HOURS_BASELINE 94
#define MINUTES_X 58
#define MINUTES_BASELINE 175
// Overlap setting: the rows interlock and the hours are drawn over the minutes.
#define HOURS_BASELINE_OVERLAP 104
#define MINUTES_BASELINE_OVERLAP 164
#define HALFTONE_ORIGIN GPoint(26, 12)
#define RAIL_X 10
#define RAIL_TOP 8
#define RAIL_BOTTOM_GAP 6            // between the date and the strip
#define STRIP_H 24
#define STRIP_PAD 6
#define STRIP_ICON_GAP 3
#define BATTERY_W 13

// ---- weather timing (s) ---------------------------------------------------

#define WEATHER_MAX_AGE (30 * 60)
#define WEATHER_RETRY (5 * 60)       // between requests the phone didn't answer

// ---- persist keys (1-4 held v1.0 data and are deleted on start) -----------

#define P_SETTINGS 5
#define P_WEATHER 6

// ---- state ----------------------------------------------------------------

enum { COMP_WEATHER, COMP_HEART_RATE, COMP_STEPS, COMP_BATTERY, COMP_COUNT };

typedef enum {
  DATE_EU,          // 25.11.2026
  DATE_US,          // 11/25/2026
  DATE_ISO,         // 2026-11-25
  DATE_DAY_MONTH,   // 25 NOV 2026
  DATE_MONTH_DAY,   // NOV 25 2026
  DATE_FORMAT_COUNT
} DateFormat;

typedef enum {
  VIBE_OFF,
  VIBE_SHORT,
  VIBE_DOUBLE,
  VIBE_LONG,
  VIBE_HEARTBEAT,
  VIBE_COUNT
} HourlyVibe;

// New fields go at the end: settings saved by an older build are shorter,
// and persist_read_data leaves the missing tail at its default.
typedef struct {
  bool show[COMP_COUNT];
  bool fahrenheit;
  uint8_t date_format;
  uint8_t hourly_vibe;
  bool overlap_digits;
} Settings;

#define TEMP_UNKNOWN INT32_MIN

typedef struct {
  int32_t temp_x10;   // °C × 10
  int32_t code;       // WMO weather code, -1 if unknown
  int32_t updated;    // unix time of the last reading, 0 if none
} Weather;

static Settings s_settings = {
  .show = {true, true, true, true},
  .fahrenheit = false,
  .date_format = DATE_EU,
  .hourly_vibe = VIBE_OFF,
  .overlap_digits = false,
};
static Weather s_weather = {.temp_x10 = TEMP_UNKNOWN, .code = -1, .updated = 0};
static time_t s_last_request;

static struct tm s_now;
static uint8_t s_battery = 100;
static bool s_charging;
static int32_t s_steps;
static int32_t s_bpm;
static bool s_health_subscribed;

static Window *s_window;
static Layer *s_canvas;
static GFont s_font;
static GBitmap *s_digits[10];
static GBitmap *s_halftone;
static GBitmap *s_rail_sheet[2];                       // bold, regular
static GBitmap *s_rail_glyph[2][RAIL_GLYPH_COUNT];     // sub-bitmaps of the sheets

// ---- pixel icons ----------------------------------------------------------

static const char *const ICON_CLOUD[] = {
  "....####....",
  "..##....##..",
  ".#........#.",
  "#..........#",
  "#..........#",
  ".##########.",
};
static const char *const ICON_SUN[] = {
  "...#..#..#..",
  "....####....",
  "..##....##..",
  ".##......##.",
  "..##....##..",
  "....####....",
  "...#..#..#..",
};
static const char *const ICON_RAIN[] = {
  "....####....",
  "..##....##..",
  ".#........#.",
  ".##########.",
  "..#..#..#...",
  ".#..#..#....",
};
static const char *const ICON_SNOW[] = {
  "....####....",
  "..##....##..",
  ".#........#.",
  ".##########.",
  "..#..#..#...",
  "...#..#..#..",
};
static const char *const ICON_STORM[] = {
  "....####....",
  "..##....##..",
  ".#........#.",
  ".####..####.",
  ".....##.....",
  "....##......",
  ".....#......",
};
static const char *const ICON_FOG[] = {
  "##########..",
  "............",
  "..##########",
  "............",
  "##########..",
};
static const char *const ICON_HEART[] = {
  ".##..##.",
  "########",
  "########",
  "########",
  ".######.",
  "..####..",
  "...##...",
};
static const char *const ICON_STEPS[] = {
  ".###.....",
  ".#.#.....",
  ".#.####..",
  ".#....###",
  ".#......#",
  ".########",
};

typedef struct {
  const char *const *rows;
  int h;
} Icon;

#define ICON(a) ((Icon){(a), (int)(sizeof(a) / sizeof((a)[0]))})

static int icon_width(Icon icon) { return (int)strlen(icon.rows[0]); }

// Draws each run of '#' in a row as one rect.
static void draw_icon(GContext *ctx, Icon icon, GPoint origin, GColor color) {
  graphics_context_set_fill_color(ctx, color);
  for (int y = 0; y < icon.h; y++) {
    const char *row = icon.rows[y];
    for (int x = 0; row[x];) {
      if (row[x] != '#') { x++; continue; }
      int start = x;
      while (row[x] == '#') x++;
      graphics_fill_rect(ctx, GRect(origin.x + start, origin.y + y, x - start, 1), 0, GCornerNone);
    }
  }
}

static Icon weather_icon(int code) {
  if (code < 0) return ICON(ICON_CLOUD);
  if (code <= 1) return ICON(ICON_SUN);
  if (code <= 3) return ICON(ICON_CLOUD);
  if (code == 45 || code == 48) return ICON(ICON_FOG);
  if ((code >= 71 && code <= 77) || code == 85 || code == 86) return ICON(ICON_SNOW);
  if (code >= 95) return ICON(ICON_STORM);
  if (code >= 51) return ICON(ICON_RAIN);
  return ICON(ICON_CLOUD);
}

// ---- helpers --------------------------------------------------------------

static void upcase(char *s) {
  for (; *s; s++) if (*s >= 'a' && *s <= 'z') *s -= 'a' - 'A';
}

// Settings arrive from Clay as ints (toggles) or strings (selects).
static int32_t tuple_int(const Tuple *t) {
  switch (t->type) {
    case TUPLE_CSTRING: {
      const char *c = t->value->cstring;
      bool neg = (*c == '-');
      if (neg) c++;
      int32_t v = 0;
      while (*c >= '0' && *c <= '9') v = v * 10 + (*c++ - '0');
      return neg ? -v : v;
    }
    case TUPLE_INT:
      if (t->length == 1) return t->value->int8;
      if (t->length == 2) return t->value->int16;
      return t->value->int32;
    case TUPLE_UINT:
      if (t->length == 1) return t->value->uint8;
      if (t->length == 2) return t->value->uint16;
      return (int32_t)t->value->uint32;
    default:
      return 0;
  }
}

// ---- health ---------------------------------------------------------------

// Returns whether a shown value changed.
static bool update_health(void) {
#if defined(PBL_HEALTH)
  int32_t steps = s_steps, bpm = s_bpm;
  if (s_settings.show[COMP_STEPS]) {
    time_t start = time_start_of_today(), end = time(NULL);
    if (health_service_metric_accessible(HealthMetricStepCount, start, end) &
        HealthServiceAccessibilityMaskAvailable) {
      steps = (int32_t)health_service_sum_today(HealthMetricStepCount);
    }
  }
  if (s_settings.show[COMP_HEART_RATE]) {
    bpm = (int32_t)health_service_peek_current_value(HealthMetricHeartRateBPM);
  }
  bool changed = steps != s_steps || bpm != s_bpm;
  s_steps = steps;
  s_bpm = bpm;
  return changed;
#else
  return false;
#endif
}

#if defined(PBL_HEALTH)
static void health_handler(HealthEventType event, void *context) {
  if (event == HealthEventSleepUpdate || event == HealthEventMetricAlert) return;
  if (update_health()) layer_mark_dirty(s_canvas);
}
#endif

// Subscribes to health events only while steps or heart rate are shown.
static void update_health_subscription(void) {
#if defined(PBL_HEALTH)
  bool want = s_settings.show[COMP_STEPS] || s_settings.show[COMP_HEART_RATE];
  if (want && !s_health_subscribed) {
    s_health_subscribed = health_service_events_subscribe(health_handler, NULL);
  } else if (!want && s_health_subscribed) {
    health_service_events_unsubscribe();
    s_health_subscribed = false;
  }
  update_health();
#endif
}

// ---- weather & settings messages ------------------------------------------

static void request_weather(void) {
  DictionaryIterator *out;
  if (app_message_outbox_begin(&out) != APP_MSG_OK) return;
  dict_write_uint8(out, MESSAGE_KEY_REQUEST, 1);
  app_message_outbox_send();
  s_last_request = time(NULL);
}

static void maybe_request_weather(time_t now) {
  if (!s_settings.show[COMP_WEATHER]) return;
  if (now - s_weather.updated < WEATHER_MAX_AGE) return;
  if (now - s_last_request < WEATHER_RETRY) return;
  request_weather();
}

static void inbox_received(DictionaryIterator *iter, void *context) {
  Tuple *t;

  Tuple *temp = dict_find(iter, MESSAGE_KEY_TEMP_X10);
  Tuple *code = dict_find(iter, MESSAGE_KEY_CODE);
  if (temp || code) {
    Tuple *updated = dict_find(iter, MESSAGE_KEY_UPDATED);
    if (temp) s_weather.temp_x10 = tuple_int(temp);
    if (code) s_weather.code = tuple_int(code);
    // A cached reading from the phone keeps its own age.
    s_weather.updated = updated ? tuple_int(updated) : (int32_t)time(NULL);
    persist_write_data(P_WEATHER, &s_weather, sizeof(s_weather));
  }

  // MESSAGE_KEY_* are linker symbols, not constants, so this array can't be static.
  const uint32_t show_keys[COMP_COUNT] = {
    MESSAGE_KEY_SHOW_WEATHER, MESSAGE_KEY_SHOW_HEART_RATE,
    MESSAGE_KEY_SHOW_STEPS, MESSAGE_KEY_SHOW_BATTERY,
  };
  bool settings = false;
  for (int i = 0; i < COMP_COUNT; i++) {
    if ((t = dict_find(iter, show_keys[i]))) {
      s_settings.show[i] = tuple_int(t) != 0;
      settings = true;
    }
  }
  if ((t = dict_find(iter, MESSAGE_KEY_TEMP_UNIT))) {
    if (t->type == TUPLE_CSTRING) {
      const char *unit = t->value->cstring;
      s_settings.fahrenheit = (*unit == 'F');
    } else {
      s_settings.fahrenheit = tuple_int(t) != 0;
    }
    settings = true;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_DATE_FORMAT))) {
    int32_t f = tuple_int(t);
    s_settings.date_format = (f >= 0 && f < DATE_FORMAT_COUNT) ? (uint8_t)f : DATE_EU;
    settings = true;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_HOURLY_VIBE))) {
    int32_t v = tuple_int(t);
    s_settings.hourly_vibe = (v >= 0 && v < VIBE_COUNT) ? (uint8_t)v : VIBE_OFF;
    settings = true;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_OVERLAP_DIGITS))) {
    s_settings.overlap_digits = tuple_int(t) != 0;
    settings = true;
  }
  if (settings) {
    persist_write_data(P_SETTINGS, &s_settings, sizeof(s_settings));
    update_health_subscription();
    maybe_request_weather(time(NULL));
  }

  layer_mark_dirty(s_canvas);
}

// ---- drawing --------------------------------------------------------------

static void tint(GBitmap *bmp, GColor color) {
  GColor *palette = gbitmap_get_palette(bmp);
  if (!palette) return;
  for (int i = 0; i < 2; i++) {
    if (palette[i].a != 0) palette[i] = color;
  }
}

static void draw_bitmap_at(GContext *ctx, GBitmap *bmp, GPoint origin) {
  GSize size = gbitmap_get_bounds(bmp).size;
  graphics_draw_bitmap_in_rect(ctx, bmp, (GRect){origin, size});
}

// Draws two digits left-aligned from `left`, sitting on `baseline`.
static void draw_digit_pair(GContext *ctx, int value, int left, int baseline, GColor color) {
  int top = baseline - DIGIT_BASELINE;
  int tens = value / 10, ones = value % 10;
  tint(s_digits[tens], color);
  draw_bitmap_at(ctx, s_digits[tens], GPoint(left, top));
  tint(s_digits[ones], color);
  draw_bitmap_at(ctx, s_digits[ones], GPoint(left + DIGIT_ADVANCE[tens], top));
}

static int rail_index(char c) {
  const char *p = strchr(RAIL_CHARS, c);
  return (p && c) ? (int)(p - RAIL_CHARS) : -1;
}

static int rail_length(const char *text, const RailGlyph *metrics) {
  int len = 0;
  for (; *text; text++) {
    int i = rail_index(*text);
    if (i >= 0) len += metrics[i].h;
  }
  return len;
}

// Draws text top-to-bottom (rotated 90° clockwise) starting at `origin`.
static void draw_rail(GContext *ctx, const char *text, int style, const RailGlyph *metrics,
                      GPoint origin) {
  for (; *text; text++) {
    int i = rail_index(*text);
    if (i < 0) continue;
    if (s_rail_glyph[style][i]) draw_bitmap_at(ctx, s_rail_glyph[style][i], origin);
    origin.y += metrics[i].h;
  }
}

static void format_date(char *buf, size_t size, const struct tm *t) {
  static const char *const formats[DATE_FORMAT_COUNT] = {
    [DATE_EU] = "%d.%m.%Y",
    [DATE_US] = "%m/%d/%Y",
    [DATE_ISO] = "%Y-%m-%d",
    [DATE_DAY_MONTH] = "%d %b %Y",
    [DATE_MONTH_DAY] = "%b %d %Y",
  };
  uint8_t f = s_settings.date_format < DATE_FORMAT_COUNT ? s_settings.date_format : DATE_EU;
  strftime(buf, size, formats[f], t);
  upcase(buf);
}

static void draw_battery(GContext *ctx, GPoint o) {
  graphics_context_set_stroke_color(ctx, GColorWhite);
  graphics_draw_rect(ctx, GRect(o.x, o.y, BATTERY_W, 7));
  graphics_context_set_fill_color(ctx, s_charging ? GColorGreen : GColorWhite);
  int w = (9 * s_battery + 50) / 100;
  if (w > 0) graphics_fill_rect(ctx, GRect(o.x + 2, o.y + 2, w, 3), 0, GCornerNone);
}

typedef struct {
  Icon icon;          // unused for the battery
  GColor icon_color;
  bool battery;
  char text[16];
  int width;
} StripItem;

static int collect_strip_items(StripItem *items) {
  int n = 0;
  if (s_settings.show[COMP_WEATHER]) {
    StripItem *it = &items[n++];
    *it = (StripItem){.icon = weather_icon(s_weather.code), .icon_color = GColorWhite};
    if (s_weather.temp_x10 == TEMP_UNKNOWN) {
      strcpy(it->text, "--");
    } else {
      int32_t t10 = s_settings.fahrenheit ? s_weather.temp_x10 * 9 / 5 + 320 : s_weather.temp_x10;
      int deg = (int)((t10 >= 0 ? t10 + 5 : t10 - 5) / 10);
      snprintf(it->text, sizeof(it->text), "%d°%c", deg, s_settings.fahrenheit ? 'F' : 'C');
    }
  }
  if (s_settings.show[COMP_HEART_RATE]) {
    StripItem *it = &items[n++];
    *it = (StripItem){.icon = ICON(ICON_HEART), .icon_color = ACCENT};
    if (s_bpm > 0) snprintf(it->text, sizeof(it->text), "%d", (int)s_bpm);
    else strcpy(it->text, "--");
  }
  if (s_settings.show[COMP_STEPS]) {
    StripItem *it = &items[n++];
    *it = (StripItem){.icon = ICON(ICON_STEPS), .icon_color = GColorWhite};
    snprintf(it->text, sizeof(it->text), "%d", (int)s_steps);
  }
  if (s_settings.show[COMP_BATTERY]) {
    StripItem *it = &items[n++];
    *it = (StripItem){.battery = true};
    snprintf(it->text, sizeof(it->text), "%d%%", s_battery);
  }
  return n;
}

static void draw_strip(GContext *ctx, GRect bounds) {
  StripItem items[COMP_COUNT];
  int n = collect_strip_items(items);
  if (n == 0) return;

  GRect strip = GRect(0, bounds.size.h - STRIP_H, bounds.size.w, STRIP_H);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, strip, 0, GCornerNone);

  const int text_h = 12;
  int total = 0;
  for (int i = 0; i < n; i++) {
    GSize ts = graphics_text_layout_get_content_size(items[i].text, s_font,
                                                     GRect(0, 0, 100, text_h),
                                                     GTextOverflowModeFill, GTextAlignmentLeft);
    int iw = items[i].battery ? BATTERY_W : icon_width(items[i].icon);
    items[i].width = iw + STRIP_ICON_GAP + ts.w;
    total += items[i].width;
  }

  // Spread items edge to edge; a single item is centred.
  int x = STRIP_PAD, space = 0;
  if (n == 1) {
    x = (strip.size.w - total) / 2;
  } else {
    space = (strip.size.w - 2 * STRIP_PAD - total) / (n - 1);
    if (space < 2) space = 2;
  }

  int mid = strip.origin.y + STRIP_H / 2;
  graphics_context_set_text_color(ctx, GColorWhite);
  for (int i = 0; i < n; i++) {
    int iw;
    if (items[i].battery) {
      iw = BATTERY_W;
      draw_battery(ctx, GPoint(x, mid - 4));
    } else {
      iw = icon_width(items[i].icon);
      draw_icon(ctx, items[i].icon, GPoint(x, mid - (items[i].icon.h + 1) / 2),
                items[i].icon_color);
    }
    // Silkscreen 8 px: caps sit 4 px below the top of the line box.
    graphics_draw_text(ctx, items[i].text, s_font,
                       GRect(x + iw + STRIP_ICON_GAP, mid - 7, 100, text_h),
                       GTextOverflowModeFill, GTextAlignmentLeft, NULL);
    x += items[i].width + space;
  }
}

static void canvas_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  graphics_context_set_compositing_mode(ctx, GCompOpSet);

  // The window background is white, so only draw what sits on it.
  draw_bitmap_at(ctx, s_halftone, HALFTONE_ORIGIN);

  // Date rail: weekday from the top, full date down to the strip.
  char day[12], date[16];
  strftime(day, sizeof(day), "%A", &s_now);
  upcase(day);
  format_date(date, sizeof(date), &s_now);
  draw_rail(ctx, day, 0, RAIL_BOLD, GPoint(RAIL_X, RAIL_TOP));
  int date_top = b.size.h - STRIP_H - RAIL_BOTTOM_GAP - rail_length(date, RAIL_REGULAR);
  draw_rail(ctx, date, 1, RAIL_REGULAR, GPoint(RAIL_X, date_top));

  // Staggered rows: minutes sit lower and further right. They either keep a
  // small gap or interlock, with the hours drawn over the minutes.
  int hour = s_now.tm_hour;
  if (!clock_is_24h_style()) {
    hour %= 12;
    if (hour == 0) hour = 12;
  }
  bool overlap = s_settings.overlap_digits;
  draw_digit_pair(ctx, s_now.tm_min, MINUTES_X,
                  overlap ? MINUTES_BASELINE_OVERLAP : MINUTES_BASELINE, ACCENT);
  draw_digit_pair(ctx, hour, HOURS_X,
                  overlap ? HOURS_BASELINE_OVERLAP : HOURS_BASELINE, GColorBlack);

  draw_strip(ctx, b);
}

// ---- services -------------------------------------------------------------

static void battery_handler(BatteryChargeState state) {
  if (state.charge_percent == s_battery && state.is_charging == s_charging) return;
  s_battery = state.charge_percent;
  s_charging = state.is_charging;
  if (s_settings.show[COMP_BATTERY]) layer_mark_dirty(s_canvas);
}

static void play_hourly_vibe(void) {
  if (quiet_time_is_active()) return;
  static const uint32_t double_tap[] = {80, 120, 80};
  static const uint32_t heartbeat[] = {60, 100, 120};
  switch (s_settings.hourly_vibe) {
    case VIBE_SHORT: vibes_short_pulse(); break;
    case VIBE_LONG: vibes_long_pulse(); break;
    case VIBE_DOUBLE:
      vibes_enqueue_custom_pattern((VibePattern){.durations = double_tap, .num_segments = 3});
      break;
    case VIBE_HEARTBEAT:
      vibes_enqueue_custom_pattern((VibePattern){.durations = heartbeat, .num_segments = 3});
      break;
    default: break;
  }
}

static void tick_handler(struct tm *tick_time, TimeUnits changed) {
  s_now = *tick_time;
  if (changed & HOUR_UNIT) play_hourly_vibe();
  maybe_request_weather(time(NULL));
  layer_mark_dirty(s_canvas);
}

// ---- lifecycle ------------------------------------------------------------

static void load_resources(void) {
  static const uint32_t digit_ids[10] = {
    RESOURCE_ID_DIGIT_0, RESOURCE_ID_DIGIT_1, RESOURCE_ID_DIGIT_2, RESOURCE_ID_DIGIT_3,
    RESOURCE_ID_DIGIT_4, RESOURCE_ID_DIGIT_5, RESOURCE_ID_DIGIT_6, RESOURCE_ID_DIGIT_7,
    RESOURCE_ID_DIGIT_8, RESOURCE_ID_DIGIT_9,
  };
  for (int i = 0; i < 10; i++) s_digits[i] = gbitmap_create_with_resource(digit_ids[i]);
  s_halftone = gbitmap_create_with_resource(RESOURCE_ID_HALFTONE);
  s_font = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_SILKSCREEN_BOLD_8));

  s_rail_sheet[0] = gbitmap_create_with_resource(RESOURCE_ID_RAIL_BOLD);
  s_rail_sheet[1] = gbitmap_create_with_resource(RESOURCE_ID_RAIL_REGULAR);
  const RailGlyph *metrics[2] = {RAIL_BOLD, RAIL_REGULAR};
  for (int s = 0; s < 2; s++) {
    for (int i = 0; i < RAIL_GLYPH_COUNT; i++) {
      s_rail_glyph[s][i] = gbitmap_create_as_sub_bitmap(
          s_rail_sheet[s], GRect(0, metrics[s][i].y, RAIL_GLYPH_WIDTH, metrics[s][i].h));
    }
  }
}

static void unload_resources(void) {
  for (int s = 0; s < 2; s++) {
    for (int i = 0; i < RAIL_GLYPH_COUNT; i++) gbitmap_destroy(s_rail_glyph[s][i]);
    gbitmap_destroy(s_rail_sheet[s]);
  }
  for (int i = 0; i < 10; i++) gbitmap_destroy(s_digits[i]);
  gbitmap_destroy(s_halftone);
  fonts_unload_custom_font(s_font);
}

static void load_state(void) {
  for (uint32_t key = 1; key <= 4; key++) {
    if (persist_exists(key)) persist_delete(key);
  }
  if (persist_exists(P_SETTINGS)) persist_read_data(P_SETTINGS, &s_settings, sizeof(s_settings));
  if (persist_exists(P_WEATHER)) persist_read_data(P_WEATHER, &s_weather, sizeof(s_weather));
}

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  s_canvas = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_canvas, canvas_update);
  layer_add_child(root, s_canvas);
}

static void window_unload(Window *window) {
  layer_destroy(s_canvas);
}

static void init(void) {
  load_state();
  load_resources();

  time_t now = time(NULL);
  s_now = *localtime(&now);
  BatteryChargeState bs = battery_state_service_peek();
  s_battery = bs.charge_percent;
  s_charging = bs.is_charging;

  s_window = window_create();
  window_set_background_color(s_window, GColorWhite);
  window_set_window_handlers(s_window, (WindowHandlers){
    .load = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);

  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
  battery_state_service_subscribe(battery_handler);
  update_health_subscription();

  app_message_register_inbox_received(inbox_received);
  app_message_open(256, 64);
}

static void deinit(void) {
  tick_timer_service_unsubscribe();
  battery_state_service_unsubscribe();
#if defined(PBL_HEALTH)
  if (s_health_subscribed) health_service_events_unsubscribe();
#endif
  window_destroy(s_window);
  unload_resources();
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
