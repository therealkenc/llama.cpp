#include "mtmd-helper.h"
#include "mtmd.h"

#include <dirent.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static int open_fd_count() {
    DIR * dir = opendir("/proc/self/fd");
    if (!dir) {
        return -1;
    }
    int count = 0;
    while (readdir(dir) != nullptr) {
        count++;
    }
    closedir(dir);
    return count;
}

static std::vector<unsigned char> read_file(const char * path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.good()) {
        return {};
    }
    return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
}

static bool drain_video(const std::vector<unsigned char> & data) {
    auto params                  = mtmd_helper_video_init_params_default();
    params.fps_target            = 1.0f;
    params.timestamp_interval_ms = 0;
    mtmd_helper_video * video    = mtmd_helper_video_init_from_buf(nullptr, data.data(), data.size(), params);
    if (!video) {
        return false;
    }

    for (;;) {
        mtmd_bitmap * bitmap = nullptr;
        char *        text   = nullptr;
        const int32_t result = mtmd_helper_video_read_next(video, &bitmap, &text);
        mtmd_bitmap_free(bitmap);
        free(text);
        if (result == -1) {
            break;
        }
        if (result != 0) {
            mtmd_helper_video_free(video);
            return false;
        }
    }
    mtmd_helper_video_free(video);
    return true;
}

int main(int argc, char ** argv) {
    if (argc != 2) {
        return 1;
    }
    if (std::system("ffmpeg -version >/dev/null 2>&1") != 0 || std::system("ffprobe -version >/dev/null 2>&1") != 0) {
        return 77;
    }
    const std::vector<unsigned char> data = read_file(argv[1]);
    if (data.empty() || !drain_video(data)) {
        return 1;
    }
    const int before = open_fd_count();
    for (int i = 0; i < 4; i++) {
        if (!drain_video(data)) {
            return 1;
        }
    }
    const int after = open_fd_count();
    std::printf("video descriptor count: before=%d after=%d\n", before, after);
    return before >= 0 && after == before ? 0 : 1;
}
