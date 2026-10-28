#
#  Generate the input files for the passthrough tests in directory 'dir'
#  using the osmium binary 'osmium'.
#
#  passthrough-input.osm.pbf has more objects than fit into one PBF block
#  (8000), so there are several blocks per type:
#
#  block 1: nodes -2 to -200 and 2 to 15800
#  block 2: nodes 15802 to 31800
#  block 3: nodes 31802 to 40000
#  block 4: ways 2 to 16000
#  block 5: ways 16002 to 20000
#  block 6: relations 2 to 16000 (version 3)
#  block 7: relations 16002 to 20000 (version 3)
#
#  All IDs are even, so there are gaps for new objects.
#
#  passthrough-duplicate.osm.pbf has two blocks: nodes 1 to 8000 and nodes
#  8000 (again) to 9000.
#
#  passthrough-signed-order.osm.pbf has two blocks: nodes -16000 to -2 in
#  that order (which is not the order used by libosmium) and nodes 2 to 16000.
#
#  passthrough-strings.osm.pbf has blocks with nodes 2 to 16000, 16002 to
#  32000, 32002 to 40000, ways 2 to 16000 and 16002 to 20000. All objects
#  have a different name tag, so the string tables are large.
#

# Append objects with IDs from _first to _last (step _step) to the OPL file
# _name. _line is the OPL line with ID as placeholder for the ID. Lines are
# written in chunks, because appending to a long string is slow in CMake.
function(append_objects _name _first _last _step _line)
    set(_chunk "")
    foreach(_id RANGE ${_first} ${_last} ${_step})
        string(REPLACE "ID" "${_id}" _object "${_line}")
        string(APPEND _chunk "${_object}\n")
        string(LENGTH "${_chunk}" _length)
        if(_length GREATER 100000)
            file(APPEND ${dir}/${_name}.opl "${_chunk}")
            set(_chunk "")
        endif()
    endforeach()
    file(APPEND ${dir}/${_name}.opl "${_chunk}")
endfunction()

function(convert_to_pbf _name)
    execute_process(
        COMMAND ${osmium} cat ${dir}/${_name}.opl -o ${dir}/${_name}.osm.pbf -O
        RESULT_VARIABLE _result
    )
    if(_result)
        message(FATAL_ERROR "Error when creating ${_name}.osm.pbf")
    endif()
endfunction()

set(_meta "c1 t2020-01-01T00:00:00Z i1 utest")

file(WRITE ${dir}/passthrough-input.opl "")
append_objects(passthrough-input 2 200 2 "n-ID v1 dV ${_meta} T x1 y1")
append_objects(passthrough-input 2 40000 2 "nID v1 dV ${_meta} T x1 y1")
append_objects(passthrough-input 2 20000 2 "wID v1 dV ${_meta} T Nn2,n4")
append_objects(passthrough-input 2 20000 2 "rID v3 dV ${_meta} T Mn2@,w2@role")
convert_to_pbf(passthrough-input)

file(WRITE ${dir}/passthrough-duplicate.opl "")
append_objects(passthrough-duplicate 1 8000 1 "nID v1 dV ${_meta} T x1 y1")
append_objects(passthrough-duplicate 8000 9000 1 "nID v2 dV ${_meta} T x2 y2")
convert_to_pbf(passthrough-duplicate)

file(WRITE ${dir}/passthrough-signed-order.opl "")
append_objects(passthrough-signed-order -16000 -2 2 "nID v1 dV ${_meta} T x1 y1")
append_objects(passthrough-signed-order 2 16000 2 "nID v1 dV ${_meta} T x1 y1")
convert_to_pbf(passthrough-signed-order)

file(WRITE ${dir}/passthrough-strings.opl "")
append_objects(passthrough-strings 2 40000 2 "nID v1 dV ${_meta} Tname=nodeID x1 y1")
append_objects(passthrough-strings 2 20000 2 "wID v1 dV ${_meta} Tname=wayID Nn2,n4")
convert_to_pbf(passthrough-strings)
