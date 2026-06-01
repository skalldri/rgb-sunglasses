#
# Copyright (c) 2024 Stuart Alldritt
#
# SPDX-License-Identifier: Apache-2.0
#

# Apply DTS overlay that enables the net_core_sim_flash driver on the app core.
# This provides MCUMgr with a virtual flash device for the net core image slot.
# ${DEFAULT_IMAGE} is the sysbuild main-application image name, derived from the
# application directory basename (e.g. "fw"). Use it rather than a hardcoded name
# so this overlay keeps applying if the firmware directory is ever renamed.
add_overlay_dts(${DEFAULT_IMAGE} ${APP_DIR}/overlays/net_core_sim_flash.overlay)
