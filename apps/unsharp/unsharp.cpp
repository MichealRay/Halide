#include "Halide.h"
#include <stdio.h>
using namespace Halide;

int main(int argc, char **argv) {

    ImageParam in(Float(32), 3);

    // Define a 7x7 Gaussian Blur with a repeat-edge boundary condition.
    float sigma = 1.5f;

    Var x, y, c;
    Func kernel("kernel");
    kernel(x) = exp(-x*x/(2*sigma*sigma)) / (sqrtf(2*M_PI)*sigma);

    Func in_bounded = BoundaryConditions::repeat_edge(in);

    Func gray("gray");
    gray(x, y) = max(in_bounded(x, y, 0),
                     max(in_bounded(x, y, 1),
                         in_bounded(x, y, 2)));

    Func blur_y("blur_y");
    blur_y(x, y) = (kernel(0) * gray(x, y) +
                    kernel(1) * (gray(x, y-1) +
                                 gray(x, y+1)) +
                    kernel(2) * (gray(x, y-2) +
                                 gray(x, y+2)) +
                    kernel(3) * (gray(x, y-3) +
                                 gray(x, y+3)));

    Func blur_x("blur_X");
    blur_x(x, y) = (kernel(0) * blur_y(x, y) +
                    kernel(1) * (blur_y(x-1, y) +
                                 blur_y(x+1, y)) +
                    kernel(2) * (blur_y(x-2, y) +
                                 blur_y(x+2, y)) +
                    kernel(3) * (blur_y(x-3, y) +
                                 blur_y(x+3, y)));

    Func sharpen("sharpen");
    sharpen(x, y) = 2 * gray(x, y) - blur_x(x, y);

    Func ratio("ratio");
    ratio(x, y) = sharpen(x, y) / gray(x, y);

    Func unsharp("unsharp");
    unsharp(x, y, c) = min(ratio(x, y), 1.5f) * in(x, y, c);

    unsharp.bound(x, 0, 1536).bound(y, 0, 2560).bound(c, 0, 3);
    // Pick a schedule
    int schedule = atoi(argv[1]);

    Target target = get_target_from_environment();
    if (schedule == 0) {
        blur_y.compute_at(unsharp, y).vectorize(x, 8);
        ratio.compute_at(unsharp, y).vectorize(x, 8);
        unsharp.vectorize(x, 8).parallel(y).reorder(x, c, y);
    }

    if (schedule == -1)
        unsharp.compile_to_file("unsharp", {in}, target, true);
    else
        unsharp.compile_to_file("unsharp", {in}, target);

    return 0;
}
