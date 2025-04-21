project(
    'openscan-mm-adapter',
    'cpp',
    default_options: [
        'warning_level=2',
    ],
)

openscanlib_dep = dependency(
    'OpenScanLib',
    fallback: ['OpenScanLib', 'OpenScanLib'],
    static: true,
    default_options: [
        'devicelib=enabled',
        'apilib=enabled',
        'docs=disabled',
        'tests=disabled',
    ],
)

mmdevice_dep = dependency(
    'MMDevice',
    fallback: 'MMDevice',
)

mmda = shared_module(
    'mmgr_dal_OpenScan',
    'OpenScan.cpp',
    name_suffix: 'dll',
    dependencies: [
        openscanlib_dep,
        mmdevice_dep,
    ],
    cpp_args: [
        '-D_CRT_SECURE_NO_WARNINGS',
        '-DMODULE_EXPORTS',
    ],
)
