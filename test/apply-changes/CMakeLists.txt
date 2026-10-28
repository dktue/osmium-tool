#-----------------------------------------------------------------------------
#
#  CMake Config
#
#  Osmium Tool Tests - apply-changes
#
#-----------------------------------------------------------------------------

function(check_apply_changes _name _options _input _change _type _output)
    check_output(apply-changes ${_name} "apply-changes ${_options} --generator=test -f ${_type} apply-changes/${_input} apply-changes/${_change}" "apply-changes/${_output}")
endfunction()

check_apply_changes(data              ""                            input-data.osm input-change.osc "osm" output-data.osm)

add_test(NAME check-apply-changes-mixed1 COMMAND osmium apply-changes ${CMAKE_SOURCE_DIR}/test/apply-changes/input-data.osm ${CMAKE_SOURCE_DIR}/test/apply-changes/input-changes.osc -f osh)
set_tests_properties(check-apply-changes-mixed1 PROPERTIES WILL_FAIL true)

add_test(NAME check-apply-changes-mixed2 COMMAND osmium apply-changes ${CMAKE_SOURCE_DIR}/test/apply-changes/input-data.osh ${CMAKE_SOURCE_DIR}/test/apply-changes/input-changes.osc -f osm)
set_tests_properties(check-apply-changes-mixed2 PROPERTIES WILL_FAIL true)

check_apply_changes(history-osh-osh    ""               input-history.osh input-change.osc "osh" output-history.osh)
check_apply_changes(history-osh-osh-wh "--with-history" input-history.osh input-change.osc "osh" output-history.osh)
check_apply_changes(history-osm-osh-wh "--with-history" input-history.osm input-change.osc "osh" output-history.osh)
check_apply_changes(history-osh-osm-wh "--with-history" input-history.osh input-change.osc "osm" output-history.osh)

check_apply_changes(data-low "--locations-on-ways" input-data-low.osm input-change.osc "osm" output-data-low.osm)

#-----------------------------------------------------------------------------
# Test some patching features useful to redact existing OSM files
#-----------------------------------------------------------------------------
# Version 1 contains a copyright violation. The diff file contains a v2 object (with visible=false) and a modified v1 object.
check_apply_changes(redact-and-update "--redact" input-redact-and-update.osh input-redact-and-update.osc "osh" output-redact-and-update.osh)

# The input history file contains version 1 and 2, the diff wants to replace v1 by a deleted object (to redact the sensitive data in v1).
# The output history file should contain the redacted version 1 and an unmodified copy of v2.
check_apply_changes(patch-old-version "--redact" input-patch-old-version.osh input-patch-old-version.osc "osh" output-patch-old-version.osh)

# The input history file contains version 1 and 2 of an object. The diff wants to remove the metadata of v1. Version 2 should stay untouched.
check_apply_changes(redact-metadata "--redact" input-redact-metadata.osh input-redact-metadata.osc "osh" output-redact-metadata.osh)

#-----------------------------------------------------------------------------
# Test the application of diffs which have only some metadata fields on files which have only some metadata fields
#-----------------------------------------------------------------------------
# The input file has version and timestamp fields, the diff has only version fields.
check_apply_changes(version-on-version-timestamp "" input-version+timestamp.osm input-version.osc "osm" output-version-applied-on-version+timestamp.osm)
check_apply_changes(version-on-version-timestamp-low "--locations-on-ways" input-version+timestamp.osm input-version.osc "osm" output-version-applied-on-version+timestamp-low.osm)

# The input file has all metadata fields, the diff has only version fields.
check_apply_changes(version-on-all "" input-data.osm input-change-only-version.osc "osm" output-version-applied-on-all.osm)

#-----------------------------------------------------------------------------
# Regression tests
#-----------------------------------------------------------------------------
check_apply_changes(same-id "" input-same-id.osm input-same-id.osc "osm" output-same-id.osm)

# Several change files contain the same objects with the same version and
# timestamp. The object from the change file given last on the command line
# must win.
check_apply_changes(same-version-123 "" input-same-id.osm "input-same-version-1.osc apply-changes/input-same-version-2.osc apply-changes/input-same-version-3.osc" "osm" output-same-version-123.osm)
check_apply_changes(same-version-321 "" input-same-id.osm "input-same-version-3.osc apply-changes/input-same-version-2.osc apply-changes/input-same-version-1.osc" "osm" output-same-version-321.osm)

# An error in a change file that is not the first one.
add_test(NAME check-apply-changes-error-second COMMAND osmium apply-changes ${CMAKE_SOURCE_DIR}/test/apply-changes/input-data.osm ${CMAKE_SOURCE_DIR}/test/apply-changes/input-change.osc ${CMAKE_SOURCE_DIR}/test/apply-changes/input-broken.osc -f osm)
set_tests_properties(check-apply-changes-error-second PROPERTIES WILL_FAIL true)

#-----------------------------------------------------------------------------
# Test copying of unchanged blocks from PBF input files (passthrough)
#-----------------------------------------------------------------------------
# The input files are generated, see generate_passthrough_input.cmake for
# their content. Each test runs apply-changes with and without passthrough
# and checks that the results are the same and that the expected number of
# blocks was copied.

