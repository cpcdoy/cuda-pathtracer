#pragma once

#include <cuda.h>
#include <driver_types.h>

#include "../driver/cuda_helper.h"

__device__ inline float3
uncharted_tonemap(float3 x)
{
   const float A = 0.15;
   const float B = 0.50;
   const float C = 0.10;
   const float D = 0.20;
   const float E = 0.02;
   const float F = 0.30;

   return ((x*(A*x+C*B)+D*E)/(x*(A*x+B)+D*F))-E/F;
}

__device__ inline float3
exposure(float3 color)
{
  const float exposure_bias = 2.0f;
  float3 curr = uncharted_tonemap(exposure_bias * color);

  const float3 W = make_float3(11.2);
  float3 white_scale = 1.0f / uncharted_tonemap(W);

  return curr * white_scale;
}

__device__ inline void
camera_dof(scene::Ray& r, const scene::Camera& cam, curandState *rand_state)
{
	//Focus distance
	float3 focal_point = 2.f * r.dir;
	float random_angle = curand_uniform(rand_state) * 2.0f * M_PI;
	// Aperture size
	float random_radius = curand_uniform(rand_state) * 0.125f;
	float3  random_aperture_pos = (cos(random_angle) * cam.u + sin(random_angle) * cam.v) * random_radius;
	// Point on aperture to focal point
	float3 final_ray_dir = normalize(focal_point - random_aperture_pos);

	r.origin += random_aperture_pos;
	r.dir = final_ray_dir;
}