#include <algorithm>

#include <ctype.h>
#include <cuda.h>
#include <cuda_runtime.h>

#include <fstream>
#include <iostream>

#define _USE_MATH_DEFINES
#include <math.h>

#include <sstream>
#include <string>

#include <unordered_map>

#include <driver/cuda_helper.h>
#include <scene/material_loader.h>
#include <shaders/cutils_math.h>
#include <utils/utils.h>

#define TINYOBJLOADER_IMPLEMENTATION
#include <scene/scene.h>

namespace scene {
namespace {
using ShapeVector = std::vector<tinyobj::shape_t>;
using MaterialVector = std::vector<tinyobj::material_t>;
using Real = tinyobj::real_t;

float
parse_float(std::stringstream& iss, float default_val)
{
  if (iss.peek() == std::char_traits<char>::eof())
    return default_val;

  float v = 0.0;
  iss >> v;

  return v;
}

bool
parse_double3(float3& out, std::stringstream& iss)
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

bool
parse_camera(scene::Camera& cam, std::stringstream& iss)
{
  constexpr float DEFAULT_SPEED = 1.4f;

  if (!parse_double3(cam.position, iss))
    return false;
  if (!parse_double3(cam.dir, iss))
    return false;

  cam.dir = normalize(cam.dir);

  if (iss.peek() == std::char_traits<char>::eof())
    return false;
  iss >> cam.fov_x;
  cam.fov_x = (cam.fov_x * M_PI) / 180.0;

  cam.focus_dist = parse_float(iss, 2.0f);
  cam.aperture = parse_float(iss, 0.125f);

  cam.speed = DEFAULT_SPEED;

  return true;
}

void
parse_scene(std::string filename, scene::SceneData& out_scene,
            scene::Camera& cam, std::string& objfile, std::string& cubemap)
{
  std::ifstream file;
  file.open(filename);
  if (!file.is_open()) {
    std::cerr << "arttracer: parse_scene(): failed to open '" << filename << "'"
              << std::endl;
    return;
  }

  // Creates default camera, used when no camera is found in the .scene file
  cam.u = make_float3(1.0, 0.0, 0.0);
  cam.v = make_float3(0.0, -1.0, 0.0);
  cam.fov_x = (90.0 * M_PI) / 180.0;
  cam.dir = cross(cam.u, cam.v);

  // Contains every lights. Because we do not use vector
  // on CUDA, we will need to make a deep copy of it.
  std::vector<LightProp> light_vec;

  std::string line;
  std::string token;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#')
      continue;

    std::stringstream iss(line);
    iss >> token;

    if (token == "p_light") {
      LightProp plight;
      if (!parse_double3(plight.vec, iss)) {
        std::cerr << "parse_scene(): error parsing p_light pos" << std::endl;
        continue;
      }
      if (!parse_double3(plight.color, iss)) {
        std::cerr << "parse_scene(): error parsing p_light color" << std::endl;
        continue;
      }
      if (iss.peek() == std::char_traits<char>::eof()) {
        std::cerr << "parse_scene(): error parsing p_light emission"
                  << std::endl;
        continue;
      }
      iss >> plight.emission;
      if (iss.peek() == std::char_traits<char>::eof()) {
        std::cerr << "parse_scene(): error parsing p_light radius" << std::endl;
        continue;
      }
      iss >> plight.radius;
      light_vec.push_back(plight);
    } else if (token == "scene") {
      if (iss.peek() == std::char_traits<char>::eof()) {
        std::cerr << "parse_scene(): error parsing scene file name"
                  << std::endl;
        continue;
      }

      iss >> objfile;
    } else if (token == "camera") {
      if (!parse_camera(cam, iss)) {
        std::cerr << "parse_scene(): error parsing the camera" << std::endl;
        continue;
      }
    } else if (token == "cubemap")
      if (iss.peek() != std::char_traits<char>::eof())
        iss >> cubemap;
  }

  out_scene.lights.size = 0;
  if (light_vec.size() == 0)
    return;

  // Copies lights back to CUDA
  const LightProp* lights = &light_vec[0];
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
void
upload_materials(const MaterialVector& materials, scene::SceneData* scene,
                 const std::string& base_folder)
{
  auto* mat_loader = MaterialLoader::instance();
  mat_loader->set(&materials, base_folder);

  std::vector<scene::Material> cpu_mat;

  mat_loader->load(cpu_mat);

  // Uploads every materials to the GPU
  if (cpu_mat.size()) {
    cudaMalloc(&scene->materials.data,
               cpu_mat.size() * sizeof(scene::Material));
    cudaThrowError();
    cudaMemcpy(scene->materials.data, &cpu_mat[0],
               cpu_mat.size() * sizeof(scene::Material),
               cudaMemcpyHostToDevice);
    cudaThrowError();
  }

  scene->materials.size = cpu_mat.size();
}

void
upload_meshes(const ShapeVector& shapes, const tinyobj::attrib_t& attrib,
              Buffer<Mesh>& out_meshes)
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