set(_passthrough_dir ${CMAKE_CURRENT_BINARY_DIR}/passthrough)

add_test(NAME apply-changes-passthrough-generate-input
         COMMAND ${CMAKE_COMMAND}
                 -D osmium:FILEPATH=$<TARGET_FILE:osmium>
                 -D dir:PATH=${_passthrough_dir}
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/generate_passthrough_input.cmake)
set_tests_properties(apply-changes-passthrough-generate-input PROPERTIES FIXTURES_SETUP apply_changes_passthrough)

function(check_passthrough _name _input _changes _options _expect)
    add_test(NAME apply-changes-passthrough-${_name}
             COMMAND ${CMAKE_COMMAND}
                     -D osmium:FILEPATH=$<TARGET_FILE:osmium>
                     -D dir:PATH=${CMAKE_CURRENT_SOURCE_DIR}
                     -D tmpdir:PATH=${_passthrough_dir}/${_name}
                     -D input:FILEPATH=${_passthrough_dir}/${_input}
                     -D changes=${_changes}
                     -D options=${_options}
                     -D expect=${_expect}
                     ${ARGN}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/run_passthrough_test.cmake)
    set_tests_properties(apply-changes-passthrough-${_name} PROPERTIES FIXTURES_REQUIRED apply_changes_passthrough)
endfunction()

# Blocks without changes between their first object and the first object of
# the next block are copied without decoding them.
function(expect_blocks _var _copied _not_decoded _encoded)
    set(${_var} "Copied ${_copied} unchanged PBF blocks \\(${_not_decoded} without decoding them\\), re-encoded ${_encoded} blocks" PARENT_SCOPE)
endfunction()

function(check_blocks _name _changes _copied _not_decoded _encoded)
    expect_blocks(_expect ${_copied} ${_not_decoded} ${_encoded})
    check_passthrough(${_name} passthrough-input.osm.pbf "${_changes}" "" "${_expect}" ${ARGN})
endfunction()

check_blocks(empty         passthrough-empty.osc         7 7 0)
check_blocks(boundaries    passthrough-boundaries.osc    5 5 2)
check_blocks(gaps          passthrough-gaps.osc          7 4 0)
check_blocks(create-inside passthrough-create-inside.osc 6 6 1)
check_blocks(delete        passthrough-delete.osc        4 4 3)
check_blocks(delete-absent passthrough-delete-absent.osc 7 5 0)
check_blocks(nodes         passthrough-nodes.osc         5 5 2)
check_blocks(stale         passthrough-stale.osc         6 6 1)
check_blocks(negative      passthrough-negative.osc      6 6 1)
check_blocks(order12       "passthrough-order1.osc passthrough-order2.osc" 6 6 1)
check_blocks(order21       "passthrough-order2.osc passthrough-order1.osc" 6 6 1)

# In the node, way and relation blocks the unchanged objects are copied
# without decoding them, so the output is not byte-identical to the classic
# one.
expect_blocks(_expect 0 0 7)
check_passthrough(all passthrough-input.osm.pbf passthrough-all.osc "" "${_expect} \\(6 of them copying unchanged objects\\)")

# One of the change files is read from STDIN.
expect_blocks(_expect 6 6 1)
check_passthrough(stdin passthrough-input.osm.pbf "passthrough-order1.osc -" "--change-file-format=osc" "${_expect}" -D stdin=passthrough-order2.osc)

# The second block starts with the same object the first one ends with. This
# is only noticed if the first block is decoded, so there is a change in it.
expect_blocks(_expect 0 0 2)
check_passthrough(duplicate passthrough-duplicate.osm.pbf passthrough-duplicate.osc "" "${_expect}")

# Blocks with large string tables, so the first object is not in the first
# part of the data uncompressed to find it.
expect_blocks(_expect 4 4 1)
check_passthrough(strings passthrough-strings.osm.pbf passthrough-boundaries.osc "" "${_expect}")

function(check_no_passthrough _name _input _options _reason)
    check_passthrough(${_name} ${_input} passthrough-boundaries.osc "${_options}" "Not copying unchanged blocks: ${_reason}" ${ARGN})
endfunction()

check_no_passthrough(disabled    passthrough-input.osm.pbf "--no-passthrough"                    "disabled on command line")
check_no_passthrough(input-opl   passthrough-input.opl     ""                                    "input is not a PBF file")
check_no_passthrough(output-osm  passthrough-input.osm.pbf ""                                    "output is not a PBF file" -D suffix=osm)
check_no_passthrough(compression passthrough-input.osm.pbf "-f pbf,pbf_compression=none"         "output compression is set")
check_no_passthrough(dense-nodes passthrough-input.osm.pbf "-f pbf,pbf_dense_nodes=false"        "output without dense nodes")
check_no_passthrough(metadata    passthrough-input.osm.pbf "-f pbf,add_metadata=version+timestamp" "output without some metadata")

# Negative IDs are not in the order libosmium uses, the results must still
# be the same. The order inside blocks that are not decoded is not checked.
expect_blocks(_expect 2 2 0)
check_passthrough(signed-order passthrough-signed-order.osm.pbf passthrough-empty.osc "" "${_expect}")
check_passthrough(signed-order-changed passthrough-signed-order.osm.pbf passthrough-negative.osc "" "${_expect}")
