// ======================================================================== //
// Copyright 2009-2019 Intel Corporation                                    //
//                                                                          //
// Licensed under the Apache License, Version 2.0 (the "License");          //
// you may not use this file except in compliance with the License.         //
// You may obtain a copy of the License at                                  //
//                                                                          //
//     http://www.apache.org/licenses/LICENSE-2.0                           //
//                                                                          //
// Unless required by applicable law or agreed to in writing, software      //
// distributed under the License is distributed on an "AS IS" BASIS,        //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. //
// See the License for the specific language governing permissions and      //
// limitations under the License.                                           //
// ======================================================================== //

#include "MPIDistributedDevice.h"
#include "api/ISPCDevice.h"
#include "camera/Camera.h"
#include "common/Data.h"
#include "common/DistributedWorld.h"
#include "common/Group.h"
#include "common/Instance.h"
#include "common/MPICommon.h"
#include "fb/DistributedFrameBuffer.h"
#include "geometry/GeometricModel.h"
#include "lights/Light.h"
#include "ospcommon/tasking/tasking_system_init.h"
#include "ospcommon/utility/getEnvVar.h"
#include "render/DistributedLoadBalancer.h"
#include "render/RenderTask.h"
#include "render/distributed/DistributedRaycast.h"
#include "volume/VolumetricModel.h"
#include "volume/transferFunction/TransferFunction.h"

