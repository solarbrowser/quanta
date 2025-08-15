/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "../../include/PhotonCore/PhotonCoreTurbo.h"

namespace Quanta {

// 💥 PHOTON CORE TURBO static members
uint32_t PhotonCoreTurbo::turbo_level_ = 0;
uint32_t PhotonCoreTurbo::nitro_injections_ = 0;
bool PhotonCoreTurbo::overdrive_active_ = false;

} // namespace Quanta