cmake_minimum_required(VERSION 3.21.3)

set(SOURCES_COMMON
    ${RCL_INSTALL_DIR}/source/ti/drivers/rcl/hal/cc23x0rx/hal_cc23x0rx.c
    ${RCL_INSTALL_DIR}/source/ti/drivers/rcl/LRF.c
    ${RCL_INSTALL_DIR}/source/ti/drivers/rcl/LRFCC23X0.c
    ${RCL_INSTALL_DIR}/source/ti/drivers/rcl/RCL_Buffer.c
    ${RCL_INSTALL_DIR}/source/ti/drivers/rcl/RCL_Debug.c
    ${RCL_INSTALL_DIR}/source/ti/drivers/rcl/RCL_Lite.c
    ${RCL_INSTALL_DIR}/source/ti/drivers/rcl/RCL_Profiling.c
    ${RCL_INSTALL_DIR}/source/ti/drivers/rcl/RCL_Scheduler.c
    ${RCL_INSTALL_DIR}/source/ti/drivers/rcl/RCL_Tracer.c
    ${RCL_INSTALL_DIR}/source/ti/drivers/rcl/RCL.c
    handlers/ble5.c
    handlers/generic.c
    handlers/adc_noise.c
    wrappers/RCL_AdcNoise.c
)

set(SOURCES_CC23X0R5
    ${SOURCES_COMMON}
    handlers/ble_cs.c
)

set(SOURCES_CC23X0R2
    ${SOURCES_COMMON}
)
