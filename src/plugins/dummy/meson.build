# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

plugin_deps = [nixl_infra, nixl_common_dep]

dummy_sources = [
    'dummy_backend.h',
    'dummy_plugin.cpp',
]

if 'DUMMY' in static_plugins
    dummy_backend_lib = static_library('DUMMY',
        dummy_sources,
        dependencies: plugin_deps,
        cpp_args: compile_flags,
        include_directories: [nixl_inc_dirs, utils_inc_dirs],
        install: false,
        name_prefix: 'libplugin_')
else
    dummy_backend_lib = shared_library('DUMMY',
        dummy_sources,
        dependencies: plugin_deps,
        cpp_args: ['-fPIC'],
        include_directories: [nixl_inc_dirs, utils_inc_dirs],
        install: true,
        name_prefix: 'libplugin_',
        install_dir: plugin_install_dir,
        install_rpath: '$ORIGIN/..')
    if get_option('buildtype') == 'debug'
        run_command('sh', '-c',
            'echo "DUMMY=' + dummy_backend_lib.full_path() + '" >> ' + plugin_build_dir + '/pluginlist',
            check: true
        )
    endif
endif

dummy_backend_interface = declare_dependency(link_with: dummy_backend_lib)
