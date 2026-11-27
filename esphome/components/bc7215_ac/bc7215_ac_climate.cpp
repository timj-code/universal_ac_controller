#include "bc7215_ac_climate.h"

#include <cmath>
#include <cstring>
#include "esphome/core/hal.h"

#include "esphome/core/log.h"
#include "esphome/core/defines.h"
#include "esphome/core/version.h"
#include "esphome/components/wifi/wifi_component.h"

namespace esphome {
namespace bc7215_ac {

static const char *const TAG = "bc7215_ac.climate";
std::string VERSION_STRING;

void BC7215ACClimate::setup() {
  ESP_LOGCONFIG(TAG, "Setting up BC7215 AC climate adapter...");

  this->ac_ = std::make_unique<bc7215::BC7215AC>(
      this->uart_num_,			// ESP32 UART number
      this->bc7215_rx_pin_,		// ESP32 TX pin
      this->bc7215_tx_pin_,		// ESP32 RX pin
      this->busy_pin_,			// ESP32 BUSY pin
      this->mod_pin_);			// ESP32 MOD pin

  const esp_err_t err = this->ac_->begin();
  if (err != ESP_OK) {
    this->set_status_(std::string("BC7215AC begin failed: ") + esp_err_to_name(err));
    this->mark_failed();
    return;
  }

  
  VERSION_STRING = this->ac_->lib_version() ; 
  if (this->library_unit_celsius_) {
    this->ac_->set_celsius();
  } else {
    this->ac_->set_fahrenheit();
  }

  this->mode = climate::CLIMATE_MODE_OFF;
  this->target_temperature = 25.0f;
  this->action = climate::CLIMATE_ACTION_OFF;
  this->set_fan_mode_(climate::CLIMATE_FAN_AUTO);
  this->power_on_ = false;

  this->pref_ = this->make_entity_preference<StoredPairingData>(kPreferenceVersion);
  this->user_pref_ = this->make_entity_preference<StoredUserFunction>(kUserPreferenceKey);

  this->load_pairing_();
  this->load_user_function_();
  this->user_function_active_ = false;
  this->restore_led_mode_();

  this->publish_state();
}

void BC7215ACClimate::loop() {
  this->handle_pairing_loop_();
  this->handle_parsing_loop_();
  this->update_led_();
}

void BC7215ACClimate::dump_config() {
  ESP_LOGCONFIG(TAG, "BC7215 AC Climate:");
  ESP_LOGCONFIG(TAG, "  Project name: %s", ESPHOME_PROJECT_NAME);
  ESP_LOGCONFIG(TAG, "  Project version: %s", ESPHOME_PROJECT_VERSION);
  ESP_LOGCONFIG(TAG, "  UART: %d", static_cast<int>(this->uart_num_));
  ESP_LOGCONFIG(TAG, "  BC7215 TX pin: GPIO%d", static_cast<int>(this->bc7215_tx_pin_));
  ESP_LOGCONFIG(TAG, "  BC7215 RX pin: GPIO%d", static_cast<int>(this->bc7215_rx_pin_));
  ESP_LOGCONFIG(TAG, "  BUSY pin: GPIO%d", static_cast<int>(this->busy_pin_));
  ESP_LOGCONFIG(TAG, "  MOD pin: GPIO%d", static_cast<int>(this->mod_pin_));
  ESP_LOGCONFIG(TAG, "  AC Library Version: %s", this->ac_->lib_version());
  ESP_LOGCONFIG(TAG, "  Library temperature unit: %s", this->library_unit_celsius_ ? "Celsius" : "Fahrenheit");
  ESP_LOGCONFIG(TAG, "  Paired: %s", this->is_paired() ? "yes" : "no");
}

climate::ClimateTraits BC7215ACClimate::traits() {
  auto traits = climate::ClimateTraits();

  traits.add_supported_mode(climate::CLIMATE_MODE_OFF);
  traits.add_supported_mode(climate::CLIMATE_MODE_AUTO);
  traits.add_supported_mode(climate::CLIMATE_MODE_HEAT_COOL);
  traits.add_supported_mode(climate::CLIMATE_MODE_COOL);
  traits.add_supported_mode(climate::CLIMATE_MODE_HEAT);
  traits.add_supported_mode(climate::CLIMATE_MODE_DRY);
  traits.add_supported_mode(climate::CLIMATE_MODE_FAN_ONLY);

  traits.add_supported_fan_mode(climate::CLIMATE_FAN_AUTO);
  traits.add_supported_fan_mode(climate::CLIMATE_FAN_LOW);
  traits.add_supported_fan_mode(climate::CLIMATE_FAN_MEDIUM);
  traits.add_supported_fan_mode(climate::CLIMATE_FAN_HIGH);

  // ESPHome climate temperatures are Celsius internally.
  // Home Assistant can display Fahrenheit according to user settings.
  traits.set_visual_min_temperature(16.0f);
  traits.set_visual_max_temperature(30.0f);
  traits.set_visual_temperature_step(1.0f);

  return traits;
}

void BC7215ACClimate::control(const climate::ClimateCall &call) {
  if (this->pairing_) {
    this->set_status_(
        "Pairing or user-function learning in progress.");
    return;
  }
  if (!this->ac_ || !this->is_paired()) {
    this->set_status_("Not paired. Press pair first.");
    return;
  }

  if (call.get_target_temperature().has_value()) {
    this->target_temperature = *call.get_target_temperature();
	this->ac_pressed_key_ = 0;
  }

  if (call.get_fan_mode().has_value()) {
    this->set_fan_mode_(*call.get_fan_mode());
	this->ac_pressed_key_ = 3;
  }

  if (call.get_mode().has_value()) {
    this->mode = *call.get_mode();
	this->ac_pressed_key_ = 2;
  }

  if (this->send_current_state_()) {
    // Successful network climate command resumes the ordinary LED display.
    if (this->led_alt_hold_off_) {
      this->led_alt_hold_off_ = false;
      this->restore_led_mode_();
    }
    this->set_status_("AC command sent.");
  } else {
    this->set_status_("AC command failed.");
  }

  this->publish_state();
}

void BC7215ACClimate::start_pairing() {
  if (!this->ac_) {
    this->set_status_("BC7215AC is not ready.");
    return;
  }

  if (this->pairing_) {
    return;
  }

  // Single-click Pair (or HA Pair button) ends the ALT LED hold-off.
  // Do not do this in physical_button_pressed(): double-clicks also press GPIO9.
  this->led_alt_hold_off_ = false;
  this->restore_led_mode_();

  // If User base is currently active, restore normal base first.
  if (this->user_function_active_) {
    if (!this->ac_->replace_base(this->normal_base_before_user_)) {
      this->set_status_("Failed to restore normal base.");
      return;
    }

    this->user_function_active_ = false;
  }

  // Remember whether this is a first pairing or a re-pair.
  this->pairing_had_existing_pair_ = this->is_paired();

  // User learning is NOT selected yet.
  this->user_learning_ = false;

  // For first pairing, clear the old runtime state first.
  //
  // IMPORTANT:
  // mark_unpaired_() sets pairing_=false and pair_state_=IDLE,
  // so it MUST be called before entering PAIRING state.
  if (!this->pairing_had_existing_pair_) {
    this->mark_unpaired_();
  }

  // Now enter pairing state.
  this->parsing_state_ = ParsingState::STOP;
  this->pair_state_ = PairState::PAIRING;
  this->pairing_ = true;
  this->match_index_ = 0;

  if (!this->pairing_had_existing_pair_) {
    this->set_status_(
        "Pairing: send 25°C Cool from the remote.");
  } else {
    this->set_status_(
        "Press Learn User Function in HA to learn, "
        "or send 25°C Cool to re-pair.");
  }

  this->ac_->start_capture(1);
  this->led_mode_ = LedMode::WAITING_IR;
  this->led_mode_start_ms_ = millis();
}

void BC7215ACClimate::start_user_function_learning() {
  if (!this->ac_) {
    this->set_status_("BC7215AC is not ready.");
    return;
  }

  // A user function modifies the base of a previously paired AC protocol.
  // Never call init() on a user-function IR frame.
  if (!this->is_paired()) {
    this->set_status_(
        "AC not paired. Single-click the physical button and pair with 25°C Cool first.");
    return;
  }
  if (this->pairing_ && this->user_learning_) {
    this->set_status_("User function learning already in progress.");
    return;
  }

  // If the user function is currently applied, restore the normal base.
  // The new function must be learned relative to that normal base.
  if (this->user_function_active_) {
    if (!this->ac_->replace_base(this->normal_base_before_user_)) {
      this->set_status_("Failed to restore normal base before learning.");
      return;
    }
    this->user_function_active_ = false;
  }

  // Crucial: pairing uses start_capture(1); learning needs start_capture(0).
  // Stop the prior capture (also when idle parsing is running) before
  // switching modes, or a user-function frame may be handled as pairing.
  this->ac_->stop_capture();

  this->pairing_had_existing_pair_ = true;
  this->user_learning_ = true;
  this->parsing_state_ = ParsingState::STOP;
  this->pair_state_ = PairState::PAIRING;
  this->pairing_ = true;

  this->ac_->start_capture(0);
  this->led_mode_ = LedMode::WAITING_IR;
  this->led_mode_start_ms_ = millis();
  this->set_status_(
      "User function learning: press the desired button on the AC remote.");
  ESP_LOGI(TAG, "User function learning started (simple IR capture mode).");
}

void BC7215ACClimate::stop_pairing() {
  if (this->ac_ && this->pairing_) {
    this->ac_->stop_capture();
  }
  this->pairing_ = false;
  this->pair_state_ = PairState::IDLE;
  this->set_status_("Pairing stopped.");
  this->restore_led_mode_();
}

void BC7215ACClimate::builtin_or_next() {
  if (!this->ac_) {
    this->set_status_("BC7215AC is not ready.");
    return;
  }

  // A double-click shows its protocol count, then leaves the LED off until
  // a network climate command or a single-click Pair is executed.
  this->led_alt_hold_off_ = true;

  if (this->pairing_) {
    this->ac_->stop_capture();
    this->pairing_ = false;
    this->pair_state_ = PairState::IDLE;
  }

  if (this->is_paired()) {
    if (this->try_match_next_()) {
	  this->parsing_state_ = ParsingState::IDLE;
      this->save_pairing_();
      this->start_led_feedback_(this->match_index_, 200, 250);
      return;
    }
  }
  if (this->try_next_predefined_()) {
	this->parsing_state_ = ParsingState::STOP;
    this->save_pairing_();
    this->start_led_feedback_(this->predefined_number_, 200, 250);
    return;
  }

  // No alternative protocol: keep the LED off as well.
  this->restore_led_mode_();
}

bool BC7215ACClimate::is_paired() const {
  return this->ac_ && this->ac_->init_ok;
}

uint8_t BC7215ACClimate::predefined_count() const {
  if (!this->ac_) {
    return 0;
  }
  return this->ac_->predefined_count();
}

void BC7215ACClimate::set_status_(const std::string &message) {
  this->status_message_ = message;
  ESP_LOGI(TAG, "%s", message.c_str());
}

void BC7215ACClimate::handle_pairing_loop_() {
  if (!this->pairing_ || !this->ac_) {
    return;
  }

  if (this->pair_state_ != PairState::PAIRING) {
    return;
  }

  if (!this->ac_->signal_captured()) {
    return;
  }

  this->ac_->stop_capture();
  this->log_data_packet_("RX", reinterpret_cast<const bc7215DataVarPkt_t*>(&this->ac_->sample_data[0]));
  this->pairing_ = false;
  this->pair_state_ = PairState::IDLE;

  if (this->user_learning_) {
    // ------------------------------------------------
    // USER FUNCTION LEARNING
    // ------------------------------------------------
  
    this->user_learning_ = false;
  
    bc7215DataMaxPkt_t original_base{};
  
    if (!this->copy_current_base_(original_base)) {
      this->parsing_state_ = ParsingState::IDLE;
      this->set_status_("Store current base failed.");
      return;
    }
  
    // New function: operate directly on captured IR.
    if (!this->ac_->replace_base()) {
      this->parsing_state_ = ParsingState::IDLE;
      this->set_status_("This function has different format. User function learning failed.");
      return;
    }
  
    // replace_base() succeeded, so current base is now
    // the learned user-function base.
    if (!this->copy_current_base_(this->user_base_)) {
      this->ac_->replace_base(original_base);
  
      this->parsing_state_ = ParsingState::IDLE;
      this->set_status_("Failed to store user function.");
      return;
    }
  
    this->user_base_valid_ = true;
    this->user_function_active_ = false;
  
    const bool saved = this->save_user_function_();
  
    // Always return to the original normal base.
    const bool restored =
        this->ac_->replace_base(original_base);
  
    this->parsing_state_ = ParsingState::IDLE;
    this->pairing_had_existing_pair_ = false;
  
    if (!restored) {
      this->set_status_("Failed to restore normal base.");
    } else if (!saved) {
      this->set_status_(
          "User function learned, but flash save failed.");
    } else {
      this->set_status_("User function saved.");
    }
  
    return;
  }

  if (this->ac_->init()) {
    this->match_index_ = 0;
	this->parsing_state_ = ParsingState::IDLE;
    this->save_pairing_();
    this->set_status_("AC Paired");
    this->publish_state();
  } else {
    this->mark_unpaired_();
    this->set_status_("Pair failed. Single-click GPIO9 to retry, or double-click to try an alternate protocol.");
  }
}

void BC7215ACClimate::handle_parsing_loop_() {
  if (this->pairing_ || !this->ac_ || this->ac_->is_busy() || !this->is_paired()) {
    return;
  }

  if (this->parsing_state_ == ParsingState::STOP) {
    return;
  }

  if (this->parsing_state_ == ParsingState::IDLE) {
    this->ac_->start_capture(0);
	this->parsing_state_ = ParsingState::PARSING;
    return;
  }

  if (!this->ac_->signal_captured()) {
    return;
  }

  this->ac_->stop_capture();
  this->log_data_packet_("RX", reinterpret_cast<const bc7215DataVarPkt_t*>(&this->ac_->sample_data[0]));
  this->last_base_ = *(reinterpret_cast<const bc7215DataMaxPkt_t*>(this->ac_->data_packet()));

  int t = -1;
  int m = -1;
  int f = -1;
  int p = -1;
  if (this->ac_->parse(t, m, f, p)) {
    ESP_LOGI(TAG, "Parsed t=%d, m=%d, f=%d, p=%d", t, m, f, p);
    this->mode = this->map_bc7215_mode_to_ha_(m);
	this->set_fan_mode_(this->map_bc7215_fan_to_ha_(f));
	this->target_temperature = library_temp_to_climate_temp_(t);
	if (p == 0) {
	  this->mode = climate::CLIMATE_MODE_OFF;
      this->action = climate::CLIMATE_ACTION_OFF;
	  this->power_on_ = false;
	  this->ac_->replace_base(last_base_);
	} else if (p == 2) {
	  if (this->mode != climate::CLIMATE_MODE_OFF) {
	    this->mode = climate::CLIMATE_MODE_OFF;
        this->action = climate::CLIMATE_ACTION_OFF;
		this->power_on_ = !this->power_on_;
		if (!this->power_on_) {
	  		this->ac_->replace_base(last_base_);
		}
	  }
	}
  }
  else {
    ESP_LOGI(TAG, "Parsing Failed.");
  }

  this->update_action_from_mode_();
  this->publish_state();
  this->parsing_state_ = ParsingState::IDLE;
}

bool BC7215ACClimate::load_pairing_() {
  StoredPairingData saved{};

  if (!this->pref_.load(&saved)) {
    ESP_LOGI(TAG, "No saved BC7215 AC pairing data.");
    this->mark_unpaired_();
    return false;
  }

  // A zero-length saved packet is our durable "unpaired" tombstone.
  if (saved.data.bitLen == 0) {
    this->mark_unpaired_();
    return false;
  }

  const bool configured_unit_celsius = this->library_unit_celsius_;

  this->library_unit_celsius_ = saved.library_unit_celsius;
  if (this->library_unit_celsius_) {
    this->ac_->set_celsius();
  } else {
    this->ac_->set_fahrenheit();
  }

  if (!this->ac_->init(saved.status, saved.data, saved.format)) {
    ESP_LOGW(TAG, "Saved BC7215 AC pairing data exists, but init failed.");
    this->library_unit_celsius_ = configured_unit_celsius;
    if (this->library_unit_celsius_) {
      this->ac_->set_celsius();
    } else {
      this->ac_->set_fahrenheit();
    }
    this->mark_unpaired_();
    return false;
  }

  this->parsing_state_ = saved.parsing_state;
  this->match_index_ = 0;

  for (uint8_t i = 0; i < saved.match_index; ++i) {
    if (!this->ac_->match_next()) {
      ESP_LOGW(TAG, "Saved match index %u cannot be restored.", saved.match_index);
      this->match_index_ = 0;
	  this->ac_->init(saved.status, saved.data, saved.format);	// use the first match
	  return true;
    }
    ++this->match_index_;
  }

  this->set_status_("Paired. Lib. Ver." + VERSION_STRING);
  ESP_LOGI(TAG, "BC7215 AC pairing restored. match_index=%u, unit=%s",
           this->match_index_, this->library_unit_celsius_ ? "C" : "F");
  return true;
}

bool BC7215ACClimate::save_pairing_() {
  if (!this->ac_ || !this->ac_->init_ok) {
    ESP_LOGW(TAG, "Cannot save pairing data: AC library is not initialized.");
    return false;
  }

  uint8_t status = this->ac_->status_byte();
  const bc7215DataVarPkt_t *src_data = this->ac_->data_packet();
  const bc7215FormatPkt_t *src_format = this->ac_->format_packet();
  this->log_format_packet_("PAIRED FORMAT", src_format);
  this->log_data_packet_("PAIRED DATA", src_data);

  if (src_data == nullptr || src_format == nullptr) {
    ESP_LOGW(TAG, "Cannot save pairing data: data or format packet is null.");
    return false;
  }

  StoredPairingData saved{};

  const uint16_t data_bytes = static_cast<uint16_t>((src_data->bitLen + 7) / 8);
  if (data_bytes == 0 || data_bytes > sizeof(saved.data.data)) {
    ESP_LOGW(TAG, "Cannot save pairing data: invalid data size %u.", data_bytes);
    return false;
  }

  saved.data.bitLen = src_data->bitLen;
  std::memcpy(saved.data.data, src_data->data, data_bytes);
  saved.format = *src_format;
  saved.status = status;
  saved.match_index = this->match_index_;
  saved.parsing_state = this->parsing_state_;
  saved.library_unit_celsius = this->library_unit_celsius_;

  if (!this->pref_.save(&saved)) {
    ESP_LOGW(TAG, "Failed to save BC7215 AC pairing data.");
    return false;
  }

  ESP_LOGI(TAG, "BC7215 AC pairing saved. match_index=%u, unit=%s",
           saved.match_index, saved.library_unit_celsius ? "C" : "F");
  return true;
}

bool BC7215ACClimate::try_next_predefined_() {
  const uint8_t count = this->predefined_count();

  this->predefined_number_++;
  this->match_index_ = 0;
  if (this->ac_->init_predefined(this->predefined_number_-1)) {
    std::string msg = "Use built-in Protocol ";
    msg += std::to_string(this->predefined_number_);
    this->set_status_(msg);
    this->send_test_command_();
    this->publish_state();
    return true;
  }
  if (this->predefined_number_ > count)
  {
    this->predefined_number_ = 0;
  }
  this->mark_unpaired_();
  return false;
}

bool BC7215ACClimate::try_match_next_() {
  bool result;
  result = this->ac_->match_next();
  if (result) {
  	this->match_index_++;
      std::string msg = "Alt matched protocol ";
      msg += std::to_string(this->match_index_);
      this->set_status_(msg);
      this->send_test_command_();
      this->publish_state();
  } else {
    this->match_index_ = 0;
  }
  return result;
}

bool BC7215ACClimate::send_current_state_() {
  if (!this->ac_ || !this->is_paired()) {
    return false;
  }

  this->ac_->stop_capture();
  this->parsing_state_ = ParsingState::IDLE;

  if (this->mode == climate::CLIMATE_MODE_OFF) {
    const auto *pkt = this->ac_->off();
  	ESP_LOGW(TAG, "SET TO OFF by Home Assistant");
  	this->log_data_packet_("TX", pkt);
    this->action = climate::CLIMATE_ACTION_OFF;
	this->power_on_ = false;
    return pkt != nullptr;
  }

  const int temp = this->climate_temp_to_library_temp_(this->target_temperature);
  const int mode = this->map_mode_to_bc7215_(this->mode);
  const int fan = this->map_fan_to_bc7215_();

  if (!this->power_on_)
  {
  	this->ac_->on();
	ESP_LOGW(TAG, "SET TO ON by Home Assistant.");
	this->power_on_ = true;
  }
  const auto *pkt = this->ac_->set_to(temp, mode, fan, this->ac_pressed_key_);
  ESP_LOGW(TAG, "SET TO Temp=%d, Mode=%d, Fan=%d by Home Assistant", temp, mode, fan);
  this->log_data_packet_("TX", pkt);
  this->update_action_from_mode_();
  return pkt != nullptr;
}

bool BC7215ACClimate::send_test_command_() {
  if (!this->ac_ || !this->is_paired()) {
    return false;
  }

  // Test with a safe common command: Cool 25C, fan auto.
  const int temp = this->climate_temp_to_library_temp_(25.0f);
  const auto *pkt = this->ac_->set_to(temp, 1, 0);

  this->mode = climate::CLIMATE_MODE_COOL;
  this->target_temperature = 25.0f;
  this->set_fan_mode_(climate::CLIMATE_FAN_AUTO);
  this->action = climate::CLIMATE_ACTION_COOLING;

  return pkt != nullptr;
}

void BC7215ACClimate::mark_unpaired_() {
  if (this->ac_) {
    this->ac_->init_ok = false;
  }

  this->mode = climate::CLIMATE_MODE_OFF;
  this->target_temperature = 25.0f;
  this->set_fan_mode_(climate::CLIMATE_FAN_AUTO);
  this->action = climate::CLIMATE_ACTION_OFF;

  this->pairing_ = false;
  this->pair_state_ = PairState::IDLE;
  this->parsing_state_ = ParsingState::STOP;
  this->match_index_ = 0;
  this->set_status_("Not Paired. Prepare remote to 25°C/Cool");

  this->restore_led_mode_();
  this->publish_state();
}

int BC7215ACClimate::map_mode_to_bc7215_(climate::ClimateMode mode) const {
  // BC7215 AC example table:
  // 0 Auto, 1 Cool, 2 Heat, 3 Dry, 4 Fan
  switch (mode) {
    case climate::CLIMATE_MODE_AUTO:
	case climate::CLIMATE_MODE_HEAT_COOL:
      return 0;
    case climate::CLIMATE_MODE_COOL:
      return 1;
    case climate::CLIMATE_MODE_HEAT:
      return 2;
    case climate::CLIMATE_MODE_DRY:
      return 3;
    case climate::CLIMATE_MODE_FAN_ONLY:
      return 4;
    default:
      return 0;
  }
}

int BC7215ACClimate::map_fan_to_bc7215_() const {
  // BC7215 AC example table:
  // 0 Auto, 1 Low, 2 Medium, 3 High
  if (!this->fan_mode.has_value()) {
    return 0;
  }

  switch (*this->fan_mode) {
    case climate::CLIMATE_FAN_AUTO:
      return 0;
    case climate::CLIMATE_FAN_LOW:
      return 1;
    case climate::CLIMATE_FAN_MEDIUM:
      return 2;
    case climate::CLIMATE_FAN_HIGH:
      return 3;
    default:
      return 0;
  }
}

climate::ClimateMode BC7215ACClimate::map_bc7215_mode_to_ha_(int m) const {

  switch (m) {
    case 0:
      return climate::CLIMATE_MODE_HEAT_COOL;
//      return climate::CLIMATE_MODE_AUTO;

    case 1:
      return climate::CLIMATE_MODE_COOL;

    case 2:
      return climate::CLIMATE_MODE_HEAT;

    case 3:
      return climate::CLIMATE_MODE_DRY;

    case 4:
      return climate::CLIMATE_MODE_FAN_ONLY;

    default:
      return this->mode;
  }
}

climate::ClimateFanMode BC7215ACClimate::map_bc7215_fan_to_ha_(int f) const {
  // BC7215 AC example table:
  // 0 Auto, 1 Low, 2 Medium, 3 High
  switch (f) {
    case 0:
      return climate::CLIMATE_FAN_AUTO;

    case 1:
      return climate::CLIMATE_FAN_LOW;

    case 2:
      return climate::CLIMATE_FAN_MEDIUM;

    case 3:
      return climate::CLIMATE_FAN_HIGH;

    default:
      return *this->fan_mode;
  }
}

int BC7215ACClimate::climate_temp_to_library_temp_(float temp_c) const {
  if (std::isnan(temp_c)) {
    temp_c = 25.0f;
  }

  if (this->library_unit_celsius_) {
    return static_cast<int>(std::lround(temp_c));
  }

  // ESPHome climate uses Celsius internally. Convert to Fahrenheit only for
  // the BC7215 AC library if the library is using Fahrenheit protocol tables.
  return static_cast<int>(std::lround(temp_c * 9.0f / 5.0f + 32.0f));
}

float BC7215ACClimate::library_temp_to_climate_temp_(int t) const {
  if (this->library_unit_celsius_) {
    if ((t < 16) || (t > 30)) {
	  return this->target_temperature;
	}
    return static_cast<float>(t);
  }

  if ((t < 60) || (t > 88)) {
	return this->target_temperature;
  }
  return (static_cast<float>(t) - 32.0f) * 5.0f / 9.0f;
}

void BC7215ACClimate::update_action_from_mode_() {
  switch (this->mode) {
    case climate::CLIMATE_MODE_COOL:
      this->action = climate::CLIMATE_ACTION_COOLING;
      break;
    case climate::CLIMATE_MODE_HEAT:
      this->action = climate::CLIMATE_ACTION_HEATING;
      break;
    case climate::CLIMATE_MODE_DRY:
      this->action = climate::CLIMATE_ACTION_DRYING;
      break;
    case climate::CLIMATE_MODE_FAN_ONLY:
      this->action = climate::CLIMATE_ACTION_FAN;
      break;
    case climate::CLIMATE_MODE_AUTO:
    case climate::CLIMATE_MODE_HEAT_COOL:
      this->action = climate::CLIMATE_ACTION_IDLE;
      break;
    default:
      this->action = climate::CLIMATE_ACTION_IDLE;
      break;
  }
}

void BC7215ACClimate::log_data_packet_(const char *label, const bc7215DataVarPkt_t *pkt) const {

  if (pkt == nullptr) {
    ESP_LOGD(TAG, "%s: <null packet>", label);
    return;
  }

  const uint16_t data_bytes =
      static_cast<uint16_t>((pkt->bitLen + 7) / 8);

  std::string hex;
  hex.reserve(data_bytes * 3);

  char buf[4];

  for (uint16_t i = 0; i < data_bytes; ++i) {
    std::snprintf(
        buf,
        sizeof(buf),
        "%02X",
        static_cast<unsigned int>(pkt->data[i]));

    if (!hex.empty()) {
      hex += ' ';
    }

    hex += buf;
  }

  ESP_LOGD(
      TAG,
      "%s: bitLen=%u, bytes=%u: %s",
      label,
      static_cast<unsigned int>(pkt->bitLen),
      static_cast<unsigned int>(data_bytes),
      hex.c_str());
}

void BC7215ACClimate::log_format_packet_(
    const char *label,
    const bc7215FormatPkt_t *pkt) const {

  if (pkt == nullptr) {
    ESP_LOGD(TAG, "%s: <null format packet>", label);
    return;
  }

  const uint8_t *raw =
      reinterpret_cast<const uint8_t *>(pkt);

  const size_t bytes = sizeof(bc7215FormatPkt_t);

  std::string hex;
  hex.reserve(bytes * 3);

  char buf[4];

  for (size_t i = 0; i < bytes; ++i) {
    std::snprintf(
        buf,
        sizeof(buf),
        "%02X",
        static_cast<unsigned int>(raw[i]));

    if (!hex.empty()) {
      hex += ' ';
    }

    hex += buf;
  }

  ESP_LOGD(
      TAG,
      "%s: bytes=%u: %s",
      label,
      static_cast<unsigned int>(bytes),
      hex.c_str());
}

bool BC7215ACClimate::copy_current_base_(bc7215DataMaxPkt_t &dst) const {
  if (!this->ac_) {
    return false;
  }

  const bc7215DataVarPkt_t *src = this->ac_->data_packet();
  if (src == nullptr) {
    return false;
  }

  const uint16_t bytes =
      static_cast<uint16_t>((src->bitLen + 7) / 8);

  if (bytes == 0 || bytes > sizeof(dst.data)) {
    return false;
  }

  std::memset(&dst, 0, sizeof(dst));
  dst.bitLen = src->bitLen;
  std::memcpy(dst.data, src->data, bytes);

  return true;
}

bool BC7215ACClimate::save_user_function_() {
  if (!this->user_base_valid_) {
    return false;
  }

  StoredUserFunction saved{};
  saved.valid = true;
  saved.data = this->user_base_;

  if (!this->user_pref_.save(&saved)) {
    ESP_LOGW(TAG, "Failed to save user function.");
    return false;
  }

  ESP_LOGI(TAG, "User function saved.");
  return true;
}

bool BC7215ACClimate::load_user_function_() {
  StoredUserFunction saved{};

  if (!this->user_pref_.load(&saved) || !saved.valid) {
    this->user_base_valid_ = false;
    ESP_LOGI(TAG, "No saved user function.");
    return false;
  }

  const uint16_t bytes =
      static_cast<uint16_t>((saved.data.bitLen + 7) / 8);

  if (bytes == 0 || bytes > sizeof(saved.data.data)) {
    this->user_base_valid_ = false;
    ESP_LOGW(TAG, "Saved user function is invalid.");
    return false;
  }

  this->user_base_ = saved.data;
  this->user_base_valid_ = true;
  this->user_function_active_ = false;

  ESP_LOGI(TAG, "User function restored from flash.");
  return true;
}

void BC7215ACClimate::clear_user_function_() {
  this->user_base_valid_ = false;
  this->user_function_active_ = false;

  StoredUserFunction saved{};
  saved.valid = false;
  this->user_pref_.save(&saved);
}

void BC7215ACClimate::user_button() {
  if (!this->ac_) {
    this->set_status_("BC7215AC is not ready.");
    return;
  }

  if (this->pairing_) {
    // Backwards-compatible physical Pair -> User Button sequence.
    // Use the same reliable capture-mode transition as the dedicated
    // "Learn User Function" button in Home Assistant.
    this->start_user_function_learning();
    return;
  }

  // Normal User Button toggle logic follows here...
  if (!this->is_paired()) {
    this->set_status_("AC is not paired.");
    return;
  }

  if (!this->user_base_valid_) {
    this->set_status_("No user function saved.");
    return;
  }

  if (!this->user_function_active_) {
    // Remember whatever the current normal base is.
    if (!this->copy_current_base_(
            this->normal_base_before_user_)) {
      this->set_status_("Failed to save current base.");
      return;
    }

    if (!this->ac_->replace_base(this->user_base_)) {
      this->set_status_("Failed to enable user function.");
      return;
    }

    this->user_function_active_ = true;
    this->set_status_("User function enabled.");
  } else {
    if (!this->ac_->replace_base(
            this->normal_base_before_user_)) {
      this->set_status_("Failed to restore normal base.");
      return;
    }

    this->user_function_active_ = false;
    this->set_status_("User function disabled.");
  }
  this->send_current_state_();
  this->publish_state();
}


// -----------------------------------------------------------------------------
// Physical button. Hold >=5s to show erase warning. Erase only on release.
// Short and double clicks are identified by YAML on_multi_click separately.
// -----------------------------------------------------------------------------
void BC7215ACClimate::physical_button_pressed() {
  this->physical_button_down_ = true;
  this->physical_button_down_ms_ = millis();
}

bool BC7215ACClimate::physical_button_released() {
  if (!this->physical_button_down_) return false;
  this->physical_button_down_ = false;
  const bool long_press = static_cast<uint32_t>(millis() - this->physical_button_down_ms_) >= 5000;
  if (this->led_mode_ == LedMode::ERASE_WARNING)
    this->restore_led_mode_();
  if (long_press)
    this->set_status_("Long press confirmed: factory reset (WiFi and pairing).");
  // Actual NVS reset/reboot is dispatched via YAML's factory_reset button.
  return long_press;
}

bool BC7215ACClimate::clear_pairing() {
  if (!this->ac_) {
    this->set_status_("Cannot clear pairing: controller not ready.");
    return false;
  }

  this->ac_->stop_capture();
  this->pairing_ = false;
  this->user_learning_ = false;
  this->pairing_had_existing_pair_ = false;
  this->user_function_active_ = false;
  this->user_base_valid_ = false;
  this->predefined_number_ = 0;
  this->power_on_ = false;
  this->user_base_ = {};
  this->normal_base_before_user_ = {};

  // Preserve the on-flash structure and its preference key so older versions
  // remain compatible. Empty packet marks the pairing record invalid.
  StoredPairingData empty_pairing{};
  StoredUserFunction empty_user{};
  const bool pairing_saved = this->pref_.save(&empty_pairing);
  const bool user_saved = this->user_pref_.save(&empty_user);
  const bool synced = global_preferences->sync();

  this->mark_unpaired_();
  if (pairing_saved && user_saved && synced) {
    this->set_status_("BC7215 pairing and user function cleared.");
  } else {
    this->set_status_("Pairing cleared in RAM; flash save failed.");
    ESP_LOGE(TAG, "Pair erase: pair=%d user=%d sync=%d",
             pairing_saved, user_saved, synced);
  }
  return pairing_saved && user_saved && synced;
}

BC7215ACClimate::LedMode BC7215ACClimate::normal_led_mode_() const {
  auto *wifi = wifi::global_wifi_component;
  // With WiFi setup configured by captive portal/Improv, has_sta() indicates
  // whether station credentials have been provisioned or restored from flash.
  // This must NOT be used with fixed wifi.ssid/password in YAML.
  if (wifi == nullptr || !wifi->has_sta())
    return this->is_paired() ? LedMode::PAIRED_NO_WIFI : LedMode::WIFI_NOT_CONFIGURED;
  if (!wifi->is_connected())
    return LedMode::WIFI_CONNECTING;
  return this->is_paired() ? LedMode::PAIRED : LedMode::UNPAIRED;
}

void BC7215ACClimate::restore_led_mode_() {
  this->led_mode_ = this->pairing_ ? LedMode::WAITING_IR
                                  : (this->led_alt_hold_off_ ? LedMode::ALT_OFF
                                                            : this->normal_led_mode_());
  this->led_mode_start_ms_ = millis();
}

void BC7215ACClimate::start_led_feedback_(uint16_t count, uint32_t on_ms, uint32_t off_ms) {
  if (count == 0) {
    this->restore_led_mode_();
    return;
  }
  this->led_feedback_count_ = count;
  this->led_feedback_on_ms_ = on_ms;
  this->led_feedback_off_ms_ = off_ms;
  this->led_mode_ = LedMode::FEEDBACK;
  this->led_mode_start_ms_ = millis();
}

void BC7215ACClimate::update_led_() {
  if (this->led_output_ == nullptr) return;
  const uint32_t now = millis();
  if (static_cast<uint32_t>(now - this->led_update_ms_) < 20) return;
  this->led_update_ms_ = now;

  if (this->physical_button_down_ &&
      static_cast<uint32_t>(now - this->physical_button_down_ms_) >= 5000 &&
      this->led_mode_ != LedMode::ERASE_WARNING) {
    this->led_mode_ = LedMode::ERASE_WARNING;
    this->led_mode_start_ms_ = now;
  }

  // Normal modes dynamically track both WiFi connectivity and AC pairing.
  // Pairing, protocol feedback and erase warning have priority.
  if (this->led_mode_ != LedMode::ERASE_WARNING &&
      this->led_mode_ != LedMode::FEEDBACK) {
    LedMode desired = this->pairing_ ? LedMode::WAITING_IR
                                     : (this->led_alt_hold_off_ ? LedMode::ALT_OFF
                                                               : this->normal_led_mode_());
    if (this->led_mode_ != desired) {
      this->led_mode_ = desired;
      this->led_mode_start_ms_ = now;
    }
  }

  const uint32_t elapsed = static_cast<uint32_t>(now - this->led_mode_start_ms_);
  float brightness = 0.0f;
  switch (this->led_mode_) {
    case LedMode::ALT_OFF:
      brightness = 0.0f;
      break;
    case LedMode::WIFI_NOT_CONFIGURED:
      brightness = 1.0f;
      break;
    case LedMode::WIFI_CONNECTING:
      brightness = (elapsed % 1000 < 500) ? 1.0f : 0.0f;
      break;
    case LedMode::PAIRED_NO_WIFI: {
      const uint32_t t = elapsed % 2000;
      brightness = (t < 100 || (t >= 250 && t < 350)) ? 1.0f : 0.0f;
      break;
    }
    case LedMode::PAIRED: {
      constexpr float TWO_PI = 6.28318530718f;
      const float phase = static_cast<float>(elapsed % 3000) / 3000.0f;
      brightness = 0.5f - 0.5f * cosf(TWO_PI * phase);
      break;
    }
    case LedMode::UNPAIRED:
      brightness = (elapsed % 2000 < 100) ? 1.0f : 0.0f;
      break;
    case LedMode::WAITING_IR:
    case LedMode::ERASE_WARNING:
      brightness = (elapsed % 333 < 167) ? 1.0f : 0.0f;
      break;
    case LedMode::FEEDBACK: {
      const uint32_t period = this->led_feedback_on_ms_ + this->led_feedback_off_ms_;
      const uint64_t total = static_cast<uint64_t>(period) * this->led_feedback_count_;
      if (period == 0 || elapsed >= total) {
        this->restore_led_mode_();
        // ALT feedback is finished; stay off until an explicit resume action.
        brightness = 0.0f;
      } else {
        brightness = (elapsed % period < this->led_feedback_on_ms_) ? 1.0f : 0.0f;
      }
      break;
    }
  }
  this->led_output_->set_level(brightness);
}

}  // namespace bc7215_ac
}  // namespace esphome
