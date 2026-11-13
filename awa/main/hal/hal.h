/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

/*
 * BACKWARD COMPATIBILITY LAYER
 * This file now redirects to the new modular architecture (HalFacade)
 * All existing code using GetHAL() will continue to work without modification
 */

#include "hal_facade.h"

/* Type alias for backward compatibility - Hal is now an alias for HalFacade */
using Hal = HalFacade;

/* GetHAL() is already defined in hal_facade.h, no need to redefine here */
