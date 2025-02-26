#include "jbd_bms_ble.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"

namespace esphome {
namespace jbd_bms_ble {

static const char *const TAG = "jbd_bms_ble";

static const uint16_t JBD_BMS_SERVICE_UUID = 0xFF00;
static const uint16_t JBD_BMS_NOTIFY_CHARACTERISTIC_UUID = 0xFF01;
static const uint16_t JBD_BMS_CONTROL_CHARACTERISTIC_UUID = 0xFF02;

static const uint16_t MAX_RESPONSE_SIZE = 72;

static const uint8_t JBD_PKT_START = 0xDD;
static const uint8_t JBD_PKT_END = 0x77;
static const uint8_t JBD_CMD_READ = 0xA5;
static const uint8_t JBD_CMD_WRITE = 0x5A;

static const uint8_t JBD_CMD_HWINFO = 0x03;
static const uint8_t JBD_CMD_CELLINFO = 0x04;
static const uint8_t JBD_CMD_HWVER = 0x05;

static const uint8_t JBD_CMD_CAP_REM = 0xE0;   // Set remaining capacity
static const uint8_t JBD_CMD_MOS = 0xE1;       // Set charging/discharging bitmask
static const uint8_t JBD_CMD_BALANCER = 0xE2;  // Enable/disable balancer

static const uint8_t JBD_MOS_CHARGE = 0x01;
static const uint8_t JBD_MOS_DISCHARGE = 0x02;

static const uint8_t ERRORS_SIZE = 16;
static const char *const ERRORS[ERRORS_SIZE] = {
    "Cell overvoltage",               // 0x00
    "Cell undervoltage",              // 0x01
    "Pack overvoltage",               // 0x02
    "Pack undervoltage",              // 0x03
    "Charging over temperature",      // 0x04
    "Charging under temperature",     // 0x05
    "Discharging over temperature",   // 0x06
    "Discharging under temperature",  // 0x07
    "Charging overcurrent",           // 0x08
    "Discharging overcurrent",        // 0x09
    "Short circuit",                  // 0x0A
    "IC front-end error",             // 0x0B
    "Mosfet Software Lock",           // 0x0C
    "Charge timeout Close",           // 0x0D
    "Unknown (0x0E)",                 // 0x0E
    "Unknown (0x0F)",                 // 0x0F
};

void JbdBmsBle::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                    esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_OPEN_EVT: {
      break;
    }
    case ESP_GATTC_DISCONNECT_EVT: {
      this->node_state = espbt::ClientState::IDLE;

      // this->publish_state_(this->voltage_sensor_, NAN);
      break;
    }
    case ESP_GATTC_SEARCH_CMPL_EVT: {
      auto *char_notify = this->parent_->get_characteristic(JBD_BMS_SERVICE_UUID, JBD_BMS_NOTIFY_CHARACTERISTIC_UUID);
      if (char_notify == nullptr) {
        ESP_LOGE(TAG, "[%s] No notify service found at device, not an JBD BMS..?",
                 this->parent_->address_str().c_str());
        break;
      }
      this->char_notify_handle_ = char_notify->handle;

      auto status = esp_ble_gattc_register_for_notify(this->parent()->get_gattc_if(), this->parent()->get_remote_bda(),
                                                      char_notify->handle);
      if (status) {
        ESP_LOGW(TAG, "esp_ble_gattc_register_for_notify failed, status=%d", status);
      }

      auto *char_command = this->parent_->get_characteristic(JBD_BMS_SERVICE_UUID, JBD_BMS_CONTROL_CHARACTERISTIC_UUID);
      if (char_command == nullptr) {
        ESP_LOGE(TAG, "[%s] No control service found at device, not an JBD BMS..?",
                 this->parent_->address_str().c_str());
        break;
      }
      this->char_command_handle_ = char_command->handle;
      break;
    }
    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
      this->node_state = espbt::ClientState::ESTABLISHED;

      ESP_LOGI(TAG, "Request device info");
      this->send_command_(JBD_CMD_READ, JBD_CMD_HWVER);

      break;
    }
    case ESP_GATTC_NOTIFY_EVT: {
      if (param->notify.handle != this->char_notify_handle_)
        break;

      ESP_LOGVV(TAG, "Notification received: %s",
                format_hex_pretty(param->notify.value, param->notify.value_len).c_str());

      this->assemble_(param->notify.value, param->notify.value_len);

      break;
    }
    default:
      break;
  }
}

