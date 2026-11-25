// Poster — a Pebble Time 2 (emery, 200x228) watchface.
// Staggered monospace digits over a halftone block, a vertical date rail and a
// black status strip with weather, heart rate, steps and battery.
#include <pebble.h>
#include "assets.h"

#define ACCENT GColorOrange          // #FF5500
#define STRIP_H 24
#define WEATHER_REFRESH_MIN 30

// Persist keys (1 held whole degrees in v1 and is no longer read)
#define P_CODE 2
#define P_WEATHER_TIME 3
#define P_TEMP_X10 4
#define P_SETTINGS 5

#define TEMP_UNKNOWN INT32_MIN

// Status strip complications, in display order.
enum { COMP_WEATHER, COMP_HEART_RATE, COMP_STEPS, COMP_BATTERY, COMP_COUNT };

typedef enum {
  DATE_EU,          // 25.11.2026
  DATE_US,          // 11/25/2026
  DATE_ISO,         // 2026-11-25
  DATE_DAY_MONTH,   // 25 NOV 2026
  DATE_MONTH_DAY,   // NOV 25 2026
  DATE_FORMAT_COUNT
} DateFormat;

typedef struct {
  bool show[COMP_COUNT];
  bool fahrenheit;
  uint8_t date_format;
} Settings;

typedef enum { WX_NONE, WX_CLEAR, WX_CLOUD, WX_FOG, WX_RAIN, WX_SNOW, WX_STORM } WxKind;

static Window *s_window;
static Layer *s_canvas;
static GBitmap *s_digits[10];
static GBitmap *s_rail_bold, *s_rail_regular;
static GFont s_font;

static Settings s_settings = {
  .show = {true, true, true, true},
  .fahrenheit = false,
  .date_format = DATE_EU,
};

static struct tm s_now;
static int32_t s_temp_x10 = TEMP_UNKNOWN;   // °C × 10
static WxKind s_wx = WX_NONE;
static uint8_t s_battery = 100;
static bool s_charging;
static int32_t s_steps;
static int32_t s_bpm;

// ---- pixel icons ---------------------------------------------------------

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

#define ROWS(a) ((int)(sizeof(a) / sizeof((a)[0])))

static void draw_icon(GContext *ctx, const char *const *rows, int nrows, GPoint origin,
                      GColor color) {
  graphics_context_set_fill_color(ctx, color);
  for (int y = 0; y < nrows; y++) {
    for (int x = 0; rows[y][x]; x++) {
      if (rows[y][x] == '#') {
        graphics_fill_rect(ctx, GRect(origin.x + x, origin.y + y, 1, 1), 0, GCornerNone);
      }
    }
  }
}

static int icon_width(const char *const *rows) { return (int)strlen(rows[0]); }

// ---- weather -------------------------------------------------------------

static WxKind wx_from_wmo(int code) {
  if (code < 0) return WX_NONE;
  if (code <= 1) return WX_CLEAR;
  if (code <= 3) return WX_CLOUD;
  if (code == 45 || code == 48) return WX_FOG;
  if ((code >= 71 && code <= 77) || code == 85 || code == 86) return WX_SNOW;
  if (code >= 95) return WX_STORM;
  if (code >= 51) return WX_RAIN;
  return WX_CLOUD;
}

static void wx_icon(WxKind kind, const char *const **rows, int *nrows) {
  switch (kind) {
    case WX_CLEAR: *rows = ICON_SUN;   *nrows = ROWS(ICON_SUN);   break;
    case WX_FOG:   *rows = ICON_FOG;   *nrows = ROWS(ICON_FOG);   break;
    case WX_RAIN:  *rows = ICON_RAIN;  *nrows = ROWS(ICON_RAIN);  break;
    case WX_SNOW:  *rows = ICON_SNOW;  *nrows = ROWS(ICON_SNOW);  break;
    case WX_STORM: *rows = ICON_STORM; *nrows = ROWS(ICON_STORM); break;
    default:       *rows = ICON_CLOUD; *nrows = ROWS(ICON_CLOUD); break;
  }
}

static void request_weather(void) {
  DictionaryIterator *out;
  if (app_message_outbox_begin(&out) != APP_MSG_OK) return;
  dict_write_uint8(out, MESSAGE_KEY_REQUEST, 1);
  app_message_outbox_send();
}

