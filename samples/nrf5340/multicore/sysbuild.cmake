#
# Copyright (c) 2023 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

# Add cpunet image
ExternalZephyrProject_Add(
    APPLICATION multicore_net
    SOURCE_DIR ${APP_DIR}/cpunet
    BOARD ${SB_CONFIG_MULTICORE_REMOTE_BOARD}
  )

# Add a dependency so that the remote sample will be built and flashed first
add_dependencies(multicore multicore_net)
# Place remote image first in the image list
set(IMAGES "multicore_net" ${IMAGES})
