#pragma once

#include <string>

#include <tiny_obj_loader.h>

#include "scene_data.h"

namespace scene
{
  // TODO: Remove CPU usage which makes a lot of garbage
  // as we do not support CPU anymore.
  class Scene
  {
    public:
      Scene(const std::string& filepath);
      Scene(const std::string&& filepath);

    public:
      void
      upload(scene::Camera *camera);

      void
      release();

      const inline std::string&
      getSceneName()
      {
        return _filepath;
      }

      const inline struct SceneData *
      getUploadedScenePointer()
      {
        return _d_scene_data;
      }

      inline bool
      ready()
      {
        return _ready;
      }

      inline std::string&
      error()
      {
        return _load_error;
      }

    private:
      void
      upload_gpu(const std::vector<tinyobj::shape_t> &shapes,
        const std::vector<tinyobj::material_t>& materials,
        const tinyobj::attrib_t attrib,
        const std::string& base_folder);

      void
      release_gpu();

    private:
      std::string _filepath;

      bool _uploaded;
      bool _ready;
      std::string _load_error;

      struct scene::Camera _init_camera;

      struct SceneData *_scene_data;
      struct SceneData *_d_scene_data;
  };

} // namespace scene
