/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "../../include/PhotonCore/PhotonCorePerformance.h"

namespace Quanta {

// 💫 PHOTON CORE PERFORMANCE static member definitions
uint64_t PhotonCorePerformance::diamond_start_time_ = 0;
const char* PhotonCorePerformance::diamond_operation_name_ = nullptr;
uint64_t PhotonCorePerformance::diamond_total_operations_ = 0;
uint64_t PhotonCorePerformance::diamond_total_time_ = 0;
uint64_t PhotonCorePerformance::diamond_flash_counter_ = 0;

} // namespace Quanta