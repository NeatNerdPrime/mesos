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

#include "common/protobuf_utils.hpp"

#include "slave/containerizer/mesos/paths.hpp"
#include "slave/containerizer/mesos/isolators/cgroups2/cgroups2.hpp"
#include "slave/containerizer/mesos/isolators/cgroups2/controllers/core.hpp"
#include "slave/containerizer/mesos/isolators/cgroups2/controllers/cpu.hpp"
#include "slave/containerizer/mesos/isolators/cgroups2/controllers/memory.hpp"
#include "slave/containerizer/mesos/isolators/cgroups2/controllers/perf_event.hpp"
#include "slave/containerizer/mesos/isolators/cgroups2/controllers/devices.hpp"
#include "slave/containerizer/mesos/isolators/cgroups2/controllers/io.hpp"
#include "slave/containerizer/mesos/isolators/cgroups2/controllers/hugetlb.hpp"
#include "slave/containerizer/mesos/isolators/cgroups2/controllers/cpuset.hpp"
#include "slave/containerizer/mesos/isolators/cgroups2/controllers/pids.hpp"

#include <set>
#include <string>
#include <vector>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/id.hpp>
#include <process/pid.hpp>

#include <stout/foreach.hpp>
#include <stout/os.hpp>
#include <stout/strings.hpp>

#include "linux/cgroups2.hpp"
#include "linux/fs.hpp"
#include "linux/ns.hpp"
#include "linux/systemd.hpp"

using mesos::slave::ContainerClass;
using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::ContainerLimitation;
using mesos::slave::ContainerState;
using mesos::slave::Isolator;

using process::Failure;
using process::Future;
using process::Owned;
using process::PID;