// Settings arrive from Clay as ints (toggles) or strings (selects).
static int32_t tuple_int(const Tuple *t) {
  switch (t->type) {
    case TUPLE_CSTRING: {
      const char *c = t->value->cstring;
      int32_t v = 0;
      bool neg = (*c == '-');
      if (neg) c++;
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

static void inbox_received(DictionaryIterator *iter, void *context) {
  Tuple *t;
  bool weather = false, settings = false;

  if ((t = dict_find(iter, MESSAGE_KEY_TEMP_X10))) {
    s_temp_x10 = tuple_int(t);
    persist_write_int(P_TEMP_X10, s_temp_x10);
    weather = true;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_CODE))) {
    s_wx = wx_from_wmo(tuple_int(t));
    persist_write_int(P_CODE, tuple_int(t));
    weather = true;
  }
  if (weather) persist_write_int(P_WEATHER_TIME, (int32_t)time(NULL));

  const uint32_t show_keys[COMP_COUNT] = {
    MESSAGE_KEY_SHOW_WEATHER, MESSAGE_KEY_SHOW_HEART_RATE,
    MESSAGE_KEY_SHOW_STEPS, MESSAGE_KEY_SHOW_BATTERY,
  };
  for (int i = 0; i < COMP_COUNT; i++) {
    if ((t = dict_find(iter, show_keys[i]))) {
      s_settings.show[i] = tuple_int(t) != 0;
      settings = true;
    }
  }
  if ((t = dict_find(iter, MESSAGE_KEY_TEMP_UNIT))) {
    const char *unit = t->value->cstring;
    s_settings.fahrenheit = (t->type == TUPLE_CSTRING) ? (*unit == 'F') : (tuple_int(t) != 0);
    settings = true;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_DATE_FORMAT))) {
    int32_t f = tuple_int(t);
    s_settings.date_format = (f >= 0 && f < DATE_FORMAT_COUNT) ? (uint8_t)f : DATE_EU;
    settings = true;
  }
  if (settings) persist_write_data(P_SETTINGS, &s_settings, sizeof(s_settings));

  layer_mark_dirty(s_canvas);
}

// ---- drawing -------------------------------------------------------------

static void tint(GBitmap *bmp, GColor color) {
  GColor *palette = gbitmap_get_palette(bmp);
  if (!palette) return;
  for (int i = 0; i < 2; i++) {
    if (palette[i].a != 0) palette[i] = color;
  }
}

// Draws two digits left-aligned from `left`, sitting on `baseline`.
static void draw_digit_pair(GContext *ctx, int a, int b, int left, int baseline, GColor color) {
  int top = baseline - DIGIT_BASELINE;
  int xs[2] = {left, left + DIGIT_ADVANCE[a]};
  int digits[2] = {a, b};
  for (int i = 0; i < 2; i++) {
    GBitmap *bmp = s_digits[digits[i]];
    tint(bmp, color);
    GSize size = gbitmap_get_bounds(bmp).size;
    graphics_draw_bitmap_in_rect(ctx, bmp, GRect(xs[i], top, size.w, size.h));
  }
}

static void draw_halftone(GContext *ctx, GRect r) {
  graphics_context_set_fill_color(ctx, GColorBlack);
  for (int y = r.origin.y; y < r.origin.y + r.size.h; y += 4) {
    for (int x = r.origin.x; x < r.origin.x + r.size.w; x += 4) {
      graphics_fill_rect(ctx, GRect(x, y, 2, 2), 0, GCornerNone);
    }
  }
}

static int rail_index(char c) {
  const char *p = strchr(RAIL_CHARS, c);
  return p ? (int)(p - RAIL_CHARS) : -1;
}

static int rail_length(const char *text, const RailGlyph *glyphs) {
  int len = 0;
  for (const char *c = text; *c; c++) {
    int i = rail_index(*c);
    if (i >= 0) len += glyphs[i].h;
  }
  return len;
}

// Draws text top-to-bottom (rotated 90° clockwise) starting at `origin`.
static void draw_rail(GContext *ctx, const char *text, GBitmap *sheet, const RailGlyph *glyphs,
                      GPoint origin) {
  int y = origin.y;
  for (const char *c = text; *c; c++) {
    int i = rail_index(*c);
    if (i < 0) continue;
    GBitmap *g = gbitmap_create_as_sub_bitmap(
        sheet, GRect(0, glyphs[i].y, RAIL_GLYPH_WIDTH, glyphs[i].h));
    if (g) {
      graphics_draw_bitmap_in_rect(ctx, g, GRect(origin.x, y, RAIL_GLYPH_WIDTH, glyphs[i].h));
      gbitmap_destroy(g);
    }
    y += glyphs[i].h;
  }
}

