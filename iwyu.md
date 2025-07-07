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