void JbdBmsBle::assemble_(const uint8_t *data, uint16_t length) {
  if (this->frame_buffer_.size() > MAX_RESPONSE_SIZE) {
    ESP_LOGW(TAG, "Maximum response size exceeded");
    this->frame_buffer_.clear();
  }

  // Flush buffer on every preamble
  if (data[0] == 0xDD && data[2] == 0x00) {
    this->frame_buffer_.clear();
  }

  this->frame_buffer_.insert(this->frame_buffer_.end(), data, data + length);

  if (this->frame_buffer_.back() == JBD_PKT_END) {
    const uint8_t *raw = &this->frame_buffer_[0];

    uint8_t function = raw[1];
    uint16_t data_len = raw[3];
    uint16_t frame_len = 4 + data_len + 3;
    if (frame_len != this->frame_buffer_.size()) {
      ESP_LOGW(TAG, "Invalid frame length");
      this->frame_buffer_.clear();
      return;
    }

    uint16_t computed_crc = chksum_(raw + 2, data_len + 2);
    uint16_t remote_crc = uint16_t(raw[frame_len - 3]) << 8 | (uint16_t(raw[frame_len - 2]) << 0);
    if (computed_crc != remote_crc) {
      ESP_LOGW(TAG, "JbdBms CRC Check failed! %04X != %04X", computed_crc, remote_crc);
      this->frame_buffer_.clear();
      return;
    }

    std::vector<uint8_t> data(this->frame_buffer_.begin() + 4, this->frame_buffer_.end() - 3);

    this->on_jbd_bms_ble_data_(function, data);
    this->frame_buffer_.clear();
  }
}

void JbdBmsBle::update() {
  if (this->enable_fake_traffic_) {
    // Start: 0xDD 0x03 0x00 0x1D
    this->on_jbd_bms_ble_data_(JBD_CMD_HWINFO, {0x06, 0x18, 0x00, 0x00, 0x01, 0xF2, 0x01, 0xF4, 0x00, 0x00, 0x2C,
                                                0x7C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x64, 0x03, 0x04,
                                                0x03, 0x0B, 0x8B, 0x0B, 0x8A, 0x0B, 0x84, 0xFA, 0x8D, 0x77});
    // End: 0xFA 0x8D 0x77

    // Start: 0xDD 0x04 0x00 0x08
    this->on_jbd_bms_ble_data_(JBD_CMD_CELLINFO, {0x0F, 0x45, 0x0F, 0x3D, 0x0F, 0x37, 0x0F, 0x3D, 0xFE, 0xC6, 0x77});
    // End: 0xFE 0xC6 0x77

    // Start: 0xDD 0x05 0x00 0x19
    this->on_jbd_bms_ble_data_(JBD_CMD_HWVER,
                               {0x4A, 0x42, 0x44, 0x2D, 0x53, 0x50, 0x30, 0x34, 0x53, 0x30, 0x33, 0x34, 0x2D,
                                0x4C, 0x34, 0x53, 0x2D, 0x32, 0x30, 0x30, 0x41, 0x2D, 0x42, 0x2D, 0x55});
    // End: 0xFA, 0x08, 0x77
  }

  if (this->node_state != espbt::ClientState::ESTABLISHED) {
    ESP_LOGW(TAG, "[%s] Not connected", this->parent_->address_str().c_str());
    return;
  }

  this->send_command_(JBD_CMD_READ, JBD_CMD_HWINFO);
}

void JbdBmsBle::on_jbd_bms_ble_data_(const uint8_t &function, const std::vector<uint8_t> &data) {
  switch (function) {
    case JBD_CMD_HWINFO:
      this->on_hardware_info_data_(data);
      break;
    case JBD_CMD_CELLINFO:
      this->on_cell_info_data_(data);
      break;
    case JBD_CMD_HWVER:
      this->on_hardware_version_data_(data);
      break;
    default:
      ESP_LOGW(TAG, "Unhandled response received: %s", format_hex_pretty(&data.front(), data.size()).c_str());
  }
}

