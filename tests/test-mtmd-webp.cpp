#include "mtmd-helper.h"
#include "mtmd.h"

#include <cstdio>

int main(int argc, char ** argv) {
    if (argc != 2) {
        return 1;
    }
    mtmd_helper_bitmap_wrapper result = mtmd_helper_bitmap_init_from_file(
        nullptr, argv[1], false, mtmd_helper_init_opt_default());
    const bool valid = result.bitmap && !result.video_ctx && mtmd_bitmap_get_nx(result.bitmap) == 48 &&
                       mtmd_bitmap_get_ny(result.bitmap) == 48 &&
                       mtmd_bitmap_get_n_bytes(result.bitmap) == 48 * 48 * 3 && mtmd_bitmap_get_data(result.bitmap);
    std::printf("static WebP decoded as %s\n", result.video_ctx ? "video" : "image");
    mtmd_bitmap_free(result.bitmap);
    mtmd_helper_video_free(result.video_ctx);
    return valid ? 0 : 1;
}
