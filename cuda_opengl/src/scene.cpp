#include <algorithm>

#include <ctype.h>
#include <cuda.h>
#include <cuda_runtime.h>

#include <glm/geometric.hpp>
#include <fstream>
#include <iostream>

#define _USE_MATH_DEFINES
#include <math.h>

#include <sstream>
#include <string>

#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>

#include <unordered_map>

#include "driver/cuda_helper.h"
#include "material_loader.h"

#define TINYOBJLOADER_IMPLEMENTATION
#include "scene.h"

#include "texture_utils.h"

namespace scene
{
  namespace
  {
    using ShapeVector = std::vector<tinyobj::shape_t>;
    using MaterialVector = std::vector<tinyobj::material_t>;
    using Real = tinyobj::real_t;

    /// <summary>
    /// Checks whether a string is an hexadecimal number of not.
    /// </summary>
    /// <param name="s">String to check</param>
    /// <returns>
    ///   <c>true</c> if the specified string is hexa; otherwise, <c>false</c>.
    /// </returns>
    bool isHexa(const std::string& s)
    {
      return s.compare(0, 2, "0x") == 0
        && s.size() > 2
        && s.find_first_not_of("0123456789abcdefABCDEF", 2) == std::string::npos;
    }

    float *createUnitTexture(unsigned int nb_chan, unsigned int color,
      unsigned int nb_faces)
    {
      static const unsigned int DEFAULT_SIZE = 1;

      float r = ((color >> 16) & 0xFF) / 255.0f;
      float g = ((color >> 8) & 0xFF) / 255.0f;
      float b = (color & 0xFF) / 255.0f;

      unsigned int nb_elt = DEFAULT_SIZE * DEFAULT_SIZE * nb_chan;
      float *img = new float[nb_elt * nb_faces];
      for (unsigned int n = 0; n < nb_faces; ++n)
      {
        for (unsigned int i = 0; i < nb_elt; i += nb_chan)
        {
          img[n * nb_elt + i] = r;
          img[n * nb_elt + i + 1] = g;
          img[n * nb_elt + i + 2] = b;
          img[n * nb_elt + i + 3] = 0.0;
        }
      }
      return img;
    }

    bool parse_double3(glm::vec3 &out, std::stringstream& iss)
    {
      if (iss.peek() == std::char_traits<char>::eof())
        return false;

      std::string token;

      if ((iss >> out.x).peek() == std::char_traits<char>::eof())
        return false;
      if ((iss >> out.y).peek() == std::char_traits<char>::eof())
        return false;

      iss >> out.z;
      return true;
    }

    bool parse_double3(tinyobj::real_t out[3], std::stringstream& iss)
    {
      if (iss.peek() == std::char_traits<char>::eof())
        return false;

      std::string token;

      if ((iss >> out[0]).peek() == std::char_traits<char>::eof())
        return false;
      if ((iss >> out[1]).peek() == std::char_traits<char>::eof())
        return false;

      iss >> out[2];
      return true;
    }

    bool parse_camera(scene::Camera &cam, std::stringstream& iss)
    {
      if (!parse_double3(cam.position, iss)) return false;
      if (!parse_double3(cam.u, iss)) return false;
      if (!parse_double3(cam.v, iss)) return false;

      if (iss.peek() == std::char_traits<char>::eof()) return false;

      cam.u = glm::normalize(cam.u);
      cam.v = glm::normalize(cam.v);

      iss >> cam.fov_x;
      cam.fov_x = (cam.fov_x * M_PI) / 180.0;
      cam.dir = glm::cross(cam.u, cam.v);
      return true;
    }