using std::set;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace slave {

namespace cgroups2_paths = containerizer::paths::cgroups2;

Cgroups2IsolatorProcess::Cgroups2IsolatorProcess(
    const Flags& _flags,
    const hashmap<string, Owned<Controller>>& _controllers,
    const process::Owned<DeviceManager>& _deviceManager)
    : ProcessBase(process::ID::generate("cgroups2-isolator")),
    flags(_flags),
    controllers(_controllers),
    deviceManager(_deviceManager) {}


Cgroups2IsolatorProcess::~Cgroups2IsolatorProcess() {}


Try<Isolator*> Cgroups2IsolatorProcess::create(
    const Flags& flags,
    const Owned<DeviceManager>& deviceManager)
{
  hashmap<string, Try<Owned<ControllerProcess>>(*)(const Flags&)> creators = {
    {"core", &CoreControllerProcess::create},
    {"cpu", &CpuControllerProcess::create},
    {"mem", &MemoryControllerProcess::create},
    {"perf_event", &PerfEventControllerProcess::create},
    {"io", &IoControllerProcess::create},
    {"hugetlb", &HugetlbControllerProcess::create},
    {"cpuset", &CpusetControllerProcess::create},
    {"pids", &PidsControllerProcess::create}
  };

  hashmap<string, Try<Owned<ControllerProcess>>(*)(
      const Flags&,
      const Owned<DeviceManager>)>
    creatorsWithDeviceManager = {
      {"devices", &DeviceControllerProcess::create},
    };

  hashmap<string, Owned<Controller>> controllers;

  // The "core" controller is always enabled because the "cgroup.*" control
  // files which it interfaces with exist and are updated for all cgroups.
  set<string> controllersToCreate = { "core" };

  if (strings::contains(flags.isolation, "cgroups/all")) {
    foreachkey (const string& creator, creators) {
      controllersToCreate.insert(creator);
    }
    foreachkey (const string& creator, creatorsWithDeviceManager) {
      controllersToCreate.insert(creator);
    }
  } else {
    foreach (string isolator, strings::tokenize(flags.isolation, ",")) {
      if (!strings::startsWith(isolator, "cgroups/")) {
        // Skip when the isolator is not related to cgroups.
        continue;
      }

      isolator = strings::remove(isolator, "cgroups/", strings::Mode::PREFIX);
      if (!creators.contains(isolator)
          && !creatorsWithDeviceManager.contains(isolator)) {
        return Error(
          "Unknown or unsupported isolator 'cgroups/" + isolator + "'");
      }

      controllersToCreate.insert(isolator);
    }
  }

  foreach (const string& controllerName, controllersToCreate) {
    if (creators.count(controllerName) == 0
        && creatorsWithDeviceManager.count(controllerName) == 0) {
      return Error(
        "Cgroups v2 controller '" + controllerName + "' is not supported.");
    }

    Try<Owned<ControllerProcess>> process = creators.contains(controllerName)
        ? creators.at(controllerName)(flags)
        : creatorsWithDeviceManager.at(controllerName)(flags, deviceManager);
    if (process.isError()) {
      return Error("Failed to create controller '" + controllerName + "': "
                   + process.error());
    }

    Owned<Controller> controller = Owned<Controller>(new Controller(*process));
    controllers.put(controllerName, controller);
  }


  Owned<MesosIsolatorProcess> process(
      new Cgroups2IsolatorProcess(flags, controllers, deviceManager));
  return new MesosIsolator(process);
}


bool Cgroups2IsolatorProcess::supportsNesting()
{
  return true;
}


bool Cgroups2IsolatorProcess::supportsStandalone()
{
  return true;
}


Future<Option<ContainerLaunchInfo>> Cgroups2IsolatorProcess::prepare(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{

  if (infos.contains(containerId)) {
    return Failure("Container with id '" + stringify(containerId) + "'"
                   " has already been prepared");
  }


  // Based on MESOS-9305, there seems to be a possibility that the root
  // folder may be deleted underneath us. Since we make use of subtree_control
  // to determine a cgroup and its descendents' access to controllers, we
  // we can't just recursively create the folders. Hence, we crash if the root
  // folder is not found, as it will allow us to restart and go through agent's
  // main logic which sets up the root cgroup and its subtree control.
  CHECK(cgroups2::exists(flags.cgroups_root));

  // Create the non-leaf and leaf cgroups for the container, enable
  // controllers in the non-leaf cgroup, and `prepare` each of the controllers.
  const string nonLeafCgroup = cgroups2_paths::container(
      flags.cgroups_root, containerId);
  if (cgroups2::exists(nonLeafCgroup)) {
    return Failure("Cgroup '" + nonLeafCgroup + "' already exists");
  }

  Try<Nothing> create = cgroups2::create(nonLeafCgroup, true);
  if (create.isError()) {
    return Failure("Failed to create cgroup '" + nonLeafCgroup + "': "
                   + create.error());
  }

  const string leafCgroup = cgroups2_paths::container(
      flags.cgroups_root, containerId, true);
  if (cgroups2::exists(leafCgroup)) {
    return Failure("Cgroup '" + leafCgroup + "' already exists");
  }

  create = cgroups2::create(leafCgroup, true);
  if (create.isError()) {
    return Failure("Failed to create cgroup '" + leafCgroup + "': "
                   + create.error());
  }

  LOG(INFO) << "Created cgroups '" << nonLeafCgroup << "'"
            << " and '" << leafCgroup << "'";

  const bool shareCgroups =
    containerId.has_parent() &&
    ((containerConfig.has_container_info() &&
      containerConfig.container_info().has_linux_info() &&
      containerConfig.container_info().linux_info().has_share_cgroups())
       ? containerConfig.container_info().linux_info().share_cgroups()
       : true);

  infos[containerId] = Owned<Info>(
      new Info(containerId, nonLeafCgroup, leafCgroup, !shareCgroups));

  if (shareCgroups) {
    return __prepare(containerId, containerConfig);
  }

  CHECK(containerConfig.container_class() != ContainerClass::DEBUG);

  vector<Future<Nothing>> prepares;
  hashset<string> skip_enable = {"core", "perf_event", "devices"};
  foreachvalue (const Owned<Controller>& controller, controllers) {
    // The "core", "perf_event" and "devices" controllers do not exist in
    // cgroup.controllers file, and therefore we cannot call
    // cgroups2::controllers::enable with it as it cannot be written into
    // cgroup.subtree_control, but we still need to push it into the controllers
    // of the containers, so we will only skip the call for
    // cgroups2::controllers::enable.
    if (!skip_enable.contains(controller->name())) {
      vector<string> cgroup_tokens = strings::tokenize(
          strings::remove(nonLeafCgroup, flags.cgroups_root, strings::PREFIX),
          "/");
      string current_cgroup = flags.cgroups_root;
      foreach (const string& token, cgroup_tokens) {
        current_cgroup = path::join(current_cgroup, token);
        Try<Nothing> enable =
          cgroups2::controllers::enable(current_cgroup, {controller->name()});
        if (enable.isError()) {
          return Failure(
              "Failed to enable controller '" + controller->name() + "'"
              " in cgroup '" + current_cgroup + "': " + enable.error());
        }
      }
    }

    // We don't enable the controllers in the leaf cgroup because of the
    // no internal process constraint. For instance, enabling the "memory"
    // controller in the leaf cgroup will prevent us from putting the container
    // process inside of the leaf cgroup; writing to 'cgroup.procs' will fail.
    //
    // If a container wants to self-manage its cgroups, the container will
    // have to create a new cgroup off of the leaf cgroup and move itself into
    // the new cgroup, before it can enable controllers in the leaf.
    //
    // Example:
    // 1. Create /leaf/mycgroup.
    // 2. Write ::getpid() to /leaf/mycgroup/cgroup.procs.
    // 3. Enable controllers in /leaf, which will apply constraints to
    //    /leaf/mycgroup.

    infos[containerId]->controllers.insert(controller->name());
    prepares.push_back(
        controller->prepare(containerId, nonLeafCgroup, containerConfig));
  }

  // Copied from cgroups v1 isolator logic:
  //
  // Chown the leaf cgroup so the executor or a nested container whose
  // `share_cgroups` is false can create nested cgroups. Do
  // not recurse so the control files are still owned by the slave
  // user and thus cannot be changed by the executor.
  //
  // TODO(haosdent): Multiple tasks under the same user can change
  // cgroups settings for each other. A better solution is using
  // cgroups namespaces and user namespaces to achieve the goal.
  //
  // NOTE: We only need to handle the case where 'flags.switch_user'
  // is true (i.e., 'containerConfig.has_user() == true'). If
  // 'flags.switch_user' is false, the cgroup will be owned by root
  // anyway since cgroups isolator requires root permission.
  if (containerConfig.has_user()) {
    Option<string> user;
    if (containerConfig.has_task_info() && containerConfig.has_rootfs()) {
      // Command task that has a rootfs. In this case, the executor
      // will be running under root, and the command task itself
      // might be running under a different user.
      //
      // TODO(jieyu): The caveat here is that if the 'user' in
      // task's command is not set, we don't know exactly what user
      // the task will be running as because we don't know the
      // framework user. We do not support this case right now.
      if (containerConfig.task_info().command().has_user()) {
        user = containerConfig.task_info().command().user();
      }
    } else {
      user = containerConfig.user();
    }

    if (user.isSome()) {
      string path = cgroups2::path(leafCgroup);
      VLOG(1) << "Chown the cgroup at '" << path << "'"
              << " to user '" << *user << "' for container " << containerId;

      Try<Nothing> chown = os::chown(*user, path, false);

      if (chown.isError()) {
        return Failure("Failed to chown the cgroup at '" + path + "'"
                       " to user '" + *user + "': " + chown.error());
      }
    }
  }

  return await(prepares)
    .then(defer(
        PID<Cgroups2IsolatorProcess>(this),
        &Cgroups2IsolatorProcess::_prepare,
        containerId,
        containerConfig,
        lambda::_1));
}


Future<Option<ContainerLaunchInfo>> Cgroups2IsolatorProcess::_prepare(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig,
    const vector<Future<Nothing>>& futures)
{
  vector<string> errors;
  foreach (const Future<Nothing>& future, futures) {
    if (!future.isReady()) {
      errors.push_back(future.isFailed() ? future.failure() : "discarded");
    }
  }

  if (!errors.empty()) {
    return Failure("Failed to prepare controllers: "
                   + strings::join(", ", errors));
  }

  return update(
      containerId,
      containerConfig.resources(),
      containerConfig.limits())
    .then(defer(
        PID<Cgroups2IsolatorProcess>(this),
        &Cgroups2IsolatorProcess::__prepare,
        containerId,
        containerConfig));
}


Future<Option<ContainerLaunchInfo>> Cgroups2IsolatorProcess::__prepare(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  // Only create cgroup mounts for containers with rootfs.
  //
  // TODO(bmahler): Consider adding cgroup namespace isolation for containers
  // without a rootfs, which seems to be a useful feature?
  if (!containerConfig.has_rootfs()) {
    return None();
  }

  Owned<Info> info = cgroupInfo(containerId);
  if (!info.get()) {
    return Failure("Failed to get cgroup for container"
                   " '" + stringify(containerId) + "'");
  }

  ContainerLaunchInfo launchInfo;

  // Create a new cgroup namespace. The child process will only be able to
  // see the cgroups that are in its cgroup subtree.
  launchInfo.add_clone_namespaces(CLONE_NEWCGROUP);

  // Create a new mount namespace and mount the root cgroup at /sys/fs/cgroup.
  // TODO(bmahler): Is this the right way to mount?
  launchInfo.add_clone_namespaces(CLONE_NEWNS);
  *launchInfo.add_mounts() = protobuf::slave::createContainerMount(
      cgroups2::path(info->cgroup_leaf) ,
      path::join(containerConfig.rootfs(), "/sys/fs/cgroup"),
      MS_BIND | MS_REC);

  // TODO(qianzhang): This is a hack to pass the container-specific cgroups
  // mounts and the symbolic links to the command executor to do for the
  // command task. The reasons that we do it in this way are:
  //   1. We need to ensure the container-specific cgroups mounts are done
  //      only in the command task's mount namespace but not in the command
  //      executor's mount namespace.
  //   2. Even it's acceptable to do the container-specific cgroups mounts
  //      in the command executor's mount namespace and the command task
  //      inherit them from there (i.e., here we just return `launchInfo`
  //      rather than passing it via `--task_launch_info`), the container
  //      specific cgroups mounts will be hidden by the `sysfs` mounts done in
  //      `mountSpecialFilesystems()` when the command executor launches the
  //      command task.
  if (containerConfig.has_task_info()) {
    ContainerLaunchInfo _launchInfo;

    _launchInfo.mutable_command()->add_arguments(
        "--task_launch_info=" +
        stringify(JSON::protobuf(launchInfo)));

    return _launchInfo;
  }

  return launchInfo;
}


Future<Nothing> Cgroups2IsolatorProcess::recover(
    const vector<ContainerState>& states,
    const hashset<ContainerID>& orphans)
{
  // Recover containers from checkpointed data:
  vector<Future<Nothing>> recovers;
  foreach (const ContainerState& state, states) {
    const bool shareCgroups =
      state.container_id().has_parent() &&
      ((state.has_container_info() && state.container_info().has_linux_info() &&
        state.container_info().linux_info().has_share_cgroups())
         ? state.container_info().linux_info().share_cgroups()
         : true);

    recovers.push_back(___recover(state.container_id(), !shareCgroups));
  }

  // Then recover containers we find in the cgroups hierarchy:
  return await(recovers)
    .then(defer(self(), [=](const vector<Future<Nothing>>& futures)
        -> Future<Nothing> {
      vector<string> errors;
      foreach (const Future<Nothing>& future, futures) {
        if (!future.isReady()) {
          errors.push_back(future.isFailed() ? future.failure() : "discarded");
        }
      }

      if (!errors.empty()) {
        return Failure("Failed to recover active containers: "
                      + strings::join(", ", errors));
      }

      vector<Future<Nothing>> recovers = {
          _recover(orphans),
          deviceManager->recover(states)
      };

      return collect(recovers)
        .then([]() { return Nothing(); });
    }));
}


Future<Nothing> Cgroups2IsolatorProcess::_recover(
    const hashset<ContainerID>& orphans)
{
  hashset<ContainerID> knownOrphans;
  hashset<ContainerID> unknownOrphans;

  Try<set<string>> cgroups = cgroups2::get(flags.cgroups_root);
  if (cgroups.isError()) {
    return Failure("Failed to get cgroups under '" + flags.cgroups_root + "': "
                   + cgroups.error());
  }

  foreach (const string& cgroup, *cgroups) {
    if (cgroup == cgroups2_paths::agent(flags.cgroups_root)) {
      continue;
    }

    Option<ContainerID> containerId = cgroups2_paths::containerId(
        flags.cgroups_root, cgroup);
    if (containerId.isNone()) {
      LOG(INFO) << "Cgroup '" << cgroup << "' does not correspond to a"
                << " container id and will not be recovered";
      continue;
    }

    if (infos.contains(*containerId)) {
      // Container has already been recovered.
      continue;
    }

    orphans.contains(*containerId) ?
        knownOrphans.insert(*containerId) :
        unknownOrphans.insert(*containerId);
  }

  vector<Future<Nothing>> recovers;
  foreach (const ContainerID& containerId, knownOrphans) {
    recovers.push_back(___recover(containerId));
  }

  foreach (const ContainerID& containerId, unknownOrphans) {
    recovers.push_back(___recover(containerId));
  }

  return await(recovers)
    .then(defer(
        PID<Cgroups2IsolatorProcess>(this),
        &Cgroups2IsolatorProcess::__recover,
        unknownOrphans,
        lambda::_1));
}


Future<Nothing> Cgroups2IsolatorProcess::__recover(
    const hashset<ContainerID>& unknownOrphans,
    const vector<Future<Nothing>>& futures)
{
  vector<string> errors;
  foreach (const Future<Nothing>& future, futures) {
    if (!future.isReady()) {
      errors.push_back(future.isFailed() ? future.failure() : "discarded");
    }
  }
  if (!errors.empty()) {
    return Failure("Failed to recover orphan containers: "
                   + strings::join(", ", errors));
  }

  // Known orphan cgroups will be destroyed by the containerizer using
  // the normal cleanup path, but for unknown orphans we need to clean
  // them up here:
  foreach (const ContainerID& containerId, unknownOrphans) {
    LOG(INFO) << "Cleaning up unknown orphaned container " << containerId;
    cleanup(containerId);
  }

  return Nothing();
}


Future<Nothing> Cgroups2IsolatorProcess::___recover(
    const ContainerID& containerId, bool isolate)
{
  // Remark and handle invalid container states and recover enabled controllers.
  //
  // Invalid container states:
  // 1. Missing non-leaf cgroup            => Log and create cgroup
  // 2. Missing leaf cgroup                => Log and create cgroup
  // 3. Some controllers are not enabled   => Log
  //
  // Failure modes that can lead to an invalid container state:
  //
  // 1. Mesos agent is restarted during launch.
  //    This can happen if the launcher fails to `fork`, 'this' isolator fails
  //    to `prepare` or `isolate`, among other reasons. Cgroups may be
  //    improperly configured meaning there may be missing cgroups or cgroup
  //    control files that have the wrong values.
  // 2. Mesos agent is restarted during destroy.
  //    The container fails to be destroyed so cgroups may not have been
  //    cleaned up correctly. This can result in orphan cgroups.
  // 3. Mesos agent is restarted with different flags.
  //    If the agent is started with new isolators the cgroups for the existing
  //    containers, from a previous run, won't have all the requested
  //    controllers enabled.
  //
  // If a container is missing a cgroup, we create the missing cgroup. This
  // is done exclusively so that the container can be cleanup()ed up by 'this'
  // isolator and destroy()ed by the launcher like other containers.
  // The alternative would be to break the invariant that each container has
  // a leaf and non-leaf cgroup but that requires more special-case handling.
  const string nonLeafCgroup =
    cgroups2_paths::container(flags.cgroups_root, containerId, false);
  const string leafCgroup =
    cgroups2_paths::container(flags.cgroups_root, containerId, true);

  if (!cgroups2::exists(nonLeafCgroup)) {
    LOG(WARNING) << "Container '" << stringify(containerId) << "'"
                 << " is missing the cgroup '" << nonLeafCgroup << "';"
                 << " creating missing cgroup";

    Try<Nothing> create = cgroups2::create(nonLeafCgroup);
    if (create.isError()) {
      return Failure("Failed to create cgroup '" + nonLeafCgroup + "': "
                     + create.error());
    }
  }

  if (!cgroups2::exists(leafCgroup)) {
    LOG(WARNING) << "Container '" << stringify(containerId) << "'"
                 << " is missing the cgroup '" << leafCgroup << "';"
                 << " creating missing cgroup";

    Try<Nothing> create = cgroups2::create(leafCgroup);
    if (create.isError()) {
      return Failure("Failed to create cgroup '" + leafCgroup + "': "
                     + create.error());
    }
  }

  Try<set<string>> enabled = cgroups2::controllers::enabled(nonLeafCgroup);
  if (enabled.isError()) {
    return Failure("Failed to get the enabled controllers for container"
                   " '" + stringify(containerId) + "': " + enabled.error());
  }

  vector<Future<Nothing>> recovers;
  hashset<string> recoveredControllers;
  foreachvalue (const Owned<Controller>& controller, controllers) {
    if (enabled->count(controller->name()) == 0) {
      // Controller is expected to be enabled but isn't.
      LOG(WARNING) << "Controller '" << controller->name() << "' is not enabled"
                   << " for container '" << stringify(containerId) << "'";

      continue;
    }

    recovers.push_back(controller->recover(containerId, nonLeafCgroup));
    recoveredControllers.insert(controller->name());
  }

  return await(recovers)
    .then(defer(
        PID<Cgroups2IsolatorProcess>(this),
        &Cgroups2IsolatorProcess::____recover,
        containerId,
        recoveredControllers,
        isolate,
        lambda::_1));
}


Future<Nothing> Cgroups2IsolatorProcess::____recover(
    const ContainerID& containerId,
    const hashset<string>& recoveredControllers,
    bool isolate,
    const vector<Future<Nothing>>& futures)
{
  CHECK(!infos.contains(containerId));

  vector<string> errors;
  foreach (const Future<Nothing>& future, futures) {
    if (!future.isReady()) {
      errors.push_back(future.isFailed() ? future.failure() : "discarded");
    }
  }

  if (!errors.empty()) {
    return Failure("Failed to recover controllers: "
                   + strings::join(", ", errors));
  }

  infos[containerId] = Owned<Info>(new Info(
      containerId,
      cgroups2_paths::container(flags.cgroups_root, containerId, false),
      cgroups2_paths::container(flags.cgroups_root, containerId, true),
      isolate));

  infos[containerId]->controllers = recoveredControllers;

  return Nothing();
}


Future<Nothing> Cgroups2IsolatorProcess::isolate(
    const ContainerID& containerId,
    pid_t pid)
{
  if (!infos.contains(containerId)) {
    return Failure("Unknown container '" + stringify(containerId) + "'");
  }

  if (infos[containerId]->isolate) {
    return Nothing();
  }

  vector<Future<Nothing>> isolates;

  // Move the process into the container's cgroup.
  if (infos.contains(containerId)) {
    foreachvalue (const Owned<Controller> controller, controllers) {
      isolates.push_back(controller->isolate(
          containerId,
          infos[containerId]->cgroup,
          pid));
    }
  }

  return await(isolates)
    .then(defer(
        PID<Cgroups2IsolatorProcess>(this),
        &Cgroups2IsolatorProcess::_isolate,
        lambda::_1,
        containerId,
        pid));
}


Future<Nothing> Cgroups2IsolatorProcess::_isolate(
    const vector<Future<Nothing>>& futures,
    const ContainerID& containerId,
    pid_t pid)
{
  vector<string> errors;
  foreach (const Future<Nothing>& future, futures) {
    if (!future.isReady()) {
      errors.push_back(future.isFailed() ? future.failure() : "discarded");
    }
  }

  if (!errors.empty()) {
    return Failure("Failed to prepare controllers: "
                   + strings::join(", ", errors));
  }

  Owned<Info> info = cgroupInfo(containerId);
  if (!info.get()) {
    return Failure(
        "Failed to find cgroup for container '" + stringify(containerId) + "'");
  }

  // At this point, the pid should already be placed in the leaf by linux
  // launcher, no need to assign it ourselves.
  return Nothing();
}


Future<ContainerLimitation> Cgroups2IsolatorProcess::watch(
    const ContainerID& containerId)
{
  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  foreachvalue (const Owned<Controller>& controller, controllers) {
    if (infos[containerId]->controllers.contains(controller->name())) {
      controller->watch(containerId, infos[containerId]->cgroup)
        .onAny(defer(
            PID<Cgroups2IsolatorProcess>(this),
            &Cgroups2IsolatorProcess::_watch,
            containerId,
            lambda::_1));
    }
  }

  return infos[containerId]->limitation.future();
}


void Cgroups2IsolatorProcess::_watch(
    const ContainerID& containerId,
    const Future<ContainerLimitation>& future)
{
  if (!infos.contains(containerId)) {
    return;
  }

  if (future.isPending()) {
    LOG(ERROR) << "Limitation future should be ready or failed";
    return;
  }

  infos[containerId]->limitation.set(future);
}


Future<Nothing> Cgroups2IsolatorProcess::update(
    const ContainerID& containerId,
    const Resources& resourceRequests,
    const google::protobuf::Map<string, Value::Scalar>& resourceLimits)
{
  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  if (!infos[containerId]->isolate) {
    return Failure("Update is not supported for nested containers");
  }

  vector<Future<Nothing>> updates;

  LOG(INFO) << "Updating controllers for cgroup"
            << " '" << infos[containerId]->cgroup << "'";

  foreachvalue (const Owned<Controller>& controller, controllers) {
    if (infos[containerId]->controllers.contains(controller->name())) {
      updates.push_back(controller->update(
          containerId,
          infos[containerId]->cgroup,
          resourceRequests,
          resourceLimits));
    }
  }

  return await(updates)
    .then(defer(
        PID<Cgroups2IsolatorProcess>(this),
        &Cgroups2IsolatorProcess::_update,
        lambda::_1));
}


Future<Nothing> Cgroups2IsolatorProcess::_update(
    const vector<Future<Nothing>>& futures)
{
  vector<string> errors;
  foreach (const Future<Nothing>& future, futures) {
    if (!future.isReady()) {
      errors.push_back(future.isFailed() ? future.failure() : "discarded");
    }
  }

  if (!errors.empty()) {
    return Failure("Failed to update controllers: "
                   + strings::join(", ", errors));
  }

  return Nothing();
}


Future<ResourceStatistics> Cgroups2IsolatorProcess::usage(
    const ContainerID& containerId)
{
  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  vector<Future<ResourceStatistics>> usages;
  foreachvalue (const Owned<Controller>& controller, controllers) {
    if (infos[containerId]->controllers.contains(controller->name())) {
      usages.push_back(controller->usage(
          containerId,
          infos[containerId]->cgroup));
    }
  }

  return await(usages)
    .then([containerId](const vector<Future<ResourceStatistics>>& _usages) {
      ResourceStatistics result;

      foreach (const Future<ResourceStatistics>& statistics, _usages) {
        if (statistics.isReady()) {
          result.MergeFrom(statistics.get());
        } else {
          LOG(WARNING) << "Skipping resource statistic for container "
                       << containerId << " because: "
                       << (statistics.isFailed() ? statistics.failure()
                                                 : "discarded");
        }
      }

      return result;
    });
}


Future<ContainerStatus> Cgroups2IsolatorProcess::status(
    const ContainerID& containerId)
{
  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  // If we are a nested container without isolation,
  // we try to find the status of its ancestor.
  if (!infos[containerId]->isolate) {
    return status(containerId.parent());
  }

  vector<Future<ContainerStatus>> statuses;
  foreachvalue (const Owned<Controller>& controller, controllers) {
    if (infos[containerId]->controllers.contains(controller->name())) {
      statuses.push_back(controller->status(
          containerId,
          infos[containerId]->cgroup));
    }
  }

  return await(statuses)
    .then([containerId](const vector<Future<ContainerStatus>>& _statuses) {
      ContainerStatus result;

      foreach (const Future<ContainerStatus>& status, _statuses) {
        if (status.isReady()) {
          result.MergeFrom(status.get());
        } else {
          LOG(WARNING) << "Skipping status for container " << containerId
                       << " because: "
                       << (status.isFailed() ? status.failure() : "discarded");
        }
      }

      return result;
    });
}


Future<Nothing> Cgroups2IsolatorProcess::cleanup(
    const ContainerID& containerId)
{
  if (!infos.contains(containerId)) {
    VLOG(1) << "Ignoring cleanup request for unknown container " << containerId;
    return Nothing();
  }

  vector<Future<Nothing>> cleanups;
  foreachvalue (const Owned<Controller>& controller, controllers) {
    if (infos[containerId]->controllers.contains(controller->name())) {
      cleanups.push_back(controller->cleanup(
          containerId,
          infos[containerId]->cgroup));
    }
  }

  return await(cleanups)
    .then(defer(
        PID<Cgroups2IsolatorProcess>(this),
        &Cgroups2IsolatorProcess::_cleanup,
        containerId,
        lambda::_1));
}


Future<Nothing> Cgroups2IsolatorProcess::_cleanup(
    const ContainerID& containerId,
    const vector<Future<Nothing>>& futures)
{
  CHECK(infos.contains(containerId));

  vector<string> errors;
  foreach (const Future<Nothing>& future, futures) {
    if (!future.isReady()) {
      errors.push_back(future.isFailed() ? future.failure() : "discarded");
    }
  }

  if (!errors.empty()) {
    return Failure("Failed to cleanup subsystems: "
                   + strings::join(", ", errors));
  }

  if (!cgroups2::exists(infos[containerId]->cgroup)) {
    infos.erase(containerId);
    return Nothing();
  }

  return cgroups2::destroy(infos[containerId]->cgroup)
    .then(defer(
        PID<Cgroups2IsolatorProcess>(this),
        &Cgroups2IsolatorProcess::__cleanup,
        containerId,
        lambda::_1));
}


Future<Nothing> Cgroups2IsolatorProcess::__cleanup(
    const ContainerID& containerId,
    const Future<Nothing>& future)
{
  if (future.isFailed()) {
    return Failure(
        "Failed to destroy cgroup '" + infos[containerId]->cgroup + "': "
        + (future.isFailed() ? future.failure() : "discarded"));
  }

  infos.erase(containerId);

  return Nothing();
}


Owned<Cgroups2IsolatorProcess::Info> Cgroups2IsolatorProcess::cgroupInfo(
    const ContainerID& containerId) const
{
  // `ContainerID`s are hierarchical, where each container id potentially has a
  // parent container id. Here we walk up the hierarchy until we find a
  // container id that has a corresponding info.

  Option<ContainerID> current = containerId;
  while (current.isSome()) {
    Option<Owned<Info>> info = infos.get(*current);
    if (info.isSome()) {
      return *info;
    }

    if (!current->has_parent()) {
      break;
    }
    current = current->parent();
  }

  return nullptr;
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
