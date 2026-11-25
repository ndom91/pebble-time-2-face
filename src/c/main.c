// Poster — a Pebble Time 2 (emery, 200x228) watchface.
// Big condensed digits over a halftone block, a vertical date rail and a
// black status strip with weather, heart rate, steps and battery.
#include <pebble.h>
#include "assets.h"

#define ACCENT GColorOrange          // #FF5500
#define STRIP_H 24
#define WEATHER_REFRESH_MIN 30

// Persist keys
#define P_TEMP 1
#define P_CODE 2
#define P_WEATHER_TIME 3

#define TEMP_UNKNOWN INT32_MIN

typedef enum { WX_NONE, WX_CLEAR, WX_CLOUD, WX_FOG, WX_RAIN, WX_SNOW, WX_STORM } WxKind;

static Window *s_window;
static Layer *s_canvas;
static GBitmap *s_digits[10];
static GBitmap *s_rail_bold, *s_rail_regular;
static GFont s_font;

static struct tm s_now;
static int32_t s_temp = TEMP_UNKNOWN;
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

static void inbox_received(DictionaryIterator *iter, void *context) {
  Tuple *temp = dict_find(iter, MESSAGE_KEY_TEMP);
  Tuple *code = dict_find(iter, MESSAGE_KEY_CODE);
  if (temp) {
    s_temp = temp->value->int32;
    persist_write_int(P_TEMP, s_temp);
  }
  if (code) {
    s_wx = wx_from_wmo(code->value->int32);
    persist_write_int(P_CODE, code->value->int32);
  }
  persist_write_int(P_WEATHER_TIME, (int32_t)time(NULL));
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

// Draws two digits right-aligned so the last one ends at `right`.
static void draw_digit_pair(GContext *ctx, int a, int b, int right, int top, GColor color) {
  const int tracking = -3;
  int xb = right - DIGIT_ADVANCE[b];
  int xa = xb - (DIGIT_ADVANCE[a] + tracking);
  int digits[2] = {a, b};
  int xs[2] = {xa, xb};
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

  StripItem items[4];
  memset(items, 0, sizeof(items));

  wx_icon(s_wx, &items[0].icon, &items[0].icon_rows);
  items[0].icon_color = GColorWhite;
  if (s_temp == TEMP_UNKNOWN) snprintf(items[0].text, sizeof(items[0].text), "--");
  else snprintf(items[0].text, sizeof(items[0].text), "%d°C", (int)s_temp);

  items[1].icon = ICON_HEART;
  items[1].icon_rows = ROWS(ICON_HEART);
  items[1].icon_color = ACCENT;
  if (s_bpm > 0) snprintf(items[1].text, sizeof(items[1].text), "%d", (int)s_bpm);
  else snprintf(items[1].text, sizeof(items[1].text), "--");

  items[2].icon = ICON_STEPS;
  items[2].icon_rows = ROWS(ICON_STEPS);
  items[2].icon_color = GColorWhite;
  snprintf(items[2].text, sizeof(items[2].text), "%d", (int)s_steps);

  items[3].battery = true;
  snprintf(items[3].text, sizeof(items[3].text), "%d%%", s_battery);

  const int pad = 6, gap = 3, text_h = 12;
  int total = 0;
  for (int i = 0; i < 4; i++) {
    GSize ts = graphics_text_layout_get_content_size(items[i].text, s_font,
                                                     GRect(0, 0, 100, text_h),
                                                     GTextOverflowModeFill, GTextAlignmentLeft);
    int iw = items[i].battery ? 13 : icon_width(items[i].icon);
    items[i].width = iw + gap + ts.w;
    total += items[i].width;
  }
  int space = (strip.size.w - 2 * pad - total) / 3;
  if (space < 2) space = 2;

  int x = pad;
  int mid = strip.origin.y + STRIP_H / 2;
  graphics_context_set_text_color(ctx, GColorWhite);
  for (int i = 0; i < 4; i++) {
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

static void canvas_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  graphics_context_set_compositing_mode(ctx, GCompOpSet);

  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  draw_halftone(ctx, GRect(26, 12, 98, 180));

  // Date rail: weekday from the top, full date to the bottom.
  char day[12], date[12];
  strftime(day, sizeof(day), "%A", &s_now);
  for (char *c = day; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
  strftime(date, sizeof(date), "%d.%m.%Y", &s_now);
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
  const int right = b.size.w - 3;
  const int top = 10, line = 98;
  draw_digit_pair(ctx, hour / 10, hour % 10, right, top, GColorBlack);
  draw_digit_pair(ctx, s_now.tm_min / 10, s_now.tm_min % 10, right, top + line, ACCENT);

  draw_strip(ctx, b);
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

  if (persist_exists(P_TEMP)) s_temp = persist_read_int(P_TEMP);
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
  app_message_open(64, 64);
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
