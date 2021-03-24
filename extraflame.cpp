#include "extraflame.h"
#include "esphome/core/log.h"

namespace esphome {
namespace extraflame {

static const char *TAG = "extraflame.hub";
static const char *CURRENT_REQUEST = "extraflame.hub.request";

static uint8_t get_memory_from_command(uint8_t command0) {
  if (command0 == 0x80) {
    return 0x00;
  } else if (command0 == 0xA0) {
    return 0x20;
  }
  return command0;
}

#ifdef USE_EXTRAFLAME_DUMP
static const char *TAG_DUMP = "extraflame.dump";

void ExtraflameHub::setup() {
  // Declare a service "dump_memory"
  //  - Service will be called "esphome.<NODE_NAME>_dump_memory" in Home Assistant.
  //  - The service has one arguments (type inferred from method definition):
  //     - memory: string
  //  - The function start_washer_cycle declared below will attached to the service.
  register_service(&ExtraflameHub::on_dump_memory_, "dump_memory", {"memory"});
}
#endif

void ExtraflameHub::dump_config() {
  ESP_LOGCONFIG(TAG, "ExtraflameHub:");
#ifdef USE_EXTRAFLAME_DUMP
  ESP_LOGCONFIG(TAG, "  Dump: true");
#else
  ESP_LOGCONFIG(TAG, "  Dump: false");
#endif

  for (auto *component : this->components_) {
    component->dump_config_internal();
  }

  this->check_uart_settings(1200, 2, uart::UARTParityOptions::UART_CONFIG_PARITY_NONE, 8);
}

void ExtraflameHub::loop() {
  while (this->available() >= 2 && this->status_ != NO_REQUEST) {
    auto response = *this->read_array<2>();

    ESP_LOGV(TAG, "Read value: 0x%02X 0x%02X", response[0], response[1]);

    if ((this->status_ == REQUEST_SEND && this->is_request_echo_(response, 0)) ||
        (this->status_ == REQUEST_ECHO && this->request_.command.size() == 4 && this->is_request_echo_(response, 2))) {
      ESP_LOGD(TAG, "Response was request echo");
      this->status_ = REQUEST_ECHO;
      continue;
    }

    this->cancel_timeout(CURRENT_REQUEST);
    int value = response[1];
    bool success = true;
    if (this->request_.command.size() == 4) {
      // handle write request
      if (this->request_.command[1] != response[0] || this->request_.command[2] != response[1]) {
        ESP_LOGW(TAG, "Invalid write request. Probably you are not allowed to change the value");
        this->reset_input_buffer();
        success = false;
      }
    } else {
      // handle read request

      // checksum is calculated by (memory + address + value) & 0xFF
      uint8_t checksum_calc = (this->request_.command[0] + this->request_.command[1] + response[1]) & 0xFF;
      if (checksum_calc != response[0]) {
        ESP_LOGW(TAG, "Checksum invalid. Skipping update");
        this->reset_input_buffer();
        success = false;
      }
    }

    if (success) {
      this->notify_components_(get_memory_from_command(this->request_.command[0]), this->request_.command[1],
                               int(value));
    }

    if (this->request_.on_response != nullptr) {
      this->request_.on_response(value, success);
    }
    this->status_ = NO_REQUEST;
  }
  process_request_queue_();
}

void ExtraflameHub::process_request_queue_() {
  if (this->status_ == NO_REQUEST && !this->request_queue_.empty()) {
    this->status_ = REQUEST_SEND;
    this->request_ = this->request_queue_.front();
    this->request_queue_.erase(request_queue_.begin());
    this->set_timeout(CURRENT_REQUEST, 1000, [this] {
      if (this->request_.command.size() == 2) {
        ESP_LOGW(TAG, "No response for given command: 0x%02X 0x%02X", this->request_.command[0],
                 this->request_.command[1]);
      } else {
        ESP_LOGW(TAG, "No response for given command: 0x%02X 0x%02X  0x%02X  0x%02X", this->request_.command[0],
                 this->request_.command[1], this->request_.command[2], this->request_.command[3]);
      }
      this->reset_input_buffer();
      if (this->request_.on_response != nullptr) {
        this->request_.on_response(0x00, false);
      }
      this->status_ = NO_REQUEST;
    });
    if (this->request_.command.size() == 2) {
      ESP_LOGV(TAG, "Sending request: 0x%02X 0x%02X", this->request_.command[0], this->request_.command[1]);
    } else {
      ESP_LOGW(TAG, "Sending request: 0x%02X 0x%02X  0x%02X  0x%02X", this->request_.command[0],
               this->request_.command[1], this->request_.command[2], this->request_.command[3]);
    }
    ESP_LOGV(TAG, "Sending request: 0x%02X 0x%02X", this->request_.command[0], this->request_.command[1]);
    this->write_array(this->request_.command);
  }
}

void ExtraflameHub::add_request(ExtraflameRequest request, bool priority) {
  ESP_LOGV(TAG, "Adding request");
  if (priority) {
    this->request_queue_.insert(this->request_queue_.begin(), request);
  } else {
    this->request_queue_.push_back(request);
  }
  this->process_request_queue_();
}

void ExtraflameHub::add_request(std::vector<uint8_t> command) {
  auto request = ExtraflameRequest{.command = command, .on_response = nullptr};
  this->add_request(request, false);
}

void ExtraflameHub::reset_input_buffer() {
  ESP_LOGW(TAG, "Reseting buffer and going on with next request");
  while (this->available() > 0) {
    // to clear the buffer
    this->read();
  }
}

void ExtraflameHub::add_component(ExtraflameComponent *component) { this->components_.push_back(component); }

void ExtraflameHub::notify_components_(uint8_t memory_hex, uint8_t address, int value) {
  for (auto *component : this->components_) {
    if (memory_hex == component->get_memory_hex() && address == component->get_address()) {
      component->on_read_response(value);
    }
  }
}

bool ExtraflameHub::is_request_echo_(std::array<uint8_t, 2> response, int request_part_num) {
  return this->request_.command[request_part_num] == response[0] &&
         this->request_.command[request_part_num + 1] == response[1];
}

#ifdef USE_EXTRAFLAME_DUMP
void ExtraflameHub::on_dump_memory_(std::string memory) {
  ESP_LOGD(TAG, "Starting to dump all config values of %s", memory.c_str());

  this->dump_address_(memory2hex(memory), 0x00);
}

void ExtraflameHub::dump_address_(uint8_t memory, uint8_t address) {
  auto request = ExtraflameRequest{
      .command = {memory, address}, .on_response = [this, memory, address](int value, bool success) {
        ESP_LOGI(TAG_DUMP, "Request: 0x%02X 0x%02X Response: 0x%02X -> %d", memory, address, (uint8_t) value, value);

        if (address != 0xFF) {
          this->dump_address_(memory, address + 1);
        }
      }};
  this->add_request(request);
}
#endif

ExtraflameComponent::ExtraflameComponent(std::string memory, uint8_t address) {
  this->memory_ = memory;
  this->address_ = address;
  this->memory_hex_ = memory2hex(memory);
}

void ExtraflameComponent::setup() { this->parent_->add_component(this); }

void ExtraflameComponent::update() { this->parent_->add_request({this->get_memory_hex(), this->get_address()}); }

void ExtraflameComponent::dump_config_internal() {
  this->dump_config_internal_();
  ESP_LOGCONFIG(TAG, "    Update Interval: %.1fs", this->get_update_interval() / 1000.0f);
  ESP_LOGCONFIG(TAG, "    Memory: %s", this->get_memory().c_str());
  ESP_LOGCONFIG(TAG, "    Address: 0x%02X", this->get_address());
}

}  // namespace extraflame
}  // namespace esphome