void JbdBmsBle::on_cell_info_data_(const std::vector<uint8_t> &data) {
  auto jbd_get_16bit = [&](size_t i) -> uint16_t {
    return (uint16_t(data[i + 0]) << 8) | (uint16_t(data[i + 1]) << 0);
  };

  uint8_t data_len = data.size();
  if (data_len < 2 || data_len > 64 || (data_len % 2) != 0) {
    ESP_LOGW(TAG, "Skipping cell info frame because of invalid length: %d", data_len);
    return;
  }

  // Byte Len  Payload                Content              Coeff.      Unit        Example value
  // 0     2   0x0F 0x3B              Cell voltage 1       0.001       V           3899 = 3.899
  // 2     2   0x0F 0x30              Cell voltage 2       0.001       V
  // 4     2   0x0F 0x2D              Cell voltage 3       0.001       V
  // 6     2   0x0F 0x31              Cell voltage 4       0.001       V
  uint8_t cells = std::min(data_len / 2, 32);
  float min_cell_voltage = 100.0f;
  float max_cell_voltage = -100.0f;
  float average_cell_voltage = 0.0f;
  uint8_t min_voltage_cell = 0;
  uint8_t max_voltage_cell = 0;
  for (uint8_t i = 0; i < cells; i++) {
    float cell_voltage = (float) jbd_get_16bit(0 + (i * 2)) * 0.001f;
    average_cell_voltage = average_cell_voltage + cell_voltage;
    if (cell_voltage < min_cell_voltage) {
      min_cell_voltage = cell_voltage;
      min_voltage_cell = i + 1;
    }
    if (cell_voltage > max_cell_voltage) {
      max_cell_voltage = cell_voltage;
      max_voltage_cell = i + 1;
    }
    this->publish_state_(this->cells_[i].cell_voltage_sensor_, cell_voltage);
  }
  average_cell_voltage = average_cell_voltage / cells;

  this->publish_state_(this->min_cell_voltage_sensor_, min_cell_voltage);
  this->publish_state_(this->max_cell_voltage_sensor_, max_cell_voltage);
  this->publish_state_(this->max_voltage_cell_sensor_, (float) max_voltage_cell);
  this->publish_state_(this->min_voltage_cell_sensor_, (float) min_voltage_cell);
  this->publish_state_(this->delta_cell_voltage_sensor_, max_cell_voltage - min_cell_voltage);
  this->publish_state_(this->average_cell_voltage_sensor_, average_cell_voltage);
}

