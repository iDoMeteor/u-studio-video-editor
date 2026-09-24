#pragma once

#include <gtk/gtk.h>

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace ustudio::app::timeline {

// GdkTextures for the timeline's thumbnail strips, one per thumbnail key,
// so a snapshot doesn't upload the same pixels again every frame. Holds a
// reference to each; the oldest are dropped past `capacity`. Main thread.
class TextureCache
{
  public:
    explicit TextureCache(size_t capacity) : m_capacity(capacity) {}
    ~TextureCache()
    {
        for (auto &[key, texture] : m_textures)
            g_object_unref(texture);
    }
    TextureCache(const TextureCache &) = delete;
    TextureCache &operator=(const TextureCache &) = delete;

    // The texture for `key`, made from the RGBA pixels on first use.
    GdkTexture *get(const std::string &key, const std::vector<uint8_t> &rgba, int width, int height)
    {
        if (auto it = m_textures.find(key); it != m_textures.end())
            return it->second;
        if (rgba.empty() || width <= 0 || height <= 0)
            return nullptr;
        GBytes *bytes = g_bytes_new(rgba.data(), rgba.size());
        GdkTexture *texture =
            gdk_memory_texture_new(width, height, GDK_MEMORY_R8G8B8A8, bytes, static_cast<gsize>(width) * 4);
        g_bytes_unref(bytes);
        m_textures.emplace(key, texture);
        m_order.push_back(key);
        while (m_order.size() > m_capacity) {
            auto old = m_textures.find(m_order.front());
            if (old != m_textures.end()) {
                g_object_unref(old->second);
                m_textures.erase(old);
            }
            m_order.pop_front();
        }
        return texture;
    }

  private:
    size_t m_capacity;
    std::map<std::string, GdkTexture *> m_textures;
    std::deque<std::string> m_order;
};

} // namespace ustudio::app::timeline
