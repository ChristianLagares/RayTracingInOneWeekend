#include <iostream>
#include <time.h>
#include <float.h>
#include <curand_kernel.h>
#include "vec3.h"
#include "ray.h"
#include "sphere.h"
#include "hitable_list.h"
#include "camera.h"
#include "material.h"
#include "omp.h"

#define MAXCLIP(x,maximum) ((x < maximum) ? x : maximum)
#define MINCLIP(x,minimum) ((x > minimum) ? x : minimum)
#define CLIP(x,minimum,maximum) MAXCLIP(MINCLIP(x,minimum),maximum)

// limited version of checkCudaErrors from helper_cuda.h in CUDA examples
#define checkCudaErrors(val) check_cuda( (val), #val, __FILE__, __LINE__ )

void check_cuda(cudaError_t result, char const *const func, const char *const file, int const line) {
    if (result) {
        std::cerr << "CUDA error = " << static_cast<unsigned int>(result) << " at " <<
            file << ":" << line << " '" << func << "' \n";
        // Make sure we call CUDA Device Reset before exiting
        cudaDeviceReset();
        exit(99);
    }
}

// Matching the C++ code would recurse enough into color() calls that
// it was blowing up the stack, so we have to turn this into a
// limited-depth loop instead.  Later code in the book limits to a max
// depth of 50, so we adapt this a few chapters early on the GPU.
__device__ vec3 color(const ray& r, hitable **world, curandState *local_rand_state) {
    ray cur_ray = r;
    vec3 cur_attenuation = vec3(1.0,1.0,1.0);
    for(int i = 0; i < 50; i++) {
        hit_record rec;
        if ((*world)->hit(cur_ray, 0.001f, FLT_MAX, rec)) {
            ray scattered;
            vec3 attenuation;
            if(rec.mat_ptr->scatter(cur_ray, rec, attenuation, scattered, local_rand_state)) {
                cur_attenuation *= attenuation;
                cur_ray = scattered;
            }
            else {
                return vec3(0.0,0.0,0.0);
            }
        }
        else {
            vec3 unit_direction = unit_vector(cur_ray.direction());
            float t = 0.5f*(unit_direction.y() + 1.0f);
            vec3 c = (1.0f-t)*vec3(1.0, 1.0, 1.0) + t*vec3(0.5, 0.7, 1.0);
            return cur_attenuation * c;
        }
    }
    return vec3(0.0,0.0,0.0); // exceeded recursion
}

__global__ void rand_init(curandState *rand_state) {
    if (threadIdx.x == 0 && blockIdx.x == 0) {
        curand_init(1984, 0, 0, rand_state);
    }
}

__global__ void render_init(int min_y, int max_x, int max_y, curandState *rand_state) {
    int i = threadIdx.x + blockIdx.x * blockDim.x;
    int j = threadIdx.y + blockIdx.y * blockDim.y;
    if((i >= max_x) || (j >= (max_y - min_y))) return;
    int pixel_index = (j+min_y)*max_x + i;
    int local_index = j*max_x + i;
    //Each thread gets same seed, a different sequence number, no offset
    curand_init(1984, pixel_index, 0, &rand_state[local_index]);
}

#define RND (curand_uniform(&local_rand_state))

__global__ void render(vec3 *fb, int min_y, int max_x, int max_y, int ns, camera **cam, hitable **world, curandState *rand_state) {
    int i = threadIdx.x + blockIdx.x * blockDim.x;
    int j = threadIdx.y + blockIdx.y * blockDim.y;
    if((i >= max_x) || (j >= (max_y - min_y))) return;
    //int pixel_index = (j+min_y)*max_x + i;
    int local_index = j*max_x + i;
    curandState local_rand_state = rand_state[local_index];
    vec3 col(0,0,0);
    for(int s=0; s < ns; s++) {
        float u = float(i + RND) / float(max_x);
        float v = float(j + min_y + RND) / float(max_y);
        ray r = (*cam)->get_ray(u, v, &local_rand_state);
        col += color(r, world, &local_rand_state);
    }
    rand_state[local_index] = local_rand_state;
    col /= float(ns);
    col[0] = sqrt(col[0]);
    col[1] = sqrt(col[1]);
    col[2] = sqrt(col[2]);
    fb[local_index] = col;
}


__global__ void create_world(hitable **d_list, hitable **d_world, camera **d_camera, int nx, int ny, curandState *rand_state, int dev_id, int total_devices) {
    if (threadIdx.x == 0 && blockIdx.x == 0) {
        curandState local_rand_state = *rand_state;
        d_list[0] = new sphere(vec3(0,-1000.0,-1), 1000,
                               new lambertian(vec3(0.5, 0.5, 0.5)));
        int i = 1;
        for(int a = -11; a < 11; a++) {
            for(int b = -11; b < 11; b++) {
                float choose_mat = RND;
                vec3 center(a+RND,0.2,b+RND);
                if(choose_mat < 0.8f) {
                    d_list[i++] = new sphere(center, 0.2,
                                             new lambertian(vec3(RND*RND, RND*RND, RND*RND)));
                }
                else if(choose_mat < 0.95f) {
                    d_list[i++] = new sphere(center, 0.2,
                                             new metal(vec3(0.5f*(1.0f+RND), 0.5f*(1.0f+RND), 0.5f*(1.0f+RND)), 0.5f*RND));
                }
                else {
                    d_list[i++] = new sphere(center, 0.2, new dielectric(1.5));
                }
            }
        }
        d_list[i++] = new sphere(vec3(0, 1,0),  1.0, new dielectric(1.5));
        d_list[i++] = new sphere(vec3(-4, 1, 0), 1.0, new lambertian(vec3(0.4, 0.2, 0.1)));
        d_list[i++] = new sphere(vec3(4, 1, 0),  1.0, new metal(vec3(0.7, 0.6, 0.5), 0.0));
        *rand_state = local_rand_state;
        *d_world  = new hitable_list(d_list, 22*22+1+3);

        vec3 lookfrom(13,2,3);
        vec3 lookat(0,0,0);
        float dist_to_focus = 10.0; (lookfrom-lookat).length();
        float aperture = 0.1;
        *d_camera   = new camera(lookfrom,
                                 lookat,
                                 vec3(0,1,0),
                                 30.0,
                                 float(nx)/float(ny),
                                 aperture,
                                 dist_to_focus,
                                 dev_id,
                                 total_devices);
    }
}