static void draw_battery(GContext *ctx, GPoint o) {
  graphics_context_set_stroke_color(ctx, GColorWhite);
  graphics_draw_rect(ctx, GRect(o.x, o.y, 13, 7));
  graphics_context_set_fill_color(ctx, s_charging ? GColorGreen : GColorWhite);
  int w = (9 * s_battery + 50) / 100;
  if (w > 0) graphics_fill_rect(ctx, GRect(o.x + 2, o.y + 2, w, 3), 0, GCornerNone);
}

typedef struct {
  const char *const *icon;
  int icon_rows;
  GColor icon_color;
  bool battery;
  char text[16];
  int width;
} StripItem;

static void draw_strip(GContext *ctx, GRect bounds) {
  GRect strip = GRect(0, bounds.size.h - STRIP_H, bounds.size.w, STRIP_H);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, strip, 0, GCornerNone);

  StripItem items[COMP_COUNT];
  memset(items, 0, sizeof(items));
  int n = 0;

  if (s_settings.show[COMP_WEATHER]) {
    StripItem *it = &items[n++];
    wx_icon(s_wx, &it->icon, &it->icon_rows);
    it->icon_color = GColorWhite;
    if (s_temp_x10 == TEMP_UNKNOWN) {
      snprintf(it->text, sizeof(it->text), "--");
    } else {
      int32_t t10 = s_settings.fahrenheit ? s_temp_x10 * 9 / 5 + 320 : s_temp_x10;
      int deg = (int)((t10 >= 0 ? t10 + 5 : t10 - 5) / 10);
      snprintf(it->text, sizeof(it->text), "%d°%c", deg, s_settings.fahrenheit ? 'F' : 'C');
    }
  }
  if (s_settings.show[COMP_HEART_RATE]) {
    StripItem *it = &items[n++];
    it->icon = ICON_HEART;
    it->icon_rows = ROWS(ICON_HEART);
    it->icon_color = ACCENT;
    if (s_bpm > 0) snprintf(it->text, sizeof(it->text), "%d", (int)s_bpm);
    else snprintf(it->text, sizeof(it->text), "--");
  }
  if (s_settings.show[COMP_STEPS]) {
    StripItem *it = &items[n++];
    it->icon = ICON_STEPS;
    it->icon_rows = ROWS(ICON_STEPS);
    it->icon_color = GColorWhite;
    snprintf(it->text, sizeof(it->text), "%d", (int)s_steps);
  }
  if (s_settings.show[COMP_BATTERY]) {
    StripItem *it = &items[n++];
    it->battery = true;
    snprintf(it->text, sizeof(it->text), "%d%%", s_battery);
  }

  const int pad = 6, gap = 3, text_h = 12;
  int total = 0;
  for (int i = 0; i < n; i++) {
    GSize ts = graphics_text_layout_get_content_size(items[i].text, s_font,
                                                     GRect(0, 0, 100, text_h),
                                                     GTextOverflowModeFill, GTextAlignmentLeft);
    int iw = items[i].battery ? 13 : icon_width(items[i].icon);
    items[i].width = iw + gap + ts.w;
    total += items[i].width;
  }

  // Spread items edge to edge; a single item is centred.
  int x, space;
  if (n == 1) {
    x = (strip.size.w - total) / 2;
    space = 0;
  } else {
    x = pad;
    space = (strip.size.w - 2 * pad - total) / (n - 1);
    if (space < 2) space = 2;
  }

  int mid = strip.origin.y + STRIP_H / 2;
  graphics_context_set_text_color(ctx, GColorWhite);
  for (int i = 0; i < n; i++) {
    int iw;
    if (items[i].battery) {
      iw = 13;
      draw_battery(ctx, GPoint(x, mid - 4));
    } else {
      iw = icon_width(items[i].icon);
      draw_icon(ctx, items[i].icon, items[i].icon_rows,
                GPoint(x, mid - (items[i].icon_rows + 1) / 2), items[i].icon_color);
    }
    // Silkscreen 8 px: caps sit 4 px below the top of the line box.
    graphics_draw_text(ctx, items[i].text, s_font,
                       GRect(x + iw + gap, mid - 7, 100, text_h),
                       GTextOverflowModeFill, GTextAlignmentLeft, NULL);
    x += items[i].width + space;
  }
}

