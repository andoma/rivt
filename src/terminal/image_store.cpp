#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "third_party/stb_image.h"

#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>

#include "terminal/image_store.h"
#include "terminal/kitty_graphics.h"
#include "core/util.h"
#include <algorithm>
#include <cstdio>

namespace rivt {

ImageStore::~ImageStore() {
    for (auto &[id, img] : images_) {
        if (img.gl_texture)
            glDeleteTextures(1, &img.gl_texture);
    }
}

bool ImageStore::decode_image(const std::string &base64_payload, int format,
                               int src_width, int src_height, StoredImage &out) {
    std::string raw = base64_decode(base64_payload);
    if (raw.empty()) return false;

    if (format == 100) {
        // PNG
        int w, h, channels;
        unsigned char *pixels = stbi_load_from_memory(
            (const unsigned char *)raw.data(), (int)raw.size(),
            &w, &h, &channels, 4);  // force RGBA
        if (!pixels) {
            fprintf(stderr, "rivt: failed to decode PNG image: %s\n", stbi_failure_reason());
            return false;
        }
        out.width = w;
        out.height = h;
        out.rgba_data.assign(pixels, pixels + w * h * 4);
        stbi_image_free(pixels);
        return true;
    } else if (format == 32 || format == 24) {
        // Raw RGBA or RGB
        int bpp = (format == 32) ? 4 : 3;
        if (src_width <= 0 || src_height <= 0) return false;
        size_t expected = (size_t)src_width * src_height * bpp;
        if (raw.size() < expected) return false;

        out.width = src_width;
        out.height = src_height;
        out.rgba_data.resize(src_width * src_height * 4);

        for (int i = 0; i < src_width * src_height; i++) {
            out.rgba_data[i * 4 + 0] = (uint8_t)raw[i * bpp + 0];
            out.rgba_data[i * 4 + 1] = (uint8_t)raw[i * bpp + 1];
            out.rgba_data[i * 4 + 2] = (uint8_t)raw[i * bpp + 2];
            out.rgba_data[i * 4 + 3] = (bpp == 4) ? (uint8_t)raw[i * bpp + 3] : 0xFF;
        }
        return true;
    }

    return false;
}

void ImageStore::store_image(const KittyGraphicsCommand &cmd) {
    StoredImage img;
    img.id = cmd.image_id ? cmd.image_id : next_id_++;

    if (!decode_image(cmd.payload, cmd.format, cmd.src_width, cmd.src_height, img))
        return;

    // Remove old image with same ID if exists
    auto it = images_.find(img.id);
    if (it != images_.end()) {
        if (it->second.gl_texture)
            glDeleteTextures(1, &it->second.gl_texture);
        images_.erase(it);
    }

    images_.emplace(img.id, std::move(img));
}

void ImageStore::begin_transfer(const KittyGraphicsCommand &cmd) {
    pending_id_ = cmd.image_id ? cmd.image_id : next_id_++;
    pending_placement_id_ = cmd.placement_id;
    pending_format_ = cmd.format;
    pending_src_width_ = cmd.src_width;
    pending_src_height_ = cmd.src_height;
    pending_columns_ = cmd.columns;
    pending_rows_ = cmd.rows;
    pending_z_index_ = cmd.z_index;
    pending_payload_ = cmd.payload;
}

void ImageStore::append_data(const std::string &base64_chunk) {
    pending_payload_ += base64_chunk;
}

KittyGraphicsCommand ImageStore::finish_transfer() {
    KittyGraphicsCommand cmd;
    cmd.image_id = pending_id_;
    cmd.placement_id = pending_placement_id_;
    cmd.format = pending_format_;
    cmd.src_width = pending_src_width_;
    cmd.src_height = pending_src_height_;
    cmd.columns = pending_columns_;
    cmd.rows = pending_rows_;
    cmd.z_index = pending_z_index_;
    cmd.payload = std::move(pending_payload_);

    StoredImage img;
    img.id = cmd.image_id;
    if (decode_image(cmd.payload, cmd.format, cmd.src_width, cmd.src_height, img)) {
        auto it = images_.find(img.id);
        if (it != images_.end()) {
            if (it->second.gl_texture)
                glDeleteTextures(1, &it->second.gl_texture);
            images_.erase(it);
        }
        images_.emplace(img.id, std::move(img));
    }

    pending_id_ = 0;
    pending_payload_.clear();
    return cmd;
}

void ImageStore::place_image(uint32_t image_id, uint32_t placement_id,
                              int abs_line, int col, int columns, int rows, int z_index) {
    if (images_.find(image_id) == images_.end()) return;

    ImagePlacement p;
    p.image_id = image_id;
    p.placement_id = placement_id;
    p.anchor_abs_line = abs_line;
    p.anchor_col = col;
    p.columns = columns;
    p.rows = rows;
    p.z_index = z_index;
    placements_.push_back(p);
}

void ImageStore::delete_images(const KittyGraphicsCommand &cmd) {
    switch (cmd.delete_target) {
        case 'a':
            remove_all();
            break;
        case 'i':
            // Delete all placements for this image ID
            placements_.erase(
                std::remove_if(placements_.begin(), placements_.end(),
                    [&](const ImagePlacement &p) { return p.image_id == cmd.image_id; }),
                placements_.end());
            // Also remove the image data
            {
                auto it = images_.find(cmd.image_id);
                if (it != images_.end()) {
                    if (it->second.gl_texture)
                        glDeleteTextures(1, &it->second.gl_texture);
                    images_.erase(it);
                }
            }
            break;
        case 'p':
            // Delete specific placement
            placements_.erase(
                std::remove_if(placements_.begin(), placements_.end(),
                    [&](const ImagePlacement &p) {
                        return p.image_id == cmd.image_id && p.placement_id == cmd.placement_id;
                    }),
                placements_.end());
            break;
        default:
            break;
    }
}

void ImageStore::remove_all() {
    placements_.clear();
    for (auto &[id, img] : images_) {
        if (img.gl_texture)
            glDeleteTextures(1, &img.gl_texture);
    }
    images_.clear();
}

void ImageStore::gc_placements(int min_abs_line) {
    placements_.erase(
        std::remove_if(placements_.begin(), placements_.end(),
            [min_abs_line](const ImagePlacement &p) { return p.anchor_abs_line < min_abs_line; }),
        placements_.end());

    // Remove images with no remaining placements
    std::vector<uint32_t> to_remove;
    for (auto &[id, img] : images_) {
        bool has_placement = false;
        for (const auto &p : placements_) {
            if (p.image_id == id) { has_placement = true; break; }
        }
        if (!has_placement) to_remove.push_back(id);
    }
    for (uint32_t id : to_remove) {
        auto it = images_.find(id);
        if (it != images_.end()) {
            if (it->second.gl_texture)
                glDeleteTextures(1, &it->second.gl_texture);
            images_.erase(it);
        }
    }
}

void ImageStore::ensure_texture(StoredImage &img) {
    if (!img.texture_dirty) return;
    if (img.rgba_data.empty()) return;

    if (!img.gl_texture)
        glGenTextures(1, &img.gl_texture);

    glBindTexture(GL_TEXTURE_2D, img.gl_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, img.width, img.height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, img.rgba_data.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    img.texture_dirty = false;
}

} // namespace rivt
