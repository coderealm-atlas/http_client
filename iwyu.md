# IWYU Usage.

run iwyu to reorganize the includes in the file.

## iwyu_tool.py

```sh
# Unix systems
$ mkdir build && cd build
$ CC="clang" CXX="clang++" cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ...
$ ~/include-what-you-use/iwyu_tool.py -p . > iwyu.out

./sh/remove_external_blocks.sh build/iwyu.out /home/jianglibo/bb/external/
~/include-what-you-use/fix_includes.py < ./build/iwyu.out
```


#  -ftime-trace


```sh
./build/tests/CMakeFiles/httpclient_test.dir/cmake_pch.hxx.json
./build/tests/CMakeFiles/io_monad_test.dir/io_monad_test.cpp.json
./build/tests/CMakeFiles/urls_test.dir/urls_test.cpp.json

ClangBuildAnalyzer --analyze ./build

ClangBuildAnalyzer --all ./build/ FullCapture.bin
ClangBuildAnalyzer --analyze FullCapture.bin

```