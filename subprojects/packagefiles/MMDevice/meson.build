# Note that this is a Windows-only minimal build of MMDevice

project('MMDevice', 'cpp')

mmdevice_inc = include_directories('MMDevice')

mmdevice_sources = files(
    'MMDevice/Debayer.cpp',
    'MMDevice/DeviceUtils.cpp',
    'MMDevice/ImgBuffer.cpp',
    'MMDevice/MMDevice.cpp',
    'MMDevice/ModuleInterface.cpp',
    'MMDevice/Property.cpp',
)

mmdevice_lib = static_library(
    'MMDevice',
    mmdevice_sources,
    include_directories: mmdevice_inc,
    cpp_args: [
        '-D_CRT_SECURE_NO_WARNINGS',
        '-DMODULE_EXPORTS',
    ],
)

mmdevice_dep = declare_dependency(
    include_directories: mmdevice_inc,
    link_with: mmdevice_lib,
)

meson.override_dependency(
    'MMDevice',
    mmdevice_dep,
)
