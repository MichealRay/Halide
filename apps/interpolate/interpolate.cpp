#include "Halide.h"

using namespace Halide;

#include <iostream>
#include <limits>

#include "benchmark.h"
#include "halide_image_io.h"

using namespace Halide::Tools;

using std::vector;

int main(int argc, char **argv) {
    if (argc < 3) {
        std::cerr << "Usage:\n\t./interpolate in.png out.png\n" << std::endl;
        return 1;
    }

    ImageParam input(Float(32), 3);

    // Input must have four color channels - rgba
    input.set_bounds(2, 0, 4);

    const int levels = 10;

    Func downsampled[levels];
    Func downx[levels];
    Func interpolated[levels];
    Func upsampled[levels];
    Func upsampledx[levels];
    Var x("x"), y("y"), c("c");

    Func clamped = BoundaryConditions::repeat_edge(input);

    // This triggers a bug in llvm 3.3 (3.2 and trunk are fine), so we
    // rewrite it in a way that doesn't trigger the bug. The rewritten
    // form assumes the input alpha is zero or one.
    // downsampled[0](x, y, c) = select(c < 3, clamped(x, y, c) * clamped(x, y, 3), clamped(x, y, 3));
    downsampled[0](x, y, c) = clamped(x, y, c) * clamped(x, y, 3);

    for (int l = 1; l < levels; ++l) {
        Func prev = downsampled[l-1];

        if (l == 4) {
            // Also add a boundary condition at a middle pyramid level
            // to prevent the footprint of the downsamplings to extend
            // too far off the base image. Otherwise we look 512
            // pixels off each edge.
            Expr w = input.width()/(1 << l);
            Expr h = input.height()/(1 << l);
            prev = lambda(x, y, c, prev(clamp(x, 0, w), clamp(y, 0, h), c));
        }

        downx[l](x, y, c) = (prev(x*2-1, y, c) +
                             2.0f * prev(x*2, y, c) +
                             prev(x*2+1, y, c)) * 0.25f;
        downsampled[l](x, y, c) = (downx[l](x, y*2-1, c) +
                                   2.0f * downx[l](x, y*2, c) +
                                   downx[l](x, y*2+1, c)) * 0.25f;
    }
    interpolated[levels-1](x, y, c) = downsampled[levels-1](x, y, c);
    for (int l = levels-2; l >= 0; --l) {
        upsampledx[l](x, y, c) = (interpolated[l+1](x/2, y, c) +
                                  interpolated[l+1]((x+1)/2, y, c)) / 2.0f;
        upsampled[l](x, y, c) =  (upsampledx[l](x, y/2, c) +
                                  upsampledx[l](x, (y+1)/2, c)) / 2.0f;
        interpolated[l](x, y, c) = downsampled[l](x, y, c) + (1.0f - downsampled[l](x, y, 3)) * upsampled[l](x, y, c);
    }

    Func normalize("normalize");
    normalize(x, y, c) = interpolated[0](x, y, c) / interpolated[0](x, y, 3);

    Func final("final");
    final(x, y, c) = normalize(x, y, c);

    final.bound(x, 0, 1536).bound(y, 0, 2560).bound(c, 0, 3);
    std::cout << "Finished function setup." << std::endl;

    int sched;
    Target target = get_target_from_environment();
    if (target.has_gpu_feature()) {
        sched = 4;
    } else {
        sched = 5;
    }

    switch (sched) {
    case 0:
    {
        std::cout << "Flat schedule." << std::endl;
        for (int l = 0; l < levels; ++l) {
            downsampled[l].compute_root();
            interpolated[l].compute_root();
        }
        final.compute_root();
        break;
    }
    case 1:
    {
        std::cout << "Flat schedule with vectorization." << std::endl;
        for (int l = 0; l < levels; ++l) {
            downsampled[l].compute_root().vectorize(x,4);
            interpolated[l].compute_root().vectorize(x,4);
        }
        final.compute_root();
        break;
    }
    case 2:
    {
        Var xi, yi;
        std::cout << "Flat schedule with parallelization + vectorization." << std::endl;
        for (int l = 1; l < levels-1; ++l) {
            downsampled[l]
                .compute_root()
                .parallel(y, 8)
                .vectorize(x, 4);
            interpolated[l]
                .compute_root()
                .parallel(y, 8)
                .unroll(x, 2)
                .unroll(y, 2)
                .vectorize(x, 8);
        }
        final
            .reorder(c, x, y)
            .bound(c, 0, 3)
            .unroll(c)
            .tile(x, y, xi, yi, 2, 2)
            .unroll(xi)
            .unroll(yi)
            .parallel(y, 8)
            .vectorize(x, 8)
            .bound(x, 0, input.width())
            .bound(y, 0, input.height());
        break;
    }
    case 3:
    {
        std::cout << "Flat schedule with vectorization sometimes." << std::endl;
        for (int l = 0; l < levels; ++l) {
            if (l + 4 < levels) {
                Var yo,yi;
                downsampled[l].compute_root().vectorize(x,4);
                interpolated[l].compute_root().vectorize(x,4);
            } else {
                downsampled[l].compute_root();
                interpolated[l].compute_root();
            }
        }
        final.compute_root();
        break;
    }
    case 4:
    {
        std::cout << "GPU schedule." << std::endl;

        // Some gpus don't have enough memory to process the entire
        // image, so we process the image in tiles.
        Var yo, yi, xo, xi;
        final.reorder(c, x, y).bound(c, 0, 3).vectorize(x, 4);
        final.tile(x, y, xo, yo, xi, yi, input.width()/8, input.height()/8);
        normalize.compute_at(final, xo).reorder(c, x, y).gpu_tile(x, y, 16, 16, DeviceAPI::Default_GPU).unroll(c);

        // Start from level 1 to save memory - level zero will be computed on demand
        for (int l = 1; l < levels; ++l) {
            int tile_size = 32 >> l;
            if (tile_size < 1) tile_size = 1;
            if (tile_size > 8) tile_size = 8;
            downsampled[l].compute_root();
            if (false) {
                // Outer loop on CPU for the larger ones.
                downsampled[l].tile(x, y, xo, yo, x, y, 256, 256);
            }
            downsampled[l].gpu_tile(x, y, c, tile_size, tile_size, 4, DeviceAPI::Default_GPU);
            interpolated[l].compute_at(final, xo).gpu_tile(x, y, c, tile_size, tile_size, 4, DeviceAPI::Default_GPU);
        }
        break;
    }
    case 5:
    {
        Var x_outer, y_outer, x_inner, y_inner;
        /* first 3 downsamples */
        for(int l = 1; l <=3; l++){
            downx[l].compute_at(downsampled[l], x).parallel(y).vectorize(x, VLEN);
            downsampled[l].compute_root().parallel(y).vectorize(x, VLEN);
        }
        /* remaining downsamples */
        for(int l = 4; l < levels-1; l++){
            downsampled[l].compute_root().vectorize(x, VLEN).parallel(y);
        }

        /* first L-3 upsamples */
        for(int l = levels-2; l > 3; l--){
            upsampled[l].compute_root().vectorize(x, VLEN).parallel(y);
            interpolated[l].compute_root().vectorize(x, VLEN).parallel(y);
            interpolated[l].unroll(x, 2).unroll(y, 2);
        }
        upsampled[3].compute_at(final, x_outer).vectorize(x, VLEN);
        interpolated[3].compute_at(final, x_outer).vectorize(x, VLEN);
        /* last upsamples */
        for(int l = 2; l >= 0; l--){
            upsampledx[l].compute_at(final, x_outer).vectorize(x, VLEN);
            upsampled[l].compute_at(final, x_outer).vectorize(x, VLEN);
            interpolated[l].compute_at(final, x_outer).vectorize(x, VLEN);
            interpolated[l].unroll(x, 2).unroll(y, 2);
        }
        final.tile(x, y, x_outer, y_outer, x_inner, y_inner, 128, 16);
        final.parallel(y_outer).vectorize(x_inner, VLEN);
        final.bound(x, 0, input.width());
        final.bound(y, 0, input.height());
        final.bound(c, 0, 3);
        break;
    }
    default:
        std::cout << "Auto-Schedule" << std::endl;
    }

    // JIT compile the pipeline eagerly, so we don't interfere with timing
    final.compile_jit(target);

    Image<float> in_png = load_image(argv[1]);
    Image<float> out(in_png.width(), in_png.height(), 3);
    std::cout << in_png.width() << "," << in_png.height() << std::endl;
    assert(in_png.channels() == 4);
    input.set(in_png);

    std::cout << "Running... " << std::endl;
    double best = benchmark(20, 1, [&]() { final.realize(out); });
    std::cout << " took " << best * 1e3 << " msec." << std::endl;

    //vector<Argument> args;
    //args.push_back(input);
    //final.compile_to_assembly("test.s", args, target);

    save_image(out, argv[2]);

}
