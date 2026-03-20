project('mcast2ucast', 'c',
    version: '0.1.0',
    default_options: ['warning_level=2', 'c_std=gnu11'])

dpdk_dep = dependency('libdpdk', required: true)
thread_dep = dependency('threads')

sources = files(
    'src/main.c',
    'src/config.c',
    'src/group_table.c',
    'src/rewrite.c',
    'src/dataplane.c',
    'src/control.c',
    'src/producer_notify.c',
    'src/web.c',
)

executable('mcast2ucast',
    sources,
    dependencies: [dpdk_dep, thread_dep],
    include_directories: include_directories('src'),
    install: true)
