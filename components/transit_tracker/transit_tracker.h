#pragma once

#include <map>
#include <vector>
#include <ArduinoWebsockets.h>

#include "esphome/core/component.h"
#include "esphome/components/display/display.h"
#include "esphome/components/font/font.h"
#include "esphome/components/time/real_time_clock.h"

#include "schedule_state.h"
#include "localization.h"

namespace esphome {
namespace transit_tracker {

enum RouteDisplayMode : uint8_t {
  ROUTE_DISPLAY_NAME,
  ROUTE_DISPLAY_NUMBERED,
};

struct RouteStyle {
  std::string name;
  Color color;
};

class TransitTracker : public Component {
  public:
    void setup() override;
    void loop() override;
    void dump_config() override;
    void on_shutdown() override;

    float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

    void reconnect();
    void close(bool fully = false);

    void draw_schedule(int page = -1);
    void draw_split_schedule();
    void next_page();
    int get_page_count() const;
    int get_current_page() const { return current_page_; }
    std::vector<std::string> get_unique_route_ids() const;

    Localization* get_localization() { return &this->localization_; }

    void set_display(display::Display *display) { display_ = display; }
    void set_font(font::Font *font) { font_ = font; }
    void set_rtc(time::RealTimeClock *rtc) { rtc_ = rtc; }

    void set_base_url(const std::string &base_url) { base_url_ = base_url; }
    void set_feed_code(const std::string &feed_code) { feed_code_ = feed_code; }
    void set_display_departure_times(bool display_departure_times) { display_departure_times_ = display_departure_times; }
    void set_schedule_string(const std::string &schedule_string) { schedule_string_ = schedule_string; }
    void set_list_mode(const std::string &list_mode) { list_mode_ = list_mode; }
    void set_limit(int limit) { limit_ = limit; }
    void set_request_trips(int request_trips) { request_trips_ = request_trips; }
    void set_scroll_headsigns(bool scroll_headsigns) { scroll_headsigns_ = scroll_headsigns; }

    void set_unit_display(UnitDisplay unit_display) { this->localization_.set_unit_display(unit_display); }
    void add_abbreviation(const std::string &from, const std::string &to) { abbreviations_[from] = to; }
    void set_default_route_color(const Color &color) { default_route_color_ = color; }
    void add_route_style(const std::string &route_id, const std::string &name, const Color &color) { route_styles_[route_id] = RouteStyle{name, color}; }

    void set_abbreviations_from_text(const std::string &text);
    void set_route_styles_from_text(const std::string &text);

    void set_route_display_mode(RouteDisplayMode mode) { route_display_mode_ = mode; }
    void set_time_color(const Color &color) { time_color_ = color; }
    void set_show_realtime_icon(bool show) { show_realtime_icon_ = show; }
    void set_show_line_icons(bool show) { show_line_icons_ = show; }

    void set_sort_order_from_text(const std::string &text);

    void set_realtime_color(const Color &color);

  protected:
    static constexpr int scroll_speed = 10; // pixels/second
    static constexpr int idle_time_left = 5000;
    static constexpr int idle_time_right = 1000;

    // Total horizontal gap (in pixels) left empty between the two halves in
    // draw_split_schedule(), so the left pane's right-aligned time does not abut
    // the right pane's left-aligned route label.
    static constexpr int split_gutter = 6;

    void draw_text_centered_(const char *text, Color color);
    void draw_realtime_icon_(int bottom_right_x, int bottom_right_y, unsigned long now);

    // Runs the pre-render guard checks (network, time, errors, empty schedule),
    // drawing a centered status message when something isn't ready yet.
    // Returns true when the schedule is ready to be drawn.
    bool draw_guards_();

    // Renders a single trip page (one route) into a horizontal sub-region of the
    // display. region_x is the left edge and region_width the width of the region;
    // pass the full display width for a normal full-screen render. When allow_clamp
    // is true an out-of-range page wraps back to page 0 (auto-advance behavior);
    // otherwise an out-of-range page renders nothing (used for split panes).
    void draw_schedule_region_(int page, int region_x, int region_width, bool allow_clamp);

    // Renders all upcoming trips for a single route into a horizontal sub-region.
    // Locks the schedule mutex internally; draws nothing if the route has no trips.
    void draw_route_region_(const std::string &route_id, int region_x, int region_width);

    // Route IDs in the order they were configured (parsed from schedule_string_),
    // deduplicated. Used by draw_split_schedule() so each pane is pinned to a fixed
    // line/direction rather than reordering with arrival times.
    std::vector<std::string> get_configured_route_ids_() const;

    void draw_trip(
      const Trip &trip, int trip_index, int y_offset, int font_height, unsigned long uptime, uint rtc_now,
      int region_x, int region_width,
      bool no_draw = false, int *headsign_overflow_out = nullptr, int scroll_cycle_duration = 0,
      int route_column_width = 0
    );

    Localization localization_{};
    ScheduleState schedule_state_;

    display::Display *display_;
    font::Font *font_;
    time::RealTimeClock *rtc_;

    websockets::WebsocketsClient ws_client_{};

    void on_ws_message_(websockets::WebsocketsMessage message);
    void on_ws_event_(websockets::WebsocketsEvent event, String data);
    void connect_ws_();
    int connection_attempts_ = 0;
    unsigned long last_heartbeat_ = 0;
    bool has_ever_connected_ = false;
    bool fully_closed_ = false;

    std::string base_url_;
    std::string feed_code_;
    std::string schedule_string_;
    std::string list_mode_;
    bool display_departure_times_ = true;
    int limit_;
    int request_trips_ = 0;  // 0 means same as limit_

    int current_page_ = 0;
    unsigned long last_draw_time_ = 0;

    std::map<std::string, std::string> abbreviations_;
    Color default_route_color_ = Color(0x028e51);
    std::map<std::string, RouteStyle> route_styles_;
    bool scroll_headsigns_ = false;

    RouteDisplayMode route_display_mode_ = ROUTE_DISPLAY_NAME;
    Color time_color_ = Color(0xa7a7a7);
    bool show_realtime_icon_ = true;
    bool show_line_icons_ = true;
    std::vector<std::string> sort_order_;

    Color realtime_color_ = Color(0x20FF00);
    Color realtime_color_dark_ = Color(0x00A700);
};


}  // namespace transit_tracker
}  // namespace esphome