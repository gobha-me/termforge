file(GLOB DEPS ${CMAKE_CURRENT_LIST_DIR}/deps/*.cmake)

foreach(DEP ${DEPS})
  message(STATUS "Including dependency ${DEP}")
  include(${DEP})
endforeach()
