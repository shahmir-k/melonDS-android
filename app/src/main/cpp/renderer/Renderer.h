#ifndef RENDERER_H
#define RENDERER_H

namespace MelonDSAndroid
{

enum class Renderer {
    Software = 0,
    OpenGl = 1,        // hybrid: 3D on the GPU at Nx, 2D on the CPU at native (HybridRenderer)
    Compute = 2,
    OpenGlHiRes = 3,   // full-GPU: 2D and 3D on the GPU at Nx (GLRenderer)
};

}

#endif //RENDERER_H
