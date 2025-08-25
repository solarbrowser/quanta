/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "../../include/PhotonCore/PhotonCoreSonic.h"

namespace Quanta {

//  PHOTON CORE SONIC static member definitions
alignas(64) char PhotonCoreSonic::sonic_memory_pool_[PhotonCoreSonic::SONIC_POOL_SIZE];
size_t PhotonCoreSonic::sonic_pool_offset_ = 0;

} // namespace Quanta