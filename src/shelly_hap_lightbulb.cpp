/*
 * Copyright (c) Shelly-HomeKit Contributors
 * All rights reserved
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "shelly_hap_lightbulb.hpp"
#include "shelly_main.hpp"
#include "shelly_switch.hpp"

#include "mgos.hpp"
#include "mgos_system.hpp"

#include "mgos_hap_accessory.hpp"

namespace shelly {
namespace hap {

LightBulb::LightBulb(int id, Output *out, struct mgos_config_lb *cfg)
    : Component(id),
      Service((SHELLY_HAP_IID_BASE_LIGHTING +
               (SHELLY_HAP_IID_STEP_LIGHTING * (id - 1))),
              &kHAPServiceType_LightBulb,
              kHAPServiceDebugDescription_LightBulb),
      out_(out),
      cfg_(cfg),
      auto_off_timer_(std::bind(&LightBulb::AutoOffTimerCB, this)) {
}

LightBulb::~LightBulb() {
  SaveState();
}

Component::Type LightBulb::type() const {
  return Type::kLightBulb;
}

std::string LightBulb::name() const {
  return cfg_->name;
}

Status LightBulb::Init() {
  if (!cfg_->enable) {
    LOG(LL_INFO, ("'%s' is disabled", cfg_->name));
    return Status::OK();
  }
  bool should_restore = (cfg_->initial_state == (int) InitialState::kLast);
  if (IsSoftReboot()) should_restore = true;
  if (should_restore) {
    SetOutputState("init");
  } else {
    switch (static_cast<InitialState>(cfg_->initial_state)) {
      case InitialState::kOff:
        cfg_->state = false;
        SetOutputState("init");
        break;
      case InitialState::kOn:
        cfg_->state = true;
        SetOutputState("init");
        break;
      case InitialState::kInput:
      case InitialState::kLast:
      case InitialState::kMax:
        break;
    }
  }

  uint16_t iid = svc_.iid + 1;

  // Name
  AddNameChar(iid++, cfg_->name);
  // On
  auto *on_char = new mgos::hap::BoolCharacteristic(
      iid++, &kHAPCharacteristicType_On,
      std::bind(&LightBulb::HandleOnRead, this, _1, _2, _3),
      true /* supports_notification */,
      std::bind(&LightBulb::HandleOnWrite, this, _1, _2, _3),
      kHAPCharacteristicDebugDescription_On);
  state_notify_chars_.push_back(on_char);
  AddChar(on_char);
  // Brightness
  auto *brightness_char = new mgos::hap::UInt8Characteristic(
      iid++, &kHAPCharacteristicType_Brightness, 0, 100, 1,
      std::bind(&LightBulb::HandleBrightnessRead, this, _1, _2, _3),
      true /* supports_notification */,
      std::bind(&LightBulb::HandleBrightnessWrite, this, _1, _2, _3),
      kHAPCharacteristicDebugDescription_Brightness);
  state_notify_chars_.push_back(brightness_char);
  AddChar(brightness_char);
  // // Hue
  // auto *hue_char = new mgos::hap::UInt32Characteristic(
  //     iid++, &kHAPCharacteristicType_Hue, 0, 360, 1,
  //     std::bind(&LightBulb::HandleHueRead, this, _1, _2, _3),
  //     true /* supports_notification */,
  //     std::bind(&LightBulb::HandleHueWrite, this, _1, _2, _3),
  //     kHAPCharacteristicDebugDescription_Hue);
  // state_notify_chars_.push_back(hue_char);
  // AddChar(hue_char);
  // // Saturation
  // auto *saturation_char = new mgos::hap::UInt32Characteristic(
  //     iid++, &kHAPCharacteristicType_Saturation, 0, 100, 1,
  //     std::bind(&LightBulb::HandleSaturationRead, this, _1, _2, _3),
  //     true /* supports_notification */,
  //     std::bind(&LightBulb::HandleSaturationWrite, this, _1, _2, _3),
  //     kHAPCharacteristicDebugDescription_Saturation);
  // state_notify_chars_.push_back(saturation_char);
  // AddChar(saturation_char);

  return Status::OK();
}