  for (size_t i = 0; i < nb_meshes; ++i) {
    auto& mesh = shapes[i].mesh;
    auto nb_indices = mesh.indices.size();

    size_t nb_faces = nb_indices / 3;

    Mesh& gpu_mesh = gpu_meshes[i];
    gpu_mesh.faces.size = nb_faces;

    // Creates the faces on the CPU. This is really gross regarding memory
    // consumption, but this will really speed up the intersection process
    // thanks to a better cache efficiency.
    std::vector<Face> faces(nb_faces);
    for (size_t i = 0; i < nb_indices; i += 3) {
      auto& face = faces[i / 3];
      for (size_t v = 0; v < 3; ++v) {
        tinyobj::index_t idx = mesh.indices[i + v];
        // Saves vertex
        face.vertices[v] =
          make_float3(attrib.vertices[3 * idx.vertex_index],
                      attrib.vertices[3 * idx.vertex_index + 1],
                      attrib.vertices[3 * idx.vertex_index + 2]);
        // Saves normal
        face.normals[v] = make_float3(attrib.normals[3 * idx.normal_index],
                                      attrib.normals[3 * idx.normal_index + 1],
                                      attrib.normals[3 * idx.normal_index + 2]);
        // Saves normal
        face.texcoords[v] =
          make_float2(attrib.texcoords[2 * idx.texcoord_index],
                      attrib.texcoords[2 * idx.texcoord_index + 1]);
      }
      face.material_id = mesh.material_ids[i / 3];

      // Computes a unique tangent for the whole face
      float3 edge1 = face.vertices[1] - face.vertices[0];
      float3 edge2 = face.vertices[2] - face.vertices[0];
      float2 delta_uv1 = face.texcoords[1] - face.texcoords[0];
      float2 delta_uv2 = face.texcoords[2] - face.texcoords[0];

      float f = 1.0f / (delta_uv1.x * delta_uv2.y - delta_uv2.x * delta_uv1.y);

      face.tangent.x = f * (delta_uv2.y * edge1.x - delta_uv1.y * edge2.x);
      face.tangent.y = f * (delta_uv2.y * edge1.y - delta_uv1.y * edge2.y);
      face.tangent.z = f * (delta_uv2.y * edge1.z - delta_uv1.y * edge2.z);
    }

    if (faces.size() == 0)
      continue;

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
}

Scene::Scene(const std::string& filepath)
  : _filepath(filepath)
  , _uploaded(false)
  , _ready(false)
  , _scene_data(nullptr)
  , _d_scene_data(nullptr)
{
}

Scene::Scene(const std::string&& filepath)
  : _filepath(filepath)
  , _uploaded(false)
  , _ready(false)
  , _scene_data(nullptr)
  , _d_scene_data(nullptr)
{
}

void
Scene::upload(scene::Camera* camera)
{
  if (_uploaded)
    return;

  std::vector<tinyobj::shape_t> shapes;
  std::vector<tinyobj::material_t> materials;
  tinyobj::attrib_t attrib;

  // _sceneData is allocated on the heap,
  // and allows to handle cudaMalloc & cudaFree
  _scene_data = new scene::SceneData;

  std::string objfilepath;
  std::string base_dir = "";
  std::string mtl_dir = "";
  std::string full_obj_path = "";

  parse_scene(_filepath, *_scene_data, _init_camera, objfilepath,
              _cubemap_path);

  if (camera)
    *camera = _init_camera;

  std::string::size_type pos = _filepath.find_last_of('/');
  if (pos != std::string::npos) {
    base_dir = _filepath.substr(0, pos) + "/";
    mtl_dir = base_dir;
    full_obj_path = base_dir + objfilepath;
  }

  // Extracts basedir to find MTL if any.
  pos = objfilepath.find_last_of('/');
  if (pos != std::string::npos)
    mtl_dir = base_dir + "/" + objfilepath.substr(0, pos) + "/";

  _ready = tinyobj::LoadObj(&attrib, &shapes, &materials, &_load_error,
                            full_obj_path.c_str(), mtl_dir.c_str());

  if (!_ready) {
    std::cerr << "arttracer: Scene.upload(): fail to load OBJ.\n"
              << _load_error << std::endl;

    delete _scene_data;

    _scene_data = nullptr;
    _d_scene_data = nullptr;
    return;
  }

  upload_gpu(shapes, materials, attrib, mtl_dir);

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
Scene::upload_gpu(const std::vector<tinyobj::shape_t>& shapes,
                  const std::vector<tinyobj::material_t>& materials,
                  const tinyobj::attrib_t attrib,
                  const std::string& base_folder)
{
  //
  // Lines below copy adresses given by the GPU the stack-allocated
  // SceneData struct.
  // Takes also care of making cudaMemcpy of the data.
  //
  upload_materials(materials, _scene_data, base_folder);
  upload_meshes(shapes, attrib, _scene_data->meshes);

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
  // First: Frees meshes by first retrieving pointer from the GPU,
  // and then calling cudaFree to free GPU pointed adress.
  // Here, we have a depth of 2 regarding the allocation.

  size_t nb_meshes = _scene_data->meshes.size;
  scene::Mesh* meshes = new scene::Mesh[nb_meshes];
  cudaMemcpy(meshes, _scene_data->meshes.data, nb_meshes * sizeof(scene::Mesh),
             cudaMemcpyDeviceToHost);

  for (size_t i = 0; i < nb_meshes; ++i) {
    const Mesh& mesh = meshes[i];
    cudaFree(mesh.faces.data);
  }
  delete[] meshes;

  // Frees meshes
  cudaFree(_scene_data->meshes.data);
  // Frees materials
  cudaFree(_scene_data->materials.data);
  // Frees lights
  cudaFree(_scene_data->lights.data);

  // SECOND: we can now delete the struct pointer.
  cudaFree(_d_scene_data);

  delete _scene_data;

  _uploaded = false;
}

} // namespace scene
