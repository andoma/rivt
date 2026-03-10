#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace rivt {

struct KittyGraphicsCommand;

struct StoredImage {
    uint32_t id = 0;
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba_data;
    unsigned int gl_texture = 0;
    bool texture_dirty = true;
};

struct ImagePlacement {
    uint32_t image_id = 0;
    uint32_t placement_id = 0;
    int anchor_abs_line = 0;
    int anchor_col = 0;
    int columns = 0;   // display size in cells (0 = auto from image size)
    int rows = 0;
    int z_index = 0;
};

class ImageStore {
public:
    ImageStore() = default;
    ~ImageStore();

    // Transmit: store a complete image (non-chunked or final chunk)
    void store_image(const KittyGraphicsCommand &cmd);

    // Chunked transfer
    void begin_transfer(const KittyGraphicsCommand &cmd);
    void append_data(const std::string &base64_chunk);
    bool has_pending_transfer() const { return !pending_payload_.empty() || pending_id_ != 0; }
    KittyGraphicsCommand finish_transfer();

    // Placement
    void place_image(uint32_t image_id, uint32_t placement_id,
                     int abs_line, int col, int columns, int rows, int z_index);

    // Delete
    void delete_images(const KittyGraphicsCommand &cmd);
    void remove_all();

    // Garbage collection: remove placements anchored before min_abs_line
    void gc_placements(int min_abs_line);

    // Ensure GL texture is uploaded for an image
    void ensure_texture(StoredImage &img);

    // Access
    const std::unordered_map<uint32_t, StoredImage> &images() const { return images_; }
    std::unordered_map<uint32_t, StoredImage> &images() { return images_; }
    const std::vector<ImagePlacement> &placements() const { return placements_; }

private:
    bool decode_image(const std::string &base64_payload, int format,
                      int src_width, int src_height, StoredImage &out);

    std::unordered_map<uint32_t, StoredImage> images_;
    std::vector<ImagePlacement> placements_;

    // Chunked transfer state
    uint32_t pending_id_ = 0;
    uint32_t pending_placement_id_ = 0;
    int pending_format_ = 32;
    int pending_src_width_ = 0;
    int pending_src_height_ = 0;
    int pending_columns_ = 0;
    int pending_rows_ = 0;
    int pending_z_index_ = 0;
    std::string pending_payload_;

    uint32_t next_id_ = 1;
};

} // namespace rivt
