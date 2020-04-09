// Copyright 2018-2019 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <GLFW/glfw3.h>
#include <functional>
#include "ArcballCamera.h"
// ospcommon
#include "ospcommon/containers/TransactionalBuffer.h"
#include "ospcommon/math/box.h"
#include "ospcommon/math/vec.h"
// ospray
#include "ospray/ospray.h"
#include "ospray/ospray_cpp.h"

struct WindowState
{
  bool quit;
  bool cameraChanged;
  bool fbSizeChanged;
  int spp;
  ospcommon::math::vec2i windowSize;
  ospcommon::math::vec3f eyePos;
  ospcommon::math::vec3f lookDir;
  ospcommon::math::vec3f upDir;

  WindowState();
};

class GLFWDistribOSPRayWindow
{
 public:
  GLFWDistribOSPRayWindow(const ospcommon::math::vec2i &windowSize,
      const ospcommon::math::box3f &worldBounds,
      ospray::cpp::World world,
      ospray::cpp::Renderer renderer);

  ~GLFWDistribOSPRayWindow();

  static GLFWDistribOSPRayWindow *getActiveWindow();

  ospray::cpp::World getWorld();
  void setWorld(ospray::cpp::World newWorld);

  void resetAccumulation();

  void registerDisplayCallback(
      std::function<void(GLFWDistribOSPRayWindow *)> callback);

  void registerImGuiCallback(std::function<void()> callback);

  void mainLoop();

  void addObjectToCommit(OSPObject obj);

 protected:
  void reshape(const ospcommon::math::vec2i &newWindowSize);
  void motion(const ospcommon::math::vec2f &position);
  void display();
  void startNewOSPRayFrame();
  void waitOnOSPRayFrame();
  void updateTitleBar();

  static GLFWDistribOSPRayWindow *activeWindow;

  ospcommon::math::vec2i windowSize;
  ospcommon::math::box3f worldBounds;
  ospray::cpp::World world = nullptr;
  ospray::cpp::Renderer renderer = nullptr;

  int mpiRank = -1;
  int mpiWorldSize = -1;

  // GLFW window instance
  GLFWwindow *glfwWindow = nullptr;

  // Arcball camera instance
  std::unique_ptr<ArcballCamera> arcballCamera;

  // OSPRay objects managed by this class
  ospray::cpp::Camera camera = nullptr;
  ospray::cpp::FrameBuffer framebuffer = nullptr;
  ospray::cpp::Future currentFrame = nullptr;

  // List of OSPRay handles to commit before the next frame
  ospcommon::containers::TransactionalBuffer<OSPObject> objectsToCommit;

  // OpenGL framebuffer texture
  GLuint framebufferTexture = 0;

  // optional registered display callback, called before every display()
  std::function<void(GLFWDistribOSPRayWindow *)> displayCallback;

  // toggles display of ImGui UI, if an ImGui callback is provided
  bool showUi = true;

  // optional registered ImGui callback, called during every frame to build UI
  std::function<void()> uiCallback;

  // FPS measurement of last frame
  float latestFPS{0.f};

  // The window state to be sent out over MPI to the other rendering processes
  WindowState windowState;
};
