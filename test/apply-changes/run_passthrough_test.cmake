#
#  Run "osmium apply-changes" with and without --no-passthrough and check
#  that the results contain the same objects.
#
#  Variables:
#  osmium     - osmium binary
#  dir        - directory with the change files, also used as working dir
#  tmpdir     - directory for the output files (will be recreated)
#  input      - input file
#  changes    - change file(s), separated by spaces
#  options    - further options for apply-changes, separated by spaces
#  suffix     - suffix of output files (default: osm.pbf)
#  expect     - regular expression the verbose output must match
#  same_bytes - if set, the output files must be byte-identical
#  stdin      - if set, file used as STDIN of osmium
#

if(NOT suffix)
    set(suffix "osm.pbf")
endif()

separate_arguments(changes)
separate_arguments(options)

file(REMOVE_RECURSE ${tmpdir})
file(MAKE_DIRECTORY ${tmpdir})

if(stdin)
    set(_input_file INPUT_FILE ${stdin})
endif()

function(run_osmium)
    execute_process(
        COMMAND ${osmium} ${ARGV}
        WORKING_DIRECTORY ${dir}
        ${_input_file}
        RESULT_VARIABLE _result
        ERROR_VARIABLE _stderr
    )
    if(_result)
        message(FATAL_ERROR "Error when calling 'osmium ${ARGV}': ${_stderr}")
    endif()
    set(stderr "${_stderr}" PARENT_SCOPE)
endfunction()

run_osmium(apply-changes --verbose ${input} ${changes} ${options} -o ${tmpdir}/passthrough.${suffix})
if(NOT stderr MATCHES "${expect}")
    message(SEND_ERROR "Verbose output does not match '${expect}':\n${stderr}")
endif()

list(FIND options --no-passthrough _index)
if(_index EQUAL -1)
    list(APPEND options --no-passthrough)
endif()
run_osmium(apply-changes ${input} ${changes} ${options} -o ${tmpdir}/classic.${suffix})

foreach(_name passthrough classic)
    run_osmium(cat ${tmpdir}/${_name}.${suffix} -o ${tmpdir}/${_name}.opl)
endforeach()

execute_process(
    COMMAND ${CMAKE_COMMAND} -E compare_files ${tmpdir}/passthrough.opl ${tmpdir}/classic.opl
    RESULT_VARIABLE _result
)
if(_result)
    message(SEND_ERROR "Objects in passthrough.opl and classic.opl in '${tmpdir}' differ")
endif()

if(same_bytes)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E compare_files ${tmpdir}/passthrough.${suffix} ${tmpdir}/classic.${suffix}
        RESULT_VARIABLE _result
    )
    if(_result)
        message(SEND_ERROR "Output files in '${tmpdir}' are not byte-identical")
    endif()
endif()

