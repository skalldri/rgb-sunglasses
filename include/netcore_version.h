/*
 * Copyright (c) 2024 Stuart Alldritt
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <netcore_version_protocol.h>

/**
 * @brief Retrieve the cached net core version info.
 *
 * Populated asynchronously when the IPC endpoint binds and the net core
 * responds. Callers that need the data before it arrives should retry.
 *
 * @param[out] out  Buffer to copy the cached response into.
 *
 * @retval 0        Data was available and copied to @p out.
 * @retval -EAGAIN  Response not yet received from the net core.
 */
int netcore_version_get(struct netcore_version_rsp *out);