    void parse_scene(std::string filename, scene::SceneData &out_scene,
      scene::Camera &cam, std::string& objfile, std::string& cubemapfile)
    {
      std::ifstream file;
      file.open(filename);
      if (!file.is_open())
        throw std::runtime_error("parse_scene(): failed to open '" + filename + "'");

      // Creates default camera, used when no camera is found in the .scene file
      cam.u = glm::vec3(1.0, 0.0, 0.0);
      cam.v = glm::vec3(0.0, -1.0, 0.0);
      cam.fov_x = (90.0 * M_PI) / 180.0;
      cam.dir = glm::cross(cam.u, cam.v);

      // Contains every lights. Because we do not use vector
      // on CUDA, we will need to make a deep copy of it.
      std::vector<LightProp> light_vec;

      std::string line;
      std::string token;
      while (std::getline(file, line))
      {
        if (line.empty() || line[0] == '#') continue;

        std::stringstream iss(line);
        iss >> token;

        if (token == "p_light")
        {
          LightProp plight;
          if (!parse_double3(plight.vec, iss))
            throw std::runtime_error("parse_scene(): error parsing p_light vector.");
          if (!parse_double3(plight.color, iss))
            throw std::runtime_error("parse_scene(): error parsing p_light color.");
          if (iss.peek() == std::char_traits<char>::eof())
            throw std::runtime_error("parse_scene(): error parsing p_light emission.");
          iss >> plight.emission;
          if (iss.peek() == std::char_traits<char>::eof())
            throw std::runtime_error("parse_scene(): error parsing p_light radius.");
          iss >> plight.radius;
          light_vec.push_back(plight);
        }
        else if (token == "cubemap")
        {
          if (iss.peek() == std::char_traits<char>::eof())
            throw std::runtime_error("parse_scene(): error parsing cubemap file name.");
          iss >> cubemapfile;
        }
        else if (token == "scene")
        {
          if (iss.peek() == std::char_traits<char>::eof())
            throw std::runtime_error("parse_scene(): error parsing scene file name.");
          iss >> objfile;
        }
        else if (token == "camera")
        {
          if (!parse_camera(cam, iss))
            throw std::runtime_error("parse_scene(): error parsing the camera.");
        }
      }

      if (light_vec.size() == 0)
        return;

      // Copies lights back to CUDA
      const LightProp *lights = &light_vec[0];
      size_t nb_bytes_lights = light_vec.size() * sizeof(LightProp);
      out_scene.lights.size = light_vec.size();
      cudaMalloc(&out_scene.lights.data, nb_bytes_lights);
      cudaThrowError();
      cudaMemcpy(out_scene.lights.data, lights, nb_bytes_lights,
         cudaMemcpyHostToDevice);
      cudaThrowError();
    }

    /// <summary>
    /// Uploads every materials.
    /// </summary>
    /// <param name="materials">Materials obtained from TinyObjLoader.</param>
    /// <param name="d_materials">Materials storage for the GPU.</param>
    void upload_materials(const MaterialVector &materials,
                          scene::SceneData *scene,
                          const std::string& base_folder)
    {
      MaterialLoader mat_loader(materials, base_folder);

      std::vector<scene::Texture> cpu_textures;
      std::vector<scene::Material> cpu_mat;

      mat_loader.load(cpu_textures, cpu_mat);

      std::vector<scene::Texture> gpu_textures(cpu_textures.size());

      // Uploads every textures to the GPU
      for (size_t i = 0; i < cpu_textures.size(); ++i)
      {
        const scene::Texture &cpu_tex = cpu_textures[i];
        scene::Texture &gpu_tex = gpu_textures[i];
        gpu_tex.w = cpu_tex.w;
        gpu_tex.h = cpu_tex.h;
        gpu_tex.nb_chan = cpu_tex.nb_chan;

        size_t nb_bytes = cpu_tex.w * cpu_tex.h * cpu_tex.nb_chan * sizeof(float);
        cudaMalloc(&gpu_tex.data, nb_bytes);
        cudaThrowError();
        cudaMemcpy(gpu_tex.data, cpu_tex.data, nb_bytes, cudaMemcpyHostToDevice);
      }
      if (gpu_textures.size())
      {
        cudaMalloc(&scene->textures.data, cpu_textures.size() * sizeof(scene::Texture));
        cudaThrowError();
        cudaMemcpy(scene->textures.data, &gpu_textures[0], cpu_textures.size() * sizeof(scene::Texture), cudaMemcpyHostToDevice);
      }

      // Uploads every materials to the GPU
      if (cpu_mat.size())
      {
        cudaMalloc(&scene->materials.data, cpu_mat.size() * sizeof(scene::Material));
        cudaThrowError();
        cudaMemcpy(scene->materials.data, &cpu_mat[0], cpu_mat.size() * sizeof(scene::Material), cudaMemcpyHostToDevice);
      }

      scene->materials.size = cpu_mat.size();
      scene->textures.size = cpu_mat.size();
    }

