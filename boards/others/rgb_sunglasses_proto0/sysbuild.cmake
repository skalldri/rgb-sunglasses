#
# Copyright (c) 2024 Stuart Alldritt
#
# SPDX-License-Identifier: Apache-2.0
#

# Apply DTS overlay that enables the net_core_sim_flash driver on the app core.
# This provides MCUMgr with a virtual flash device for the net core image slot.
add_overlay_dts(rgb-sunglasses ${APP_DIR}/overlays/net_core_sim_flash.overlay)
