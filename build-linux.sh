#!/usr/bin/bash

# To run this you need to setup msvc for wine https://github.com/mstorsjo/msvc-wine
# I haven't setup the .clangd file yet so it's gonna be a pain to program in if you're using clangd tho

msbuild "./Project Reboot 3.0/Project Reboot 3.0.vcxproj" /p:configuration=Release /p:platform=x64 /p:PlatformToolset=v145 /p:OutDir=../Build
# note: /p:PlatformToolset=v145 is required since the local MSBuild 18 resolved toolset (14.51) is
# incompatible with the project's committed v143 (MSB8052); earlier successful builds also recorded
# PlatformToolSet=v145 in their tlogs, so this pins in what already worked.
