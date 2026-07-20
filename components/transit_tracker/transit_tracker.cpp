#include <algorithm>

#include "transit_tracker.h"
#include "string_utils.h"

#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esphome/components/json/json_util.h"
#include "esphome/components/watchdog/watchdog.h"
#include "esphome/components/network/util.h"

namespace esphome {
namespace transit_tracker {

static const char *TAG = "transit_tracker.component";

void TransitTracker::setup() {
  this->ws_client_.onMessage([this](websockets::WebsocketsMessage message) {
    this->on_ws_message_(message);
  });

  this->ws_client_.onEvent([this](websockets::WebsocketsEvent event, String data) {
    this->on_ws_event_(event, data);
  });

  this->connect_ws_();

  this->set_interval("check_stale_trips", 10000, [this]() {
    if (this->ws_client_.available() && !this->schedule_state_.trips.empty()) {
      bool has_stale_trips = false;

      this->schedule_state_.mutex.lock();

      auto now = this->rtc_->now();
      if (now.is_valid()) {
        for (auto &trip : this->schedule_state_.trips) {
          if (now.timestamp - trip.departure_time > 60) {
            has_stale_trips = true;
            break;
          }
        }
      }

      this->schedule_state_.mutex.unlock();

      if (has_stale_trips) {
        ESP_LOGD(TAG, "Stale trips detected, reconnecting");
        ESP_LOGD(TAG, "  Current RTC time: %d", now.timestamp);
        ESP_LOGD(TAG, "  Last heartbeat: %d", this->last_heartbeat_);
        this->reconnect();
      }
    }
  });
}

void TransitTracker::loop() {
  this->ws_client_.poll();

  if (this->last_heartbeat_ != 0 && millis() - this->last_heartbeat_ > 60000) {
    ESP_LOGW(TAG, "Heartbeat timeout, reconnecting");
    this->reconnect();
    return;
  }
}

void TransitTracker::dump_config() {
  ESP_LOGCONFIG(TAG, "Transit Tracker:");
  ESP_LOGCONFIG(TAG, "  Base URL: %s", this->base_url_.c_str());
  ESP_LOGCONFIG(TAG, "  Schedule: %s", this->schedule_string_.c_str());
  ESP_LOGCONFIG(TAG, "  Limit (per page): %d", this->limit_);
  ESP_LOGCONFIG(TAG, "  Request trips: %d", this->request_trips_ > 0 ? this->request_trips_ : this->limit_);
  ESP_LOGCONFIG(TAG, "  List mode: %s", this->list_mode_.c_str());
  ESP_LOGCONFIG(TAG, "  Display departure times: %s", this->display_departure_times_ ? "true" : "false");
  ESP_LOGCONFIG(TAG, "  Scroll Headsigns: %s", this->scroll_headsigns_ ? "true" : "false");
  ESP_LOGCONFIG(TAG, "  Route display mode: %s", this->route_display_mode_ == ROUTE_DISPLAY_NUMBERED ? "numbered" : "route_name");
  ESP_LOGCONFIG(TAG, "  Show realtime icon: %s", this->show_realtime_icon_ ? "true" : "false");
  ESP_LOGCONFIG(TAG, "  Show line icons: %s", this->show_line_icons_ ? "true" : "false");
}

void TransitTracker::reconnect() {
  this->close();
  this->connect_ws_();
}

void TransitTracker::close(bool fully) {
  if (fully) {
    this->fully_closed_ = true;
  }

  this->ws_client_.close();
}

void TransitTracker::on_shutdown() {
  this->cancel_interval("check_stale_trips");
  this->close(true);
}

void TransitTracker::on_ws_message_(websockets::WebsocketsMessage message) {
  ESP_LOGV(TAG, "Received message: %s", message.rawData().c_str());

  bool valid = json::parse_json(message.rawData(), [this](JsonObject root) -> bool {
    if (root["event"].as<std::string>() == "heartbeat") {
      ESP_LOGD(TAG, "Received heartbeat");
      this->last_heartbeat_ = millis();
      return true;
    }

    if (root["event"].as<std::string>() != "schedule") {
      return true;
    }

    ESP_LOGD(TAG, "Received schedule update");

    this->schedule_state_.mutex.lock();

    this->schedule_state_.trips.clear();

    auto data = root["data"].as<JsonObject>();

    for (auto trip : data["trips"].as<JsonArray>()) {
      std::string headsign = trip["headsign"].as<std::string>();
      for (const auto &abbr : this->abbreviations_) {
        size_t pos = headsign.find(abbr.first);
        if (pos != std::string::npos) {
          ESP_LOGV(TAG, "Applying abbreviation '%s' -> '%s' in headsign", abbr.first.c_str(), abbr.second.c_str());
          headsign.replace(pos, abbr.first.length(), abbr.second);
        }
      }

      auto route_id = trip["routeId"].as<std::string>();
      auto route_style = this->route_styles_.find(route_id);

      Color route_color = this->default_route_color_;
      std::string route_name = trip["routeName"].as<std::string>();

      if (route_style != this->route_styles_.end()) {
        route_color = route_style->second.color;
        route_name = route_style->second.name;
      } else if (!trip["routeColor"].isNull()) {
        route_color = Color(std::stoul(trip["routeColor"].as<std::string>(), nullptr, 16));
      }

      this->schedule_state_.trips.push_back({
        .route_id = route_id,
        .route_name = route_name,
        .route_color = route_color,
        .headsign = headsign,
        .arrival_time = trip["arrivalTime"].as<time_t>(),
        .departure_time = trip["departureTime"].as<time_t>(),
        .is_realtime = trip["isRealtime"].as<bool>(),
      });
    }

    this->schedule_state_.mutex.unlock();

    return true;
  });

  if (!valid) {
    this->status_set_error(LOG_STR("Failed to parse schedule data"));
    return;
  }
}

void TransitTracker::on_ws_event_(websockets::WebsocketsEvent event, String data) {
  if (event == websockets::WebsocketsEvent::ConnectionOpened) {
    ESP_LOGD(TAG, "WebSocket connection opened");

    auto message = json::build_json([this](JsonObject root) {
      root["event"] = "schedule:subscribe";

      auto data = root["data"].to<JsonObject>();

      if (!this->feed_code_.empty()) {
        data["feedCode"] = this->feed_code_;
      }

      data["routeStopPairs"] = this->schedule_string_;
      data["limit"] = this->request_trips_ > 0 ? this->request_trips_ : this->limit_;
      data["sortByDeparture"] = this->display_departure_times_;
      data["listMode"] = this->list_mode_;
    });

    ESP_LOGV(TAG, "Sending message: %s", message.c_str());
    this->ws_client_.send(message.c_str());
  } else if (event == websockets::WebsocketsEvent::ConnectionClosed) {
    ESP_LOGD(TAG, "WebSocket connection closed");
    if (!this->fully_closed_ && this->connection_attempts_ == 0) {
      this->defer([this]() {
        this->connect_ws_();
      });
    }
  } else if (event == websockets::WebsocketsEvent::GotPing) {
    ESP_LOGV(TAG, "Received ping");
  } else if (event == websockets::WebsocketsEvent::GotPong) {
    ESP_LOGV(TAG, "Received pong");
  }
}

void TransitTracker::connect_ws_() {
  if (this->base_url_.empty()) {
    ESP_LOGW(TAG, "No base URL set, not connecting");
    return;
  }

  if (this->fully_closed_) {
    ESP_LOGW(TAG, "Connection fully closed, not reconnecting");
    return;
  }

  if (this->ws_client_.available(true)) {
    ESP_LOGV(TAG, "Not reconnecting, already connected");
    return;
  }

  watchdog::WatchdogManager wdm(20000);

  this->last_heartbeat_ = 0;

  ESP_LOGD(TAG, "Connecting to WebSocket server (attempt %d): %s", this->connection_attempts_, this->base_url_.c_str());

  bool connection_success = false;
  if (esphome::network::is_connected()) {
    connection_success = this->ws_client_.connect(this->base_url_.c_str());
  } else {
    ESP_LOGW(TAG, "Not connected to network; skipping connection attempt");
  }

  if (!connection_success) {
    this->connection_attempts_++;

    if (this->connection_attempts_ >= 3) {
      this->status_set_error(LOG_STR("Failed to connect to WebSocket server"));
    }

    if (this->connection_attempts_ >= 15) {
      ESP_LOGE(TAG, "Could not connect to WebSocket server within 15 attempts.");
      ESP_LOGE(TAG, "It's likely that the network is not truly connected; rebooting the device to try to recover.");
      App.reboot();
    }

    auto timeout = std::min(15000, this->connection_attempts_ * 5000);
    ESP_LOGW(TAG, "Failed to connect, retrying in %ds", timeout / 1000);

    this->set_timeout("reconnect", timeout, [this]() {
      this->connect_ws_();
    });
  } else {
    this->has_ever_connected_ = true;
    this->connection_attempts_ = 0;
    this->status_clear_error();
  }
}

void TransitTracker::set_abbreviations_from_text(const std::string &text) {
  this->abbreviations_.clear();
  for (const auto &line : split(text, '\n')) {
    auto parts = split(line, ';');

    if (parts.size() == 1) {
      // If only one part is provided, treat it as a removal (replace with empty string)
      this->add_abbreviation(parts[0], "");
      continue;
    }

    if (parts.size() != 2) {
      ESP_LOGW(TAG, "Invalid abbreviation line: %s", line.c_str());
      continue;
    }

    this->add_abbreviation(parts[0], parts[1]);
  }
}

void TransitTracker::set_route_styles_from_text(const std::string &text) {
  this->route_styles_.clear();
  for (const auto &line : split(text, '\n')) {
    auto parts = split(line, ';');
    if (parts.size() != 3) {
      ESP_LOGW(TAG, "Invalid route style line: %s", line.c_str());
      continue;
    }
    uint32_t color = std::stoul(parts[2], nullptr, 16);
    this->add_route_style(parts[0], parts[1], Color(color));
  }
}

void TransitTracker::set_sort_order_from_text(const std::string &text) {
  this->sort_order_.clear();
  for (const auto &line : split(text, '\n')) {
    if (!line.empty()) {
      this->sort_order_.push_back(line);
    }
  }
  ESP_LOGD(TAG, "Set sort order with %d entries", this->sort_order_.size());
}

void TransitTracker::next_page() {
  int total_pages = this->get_page_count();
  if (total_pages <= 1) {
    this->current_page_ = 0;
    return;
  }
  this->current_page_ = (this->current_page_ + 1) % total_pages;
  ESP_LOGD(TAG, "Advancing to page %d of %d", this->current_page_ + 1, total_pages);
}

std::vector<std::string> TransitTracker::get_unique_route_ids() const {
  std::vector<std::string> unique_routes;
  
  // Preserve the order routes appear in trips, but only add each route once
  for (const auto &trip : this->schedule_state_.trips) {
    bool found = false;
    for (const auto &route_id : unique_routes) {
      if (route_id == trip.route_id) {
        found = true;
        break;
      }
    }
    if (!found) {
      unique_routes.push_back(trip.route_id);
    }
  }
  
  return unique_routes;
}

int TransitTracker::get_page_count() const {
  if (this->schedule_state_.trips.empty()) {
    return 1;
  }
  
  // Count the number of unique routes (each route gets its own page)
  return this->get_unique_route_ids().size();
}

void TransitTracker::draw_text_centered_(const char *text, Color color) {
  int display_center_x = this->display_->get_width() / 2;
  int display_center_y = this->display_->get_height() / 2;
  this->display_->print(display_center_x, display_center_y, this->font_, color, display::TextAlign::CENTER, text);
}

void TransitTracker::set_realtime_color(const Color &color) {
  this->realtime_color_ = color;
  this->realtime_color_dark_ = Color(
    (color.r * 0.5),
    (color.g * 0.5),
    (color.b * 0.5)
  );
}

const uint8_t realtime_icon[6][6] = {
  {0, 0, 0, 3, 3, 3},
  {0, 0, 3, 0, 0, 0},
  {0, 3, 0, 0, 2, 2},
  {3, 0, 0, 2, 0, 0},
  {3, 0, 2, 0, 0, 1},
  {3, 0, 2, 0, 1, 1}
};

void HOT TransitTracker::draw_realtime_icon_(int bottom_right_x, int bottom_right_y, unsigned long uptime) {
  const int num_frames = 6;
  const int idle_frame_duration = 3000;
  const int anim_frame_duration = 200;
  const int cycle_duration = idle_frame_duration + (num_frames - 1) * anim_frame_duration;

  unsigned long cycle_time = uptime % cycle_duration;

  int frame;
  if (cycle_time < idle_frame_duration) {
    frame = 0;
  } else {
    frame = 1 + (cycle_time - idle_frame_duration) / anim_frame_duration;
  }

  auto is_segment_lit = [frame](uint8_t segment) {
    switch (segment) {
      case 1: return frame >= 1 && frame <= 3;
      case 2: return frame >= 2 && frame <= 4;
      case 3: return frame >= 3 && frame <= 5;
      default: return false;
    }
  };

  for (uint8_t i = 0; i < 6; ++i) {
    for (uint8_t j = 0; j < 6; ++j) {
      uint8_t segment_number = realtime_icon[i][j];
      if (segment_number == 0) {
        continue;
      }

      Color icon_color = is_segment_lit(segment_number) ? this->realtime_color_ : this->realtime_color_dark_;
      this->display_->draw_pixel_at(bottom_right_x - (5 - j), bottom_right_y - (5 - i), icon_color);
    }
  }
}

void TransitTracker::draw_trip(
    const Trip &trip, int trip_index, int y_offset, int font_height, unsigned long uptime, uint rtc_now,
    int region_x, int region_width,
    bool no_draw, int *headsign_overflow_out, int scroll_cycle_duration,
    int route_column_width
) {
    int region_right = region_x + region_width;
    int route_width = 0;
    int _;

    if (this->show_line_icons_) {
      // Determine what to display in the route label position
      std::string route_label;

      if (this->route_display_mode_ == ROUTE_DISPLAY_NUMBERED) {
        route_label = str_sprintf("%d ", trip_index + 1);
      } else {
        route_label = trip.route_name;
      }

      if (!no_draw) {
        this->display_->print(region_x, y_offset, this->font_, trip.route_color, display::TextAlign::TOP_LEFT, route_label.c_str());
      }

      this->font_->measure(route_label.c_str(), &route_width, &_, &_, &_);

      // Use a consistent column width when provided, so that headsign text
      // aligns across rows even when route labels have different widths
      // (e.g. "1 " is narrower than "2 " in proportional fonts).
      if (route_column_width > 0) {
        route_width = route_column_width;
      }
    }

    auto time_display = this->localization_.fmt_duration_from_now(
      this->display_departure_times_ ? trip.departure_time : trip.arrival_time,
      rtc_now
    );

    int time_width;
    this->font_->measure(time_display.c_str(), &time_width, &_, &_, &_);

    int headsign_clipping_start = this->show_line_icons_ ? region_x + route_width + 3 : region_x;
    int headsign_clipping_end = region_right - time_width - 2;

    if (!no_draw) {
      Color time_color = trip.is_realtime ? this->realtime_color_ : this->time_color_;
      this->display_->print(region_right, y_offset, this->font_, time_color, display::TextAlign::TOP_RIGHT, time_display.c_str());
    }

    if (trip.is_realtime && this->show_realtime_icon_) {
      headsign_clipping_end -= 8;

      if(!no_draw) {
        int icon_bottom_right_x = region_right - time_width - 2;
        int icon_bottom_right_y = y_offset + font_height - 6;

        this->draw_realtime_icon_(icon_bottom_right_x, icon_bottom_right_y, uptime);
      }
    }

    int headsign_max_width = headsign_clipping_end - headsign_clipping_start;

    int headsign_actual_width;
    this->font_->measure(trip.headsign.c_str(), &headsign_actual_width, &_, &_, &_);

    int headsign_overflow = headsign_actual_width - headsign_max_width;
    if (headsign_overflow_out) {
      *headsign_overflow_out = headsign_overflow;
    }

    if (no_draw) {
      return;
    }

    int scroll_offset = 0;
    if (headsign_overflow > 0 && scroll_cycle_duration > 0) {
      /// Note: The scroll may jump if headsign_clipping_end changes (e.g. due to the width of the arrival time changing).
      /// This is probably not a big deal, since the display makes sudden changes anyway (e.g. when routes are updated)
      /// and this happens relatively infrequently.

      int scroll_time = headsign_overflow * 1000 / scroll_speed;
      int scroll_cycle_time = uptime % scroll_cycle_duration;

      // Scroll idle (left side - default)
      if(scroll_cycle_time < idle_time_left) {
        // scroll_offset = 0; do nothing
      } else if (scroll_cycle_time < idle_time_left + scroll_time) {
        // Scrolling left
        int time_since_scroll_start = scroll_cycle_time - idle_time_left;
        scroll_offset = time_since_scroll_start * scroll_speed / 1000;
      } else if (scroll_cycle_time < idle_time_left + scroll_time + idle_time_right) {
        // Scroll idle (right side)
        scroll_offset = headsign_overflow;
      } else if (scroll_cycle_time < idle_time_left + 2 * scroll_time + idle_time_right){
        // Scrolling right
        int time_since_scroll_start = scroll_cycle_time - (idle_time_left + scroll_time + idle_time_right);
        scroll_offset = headsign_overflow - (time_since_scroll_start * scroll_speed / 1000);
      } else {
        // Waiting for other headsigns to finish scrolling
        // scroll_offset = 0; do nothing
      }
    }

    this->display_->start_clipping(headsign_clipping_start, 0, headsign_clipping_end, this->display_->get_height());
    this->display_->print(headsign_clipping_start - scroll_offset, y_offset, this->font_, trip.route_color, trip.headsign.c_str());
    this->display_->end_clipping();
}

bool TransitTracker::draw_guards_() {
  if (this->display_ == nullptr) {
    ESP_LOGW(TAG, "No display attached, cannot draw schedule");
    return false;
  }

  if (!esphome::network::is_connected()) {
    ESP_LOGD(TAG, "draw_schedule: waiting for network");
    this->draw_text_centered_("Waiting for network", Color(0x252627));
    return false;
  }

  if (!this->rtc_->now().is_valid()) {
    ESP_LOGD(TAG, "draw_schedule: waiting for time sync");
    this->draw_text_centered_("Waiting for time sync", Color(0x252627));
    return false;
  }

  if (this->base_url_.empty()) {
    ESP_LOGD(TAG, "draw_schedule: no base URL");
    this->draw_text_centered_("No base URL set", Color(0x252627));
    return false;
  }

  if (this->status_has_error()) {
    ESP_LOGD(TAG, "draw_schedule: status has error");
    this->draw_text_centered_("Error loading schedule", Color(0xFE4C5C));
    return false;
  }

  if (!this->has_ever_connected_) {
    ESP_LOGD(TAG, "draw_schedule: has never connected");
    this->draw_text_centered_("Loading...", Color(0x252627));
    return false;
  }

  if (this->schedule_state_.trips.empty()) {
    auto message = "No upcoming arrivals";
    if (this->display_departure_times_) {
      message = "No upcoming departures";
    }

    ESP_LOGD(TAG, "draw_schedule: no trips, showing '%s'", message);
    this->draw_text_centered_(message, Color(0x252627));
    return false;
  }

  return true;
}

void HOT TransitTracker::draw_schedule(int page) {
  // One-time diagnostic: log scroll state after all switches have restored.
  // This fires on the first draw_schedule call (well after setup completes).
  static bool scroll_diag_logged = false;
  if (!scroll_diag_logged) {
    ESP_LOGI(TAG, "Scroll diagnostic: scroll_headsigns_=%s",
             this->scroll_headsigns_ ? "true" : "false");
    scroll_diag_logged = true;
  }

  if (!this->draw_guards_()) {
    return;
  }

  int effective_page;
  bool allow_clamp;
  if (page >= 0) {
    // Explicit page requested — use it directly, no auto-advance
    effective_page = page;
    allow_clamp = false;
  } else {
    // Auto-advance mode (original behavior): advance page when
    // draw_schedule() hasn't been called for a while (indicates we
    // were on another display page and just came back)
    unsigned long now_ms = millis();
    if (this->last_draw_time_ != 0 && now_ms - this->last_draw_time_ > 500) {
      this->next_page();
    }
    this->last_draw_time_ = now_ms;
    effective_page = this->current_page_;
    allow_clamp = true;
  }

  this->draw_schedule_region_(effective_page, 0, this->display_->get_width(), allow_clamp);
}

std::vector<std::string> TransitTracker::get_configured_route_ids_() const {
  // schedule_string_ is "routeId,stopId,offset;routeId,stopId,offset;..." in the
  // order routes/stops were configured. Pull out the route IDs, preserving order
  // and dropping duplicates.
  std::vector<std::string> route_ids;
  for (const auto &pair : split(this->schedule_string_, ';')) {
    auto parts = split(pair, ',');
    if (parts.empty() || parts[0].empty()) {
      continue;
    }
    const std::string &route_id = parts[0];
    if (std::find(route_ids.begin(), route_ids.end(), route_id) == route_ids.end()) {
      route_ids.push_back(route_id);
    }
  }
  return route_ids;
}

void HOT TransitTracker::draw_split_schedule() {
  if (!this->draw_guards_()) {
    return;
  }

  int width = this->display_->get_width();
  int height = this->display_->get_height();
  int half = width / 2;

  // Inset each pane so the left pane's right-aligned time does not abut the
  // right pane's left-aligned route label at the midline.
  int left_x = 0;
  int left_width = half - split_gutter / 2;
  int right_x = half + split_gutter / 2;
  int right_width = width - right_x;

  // Pin each pane to a fixed route by configured order: left = first configured
  // route, right = second. This keeps each line/direction in a stable column
  // instead of swapping as trips depart (which arrival-order paging would do).
  // A route with no upcoming trips simply leaves its pane blank.
  auto routes = this->get_configured_route_ids_();
  if (routes.size() >= 1) {
    this->draw_route_region_(routes[0], left_x, left_width);
  }
  if (routes.size() >= 2) {
    this->draw_route_region_(routes[1], right_x, right_width);
  }

  // Subtle divider down the middle of the gutter.
  this->display_->vertical_line(half, 0, height, Color(0x202020));
}

void HOT TransitTracker::draw_schedule_region_(int page, int region_x, int region_width, bool allow_clamp) {
  // Resolve which route this page maps to, in the order routes appear in the
  // (arrival-sorted) trip list. Held briefly under the mutex while reading trips.
  this->schedule_state_.mutex.lock();
  auto unique_routes = this->get_unique_route_ids();

  int effective_page = page;
  if (effective_page >= static_cast<int>(unique_routes.size())) {
    if (allow_clamp) {
      // Auto-advance mode: clamp back to page 0
      this->current_page_ = 0;
      effective_page = 0;
    } else {
      // Explicit page beyond available data — nothing to draw
      ESP_LOGD(TAG, "draw_schedule: page %d beyond available routes (%zu), drawing nothing", page, unique_routes.size());
      this->schedule_state_.mutex.unlock();
      return;
    }
  }

  std::string target_route_id =
      effective_page < static_cast<int>(unique_routes.size()) ? unique_routes[effective_page]
                                                              : unique_routes[0];
  this->schedule_state_.mutex.unlock();

  this->draw_route_region_(target_route_id, region_x, region_width);
}

void HOT TransitTracker::draw_route_region_(const std::string &target_route_id, int region_x, int region_width) {
  this->schedule_state_.mutex.lock();

  // Filter trips to only include the target route, up to limit_
  std::vector<int> trips_to_display;
  for (int i = 0; i < static_cast<int>(this->schedule_state_.trips.size()); i++) {
    if (this->schedule_state_.trips[i].route_id == target_route_id) {
      trips_to_display.push_back(i);
      if (static_cast<int>(trips_to_display.size()) >= this->limit_) {
        break;
      }
    }
  }

  int trips_on_page = trips_to_display.size();

  if (trips_on_page == 0) {
    ESP_LOGD(TAG, "draw_schedule: no trips found for route %s", target_route_id.c_str());
    this->schedule_state_.mutex.unlock();
    return;
  }

  int nominal_font_height = this->font_->get_ascender() + this->font_->get_descender();
  unsigned long uptime = millis();
  uint rtc_now = this->rtc_->now().timestamp;

  // Pre-compute the widest route label so every row's headsign starts
  // at the same x-coordinate (avoids jitter from proportional digit widths).
  int route_column_width = 0;
  if (this->show_line_icons_) {
    int _;
    for (size_t idx = 0; idx < trips_to_display.size(); idx++) {
      int i = trips_to_display[idx];
      std::string route_label;
      if (this->route_display_mode_ == ROUTE_DISPLAY_NUMBERED) {
        route_label = str_sprintf("%zu ", idx + 1);
      } else {
        route_label = this->schedule_state_.trips[i].route_name;
      }
      int w;
      this->font_->measure(route_label.c_str(), &w, &_, &_, &_);
      route_column_width = max(route_column_width, w);
    }
  }

  int scroll_cycle_duration = 0;
  if (this->scroll_headsigns_) {
    int largest_headsign_overflow = 0;
    for (size_t idx = 0; idx < trips_to_display.size(); idx++) {
      int i = trips_to_display[idx];
      int headsign_overflow;
      this->draw_trip(this->schedule_state_.trips[i], idx, 0, nominal_font_height, uptime, rtc_now, region_x, region_width, true, &headsign_overflow, 0, route_column_width);
      largest_headsign_overflow = max(largest_headsign_overflow, headsign_overflow);
    }

    if (largest_headsign_overflow > 0) {
      int longest_scroll_time = largest_headsign_overflow * 1000 / scroll_speed;
      scroll_cycle_duration = idle_time_left + idle_time_right + 2*longest_scroll_time;
    }

    // One-time diagnostic: log scroll computation results
    static bool scroll_params_logged = false;
    if (!scroll_params_logged && trips_on_page > 0) {
      ESP_LOGI(TAG, "Scroll params: overflow=%d, cycle_duration=%d, scroll_headsigns=%s",
               largest_headsign_overflow, scroll_cycle_duration,
               this->scroll_headsigns_ ? "true" : "false");
      scroll_params_logged = true;
    }
  }

  int max_trips_height = (this->limit_ * this->font_->get_ascender()) + ((this->limit_ - 1) * this->font_->get_descender());
  int y_offset = (this->display_->get_height() - max_trips_height) / 2;
  if (y_offset < 0) y_offset = 0;

  for (size_t idx = 0; idx < trips_to_display.size(); idx++) {
    int i = trips_to_display[idx];
    this->draw_trip(this->schedule_state_.trips[i], idx, y_offset, nominal_font_height, uptime, rtc_now, region_x, region_width, false, nullptr, scroll_cycle_duration, route_column_width);
    y_offset += nominal_font_height;
  }

  this->schedule_state_.mutex.unlock();
}

}  // namespace transit_tracker
}  // namespace esphome