static bool strip_visible(void) {
  for (int i = 0; i < COMP_COUNT; i++) {
    if (s_settings.show[i]) return true;
  }
  return false;
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
  for (char *c = buf; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
}

static void canvas_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  graphics_context_set_compositing_mode(ctx, GCompOpSet);

  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  draw_halftone(ctx, GRect(26, 12, 98, 180));

  // Date rail: weekday from the top, full date to the bottom.
  char day[12], date[16];
  strftime(day, sizeof(day), "%A", &s_now);
  for (char *c = day; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
  format_date(date, sizeof(date), &s_now);
  int rail_x = 10;
  draw_rail(ctx, day, s_rail_bold, RAIL_BOLD, GPoint(rail_x, 8));
  int date_len = rail_length(date, RAIL_REGULAR);
  draw_rail(ctx, date, s_rail_regular, RAIL_REGULAR,
            GPoint(rail_x, b.size.h - STRIP_H - 6 - date_len));

  // Time
  int hour = s_now.tm_hour;
  if (!clock_is_24h_style()) {
    hour %= 12;
    if (hour == 0) hour = 12;
  }
  // Staggered rows: minutes sit lower and further right, and the hours are
  // drawn over them where the two interlock.
  draw_digit_pair(ctx, s_now.tm_min / 10, s_now.tm_min % 10, 58, 170, ACCENT);
  draw_digit_pair(ctx, hour / 10, hour % 10, 44, 104, GColorBlack);

  if (strip_visible()) draw_strip(ctx, b);
}

// ---- services ------------------------------------------------------------

static void update_health(void) {
#if defined(PBL_HEALTH)
  time_t start = time_start_of_today(), end = time(NULL);
  if (health_service_metric_accessible(HealthMetricStepCount, start, end) &
      HealthServiceAccessibilityMaskAvailable) {
    s_steps = (int32_t)health_service_sum_today(HealthMetricStepCount);
  }
  s_bpm = (int32_t)health_service_peek_current_value(HealthMetricHeartRateBPM);
#endif
}

#if defined(PBL_HEALTH)
static void health_handler(HealthEventType event, void *context) {
  update_health();
  layer_mark_dirty(s_canvas);
}
#endif

static void battery_handler(BatteryChargeState state) {
  s_battery = state.charge_percent;
  s_charging = state.is_charging;
  layer_mark_dirty(s_canvas);
}

static void tick_handler(struct tm *tick_time, TimeUnits changed) {
  s_now = *tick_time;
  int32_t last = persist_exists(P_WEATHER_TIME) ? persist_read_int(P_WEATHER_TIME) : 0;
  if (time(NULL) - last >= WEATHER_REFRESH_MIN * 60) request_weather();
  layer_mark_dirty(s_canvas);
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
  static const uint32_t digit_ids[10] = {
    RESOURCE_ID_DIGIT_0, RESOURCE_ID_DIGIT_1, RESOURCE_ID_DIGIT_2, RESOURCE_ID_DIGIT_3,
    RESOURCE_ID_DIGIT_4, RESOURCE_ID_DIGIT_5, RESOURCE_ID_DIGIT_6, RESOURCE_ID_DIGIT_7,
    RESOURCE_ID_DIGIT_8, RESOURCE_ID_DIGIT_9,
  };
  for (int i = 0; i < 10; i++) s_digits[i] = gbitmap_create_with_resource(digit_ids[i]);
  s_rail_bold = gbitmap_create_with_resource(RESOURCE_ID_RAIL_BOLD);
  s_rail_regular = gbitmap_create_with_resource(RESOURCE_ID_RAIL_REGULAR);
  s_font = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_SILKSCREEN_BOLD_8));

  if (persist_exists(P_SETTINGS)) {
    persist_read_data(P_SETTINGS, &s_settings, sizeof(s_settings));
  }
  if (persist_exists(P_TEMP_X10)) s_temp_x10 = persist_read_int(P_TEMP_X10);
  if (persist_exists(P_CODE)) s_wx = wx_from_wmo(persist_read_int(P_CODE));

  time_t t = time(NULL);
  s_now = *localtime(&t);
  BatteryChargeState bs = battery_state_service_peek();
  s_battery = bs.charge_percent;
  s_charging = bs.is_charging;
  update_health();

  s_window = window_create();
  window_set_background_color(s_window, GColorWhite);
  window_set_window_handlers(s_window, (WindowHandlers){
    .load = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);

  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
  battery_state_service_subscribe(battery_handler);
#if defined(PBL_HEALTH)
  health_service_events_subscribe(health_handler, NULL);
#endif

  app_message_register_inbox_received(inbox_received);
  app_message_open(256, 64);
}

static void deinit(void) {
  tick_timer_service_unsubscribe();
  battery_state_service_unsubscribe();
#if defined(PBL_HEALTH)
  health_service_events_unsubscribe();
#endif
  window_destroy(s_window);
  for (int i = 0; i < 10; i++) gbitmap_destroy(s_digits[i]);
  gbitmap_destroy(s_rail_bold);
  gbitmap_destroy(s_rail_regular);
  fonts_unload_custom_font(s_font);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
