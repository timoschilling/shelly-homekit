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

#include "shelly_hap_rgb.hpp"
#include "shelly_main.hpp"
#include "shelly_switch.hpp"

#include "mgos.hpp"
#include "mgos_system.hpp"

#include "mgos_hap_accessory.hpp"

namespace shelly {
namespace hap {

RGBWLight::RGBWLight(int id, Input *in, Output *out_r, Output *out_g,
                     Output *out_b, Output *out_w, struct mgos_config_lb *cfg)
    : Component(id),
      Service((SHELLY_HAP_IID_BASE_LIGHTING +
               (SHELLY_HAP_IID_STEP_LIGHTING * (id - 1))),
              &kHAPServiceType_LightBulb,
              kHAPServiceDebugDescription_LightBulb),
      in_(in),
      out_r_(out_r),
      out_g_(out_g),
      out_b_(out_b),
      out_w_(out_w),
      cfg_(cfg),
      auto_off_timer_(std::bind(&RGBWLight::AutoOffTimerCB, this)) {
}

RGBWLight::~RGBWLight() {
  if (in_ != nullptr) {
    in_->RemoveHandler(handler_id_);
  }
  SaveState();
}

Component::Type RGBWLight::type() const {
  return Type::kLightBulb;
}

std::string RGBWLight::name() const {
  return cfg_->name;
}

Status RGBWLight::Init() {
  if (!cfg_->enable) {
    LOG(LL_INFO, ("'%s' is disabled", cfg_->name));
    return Status::OK();
  }
  if (in_ != nullptr) {
    handler_id_ =
        in_->AddHandler(std::bind(&RGBWLight::InputEventHandler, this, _1, _2));
    in_->SetInvert(cfg_->in_inverted);
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
        if (in_ != nullptr &&
            cfg_->in_mode == static_cast<int>(InMode::kToggle)) {
          cfg_->state = in_->GetState();
          SetOutputState("init");
        }
        break;
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
      std::bind(&RGBWLight::HandleOnRead, this, _1, _2, _3),
      true /* supports_notification */,
      std::bind(&RGBWLight::HandleOnWrite, this, _1, _2, _3),
      kHAPCharacteristicDebugDescription_On);
  state_notify_chars_.push_back(on_char);
  AddChar(on_char);
  // Brightness
  auto *brightness_char = new mgos::hap::UInt8Characteristic(
      iid++, &kHAPCharacteristicType_Brightness, 0, 100, 1,
      std::bind(&RGBWLight::HandleBrightnessRead, this, _1, _2, _3),
      true /* supports_notification */,
      std::bind(&RGBWLight::HandleBrightnessWrite, this, _1, _2, _3),
      kHAPCharacteristicDebugDescription_Brightness);
  state_notify_chars_.push_back(brightness_char);
  AddChar(brightness_char);
  // Hue
  auto *hue_char = new mgos::hap::UInt32Characteristic(
      iid++, &kHAPCharacteristicType_Hue, 0, 360, 1,
      std::bind(&RGBWLight::HandleHueRead, this, _1, _2, _3),
      true /* supports_notification */,
      std::bind(&RGBWLight::HandleHueWrite, this, _1, _2, _3),
      kHAPCharacteristicDebugDescription_Hue);
  state_notify_chars_.push_back(hue_char);
  AddChar(hue_char);
  // Saturation
  auto *saturation_char = new mgos::hap::UInt32Characteristic(
      iid++, &kHAPCharacteristicType_Saturation, 0, 100, 1,
      std::bind(&RGBWLight::HandleSaturationRead, this, _1, _2, _3),
      true /* supports_notification */,
      std::bind(&RGBWLight::HandleSaturationWrite, this, _1, _2, _3),
      kHAPCharacteristicDebugDescription_Saturation);
  state_notify_chars_.push_back(saturation_char);
  AddChar(saturation_char);

  return Status::OK();
}

void RGBWLight::HSVtoRGBW(const HSV &hsv, RGBW &rgbw) const {
  if (hsv.s == 0.0) {
    // if saturation is zero than all rgb channels same as brightness
    rgbw.r = rgbw.g = rgbw.b = hsv.v;
  } else {
    // otherwise calc rgb from hsv
    int i = static_cast<int>(hsv.h * 6);
    float f = (hsv.h * 6.0f - i);
    float p = hsv.v * (1.0f - hsv.s);
    float q = hsv.v * (1.0f - f * hsv.s);
    float t = hsv.v * (1.0f - (1.0f - f) * hsv.s);

    switch (i % 6) {
      case 0:
        rgbw.r = hsv.v;
        rgbw.g = t;
        rgbw.b = p;
        break;

      case 1:
        rgbw.r = q;
        rgbw.g = hsv.v;
        rgbw.b = p;
        break;

      case 2:
        rgbw.r = p;
        rgbw.g = hsv.v;
        rgbw.b = t;
        break;

      case 3:
        rgbw.r = p;
        rgbw.g = q;
        rgbw.b = hsv.v;
        break;

      case 4:
        rgbw.r = t;
        rgbw.g = p;
        rgbw.b = hsv.v;
        break;

      case 5:
        rgbw.r = hsv.v;
        rgbw.g = p;
        rgbw.b = q;
        break;
    }
  }

  if (getLightMode() == LightMode::rgbw) {
    // apply white channel to rgb if activated
    rgbw.w = std::min(rgbw.r, std::min(rgbw.g, rgbw.b));
    rgbw.r = rgbw.r - rgbw.w;
    rgbw.g = rgbw.g - rgbw.w;
    rgbw.b = rgbw.b - rgbw.w;
  } else {
    // otherwise turn white channel off
    rgbw.w = 0.0f;
  }
}

void RGBWLight::SetOutputState(const char *source) {
  LOG(LL_INFO,
      ("state: %s, brightness: %i, hue: %i, saturation: %i", OnOff(cfg_->state),
       cfg_->brightness, cfg_->hue, cfg_->saturation));

  RGBW rgbw{0.0f, 0.0f, 0.0f, 0.0f};

  if (cfg_->state) {
    HSV hsv;
    hsv.h = cfg_->hue / 360.0f;
    hsv.s = cfg_->saturation / 100.0f;
    hsv.v = cfg_->brightness / 100.0f;

    HSVtoRGBW(hsv, rgbw);
  }

  out_r_->SetStatePWM(rgbw.r, source);
  out_g_->SetStatePWM(rgbw.g, source);
  out_b_->SetStatePWM(rgbw.b, source);
  out_w_->SetStatePWM(rgbw.w, source);

  if (cfg_->state && cfg_->auto_off) {
    auto_off_timer_.Reset(cfg_->auto_off_delay * 1000, 0);
  } else {
    auto_off_timer_.Clear();
  }

  for (auto *c : state_notify_chars_) {
    c->RaiseEvent();
  }
}

StatusOr<std::string> RGBWLight::GetInfo() const {
  const_cast<RGBWLight *>(this)->SaveState();
  return mgos::SPrintf("sta: %s, b: %i, h: %i, sa: %i", OnOff(cfg_->state),
                       cfg_->brightness, cfg_->hue, cfg_->saturation);
}

StatusOr<std::string> RGBWLight::GetInfoJSON() const {
  return mgos::JSONPrintStringf(
      "{id: %d, type: %d, name: %Q, state: %B, "
      " brightness: %d, hue: %d, saturation: %d, "
      " in_inverted: %B, initial: %d, in_mode: %d, "
      "auto_off: %B, auto_off_delay: %.3f}",
      id(), type(), cfg_->name, cfg_->state, cfg_->brightness, cfg_->hue,
      cfg_->saturation, cfg_->in_inverted, cfg_->initial_state, cfg_->in_mode,
      cfg_->auto_off, cfg_->auto_off_delay);
}

Status RGBWLight::SetConfig(const std::string &config_json,
                            bool *restart_required) {
  struct mgos_config_lb cfg = *cfg_;
  int8_t in_inverted = -1;
  cfg.name = nullptr;
  cfg.in_mode = -2;
  json_scanf(config_json.c_str(), config_json.size(),
             "{name: %Q, in_mode: %d, in_inverted: %B, "
             "initial_state: %d, "
             "auto_off: %B, auto_off_delay: %lf}",
             &cfg.name, &cfg.in_mode, &in_inverted, &cfg.initial_state,
             &cfg.auto_off, &cfg.auto_off_delay);

  mgos::ScopedCPtr name_owner((void *) cfg.name);
  // Validation.
  if (cfg.name != nullptr && strlen(cfg.name) > 64) {
    return mgos::Errorf(STATUS_INVALID_ARGUMENT, "invalid %s",
                        "name (too long, max 64)");
  }
  if (cfg.in_mode != -2 &&
      (cfg.in_mode < 0 || cfg.in_mode >= (int) InMode::kMax)) {
    return mgos::Errorf(STATUS_INVALID_ARGUMENT, "invalid %s", "in_mode");
  }
  if (cfg.initial_state < 0 || cfg.initial_state >= (int) InitialState::kMax ||
      (cfg_->in_mode == -1 &&
       cfg.initial_state == (int) InitialState::kInput)) {
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
  if (cfg.in_mode != -2 && cfg_->in_mode != cfg.in_mode) {
    if (cfg_->in_mode == (int) InMode::kDetached ||
        cfg.in_mode == (int) InMode::kDetached) {
      *restart_required = true;
    }
    cfg_->in_mode = cfg.in_mode;
  }
  if (in_inverted != -1 && cfg_->in_inverted != in_inverted) {
    cfg_->in_inverted = in_inverted;
    *restart_required = true;
  }
  cfg_->initial_state = cfg.initial_state;
  cfg_->auto_off = cfg.auto_off;
  cfg_->auto_off_delay = cfg.auto_off_delay;
  return Status::OK();
}

void RGBWLight::SaveState() {
  if (!dirty_) return;
  mgos_sys_config_save(&mgos_sys_config, false /* try_once */, NULL /* msg */);
  dirty_ = false;
}

Status RGBWLight::SetState(const std::string &state_json) {
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

RGBWLight::LightMode RGBWLight::getLightMode() const {
  switch (mgos_sys_config_get_shelly_mode()) {
    case 4:
      return LightMode::rgbw;
    case 3:
    default:
      return LightMode::rgb;
  }
}

void RGBWLight::AutoOffTimerCB() {
  // Don't set state if auto off has been disabled during timer run
  if (!cfg_->auto_off) return;
  if (static_cast<InMode>(cfg_->in_mode) == InMode::kActivation &&
      in_ != nullptr && in_->GetState() && cfg_->state) {
    // Input is active, re-arm.
    LOG(LL_INFO, ("Input is active, re-arming auto off timer"));
    auto_off_timer_.Reset(cfg_->auto_off_delay * 1000, 0);
    return;
  }
  cfg_->state = false;
  SetOutputState("auto_off");
}

void RGBWLight::InputEventHandler(Input::Event ev, bool state) {
  InMode in_mode = static_cast<InMode>(cfg_->in_mode);
  if (in_mode == InMode::kDetached) {
    // Nothing to do
    return;
  }
  switch (ev) {
    case Input::Event::kChange: {
      switch (static_cast<InMode>(cfg_->in_mode)) {
        case InMode::kMomentary:
          if (state) {  // Only on 0 -> 1 transitions.
            cfg_->state = !cfg_->state;
            SetOutputState("ext_mom");
          }
          break;
        case InMode::kToggle:
          cfg_->state = state;
          SetOutputState("switch");
          break;
        case InMode::kEdge:
          cfg_->state = !cfg_->state;
          SetOutputState("ext_edge");
          break;
        case InMode::kActivation:
          if (state) {
            cfg_->state = true;
            SetOutputState("ext_act");
          } else if (cfg_->state && cfg_->auto_off) {
            // On 1 -> 0 transitions do not turn on output
            // but re-arm auto off timer if running.
            auto_off_timer_.Reset(cfg_->auto_off_delay * 1000, 0);
          }
          break;
        case InMode::kAbsent:
        case InMode::kDetached:
        case InMode::kMax:
          break;
      }
      break;
    }
    case Input::Event::kLong:
      // Disable auto-off if it was active.
      if (in_mode == InMode::kMomentary) {
        auto_off_timer_.Clear();
      }
      break;
    case Input::Event::kSingle:
    case Input::Event::kDouble:
    case Input::Event::kReset:
    case Input::Event::kMax:
      break;
  }
}

HAPError RGBWLight::HandleOnRead(
    HAPAccessoryServerRef *server,
    const HAPBoolCharacteristicReadRequest *request, bool *value) {
  *value = cfg_->state;
  (void) server;
  (void) request;
  return kHAPError_None;
}

HAPError RGBWLight::HandleOnWrite(
    HAPAccessoryServerRef *server,
    const HAPBoolCharacteristicWriteRequest *request, bool value) {
  LOG(LL_INFO, ("State %d: %s", id(), OnOff(value)));
  cfg_->state = value;
  dirty_ = true;
  SetOutputState("HAP");
  state_notify_chars_[0]->RaiseEvent();
  (void) server;
  (void) request;
  return kHAPError_None;
}

HAPError RGBWLight::HandleBrightnessRead(
    HAPAccessoryServerRef *server,
    const HAPUInt8CharacteristicReadRequest *request, uint8_t *value) {
  LOG(LL_INFO, ("Brightness read %d: %d", id(), cfg_->brightness));
  *value = (uint8_t) cfg_->brightness;
  (void) server;
  (void) request;
  return kHAPError_None;
}

HAPError RGBWLight::HandleBrightnessWrite(
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

HAPError RGBWLight::HandleHueRead(
    HAPAccessoryServerRef *server,
    const HAPUInt32CharacteristicReadRequest *request, uint32_t *value) {
  LOG(LL_INFO, ("HandleHueRead"));
  *value = static_cast<uint32_t>(cfg_->hue);
  (void) server;
  (void) request;
  return kHAPError_None;
}

HAPError RGBWLight::HandleHueWrite(
    HAPAccessoryServerRef *server,
    const HAPUInt32CharacteristicWriteRequest *request, uint32_t value) {
  LOG(LL_INFO, ("Hue %d: %d", id(), static_cast<int>(value)));
  if (cfg_->hue != static_cast<int>(value)) {
    cfg_->hue = static_cast<int>(value);
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

HAPError RGBWLight::HandleSaturationRead(
    HAPAccessoryServerRef *server,
    const HAPUInt32CharacteristicReadRequest *request, uint32_t *value) {
  LOG(LL_INFO, ("HandleSaturationRead"));
  *value = static_cast<float>(cfg_->saturation);
  (void) server;
  (void) request;
  return kHAPError_None;
}

HAPError RGBWLight::HandleSaturationWrite(
    HAPAccessoryServerRef *server,
    const HAPUInt32CharacteristicWriteRequest *request, uint32_t value) {
  LOG(LL_INFO, ("Saturation %d: %d", id(), static_cast<int>(value)));
  if (cfg_->saturation != static_cast<int>(value)) {
    cfg_->saturation = static_cast<int>(value);
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