__global__ void free_world(hitable **d_list, hitable **d_world, camera **d_camera) {
    for(int i=0; i < 22*22+1+3; i++) {
        delete ((sphere *)d_list[i])->mat_ptr;
        delete d_list[i];
    }
    delete *d_world;
    delete *d_camera;
}

int main() {
    int nx = 7680;
    int ny = 4320;
    int ns = 1000;
    int tx = 8;
    int ty = 8;

    int num_devices = 0;
    cudaGetDeviceCount(&num_devices);
    if (num_devices < 1){
        return 1;
    }

    std::cerr << "Rendering a " << nx << "x" << ny << " image with " << ns << " samples per pixel ";
    std::cerr << "in " << tx << "x" << ty << " blocks.\n";

    clock_t start, stop;
    start = clock();

    int num_pixels = nx*ny;
    size_t fb_size = num_pixels*sizeof(vec3);

    vec3 *h_fb;
    h_fb = (vec3*)malloc(fb_size);

    #pragma omp parallel num_threads(num_devices)
    {
        int dev_id = omp_get_thread_num();
        cudaSetDevice(dev_id);
        cudaFree(0);

        int chunk_y = (ny/num_devices);// + (ny % num_devices != 0);
        int min_y = dev_id * chunk_y;
        int max_x = nx;
        int max_y = (dev_id+1) * chunk_y;
        max_y = (max_y > ny) ? ny : max_y;

        int private_copy_size = sizeof(vec3)*(max_x*(max_y-min_y));

        // allocate FB
        vec3 *d_fb;
        checkCudaErrors(cudaMalloc((void **)&d_fb, private_copy_size));

        // allocate random state
        curandState *d_rand_state;
        checkCudaErrors(cudaMalloc((void **)&d_rand_state, (max_x*(max_y-min_y))*sizeof(curandState)));
        curandState *d_rand_state2;
        checkCudaErrors(cudaMalloc((void **)&d_rand_state2, 1*sizeof(curandState)));

        // we need that 2nd random state to be initialized for the world creation
        rand_init<<<1,1>>>(d_rand_state2);
        checkCudaErrors(cudaGetLastError());
        checkCudaErrors(cudaDeviceSynchronize());

        // make our world of hitables & the camera
        hitable **d_list;
        int num_hitables = 22*22+1+3;
        checkCudaErrors(cudaMalloc((void **)&d_list, num_hitables*sizeof(hitable *)));
        hitable **d_world;
        checkCudaErrors(cudaMalloc((void **)&d_world, sizeof(hitable *)));
        camera **d_camera;
        checkCudaErrors(cudaMalloc((void **)&d_camera, sizeof(camera *)));
        create_world<<<1,1>>>(d_list, d_world, d_camera, nx, ny, d_rand_state2, dev_id, num_devices);
        checkCudaErrors(cudaGetLastError());
        checkCudaErrors(cudaDeviceSynchronize());

        // Render our buffer
        dim3 blocks((nx/tx)+1,((ny/ty)+1)/num_devices);
        dim3 threads(tx,ty);

        for (int device = 0; device < num_devices; device++){
            if (device != dev_id){
                cudaDeviceEnablePeerAccess(device, 0);
            }
        }

        render_init<<<blocks, threads>>>(min_y, max_x, max_y, d_rand_state);
        checkCudaErrors(cudaGetLastError());
        checkCudaErrors(cudaDeviceSynchronize());
        render<<<blocks, threads>>>(d_fb, min_y, max_x, max_y, ns, d_camera, d_world, d_rand_state);
        checkCudaErrors(cudaGetLastError());
        checkCudaErrors(cudaDeviceSynchronize());
        
        #pragma omp critical
        {
            int pixel_index = min_y*max_x;
            checkCudaErrors(cudaMemcpy(h_fb+pixel_index, d_fb, private_copy_size, cudaMemcpyDeviceToHost));
        }
        checkCudaErrors(cudaDeviceSynchronize());
        free_world<<<1,1>>>(d_list,d_world,d_camera);
        checkCudaErrors(cudaGetLastError());
        checkCudaErrors(cudaFree(d_camera));
        checkCudaErrors(cudaFree(d_world));
        checkCudaErrors(cudaFree(d_list));
        checkCudaErrors(cudaFree(d_rand_state));
        checkCudaErrors(cudaFree(d_fb));
        cudaDeviceReset();
    }
    stop = clock();
    double timer_seconds = ((double)(stop - start)) / CLOCKS_PER_SEC;
    std::cerr << "took " << timer_seconds << " seconds.\n";

    // Output FB as Image
    
    std::cout << "P3\n" << nx << " " << ny << "\n255\n";
    for (int j = ny-1; j >= 0; j--) {
        for (int i = 0; i < nx; i++) {
            size_t pixel_index = j*nx + i;
            int ir = int(255.99*h_fb[pixel_index].r());
            int ig = int(255.99*h_fb[pixel_index].g());
            int ib = int(255.99*h_fb[pixel_index].b());
            std::cout << ir << " " << ig << " " << ib << "\n";
        }
    }
    free(h_fb);
}