void JbdBmsBle::on_hardware_info_data_(const std::vector<uint8_t> &data) {
  auto jbd_get_16bit = [&](size_t i) -> uint16_t {
    return (uint16_t(data[i + 0]) << 8) | (uint16_t(data[i + 1]) << 0);
  };
  auto jbd_get_32bit = [&](size_t i) -> uint32_t {
    return (uint32_t(jbd_get_16bit(i + 0)) << 16) | (uint32_t(jbd_get_16bit(i + 2)) << 0);
  };

  // Byte Len  Payload                Content              Coeff.      Unit        Example value
  // 0     2   0x06 0x17              Total voltage                                1559
  // 2     2   0x00 0x00              Current
  // 4     2   0x01 0xF3              Residual Capacity                            499
  // 6     2   0x01 0xF4              Nominal Capacity                             500
  // 8     2   0x00 0x00              Cycle Life
  // 10    2   0x2C 0x7C              Production date
  // 12    4   0x00 0x00 0x00 0x00    Balancer Status
  // 16    2   0x00 0x00              Protection Status
  // 18    1   0x80                   Version                                      0x10 = 1.0, 0x80 = 8.0
  // 19    1   0x64                   Stage of charge
  // 20    1   0x03                   Mosfet bitmask
  // 21    2   0x04                   Cell count
  // 22    3   0x03                   Temperature sensors
  // 23    2   0x0B 0x8D              Temperature 1
  // 25    2   0x0B 0x8C              Temperature 2
  // 27    2   0x0B 0x88              Temperature 3

  ESP_LOGD(TAG, "Hardware info:");
  ESP_LOGD(TAG, "  Device model: %s", this->device_model_.c_str());

  float total_voltage = jbd_get_16bit(0) * 0.01f;
  this->publish_state_(this->total_voltage_sensor_, total_voltage);

  float current = (float) ((int16_t) jbd_get_16bit(2)) * 0.01f;
  float power = total_voltage * current;
  this->publish_state_(this->current_sensor_, current);
  this->publish_state_(this->power_sensor_, power);
  this->publish_state_(this->charging_power_sensor_, std::max(0.0f, power));               // 500W vs 0W -> 500W
  this->publish_state_(this->discharging_power_sensor_, std::abs(std::min(0.0f, power)));  // -500W vs 0W -> 500W
  this->publish_state_(this->capacity_remaining_sensor_, (float) jbd_get_16bit(4) * 0.01f);
  this->publish_state_(this->nominal_capacity_sensor_, (float) jbd_get_16bit(6) * 0.01f);
  this->publish_state_(this->charging_cycles_sensor_, (float) jbd_get_16bit(8));

  uint16_t production_date = jbd_get_16bit(10);
  ESP_LOGD(TAG, "  Date of manufacture: %d.%d.%d", 2000 + (production_date >> 9), (production_date >> 5) & 0x0f,
           production_date & 0x1f);

  uint32_t balance_status_bitmask = jbd_get_32bit(12);
  this->publish_state_(this->balancer_status_bitmask_sensor_, (float) balance_status_bitmask);
  this->publish_state_(this->balancing_binary_sensor_, balance_status_bitmask > 0);

  uint16_t errors_bitmask = jbd_get_16bit(16);
  this->publish_state_(this->errors_bitmask_sensor_, (float) errors_bitmask);
  this->publish_state_(this->errors_text_sensor_, this->error_bits_to_string_(errors_bitmask));

  this->publish_state_(this->software_version_sensor_, (data[18] >> 4) + ((data[18] & 0x0f) * 0.1f));

  this->publish_state_(this->state_of_charge_sensor_, data[19]);

  uint8_t operation_status = data[20];
  this->publish_state_(this->operation_status_bitmask_sensor_, operation_status);
  this->publish_state_(this->charging_binary_sensor_, operation_status & JBD_MOS_CHARGE);
  this->publish_state_(this->discharging_binary_sensor_, operation_status & JBD_MOS_DISCHARGE);

  this->publish_state_(this->battery_strings_sensor_, data[21]);

  uint8_t temperature_sensors = std::min(data[22], (uint8_t) 6);
  this->publish_state_(this->temperature_sensors_sensor_, temperature_sensors);
  for (uint8_t i = 0; i < temperature_sensors; i++) {
    this->publish_state_(this->temperatures_[i].temperature_sensor_,
                         (float) (jbd_get_16bit(23 + (i * 2)) - 2731) * 0.1f);
  }

  this->send_command_(JBD_CMD_READ, JBD_CMD_CELLINFO);
}

void JbdBmsBle::on_hardware_version_data_(const std::vector<uint8_t> &data) {
  // Byte Len  Payload                                              Content
  // 0    25   0x4A 0x42 0x44 0x2D 0x53 0x50 0x30 0x34 0x53 0x30
  //           0x33 0x34 0x2D 0x4C 0x34 0x53 0x2D 0x32 0x30 0x30
  //           0x41 0x2D 0x42 0x2D 0x55
  this->device_model_ = std::string(data.begin(), data.end());

  ESP_LOGI(TAG, "Hardware version:");
  ESP_LOGI(TAG, "  Model name: %s", this->device_model_.c_str());
  this->publish_state_(this->device_model_text_sensor_, this->device_model_);
}

