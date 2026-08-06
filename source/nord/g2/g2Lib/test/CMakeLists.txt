cmake_minimum_required(VERSION 3.15)

# THIS FILE HAS ONE OWNER: task BRD-0. See plan section 7.4.2.
#
# It is the REGISTRAR of every test under source/nord/g2/g2Lib/test/. A track
# adds its own add_test(NAME <name> ...) lines to its own per-track list below
# and to nothing else.
#
# EVERY TEST IS REGISTERED WITH THE EXACT NAME ITS TASK'S Check: LINE PASSES
# TO -R. Plan section 7.7 lints that correspondence.
#
# Every list is created empty by BRD-0.

include(tests_board.cmake)
include(tests_sched.cmake)
include(tests_chain.cmake)
include(tests_dsp.cmake)
include(tests_repo.cmake)
include(tests_plugin.cmake)
include(tests_proto.cmake)
include(tests_oracle.cmake)
include(tests_perf.cmake)
include(tests_int.cmake)