namespace ospray {
namespace mpi {

// Helper functions ///////////////////////////////////////////////////////

template <typename OSPRAY_TYPE, typename API_TYPE>
inline API_TYPE createLocalObject(const char *type)
{
  auto *instance = OSPRAY_TYPE::createInstance(type);
  return (API_TYPE)instance;
}

template <typename OSPRAY_TYPE, typename API_TYPE>
inline API_TYPE createDistributedObject(const char *type)
{
  auto *instance = OSPRAY_TYPE::createInstance(type);

  ObjectHandle handle;
  handle.assign(instance);

  return (API_TYPE)(int64)handle;
}

static void embreeErrorFunc(void *, const RTCError code, const char *str)
{
  postStatusMsg() << "#osp: embree internal error " << code << " : " << str;
  throw std::runtime_error("embree internal error '" + std::string(str) + "'");
}

// MPIDistributedDevice definitions ///////////////////////////////////////

MPIDistributedDevice::MPIDistributedDevice() {}

MPIDistributedDevice::~MPIDistributedDevice()
{
  if (maml::isRunning()) {
    maml::stop();
  }
  if (shouldFinalizeMPI) {
    try {
      MPI_CALL(Finalize());
    } catch (...) {
      // TODO: anything to do here? try-catch added to silence a warning...
    }
  }
}

void MPIDistributedDevice::commit()
{
  Device::commit();

  if (!initialized) {
    int _ac = 1;
    const char *_av[] = {"ospray_mpi_distributed_device"};

    auto *setComm =
        static_cast<MPI_Comm *>(getParam<void *>("worldCommunicator", nullptr));
    shouldFinalizeMPI = mpicommon::init(&_ac, _av, setComm == nullptr);

    if (setComm)
      mpicommon::worker.setTo(*setComm);
    else
      mpicommon::worker = mpicommon::world;

    auto &embreeDevice = api::ISPCDevice::embreeDevice;

    embreeDevice = rtcNewDevice(generateEmbreeDeviceCfg(*this).c_str());
    rtcSetDeviceErrorFunction(embreeDevice, embreeErrorFunc, nullptr);
    RTCError erc = rtcGetDeviceError(embreeDevice);
    if (erc != RTC_ERROR_NONE) {
      // why did the error function not get called !?
      postStatusMsg() << "#osp:init: embree internal error number " << erc;
      assert(erc == RTC_ERROR_NONE);
    }
    initialized = true;

    auto OSPRAY_FORCE_COMPRESSION =
        utility::getEnvVar<int>("OSPRAY_FORCE_COMPRESSION");
    // Turning on the compression past 64 ranks seems to be a good
    // balancing point for cost of compressing vs. performance gain
    auto enableCompression = OSPRAY_FORCE_COMPRESSION.value_or(
        mpicommon::workerSize() >= OSP_MPI_COMPRESSION_THRESHOLD);

    // TODO WILL: This will be a problem with the offload/distrib device
    // combination where maml/messaging gets init with the wrong
    // communicator
    maml::init(enableCompression);
    messaging::init(mpicommon::worker);
    maml::start();
  }

  TiledLoadBalancer::instance = make_unique<staticLoadBalancer::Distributed>();
}

OSPFrameBuffer MPIDistributedDevice::frameBufferCreate(
    const vec2i &size, const OSPFrameBufferFormat mode, const uint32 channels)
{
  ObjectHandle handle;
  auto *instance = new DistributedFrameBuffer(size, handle, mode, channels);
  handle.assign(instance);
  return (OSPFrameBuffer)(int64)handle;
}

const void *MPIDistributedDevice::frameBufferMap(
    OSPFrameBuffer _fb, OSPFrameBufferChannel channel)
{
  if (!mpicommon::IamTheMaster())
    throw std::runtime_error("Can only map framebuffer on the master!");

  auto *fb = lookupDistributedObject<FrameBuffer>(_fb);

  return fb->mapBuffer(channel);
}

void MPIDistributedDevice::frameBufferUnmap(
    const void *mapped, OSPFrameBuffer _fb)
{
  auto *fb = lookupDistributedObject<FrameBuffer>(_fb);
  fb->unmap(mapped);
}

void MPIDistributedDevice::resetAccumulation(OSPFrameBuffer _fb)
{
  auto *fb = lookupDistributedObject<FrameBuffer>(_fb);
  fb->clear();
}

OSPGroup MPIDistributedDevice::newGroup()
{
  return (OSPGroup) new Group;
}

OSPInstance MPIDistributedDevice::newInstance(OSPGroup _group)
{
  auto *group = (Group *)_group;
  auto *instance = new Instance(group);
  return (OSPInstance)instance;
}

OSPWorld MPIDistributedDevice::newWorld()
{
  ObjectHandle handle;
  auto *instance = new DistributedWorld;
  handle.assign(instance);
  return (OSPWorld)(int64)(handle);
}

OSPData MPIDistributedDevice::newSharedData(const void *sharedData,
    OSPDataType type,
    const vec3i &numItems,
    const vec3l &byteStride)
{
  return (OSPData) new Data(sharedData, type, numItems, byteStride);
}

OSPData MPIDistributedDevice::newData(OSPDataType type, const vec3i &numItems)
{
  return (OSPData) new Data(type, numItems);
}

void MPIDistributedDevice::copyData(
    const OSPData source, OSPData destination, const vec3i &destinationIndex)
{
  Data *dst = (Data *)destination;
  dst->copy(*(Data *)source, destinationIndex);
}

void MPIDistributedDevice::setVoidPtr(
    OSPObject _object, const char *bufName, void *v)
{
  auto *object = lookupObject<ManagedObject>(_object);
  object->setParam(bufName, v);
}

void MPIDistributedDevice::setString(
    OSPObject _object, const char *bufName, const char *s)
{
  auto *object = lookupObject<ManagedObject>(_object);
  object->setParam<std::string>(bufName, s);
}

int MPIDistributedDevice::loadModule(const char *name)
{
  return loadLocalModule(name);
}

void MPIDistributedDevice::setBool(
    OSPObject _object, const char *bufName, const bool b)
{
  auto *object = lookupObject<ManagedObject>(_object);
  object->setParam(bufName, b);
}

void MPIDistributedDevice::setFloat(
    OSPObject _object, const char *bufName, const float f)
{
  auto *object = lookupObject<ManagedObject>(_object);
  object->setParam(bufName, f);
}

void MPIDistributedDevice::setInt(
    OSPObject _object, const char *bufName, const int i)
{
  auto *object = lookupObject<ManagedObject>(_object);
  object->setParam(bufName, i);
}

void MPIDistributedDevice::setVec2f(
    OSPObject _object, const char *bufName, const vec2f &v)
{
  auto *object = lookupObject<ManagedObject>(_object);
  object->setParam(bufName, v);
}

void MPIDistributedDevice::setVec3f(
    OSPObject _object, const char *bufName, const vec3f &v)
{
  auto *object = lookupObject<ManagedObject>(_object);
  object->setParam(bufName, v);
}

void MPIDistributedDevice::setVec4f(
    OSPObject _object, const char *bufName, const vec4f &v)
{
  auto *object = lookupObject<ManagedObject>(_object);
  object->setParam(bufName, v);
}

void MPIDistributedDevice::setVec2i(
    OSPObject _object, const char *bufName, const vec2i &v)
{
  auto *object = lookupObject<ManagedObject>(_object);
  object->setParam(bufName, v);
}

void MPIDistributedDevice::setVec3i(
    OSPObject _object, const char *bufName, const vec3i &v)
{
  auto *object = lookupObject<ManagedObject>(_object);
  object->setParam(bufName, v);
}

void MPIDistributedDevice::setVec4i(
    OSPObject _object, const char *bufName, const vec4i &v)
{
  auto *object = lookupObject<ManagedObject>(_object);
  object->setParam(bufName, v);
}

void MPIDistributedDevice::setObject(
    OSPObject _object, const char *bufName, OSPObject _value)
{
  auto *object = lookupObject<ManagedObject>(_object);
  auto *value = lookupObject<ManagedObject>(_value);
  object->setParam(bufName, value);
}

void MPIDistributedDevice::setBox1f(
    OSPObject _object, const char *bufName, const box1f &v)
{
  auto *object = lookupObject<ManagedObject>(_object);
  object->setParam(bufName, v);
}

void MPIDistributedDevice::setBox1i(
    OSPObject _object, const char *bufName, const box1i &v)
{
  auto *object = lookupObject<ManagedObject>(_object);
  object->setParam(bufName, v);
}

void MPIDistributedDevice::setBox2f(
    OSPObject _object, const char *bufName, const box2f &v)
{
  auto *object = lookupObject<ManagedObject>(_object);
  object->setParam(bufName, v);
}

void MPIDistributedDevice::setBox2i(
    OSPObject _object, const char *bufName, const box2i &v)
{
  auto *object = lookupObject<ManagedObject>(_object);
  object->setParam(bufName, v);
}

void MPIDistributedDevice::setBox3f(
    OSPObject _object, const char *bufName, const box3f &v)
{
  auto *object = lookupObject<ManagedObject>(_object);
  object->setParam(bufName, v);
}

void MPIDistributedDevice::setBox3i(
    OSPObject _object, const char *bufName, const box3i &v)
{
  auto *object = lookupObject<ManagedObject>(_object);
  object->setParam(bufName, v);
}

void MPIDistributedDevice::setBox4f(
    OSPObject _object, const char *bufName, const box4f &v)
{
  auto *object = lookupObject<ManagedObject>(_object);
  object->setParam(bufName, v);
}

void MPIDistributedDevice::setBox4i(
    OSPObject _object, const char *bufName, const box4i &v)
{
  auto *object = lookupObject<ManagedObject>(_object);
  object->setParam(bufName, v);
}

void MPIDistributedDevice::setLinear3f(
    OSPObject _object, const char *bufName, const linear3f &v)
{
  auto *object = lookupObject<ManagedObject>(_object);
  object->setParam(bufName, v);
}

void MPIDistributedDevice::setAffine3f(
    OSPObject _object, const char *bufName, const affine3f &v)
{
  auto *object = lookupObject<ManagedObject>(_object);
  object->setParam(bufName, v);
}

OSPImageOperation MPIDistributedDevice::newImageOp(const char *type)
{
  return createLocalObject<ImageOp, OSPImageOperation>(type);
}

OSPRenderer MPIDistributedDevice::newRenderer(const char *type)
{
  return createDistributedObject<Renderer, OSPRenderer>(type);
}

OSPCamera MPIDistributedDevice::newCamera(const char *type)
{
  return createLocalObject<Camera, OSPCamera>(type);
}

OSPVolume MPIDistributedDevice::newVolume(const char *type)
{
  return createLocalObject<Volume, OSPVolume>(type);
}

OSPGeometry MPIDistributedDevice::newGeometry(const char *type)
{
  return createLocalObject<Geometry, OSPGeometry>(type);
}

OSPGeometricModel MPIDistributedDevice::newGeometricModel(OSPGeometry _geom)
{
  auto *geom = lookupObject<Geometry>(_geom);
  auto *instance = new GeometricModel(geom);
  return (OSPGeometricModel)instance;
}

OSPVolumetricModel MPIDistributedDevice::newVolumetricModel(OSPVolume _vol)
{
  auto *volume = lookupObject<Volume>(_vol);
  auto *instance = new VolumetricModel(volume);
  return (OSPVolumetricModel)instance;
}

OSPMaterial MPIDistributedDevice::newMaterial(
    const char *renderer_type, const char *material_type)
{
  auto *instance = Material::createInstance(renderer_type, material_type);
  return (OSPMaterial)instance;
}

OSPTransferFunction MPIDistributedDevice::newTransferFunction(const char *type)
{
  return createLocalObject<TransferFunction, OSPTransferFunction>(type);
}

OSPLight MPIDistributedDevice::newLight(const char *type)
{
  return createLocalObject<Light, OSPLight>(type);
}

float MPIDistributedDevice::renderFrame(OSPFrameBuffer _fb,
    OSPRenderer _renderer,
    OSPCamera _camera,
    OSPWorld _world)
{
  mpicommon::barrier(mpicommon::worker.comm).wait();
  auto f = renderFrameAsync(_fb, _renderer, _camera, _world);
  wait(f, OSP_FRAME_FINISHED);
  release(f);
  return getVariance(_fb);
}

OSPFuture MPIDistributedDevice::renderFrameAsync(OSPFrameBuffer _fb,
    OSPRenderer _renderer,
    OSPCamera _camera,
    OSPWorld _world)
{
  mpicommon::barrier(mpicommon::worker.comm).wait();
  auto *fb = lookupDistributedObject<FrameBuffer>(_fb);
  auto *renderer = lookupDistributedObject<Renderer>(_renderer);
  auto *camera = lookupObject<Camera>(_camera);
  auto *world = lookupObject<DistributedWorld>(_world);

  fb->setCompletedEvent(OSP_NONE_FINISHED);

  fb->refInc();
  renderer->refInc();
  camera->refInc();
  world->refInc();

  auto *f = new RenderTask(fb, [=]() {
    float result = renderer->renderFrame(fb, camera, world);

    fb->refDec();
    renderer->refDec();
    camera->refDec();
    world->refDec();

    return result;
  });

  return (OSPFuture)f;
}

int MPIDistributedDevice::isReady(OSPFuture _task, OSPSyncEvent event)
{
  auto *task = (Future *)_task;
  return task->isFinished(event);
}

void MPIDistributedDevice::wait(OSPFuture _task, OSPSyncEvent event)
{
  auto *task = (Future *)_task;
  task->wait(event);
}

void MPIDistributedDevice::cancel(OSPFuture _task)
{
  auto *task = (Future *)_task;
  return task->cancel();
}

float MPIDistributedDevice::getProgress(OSPFuture _task)
{
  auto *task = (Future *)_task;
  return task->getProgress();
}

float MPIDistributedDevice::getVariance(OSPFrameBuffer _fb)
{
  auto *fb = lookupDistributedObject<FrameBuffer>(_fb);
  return fb->getVariance();
}

void MPIDistributedDevice::commit(OSPObject _object)
{
  auto *object = lookupObject<ManagedObject>(_object);
  object->commit();
}

void MPIDistributedDevice::removeParam(OSPObject _object, const char *name)
{
  auto *object = lookupObject<ManagedObject>(_object);
  object->removeParam(name);
}

void MPIDistributedDevice::release(OSPObject _obj)
{
  if (!_obj)
    return;

  auto &handle = reinterpret_cast<ObjectHandle &>(_obj);
  auto *object = lookupObject<ManagedObject>(_obj);
  if (object->useCount() == 1 && handle.defined()) {
    handle.freeObject();
  } else {
    auto *obj = (ManagedObject *)_obj;
    obj->refDec();
  }
}

void MPIDistributedDevice::retain(OSPObject _obj)
{
  auto *object = lookupObject<ManagedObject>(_obj);
  object->refInc();
}

OSPTexture MPIDistributedDevice::newTexture(const char *type)
{
  return createLocalObject<Texture, OSPTexture>(type);
}

OSP_REGISTER_DEVICE(MPIDistributedDevice, mpi_distributed_device);
OSP_REGISTER_DEVICE(MPIDistributedDevice, mpi_distributed);

} // namespace mpi
} // namespace ospray

extern "C" void ospray_init_module_mpi_distributed()
{
  std::cout << "Loading MPI Distributed Device module\n";
}