void JbdBmsBle::dump_config() {  // NOLINT(google-readability-function-size,readability-function-size)
  ESP_LOGCONFIG(TAG, "JbdBmsBle:");
  ESP_LOGCONFIG(TAG, "  Fake traffic enabled: %s", YESNO(this->enable_fake_traffic_));

  LOG_SENSOR("", "Total voltage", this->total_voltage_sensor_);
  LOG_SENSOR("", "Battery strings", this->battery_strings_sensor_);
  LOG_SENSOR("", "Software version", this->software_version_sensor_);
  LOG_SENSOR("", "Current", this->current_sensor_);
  LOG_SENSOR("", "Power", this->power_sensor_);
  LOG_SENSOR("", "Charging Power", this->charging_power_sensor_);
  LOG_SENSOR("", "Discharging Power", this->discharging_power_sensor_);
  LOG_SENSOR("", "State of charge", this->state_of_charge_sensor_);
  LOG_SENSOR("", "Nominal capacity", this->nominal_capacity_sensor_);
  LOG_SENSOR("", "Charging cycles", this->charging_cycles_sensor_);
  LOG_SENSOR("", "Capacity remaining", this->capacity_remaining_sensor_);
  LOG_SENSOR("", "Average cell voltage sensor", this->average_cell_voltage_sensor_);
  LOG_SENSOR("", "Maximum cell voltage", this->max_cell_voltage_sensor_);
  LOG_SENSOR("", "Min voltage cell", this->min_voltage_cell_sensor_);
  LOG_SENSOR("", "Max voltage cell", this->max_voltage_cell_sensor_);
  LOG_SENSOR("", "Minimum cell voltage", this->min_cell_voltage_sensor_);
  LOG_SENSOR("", "Temperature 1", this->temperatures_[0].temperature_sensor_);
  LOG_SENSOR("", "Temperature 2", this->temperatures_[1].temperature_sensor_);
  LOG_SENSOR("", "Temperature 3", this->temperatures_[2].temperature_sensor_);
  LOG_SENSOR("", "Temperature 4", this->temperatures_[3].temperature_sensor_);
  LOG_SENSOR("", "Temperature 5", this->temperatures_[4].temperature_sensor_);
  LOG_SENSOR("", "Temperature 6", this->temperatures_[5].temperature_sensor_);
  LOG_SENSOR("", "Cell Voltage 1", this->cells_[0].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 2", this->cells_[1].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 3", this->cells_[2].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 4", this->cells_[3].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 5", this->cells_[4].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 6", this->cells_[5].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 7", this->cells_[6].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 8", this->cells_[7].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 9", this->cells_[8].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 10", this->cells_[9].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 11", this->cells_[10].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 12", this->cells_[11].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 13", this->cells_[12].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 14", this->cells_[13].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 15", this->cells_[14].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 16", this->cells_[15].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 17", this->cells_[16].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 18", this->cells_[17].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 19", this->cells_[18].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 20", this->cells_[19].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 21", this->cells_[20].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 22", this->cells_[21].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 23", this->cells_[22].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 24", this->cells_[23].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 25", this->cells_[24].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 26", this->cells_[25].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 27", this->cells_[26].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 28", this->cells_[27].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 29", this->cells_[28].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 30", this->cells_[29].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 31", this->cells_[30].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 32", this->cells_[31].cell_voltage_sensor_);
}

void JbdBmsBle::publish_state_(binary_sensor::BinarySensor *binary_sensor, const bool &state) {
  if (binary_sensor == nullptr)
    return;

  binary_sensor->publish_state(state);
}

void JbdBmsBle::publish_state_(sensor::Sensor *sensor, float value) {
  if (sensor == nullptr)
    return;

  sensor->publish_state(value);
}

void JbdBmsBle::publish_state_(text_sensor::TextSensor *text_sensor, const std::string &state) {
  if (text_sensor == nullptr)
    return;

  text_sensor->publish_state(state);
}

void JbdBmsBle::write_register(uint8_t address, uint16_t value) {
  this->send_command_(JBD_CMD_WRITE, JBD_CMD_MOS);  // @TODO: Pass value
}

bool JbdBmsBle::send_command_(uint8_t action, uint8_t function) {
  uint8_t frame[7];
  uint8_t data_len = 0;

  frame[0] = JBD_PKT_START;
  frame[1] = action;
  frame[2] = function;
  frame[3] = data_len;
  auto crc = chksum_(frame + 2, data_len + 2);
  frame[4] = crc >> 8;
  frame[5] = crc >> 0;
  frame[6] = JBD_PKT_END;

  ESP_LOGVV(TAG, "Send command: %s", format_hex_pretty(frame, sizeof(frame)).c_str());
  auto status =
      esp_ble_gattc_write_char(this->parent_->get_gattc_if(), this->parent_->get_conn_id(), this->char_command_handle_,
                               sizeof(frame), frame, ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);

  if (status)
    ESP_LOGW(TAG, "[%s] esp_ble_gattc_write_char failed, status=%d", this->parent_->address_str().c_str(), status);

  return (status == 0);
}

std::string JbdBmsBle::error_bits_to_string_(const uint16_t mask) {
  bool first = true;
  std::string errors_list = "";

  if (mask) {
    for (int i = 0; i < ERRORS_SIZE; i++) {
      if (mask & (1 << i)) {
        if (first) {
          first = false;
        } else {
          errors_list.append(";");
        }
        errors_list.append(ERRORS[i]);
      }
    }
  }

  return errors_list;
}

}  // namespace jbd_bms_ble
}  // namespace esphome