    void upload_meshes(const ShapeVector &shapes,
      const tinyobj::attrib_t &attrib, Buffer<Mesh> &out_meshes)
    {
      /// The Mesh structure looks like:
      /// {
      ///    tinyobj::index_t *indices;
      ///    size_t nb_indices;
      ///    int *material_ids;
      /// }
      /// `indices' and `material_idx' should also be allocated.
      size_t nb_meshes = shapes.size();

      // Contains inner pointers allocated on the GPU.
      std::vector<Mesh> gpu_meshes(nb_meshes);

      for (size_t i = 0; i < nb_meshes; ++i)
      {
        auto& mesh = shapes[i].mesh;
        auto nb_indices = mesh.indices.size();

        size_t nb_faces = nb_indices / 3;

        Mesh &gpu_mesh = gpu_meshes[i];
        gpu_mesh.faces.size = nb_faces;

        // Creates the faces on the CPU. This is really gross regarding memory
        // consumption, but this will really speed up the intersection process
        // thanks to a better cache efficiency.
        std::vector<Face> faces(nb_faces);
        for (size_t i = 0; i < nb_indices; i += 3)
        {
          auto& face = faces[i / 3];
          for (size_t v = 0; v < 3; ++v)
          {
            tinyobj::index_t idx = mesh.indices[i + v];
            // Saves vertex
            face.vertices[v] = glm::vec3(
              attrib.vertices[3 * idx.vertex_index],
              attrib.vertices[3 * idx.vertex_index + 1],
              attrib.vertices[3 * idx.vertex_index + 2]
            );
            // Saves normal
            face.normals[v] = glm::vec3(
              attrib.normals[3 * idx.normal_index],
              attrib.normals[3 * idx.normal_index + 1],
              attrib.normals[3 * idx.normal_index + 2]
            );
            // Saves normal
            face.texcoords[v] = glm::vec2(
              attrib.texcoords[2 * idx.texcoord_index],
              attrib.texcoords[2 * idx.texcoord_index + 1]
            );
          }
          face.material_id = mesh.material_ids[i / 3];

          // Computes a unique tangent for the whole face
          glm::vec3 edge1 = face.vertices[1] - face.vertices[0];
          glm::vec3 edge2 = face.vertices[2] - face.vertices[0];
          glm::vec2 delta_uv1 = face.texcoords[1] - face.texcoords[0];
          glm::vec2 delta_uv2 = face.texcoords[2] - face.texcoords[0];

          float f = 1.0f / (delta_uv1.x * delta_uv2.y - delta_uv2.x * delta_uv1.y);

          face.tangent.x = f * (delta_uv2.y * edge1.x - delta_uv1.y * edge2.x);
          face.tangent.y = f * (delta_uv2.y * edge1.y - delta_uv1.y * edge2.y);
          face.tangent.z = f * (delta_uv2.y * edge1.z - delta_uv1.y * edge2.z);
        }

        if (faces.size() == 0) continue;

        // Uploads faces to the GPU
        size_t nb_byte_faces = gpu_mesh.faces.size * sizeof(Face);
        cudaMalloc(&gpu_mesh.faces.data, nb_byte_faces);
        cudaThrowError();
        cudaMemcpy(gpu_mesh.faces.data, &faces[0], nb_byte_faces,
          cudaMemcpyHostToDevice);
        cudaThrowError();
      }

      size_t nb_byte_meshes = nb_meshes * sizeof(Mesh);
      out_meshes.size = nb_meshes;
      cudaMalloc(&out_meshes.data, nb_byte_meshes);
      cudaThrowError();
      cudaMemcpy(out_meshes.data, &gpu_meshes[0], nb_byte_meshes,
        cudaMemcpyHostToDevice);
      cudaThrowError();
    }