void LightBulb::SetOutputState(const char *source) {
  LOG(LL_INFO,
      ("state: %s, brightness: %i, hue: %i, saturation: %i",
       OnOff(cfg_->state), cfg_->brightness, cfg_->hue, cfg_->saturation));

  float brv = cfg_->state ? (cfg_->brightness / 100.0) : 0;

  out_->SetStatePWM(brv, source);

  if (cfg_->state && cfg_->auto_off) {
    auto_off_timer_.Reset(cfg_->auto_off_delay * 1000, 0);
  } else {
    auto_off_timer_.Clear();
  }

  for (auto *c : state_notify_chars_) {
    c->RaiseEvent();
  }
}

StatusOr<std::string> LightBulb::GetInfo() const {
  const_cast<LightBulb *>(this)->SaveState();
  return mgos::SPrintf("sta: %s, b: %i, h: %i, sa: %i 11111", OnOff(cfg_->state),
                       cfg_->brightness, cfg_->hue, cfg_->saturation);
}

StatusOr<std::string> LightBulb::GetInfoJSON() const {
  return mgos::JSONPrintStringf(
      "{id: %d, type: %d, name: %Q, state: %B, "
      " brightness: %d, hue: %d, saturation: %d, "
      " initial: %d, "
      "auto_off: %B, auto_off_delay: %.3f}",
      id(), type(), cfg_->name, cfg_->state, cfg_->brightness, cfg_->hue,
      cfg_->saturation, cfg_->initial_state,
      cfg_->auto_off, cfg_->auto_off_delay);
}

Status LightBulb::SetConfig(const std::string &config_json, bool *restart_required) {
  struct mgos_config_lb cfg = *cfg_;
  cfg.name = nullptr;
  json_scanf(config_json.c_str(), config_json.size(),
             "{name: %Q, "
             "initial_state: %d, "
             "auto_off: %B, auto_off_delay: %lf}",
             &cfg.name, &cfg.initial_state,
             &cfg.auto_off, &cfg.auto_off_delay);
  mgos::ScopedCPtr name_owner((void *) cfg.name);
  // Validation.
  if (cfg.name != nullptr && strlen(cfg.name) > 64) {
    return mgos::Errorf(STATUS_INVALID_ARGUMENT, "invalid %s",
                        "name (too long, max 64)");
  }
  if (cfg.initial_state < 0 || cfg.initial_state > (int) InitialState::kLast) {
    return mgos::Errorf(STATUS_INVALID_ARGUMENT, "invalid %s", "initial_state");
  }
  cfg.auto_off = (cfg.auto_off != 0);
  if (cfg.initial_state < 0 || cfg.initial_state > (int) InitialState::kMax) {
    return mgos::Errorf(STATUS_INVALID_ARGUMENT, "invalid %s", "initial_state");
  }
  // Now copy over.
  if (cfg_->name != nullptr && strcmp(cfg_->name, cfg.name) != 0) {
    mgos_conf_set_str(&cfg_->name, cfg.name);
    *restart_required = true;
  }
  cfg_->initial_state = cfg.initial_state;
  cfg_->auto_off = cfg.auto_off;
  cfg_->auto_off_delay = cfg.auto_off_delay;
  return Status::OK();
}

void LightBulb::SaveState() {
  if (!dirty_) return;
  mgos_sys_config_save(&mgos_sys_config, false /* try_once */, NULL /* msg */);
  dirty_ = false;
}

Status LightBulb::SetState(const std::string &state_json) {
  bool state;
  int brightness, hue, saturation;
  json_scanf(state_json.c_str(), state_json.size(),
             "{state: %B, brightness: %d, hue: %d, saturation: %d}", &state,
             &brightness, &hue, &saturation);

  if (cfg_->state != state) {
    cfg_->state = state;
    dirty_ = true;
  }
  if (cfg_->brightness != brightness) {
    cfg_->brightness = brightness;
    dirty_ = true;
  }
  if (cfg_->hue != hue) {
    cfg_->hue = hue;
    dirty_ = true;
  }
  if (cfg_->saturation != saturation) {
    cfg_->saturation = saturation;
    dirty_ = true;
  }

  if (dirty_) {
    SetOutputState("RPC");
  }

  return Status::OK();
}

