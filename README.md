# RayTracingInOneWeekend
A multi-core and multi-GPU implementation of Ray Tracing in a Weekend by Peter Shirley using OpenMP and CUDA.

I built upon work by Peter Shirley and Roger Allen. The Ray Tracing in One Weekend codebase was extended to exploit multiple CPU cores and GPUs. The multi-CPU version also includes the tsimd library by Jefferson Amstutz for explicit vectorization on x86 platforms. Multi-GPU parallelism is implemented using multi-threading via OpenMP.
