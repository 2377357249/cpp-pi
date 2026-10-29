# CMake generated Testfile for 
# Source directory: /Users/dongxu/Desktop/code/cpp-pi
# Build directory: /Users/dongxu/Desktop/code/cpp-pi/build-cov
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(integration "/Users/dongxu/Desktop/code/cpp-pi/build-cov/cpp-pi-it")
set_tests_properties(integration PROPERTIES  _BACKTRACE_TRIPLES "/Users/dongxu/Desktop/code/cpp-pi/CMakeLists.txt;87;add_test;/Users/dongxu/Desktop/code/cpp-pi/CMakeLists.txt;0;")
subdirs("third_party/fmt")
subdirs("third_party/glog")
subdirs("third_party/quill")
subdirs("third_party/googletest")
