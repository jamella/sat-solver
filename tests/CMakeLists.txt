ENABLE_TESTING()
# make check framework headers available
# check.h is generated in build dir

MACRO(ADD_TESTS)
  FOREACH( loop_var ${ARGV} )
    #SET_SOURCE_FILES_PROPERTIES( ${loop_var}_test.c "" )
    ADD_EXECUTABLE( ${loop_var}_test ${loop_var}_test.c )
    TARGET_LINK_LIBRARIES( ${loop_var}_test ${CHECK_LIBRARIES})
    #MESSAGE( INFO "Adding test ${loop_var}_test ${CMAKE_CURRENT_BINARY_DIR}/${loop_var}_test")
    ADD_TEST( "${loop_var}_test" "${CMAKE_CURRENT_BINARY_DIR}/${loop_var}_test" )
  ENDFOREACH( loop_var )
ENDMACRO(ADD_TESTS)

ADD_SUBDIRECTORY(core)
ADD_SUBDIRECTORY(solver)
# ADD_SUBDIRECTORY(tools)

