/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "../../include/PhotonCore/PhotonCoreVelocity.h"

namespace Quanta {

// 💫 PHOTON CORE VELOCITY static member definitions
uint32_t PhotonCoreVelocity::warp_factor_ = 1;
uint32_t PhotonCoreVelocity::stellar_boost_count_ = 0;
bool PhotonCoreVelocity::stellar_initialized_ = false;

} // namespace Quanta