    void upload_cubemap(const std::string &path, SceneData *out_scene)
    {
      static const unsigned int NB_COMP = 4;
      static const unsigned int NB_FACES = 6;
      static const unsigned int DEFAULT_COLOR = 0x05070A;

      // This pointer will contain the image data, either loaded
      // on the disk, or created by using a constant color.
      float *img = nullptr;
      unsigned int size = 1;

      // This is used to free the pointer correctly,
      // using either the delete operator, or the
      // stbi_image_free call.
      float *loaded = nullptr;

      // No cubemap was provided with the scene, we will
      // either use a given hexadecimal color, or use the default color.
      if (path.empty() || isHexa(path))
      {
        img = createUnitTexture(
          NB_COMP,
          path.empty() ? DEFAULT_COLOR : strtol(path.c_str(), NULL, 16),
          NB_FACES
        );
      }
      // A path to a cubemap has been provided, we load it and extract
      // each face from the cubecross.
      else
      {
        std::string error;
        int w, h, nb_chan;
        loaded = stbi_loadf(path.c_str(), &w, &h, &nb_chan, STBI_default);

        size = w / 4;

        // An internal error occured in stbi_loadf.
        if (!loaded)
          error = "unknown error " + path;
        // Width and height are not the same, the cubecross can not be valid.
        else if (size != h / 3)
          error = "width and height are not the same";
        // NPOT.
        else if (size & (size - 1))
          error = "size should be a power of 2";

        // No error has occured when loading the cubemap, we
        // can upload it safely.
        if (error.empty())
        {
          // CUDA cubemap textures expect the data to lay out as follows:
          // +x / -x / +y / -y / +z / -z
          img = new float[size * size * NB_COMP * NB_FACES];
          // Sends +/- X faces
          float *tmp = texture::append_cube_faces(
            img, loaded, w, 0, nb_chan, NB_COMP, true, true
          );
          // Sends +/- Y faces
          tmp = texture::append_cube_faces(
            tmp, loaded, w, 1, nb_chan, NB_COMP, false, false
          );
          // Sends +/- Z faces
          texture::append_cube_faces(
            tmp, loaded, w, 1, nb_chan, NB_COMP, true, false
          );
        }
        else
        {
          std::cerr << "arttracer: cubemap loading fail: " << error << std::endl;
          img = createUnitTexture(NB_COMP, DEFAULT_COLOR, NB_FACES);
        }

        //fail to load cubemap :
      }
      /*float *tmp = new float[6 * 2 * 2 * 4];
      size_t nb_bytes = 2 * 2 * 4;
      for (size_t i = 0; i < nb_bytes; i += 4)
      {
        tmp[0 * nb_bytes + i] = 1.0;
        tmp[0 * nb_bytes + i + 1] = 0;
        tmp[0 * nb_bytes + i + 2] = 0;
      }
      // FACE 2 +y
      for (size_t i = 0; i < nb_bytes; i += 4)
      {
        tmp[2 * nb_bytes + i] = 0;
        tmp[2 * nb_bytes + i + 1] = 1.0;
        tmp[2 * nb_bytes + i + 2] = 0;
      }
      // FACE 4 +z
      for (size_t i = 0; i < nb_bytes; i += 4)
      {
        tmp[4 * nb_bytes + i] = 0;
        tmp[4 * nb_bytes + i + 1] = 0;
        tmp[4 * nb_bytes + i + 2] = 1.0;
      }*/
      // END DEBUG

      out_scene->cubemap_desc = cudaCreateChannelDesc(32, 32, 32, 32, cudaChannelFormatKindFloat);
      cudaThrowError();

      cudaMalloc3DArray(&out_scene->cubemap, &out_scene->cubemap_desc,
        make_cudaExtent(size, size, NB_FACES), cudaArrayCubemap);
      cudaThrowError();

      cudaMemcpy3DParms myparms = { 0 };
      myparms.srcPos = make_cudaPos(0, 0, 0);
      myparms.dstPos = make_cudaPos(0, 0, 0);
      myparms.srcPtr = make_cudaPitchedPtr(img, size * sizeof(float) * NB_COMP, size, size);
      myparms.dstArray = out_scene->cubemap;
      myparms.extent = make_cudaExtent(size, size, NB_FACES);
      myparms.kind = cudaMemcpyHostToDevice;
      cudaMemcpy3D(&myparms);
      cudaThrowError();

      delete img;
      if (loaded) stbi_image_free(loaded);
    }

  }

  Scene::Scene(const std::string& filepath)
        : _filepath(filepath)
        , _uploaded(false)
        , _ready(false)
        , _d_scene_data(nullptr)
  { }

  Scene::Scene(const std::string&& filepath)
        : _filepath(filepath)
        , _uploaded(false)
        , _ready{ false }
        , _d_scene_data(nullptr)
  { }

