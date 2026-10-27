/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include "image-clean-pool.h"
#include "machine-forward.h"

int image_clean_pool_operation(Manager *manager, ImageCleanPoolMode mode, Operation **ret_operation);
