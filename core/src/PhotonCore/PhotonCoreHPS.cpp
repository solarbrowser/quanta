/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "../../include/PhotonCore/PhotonCoreHPS.h"

namespace Quanta {

uint32_t PhotonCoreHPS::ram_gb_ = 0;
uint32_t PhotonCoreHPS::gpu_cores_ = 0;
uint32_t PhotonCoreHPS::cpu_cores_ = 0;
uint64_t PhotonCoreHPS::memory_pool_size_ = 0;
uint32_t PhotonCoreHPS::parallel_threads_ = 0;
uint32_t PhotonCoreHPS::worker_threads_ = 0;

bool PhotonCoreHPS::system_detected_ = false;
bool PhotonCoreHPS::high_performance_active_ = false;
bool PhotonCoreHPS::use_all_ram_ = false;
bool PhotonCoreHPS::use_all_cores_ = false;
bool PhotonCoreHPS::use_gpu_acceleration_ = false;
bool PhotonCoreHPS::memory_optimized_ = false;
bool PhotonCoreHPS::gpu_acceleration_active_ = false;
bool PhotonCoreHPS::cpu_optimization_active_ = false;
bool PhotonCoreHPS::speed_optimized_ = false;

} // namespace Quanta