  void
  Scene::upload(bool is_cpu)
  {
    if (_uploaded)
      return;

    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    tinyobj::attrib_t attrib;

    // _sceneData is allocated on the heap,
    // and allows to handle cudaMalloc & cudaFree
    _scene_data = new SceneData;
    _camera = new Camera;

    std::string objfilepath;
    std::string cubemap_path;
    std::string base_dir = "";
    std::string mtl_dir = "";
    std::string full_obj_path = "";

    parse_scene(_filepath, *_scene_data, *_camera, objfilepath, cubemap_path);

    std::string::size_type pos = _filepath.find_last_of('/');
    if (pos != std::string::npos)
    {
      base_dir = _filepath.substr(0, pos) + "/";
      mtl_dir = base_dir;
      full_obj_path = base_dir + objfilepath;
      if (!cubemap_path.empty() && !isHexa(cubemap_path))
        cubemap_path = base_dir + cubemap_path;
    }

    // Extracts basedir to find MTL if any.
    pos = objfilepath.find_last_of('/');
    if (pos != std::string::npos)
    {
      mtl_dir = base_dir + "/" + objfilepath.substr(0, pos) + "/";
    }

    _ready = tinyobj::LoadObj(&attrib, &shapes,
        &materials, &_load_error, full_obj_path.c_str(), mtl_dir.c_str());

    if (!_ready)
    {
      delete _scene_data;
      delete _camera;
      return;
    }

    upload_gpu(shapes, materials, attrib, cubemap_path, mtl_dir);
    _uploaded = true;
  }

  void
  Scene::release()
  {
    if (!_uploaded || !_ready)
      return;

    release_gpu();
    _uploaded = false;
  }

  void
  Scene::upload_gpu(const std::vector<tinyobj::shape_t> &shapes,
    const std::vector<tinyobj::material_t>& materials,
    const tinyobj::attrib_t attrib,
    const std::string& cubemap_path,
    const std::string& base_folder)
  {
    //
    // Lines below copy adresses given by the GPU the stack-allocated
    // SceneData struct.
    // Takes also care of making cudaMemcpy of the data.
    //
    upload_materials(materials, _scene_data, base_folder);
    upload_meshes(shapes, attrib, _scene_data->meshes);
    upload_cubemap(cubemap_path, _scene_data);

    // Now the sceneData struct contains pointers to memory adresses
    // mapped by the GPU, we can send the whole struct to the GPU.
    cudaMalloc(&_d_scene_data, sizeof(struct SceneData));
    cudaThrowError();
    cudaMemcpy(_d_scene_data, _scene_data, sizeof(struct SceneData),
      cudaMemcpyHostToDevice);
    cudaThrowError();
  }

  void
  Scene::release_gpu()
  {
    // FIRST: Frees texture by first retrieving pointer from the GPU,
    // and then calling cudaFree to free GPU pointed adress.

    size_t nb_tex = _scene_data->textures.size;
    scene::Texture *textures = new scene::Texture[nb_tex];
    cudaMemcpy(textures, _scene_data->textures.data,
      nb_tex * sizeof(scene::Texture), cudaMemcpyDeviceToHost);
    cudaError_t e = cudaGetLastError();

    for (size_t i = 0; i < nb_tex; ++i) cudaFree(textures[i].data);
    delete textures;

    // SECOND: Frees meshes by first retrieving pointer from the GPU,
    // and then calling cudaFree to free GPU pointed adress.
    // Here, we have a depth of 2 regarding the allocation.

    size_t nb_meshes = _scene_data->meshes.size;
    scene::Mesh *meshes = new scene::Mesh[nb_meshes];
    cudaMemcpy(meshes, _scene_data->meshes.data,
      nb_meshes * sizeof(scene::Mesh), cudaMemcpyDeviceToHost);

    for (size_t i = 0; i < nb_meshes; ++i)
    {
      const Mesh& mesh = meshes[i];
      cudaFree(mesh.faces.data);
    }
    delete meshes;

    // Frees materials
    cudaFree(_scene_data->materials.data);
    // Frees lights
    cudaFree(_scene_data->lights.data);

    // THIRD: we can now delete the struct pointer.
    cudaFree(_d_scene_data);

    delete _camera;
    delete _scene_data;

    _uploaded = false;
  }

} // namespace scene