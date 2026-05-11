# CMake generated Testfile for 
# Source directory: /home/gef/Documents/projects/microide
# Build directory: /home/gef/Documents/projects/microide/build-perf
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[microide_tests]=] "/home/gef/Documents/projects/microide/build-perf/microide/microide_tests")
set_tests_properties([=[microide_tests]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/gef/Documents/projects/microide/CMakeLists.txt;556;add_test;/home/gef/Documents/projects/microide/CMakeLists.txt;0;")
add_test([=[microide_perf_tests]=] "/home/gef/Documents/projects/microide/build-perf/microide/microide_perf" "--smoke" "--iterations=1")
set_tests_properties([=[microide_perf_tests]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/gef/Documents/projects/microide/CMakeLists.txt;640;add_test;/home/gef/Documents/projects/microide/CMakeLists.txt;0;")
