cmake_minimum_required(VERSION 3.21.3)

set(SOURCES_COMMON
    hal/cc23x0rx/hal_cc23x0rx.c
    LRF.c
    LRFCC23X0.c
    RCL_Buffer.c
    RCL_Debug.c
    RCL_Profiling.c
    RCL_Scheduler.c
    RCL_Tracer.c
    RCL.c
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
