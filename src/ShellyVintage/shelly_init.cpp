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

#include "shelly_hap_input.hpp"
#include "shelly_hap_lightbulb.hpp"
#include "shelly_input_pin.hpp"
#include "shelly_main.hpp"

namespace shelly {

void CreatePeripherals(std::vector<std::unique_ptr<Input>> *inputs,
                       std::vector<std::unique_ptr<Output>> *outputs,
                       std::vector<std::unique_ptr<PowerMeter>> *pms,
                       std::unique_ptr<TempSensor> *sys_temp) {
  outputs->emplace_back(new OutputPin(1, 4, 1));
  (void) inputs;
  (void) sys_temp;
  (void) pms;
}

void CreateComponents(std::vector<std::unique_ptr<Component>> *comps,
                      std::vector<std::unique_ptr<mgos::hap::Accessory>> *accs,
                      HAPAccessoryServerRef *svr) {
  auto *rgb_cfg = (struct mgos_config_lb *) mgos_sys_config_get_lb1();
  std::unique_ptr<hap::LightBulb> lb(new hap::LightBulb(1, FindOutput(1), rgb_cfg));
  if (lb == nullptr || !lb->Init().ok()) {
    return;
  }
  lb->set_primary(true);
  mgos::hap::Accessory *pri_acc = (*accs)[0].get();
  pri_acc->SetCategory(kHAPAccessoryCategory_Lighting);
  pri_acc->AddService(lb.get());
  comps->emplace_back(std::move(lb));
}

}  // namespace shelly
