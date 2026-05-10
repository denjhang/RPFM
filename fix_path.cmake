# fix_path.cmake - helper to fix MSYS2 Unix paths in autogen header
cmake_path(NATIVE_PATH INFILE NATIVE)
file(READ "${INFILE}" CONTENT)
string(REPLACE "/d/" "D:/" CONTENT "${CONTENT}")
string(REPLACE "/c/" "C:/" CONTENT "${CONTENT}")
file(WRITE "${INFILE}" "${CONTENT}")
