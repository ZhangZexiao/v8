How to build V8 JavaScript engine on Windows 10 in China with VS2017 By Harry Zhang
====================================================================
read https://github.com/v8/v8/wiki

first, you need VPN, specifically, US WEST(or NON-China) VPN, to download file from Google's website.

then download depot_tools.zip from https://storage.googleapis.com/chrome-infra/depot_tools.zip

http://commondatastorage.googleapis.com/chrome-infra-docs/flat/depot_tools/docs/html/depot_tools_tutorial.html#_setting_up 

https://github.com/v8/v8/wiki/Building-with-GN

unzip depot_tools.zip to a directory, assume it is C:\chrome_v8_depot_tools

put C:\chrome_v8_depot_tools to the head of environment variable %path% permanently or temporary by 

    set path=C:\chrome_v8_depot_tools;%path%

before other commands.

C:\chrome_v8_depot_tools in %path% must before other version of python such as python 3.x, v8 depends python 2.x

add another environment variable 

    DEPOT_TOOLS_WIN_TOOLCHAIN=0 

https://chromium.googlesource.com/chromium/src/+/master/docs/windows_build_instructions.md

install Windows 10 SDK https://developer.microsoft.com/en-US/windows/downloads/windows-10-sdk

now, bring up a cmd, and execute 

    gclient 

to update depot_tools

find a folder for v8 source code, assume it is E:\chrome, by 

    md E:\chrome

make sure it is empty.

fetch source code with command 

    fetch v8

it will take an hour to download source code.

then call command 

    gclient sync

to download v8 build dependencies.

then go to "v8" directory by executing command 

    cd v8

call command 

    gn args out.gn/x64.release

to generate build environment for "ninja", it will create folder "out.gn/x64.release" under "v8" and copy Windows 10 SDK toolchain dll to the folder and create open file E:\chrome\v8\out.gn\x64.release\args.gn with notepad, you can modify parameters. "GN" is google's cmake.

now call 

    ninja -C out.gn/x64.release

to compile code, there will be 1700+ objects created.

call 

    tools\run-tests.py --gn

to test the latest build.




V8 JavaScript Engine
=============

V8 is Google's open source JavaScript engine.

V8 implements ECMAScript as specified in ECMA-262.

V8 is written in C++ and is used in Google Chrome, the open source
browser from Google.

V8 can run standalone, or can be embedded into any C++ application.

V8 Project page: https://github.com/v8/v8/wiki


Getting the Code
=============

Checkout [depot tools](http://www.chromium.org/developers/how-tos/install-depot-tools), and run

        fetch v8

This will checkout V8 into the directory `v8` and fetch all of its dependencies.
To stay up to date, run

        git pull origin
        gclient sync

For fetching all branches, add the following into your remote
configuration in `.git/config`:

        fetch = +refs/branch-heads/*:refs/remotes/branch-heads/*
        fetch = +refs/tags/*:refs/tags/*


Contributing
=============

Please follow the instructions mentioned on the
[V8 wiki](https://github.com/v8/v8/wiki/Contributing).
