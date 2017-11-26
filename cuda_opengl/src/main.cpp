#ifdef _WIN64
# include <windows.h>
#endif

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <cuda_gl_interop.h>

#include <iomanip>
#include <iostream>

#include "driver/cuda_helper.h"
#include "driver/gpu_info.h"
#include "driver/interop.h"
#include "gpu_processor.h"

#include "gui/gui_manager.h"

#include "scene/scene.h"
#include "utils.h"

constexpr unsigned int CUBEMAP_IDX = 2;

static bool g_mouse_trapped = true;

static void
glfw_init(GLFWwindow** window, const int width, const int height)
{
	if (!glfwInit())
  {
    std::cerr << "artracer: failed to initialize GLFW" << std::endl;
    exit(EXIT_FAILURE);
  }

  glfwWindowHint(GLFW_DEPTH_BITS, 0);
  glfwWindowHint(GLFW_STENCIL_BITS, 0);

  glfwWindowHint(GLFW_SRGB_CAPABLE, GL_TRUE);

  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);

  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

  *window = glfwCreateWindow(width, height, "GLFW / CUDA Interop", NULL, NULL);

	if (*window == NULL)
	{
    std::cerr << "artracer: failed to open GLFW window" << std::endl;
		glfwTerminate();
		exit(EXIT_FAILURE);
	}

  glfwMakeContextCurrent(*window);
  gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);
  glfwSwapInterval(0);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
}

static void
glfw_window_size_callback(GLFWwindow* window, int width, int height)
{
	processor::GPUProcessor* const processor =
		(processor::GPUProcessor* const)glfwGetWindowUserPointer(window);

  processor->resize(width, height);
}

void
glfw_key_callback(GLFWwindow* window, int key, int scancode, int action, int mods)
{
  static char keys[1024];

  (void) scancode;
  (void) mods;

  processor::GPUProcessor* const processor =
    (processor::GPUProcessor* const)glfwGetWindowUserPointer(window);

	if (key < 0 || key >= 1024) return;

  if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS && !keys[GLFW_KEY_ESCAPE])
  {
    g_mouse_trapped = !g_mouse_trapped;
    glfwSetInputMode(window, GLFW_CURSOR, g_mouse_trapped ? GLFW_CURSOR_HIDDEN : GLFW_CURSOR_NORMAL);
  }
  else
    processor->setKeyState(key, action != GLFW_RELEASE);

  keys[key] = action != GLFW_RELEASE;
}

void
glfw_mouse_callback(GLFWwindow* window, double xpos, double ypos)
{
	processor::GPUProcessor* const processor =
		(processor::GPUProcessor* const)glfwGetWindowUserPointer(window);

  if (!g_mouse_trapped) return;

	processor->setMoved(true);
	processor->setMousePos((float)xpos, (float)ypos);
}

std::vector<std::string>
buildScenesList(int argc, const char *argv[], unsigned int start)
{
  std::vector<std::string> scenes;

  int nb_scenes = argc - start;
  for (int i = 0; i < nb_scenes ; ++i)
  {
    std::string file = argv[i + start];
    if (!file.empty()) scenes.push_back(file);
  }

  return scenes;
}

int
main(int argc, char* argv[])
{
  if (argc < 3)
  {
    std::cerr << "artracer: missing scene argument.\n";
    std::cerr << "usage: artracer ASSET_FOLDER [SCENE 1] [SCENE2] ..." << std::endl;
    //return 1;
  }
  (void) argv;

  constexpr int ASSET_FOLDER_IDX = 1;
  /*constexpr int WINDOW_W = 1344;
  constexpr int WINDOW_H = 756;*/
  constexpr int WINDOW_W = 960;
  constexpr int WINDOW_H = 540;

  GLFWwindow* window;
  glfw_init(&window, WINDOW_W, WINDOW_H);

  gui::GUIManager::inst()->init(window);
  //scene::Scene scene(argv[1]);

  int width = 0;
  int height = 0;
  glfwGetFramebufferSize(window, &width, &height);
  glfwSetFramebufferSizeCallback(window, glfw_window_size_callback);
  glfwSetCursorPosCallback(window, glfw_mouse_callback);
  glfwSetKeyCallback(window, glfw_key_callback);
  glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);

  // Creates the processor in charge of loading the assets, by creating
  // the scenes from the command line, and running the kernel each loop.
  // DEBUG
  //const char* toto[] = { "toto", "assets", "cubemap/night.jpg", "crate_land.scene", "hut.scene" };
  const char* toto[] = { "toto", "assets", "color_sample.scene", "crate_land.scene", "hut.scene" };
  // END DEBUG
  auto asset_folder = toto[ASSET_FOLDER_IDX];
  std::vector<std::string> scenes;

  scenes = buildScenesList(5, toto, ASSET_FOLDER_IDX + 1);

  processor::GPUProcessor processor(asset_folder, scenes, WINDOW_W, WINDOW_H);
  processor.init(); // This will upload the data.
  glfwSetWindowUserPointer(window, &processor);

  const auto& interop = processor.getInterop();
  double last_time = 0.0;
  double curr_time = 0.0;
  double delta = 0.0;
  double elapsed = 0.0;

  while (!glfwWindowShouldClose(window))
  {
    curr_time = glfwGetTime();
    delta = curr_time - last_time;
    last_time = curr_time;

    ////////////////////////////
    ////      Update      //////
    ////////////////////////////

    processor.update(delta);
    // Binds data to GUI.
    gui::GUIManager::inst()->begin();
    gui::GUIManager::inst()->info(
      processor.getSceneId(), processor.getCubemapId(),
      processor.getSceneItems(), processor.getCubemapItems()
    );
    gui::GUIManager::inst()->camera(processor.getCamera(), 0);

    if (g_mouse_trapped)
      glfwSetCursorPos(window, (double)interop.half_width(), (double)interop.half_height());

    elapsed += delta;

    ////////////////////////////
    ////      Rendering      ///
    ////////////////////////////

    processor.render();
    gui::GUIManager::inst()->render();

		glfwSwapBuffers(window);
		glfwPollEvents();
	}

	//cudaDeviceSynchronize();
	//scene.release();

  gui::GUIManager::inst()->release();

	glfwDestroyWindow(window);
	glfwTerminate();

  cudaDeviceReset();

	exit(EXIT_SUCCESS);
}
