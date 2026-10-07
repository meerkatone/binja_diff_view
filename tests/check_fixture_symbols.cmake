if(NOT DEFINED SYMBOLIZED OR NOT DEFINED STRIPPED OR NOT DEFINED NM_TOOL)
	message(FATAL_ERROR "SYMBOLIZED, STRIPPED, and NM_TOOL are required")
endif()

execute_process(
	COMMAND "${NM_TOOL}" -nm "${SYMBOLIZED}"
	RESULT_VARIABLE symbolized_status
	OUTPUT_VARIABLE symbolized_symbols
	ERROR_VARIABLE symbolized_error)
if(NOT symbolized_status EQUAL 0)
	message(FATAL_ERROR "nm failed for symbolized fixture: ${symbolized_error}")
endif()
foreach(fixture_function copy_record symbol_port_fixture)
	if(NOT symbolized_symbols MATCHES "${fixture_function}")
		message(FATAL_ERROR "symbolized fixture does not contain ${fixture_function}")
	endif()
endforeach()

execute_process(
	COMMAND "${NM_TOOL}" -nm "${STRIPPED}"
	RESULT_VARIABLE stripped_status
	OUTPUT_VARIABLE stripped_symbols
	ERROR_VARIABLE stripped_error)
if(NOT stripped_status EQUAL 0)
	message(FATAL_ERROR "nm failed for stripped fixture: ${stripped_error}")
endif()
foreach(fixture_function copy_record symbol_port_fixture)
	if(stripped_symbols MATCHES "${fixture_function}")
		message(FATAL_ERROR "stripped fixture still contains ${fixture_function}")
	endif()
endforeach()

message(STATUS "fixture symbol boundary verified: copy_record and symbol_port_fixture present only in reference")
