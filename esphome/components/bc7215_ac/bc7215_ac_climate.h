#pragma once

#include <memory>
#include <string>
#include <cstdio>
#include "driver/gpio.h"
#include "driver/uart.h"

#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/components/climate/climate.h"
#include "esphome/components/output/float_output.h"
#include "esphome/components/wifi/wifi_component.h"

// Provided by the bc7215_ac_lib repository's ESP-IDF example component:
// deps/bc7215_ac_lib/examples/ESP-IDF/components/bc7215ac
#include "bc7215ac.hpp"

namespace esphome {
namespace bc7215_ac {

class BC7215ACClimate : public climate::Climate, public Component {
 private:
  void log_data_packet_(const char *label, const bc7215DataVarPkt_t *pkt) const;
  void log_format_packet_(const char *label, const bc7215FormatPkt_t *pkt) const;

 public:
  void set_uart_num(int uart_num) { this->uart_num_ = static_cast<uart_port_t>(uart_num); }
  void set_tx_pin(int pin) { this->bc7215_tx_pin_ = static_cast<gpio_num_t>(pin); }
  void set_rx_pin(int pin) { this->bc7215_rx_pin_ = static_cast<gpio_num_t>(pin); }
  void set_busy_pin(int pin) { this->busy_pin_ = static_cast<gpio_num_t>(pin); }
  void set_mod_pin(int pin) { this->mod_pin_ = static_cast<gpio_num_t>(pin); }
  void set_library_unit_celsius(bool celsius) { this->library_unit_celsius_ = celsius; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  climate::ClimateTraits traits() override;

  // Called by HA/API/climate automations.
  void control(const climate::ClimateCall &call) override;

  // These methods can be called from YAML lambdas:
  //   id(bedroom_ac).start_pairing();
  //   id(bedroom_ac).builtin_or_next();
  void start_pairing();
  // Start capturing a user-function IR frame directly without re-pairing.
  // Requires an existing AC pairing.
  void start_user_function_learning();
  void stop_pairing();
  void builtin_or_next();
  void user_button();

  // Physical button; called by ESPHome binary_sensor automations.
  void physical_button_pressed();
  // Returns true when a >=5s hold was completed: YAML then performs factory reset.
  bool physical_button_released();

  // Clear only the AC/IR pairing and user-function records (optional API).
  bool clear_pairing();

  // LEDC status indicator (provided by the YAML output component).
  void set_led_output(output::FloatOutput *led) { this->led_output_ = led; }

  // Helper methods for text_sensor/display lambdas.
  std::string status_message() const { return this->status_message_; }
  bool is_paired() const;
  bool is_pairing() const { return this->pairing_; }
  uint8_t predefined_count() const;

 protected:
  enum class PairState : uint8_t {
    IDLE,
    PAIRING,
  };

  enum class ParsingState : uint8_t {
    IDLE,
    PARSING,
	STOP,
  };

  struct StoredPairingData {
    bc7215DataMaxPkt_t data;
    bc7215FormatPkt_t format;
	uint8_t status;
    uint8_t match_index;
	ParsingState parsing_state;
    bool library_unit_celsius;
  };

  struct StoredUserFunction {
    bool valid;
    bc7215DataMaxPkt_t data;
  };

  static constexpr uint32_t kPreferenceVersion = 1;
  static constexpr uint32_t kUserPreferenceKey = 2;

  void set_status_(const std::string &message);
  void handle_pairing_loop_();
  void handle_parsing_loop_();
  void mark_unpaired_();
  enum class LedMode : uint8_t {
    WIFI_NOT_CONFIGURED,  // No STA credentials and no AC pairing: solid ON
    WIFI_CONNECTING,      // STA credentials, no WiFi connection: 1Hz blink
    UNPAIRED,             // WiFi connected, no AC pairing: 100ms / 2s
    PAIRED,               // WiFi connected and paired: 3s breathing
    PAIRED_NO_WIFI,       // AC paired, no STA credentials: double pulse / 2s
    WAITING_IR,
    ERASE_WARNING,
    FEEDBACK,
    ALT_OFF               // Idle after alternative-protocol LED feedback
  };
  void update_led_();
  void start_led_feedback_(uint16_t count, uint32_t on_ms, uint32_t off_ms);
  void restore_led_mode_();
  LedMode normal_led_mode_() const;

  bool load_pairing_();
  bool save_pairing_();

  bool load_user_function_();
  bool save_user_function_();
  void clear_user_function_();
  bool copy_current_base_(bc7215DataMaxPkt_t &dst) const;

  bool try_next_predefined_();
  bool try_match_next_();
  bool send_current_state_();
  bool send_test_command_();

  int map_mode_to_bc7215_(climate::ClimateMode mode) const;
  int map_fan_to_bc7215_() const;
  climate::ClimateMode map_bc7215_mode_to_ha_(int m) const;
  climate::ClimateFanMode map_bc7215_fan_to_ha_(int f) const;

  int climate_temp_to_library_temp_(float temp_c) const;
  float library_temp_to_climate_temp_(int t) const; 
  void update_action_from_mode_();

  uart_port_t uart_num_{UART_NUM_1};
  gpio_num_t bc7215_tx_pin_{GPIO_NUM_NC};
  gpio_num_t bc7215_rx_pin_{GPIO_NUM_NC};
  gpio_num_t busy_pin_{GPIO_NUM_NC};
  gpio_num_t mod_pin_{GPIO_NUM_NC};

  bool library_unit_celsius_{true};
  bool pairing_had_existing_pair_{false};
  bool user_learning_{false};

  bool user_base_valid_{false};
  bool user_function_active_{false};

  bc7215DataMaxPkt_t user_base_{};
  bc7215DataMaxPkt_t normal_base_before_user_{};

  ESPPreferenceObject pref_;
  ESPPreferenceObject user_pref_;

  std::unique_ptr<bc7215::BC7215AC> ac_;

  PairState pair_state_{PairState::IDLE};
  ParsingState parsing_state_{ParsingState::STOP};
  bool pairing_{false};
  uint8_t predefined_number_{0};
  uint8_t match_index_{0};
  uint8_t ac_pressed_key_{0};
  bool power_on_{false};
  bc7215DataMaxPkt_t	last_base_;

  // LEDC output pointer is managed by ESPHome.
  output::FloatOutput *led_output_{nullptr};
  LedMode led_mode_{LedMode::UNPAIRED};
  uint32_t led_mode_start_ms_{0};
  uint32_t led_update_ms_{0};
  uint16_t led_feedback_count_{0};
  uint32_t led_feedback_on_ms_{0};
  uint32_t led_feedback_off_ms_{0};
  bool led_alt_hold_off_{false};  // Resume only after a network climate command or single-click pairing
  bool physical_button_down_{false};
  uint32_t physical_button_down_ms_{0};

  std::string status_message_{"Not initialized."};
};

}  // namespace bc7215_ac
}  // namespace esphome
