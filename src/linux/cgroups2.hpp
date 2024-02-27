// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __CGROUPS_V2_HPP__
#define __CGROUPS_V2_HPP__

#include <stout/nothing.hpp>
#include <stout/try.hpp>

namespace cgroups2 {

// Checks if cgroups2 is available on the system.
bool enabled();

// Mounts the cgroups2 file system at /sys/fs/cgroup. Errors if
// the cgroups v2 file system is already mounted.
Try<Nothing> mount();

// Checks if the cgroup2 file systems is mounted at /sys/fs/cgroup,
// returns an error if the mount is found at an unexpected location.
Try<bool> mounted();

// Unmounts the cgroups2 file system from /sys/fs/cgroup. Errors if
// the cgroup2 file system is not mounted at /sys/fs/cgroup. It's the
// responsibility of the caller to ensure all child cgroups have been destroyed.
Try<Nothing> unmount();

} // namespace cgroups2

#endif // __CGROUPS_V2_HPP__