void LightBulb::AutoOffTimerCB() {
  // Don't set state if auto off has been disabled during timer run
  if (!cfg_->auto_off) return;
  cfg_->state = false;
  SetOutputState("auto_off");
}

HAPError LightBulb::HandleOnRead(HAPAccessoryServerRef *server,
                           const HAPBoolCharacteristicReadRequest *request,
                           bool *value) {
  *value = cfg_->state;
  (void) server;
  (void) request;
  return kHAPError_None;
}

HAPError LightBulb::HandleOnWrite(HAPAccessoryServerRef *server,
                            const HAPBoolCharacteristicWriteRequest *request,
                            bool value) {
  LOG(LL_INFO, ("State %d: %s", id(), OnOff(value)));
  cfg_->state = value;
  dirty_ = true;
  SetOutputState("HAP");
  state_notify_chars_[0]->RaiseEvent();
  (void) server;
  (void) request;
  return kHAPError_None;
}

HAPError LightBulb::HandleBrightnessRead(
    HAPAccessoryServerRef *server,
    const HAPUInt8CharacteristicReadRequest *request, uint8_t *value) {
  LOG(LL_INFO, ("Brightness read %d: %d", id(), cfg_->brightness));
  *value = (uint8_t) cfg_->brightness;
  (void) server;
  (void) request;
  return kHAPError_None;
}

HAPError LightBulb::HandleBrightnessWrite(
    HAPAccessoryServerRef *server,
    const HAPUInt8CharacteristicWriteRequest *request, uint8_t value) {
  LOG(LL_INFO, ("Brightness %d: %d", id(), value));
  cfg_->brightness = value;
  dirty_ = true;
  state_notify_chars_[1]->RaiseEvent();
  SetOutputState("HAP");
  (void) server;
  (void) request;
  return kHAPError_None;
}

HAPError LightBulb::HandleHueRead(HAPAccessoryServerRef *server,
                            const HAPUInt32CharacteristicReadRequest *request,
                            uint32_t *value) {
  LOG(LL_INFO, ("HandleHueRead"));
  *value = cfg_->hue;
  (void) server;
  (void) request;
  return kHAPError_None;
}

HAPError LightBulb::HandleHueWrite(HAPAccessoryServerRef *server,
                             const HAPUInt32CharacteristicWriteRequest *request,
                             uint32_t value) {
  LOG(LL_INFO, ("Hue %d: %i", id(), (int) value));
  if (cfg_->hue != (int) value) {
    cfg_->hue = value;
    dirty_ = true;
    state_notify_chars_[2]->RaiseEvent();
    SetOutputState("HAP");
  } else {
    LOG(LL_INFO, ("no Hue update"));
  }
  (void) server;
  (void) request;
  return kHAPError_None;
}

HAPError LightBulb::HandleSaturationRead(
    HAPAccessoryServerRef *server,
    const HAPUInt32CharacteristicReadRequest *request, uint32_t *value) {
  LOG(LL_INFO, ("HandleSaturationRead"));
  *value = cfg_->saturation;
  (void) server;
  (void) request;
  return kHAPError_None;
}

HAPError LightBulb::HandleSaturationWrite(
    HAPAccessoryServerRef *server,
    const HAPUInt32CharacteristicWriteRequest *request, uint32_t value) {
  LOG(LL_INFO, ("Saturation %d: %i", id(), (int) value));
  if (cfg_->saturation != (int) value) {
    cfg_->saturation = value;
    dirty_ = true;
    state_notify_chars_[3]->RaiseEvent();
    SetOutputState("HAP");
  } else {
    LOG(LL_INFO, ("no Saturation update"));
  }
  (void) server;
  (void) request;
  return kHAPError_None;
}

}  // namespace hap
}  // namespace shelly
