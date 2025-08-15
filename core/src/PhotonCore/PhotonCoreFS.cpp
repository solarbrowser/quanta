/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "../../include/PhotonCore/PhotonCoreFS.h"

namespace Quanta {

bool PhotonCoreFS::fast_mode_active_ = false;
bool PhotonCoreFS::skip_validation_ = false;
bool PhotonCoreFS::minimal_init_ = false;
uint32_t PhotonCoreFS::startup_count_ = 0;
uint32_t PhotonCoreFS::boot_optimizations_ = 0;

} // namespace Quanta