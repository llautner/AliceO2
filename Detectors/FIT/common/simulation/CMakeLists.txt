set(MODULE_NAME "FITSimulation")

O2_SETUP(NAME ${MODULE_NAME})

set(SRCS
  src/Digitizer.cxx
    )

set(HEADERS
    include/${MODULE_NAME}/Digitizer.h
    include/${MODULE_NAME}/DigitizationParameters.h
    )


Set(LINKDEF src/FITSimulationLinkDef.h)
Set(LIBRARY_NAME ${MODULE_NAME})
set(BUCKET_NAME fit_simulation_bucket)

O2_GENERATE_LIBRARY()


