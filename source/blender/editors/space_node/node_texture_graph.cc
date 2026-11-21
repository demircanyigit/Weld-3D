/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spnode
 *
 * Texture Graph evaluation: runs a Texture Graph on the compositor engine (CPU) and writes each
 * output of the graph into an image data-block named "<Graph> <Output>". Shader nodes, the 2D
 * image editor and exporters then use those images like any other image.
 */

#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "MEM_guardedalloc.h"

#include "BLI_math_vector.h"
#include "BLI_math_vector_types.hh"
#include "BLI_string.h"
#include "BLI_task.hh"
#include "BLI_timeit.hh"
#include "BLI_threads.h"
#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_set.hh"
#include "BLI_vector.hh"

#include "DNA_image_types.h"
#include "DNA_texture_types.h"
#include "DNA_node_types.h"
#include "DNA_scene_types.h"

#include "BKE_idprop.hh"
#include "BKE_image.hh"
#include "BKE_texture.h"
#include "BKE_lib_id.hh"
#include "BKE_context.hh"
#include "BKE_global.hh"
#include "BKE_main.hh"
#include "BKE_node.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_interface.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "COM_context.hh"
#include "COM_domain.hh"
#include "COM_input_descriptor.hh"
#include "COM_node_group_operation.hh"
#include "COM_realize_on_domain_operation.hh"
#include "COM_result.hh"
#include "COM_static_cache_manager.hh"
#include "COM_node_operation.hh"
#include "COM_profiler.hh"
#include "COM_utilities.hh"

#include "ED_node_c.hh"

#include "WM_api.hh"
#include "WM_types.hh"

namespace blender {

using namespace compositor;

std::string ED_texture_graph_image_name(const bNodeTree *ntree, const char *output_name)
{
  return std::string(ntree->id.name + 2) + " " + output_name;
}

/** Name of the property that marks an image as made by a graph; it holds the graph's name. */
static constexpr const char *IMAGE_OWNER_PROP = "texture_graph";

static void mark_owner(ID &id, const bNodeTree &ntree)
{
  IDProperty *props = IDP_EnsureProperties(&id);
  IDProperty *prop = IDP_GetPropertyFromGroup(props, IMAGE_OWNER_PROP);
  if (prop && prop->type == IDP_STRING && STREQ(IDP_string_get(prop), ntree.id.name + 2)) {
    return;
  }
  if (prop) {
    IDP_FreeFromGroup(props, prop);
  }
  IDP_AddToGroup(props, bke::idprop::create(IMAGE_OWNER_PROP, StringRefNull(ntree.id.name + 2))
                            .release());
}

static void mark_image_owner(Image &image, const bNodeTree &ntree)
{
  mark_owner(image.id, ntree);
}

/** Whether the ID is marked as made by this graph. Null when it has no mark at all. */
static std::optional<bool> marked_owned_by(ID &id, const bNodeTree &ntree)
{
  IDProperty *props = IDP_GetProperties(&id);
  IDProperty *prop = props ? IDP_GetPropertyFromGroup_null(props, IMAGE_OWNER_PROP) : nullptr;
  if (prop == nullptr) {
    return std::nullopt;
  }
  return prop->type == IDP_STRING && STREQ(IDP_string_get(prop), ntree.id.name + 2);
}

/** Whether the image was made by this graph: marked with its name, or (in files from before
 * marking) a generated image named after the graph. */
static bool image_owned_by(Image &image, const bNodeTree &ntree)
{
  if (const std::optional<bool> marked = marked_owned_by(image.id, ntree)) {
    return *marked;
  }
  const std::string prefix = std::string(ntree.id.name + 2) + " ";
  if (image.source != IMA_SRC_GENERATED || !STRPREFIX(image.id.name + 2, prefix.c_str())) {
    return false;
  }
  const ImageTile *tile = BKE_image_get_tile(&image, 0);
  return tile && tile->gen_type == IMA_GENTYPE_BLANK;
}

/** Delete the images of outputs the graph no longer has. */
static void remove_stale_images(Main *bmain, const bNodeTree &ntree)
{
  Set<std::string> wanted;
  wanted.add(ED_texture_graph_image_name(&ntree, "Viewer"));
  for (const bNodeTreeInterfaceSocket *output_socket : ntree.interface_outputs()) {
    wanted.add(ED_texture_graph_image_name(&ntree, output_socket->name ? output_socket->name : ""));
  }
  Vector<Tex *> stale_textures;
  for (Tex &tex : bmain->textures) {
    if (!ID_IS_LINKED(&tex) && !wanted.contains(tex.id.name + 2) &&
        marked_owned_by(tex.id, ntree).value_or(false))
    {
      stale_textures.append(&tex);
    }
  }
  for (Tex *tex : stale_textures) {
    BKE_id_delete(bmain, tex);
  }

  Vector<Image *> stale;
  for (Image &image : bmain->images) {
    if (!ID_IS_LINKED(&image) && !wanted.contains(image.id.name + 2) &&
        image_owned_by(image, ntree))
    {
      stale.append(&image);
    }
  }
  for (Image *image : stale) {
    BKE_id_delete(bmain, image);
  }
}

/**
 * Modifiers, brushes and other places that take a texture can't use an image directly, so each
 * output also gets a texture of the same name that shows its image.
 */
static void ensure_output_texture(Main *bmain, const bNodeTree &ntree, Image &image)
{
  Tex *tex = reinterpret_cast<Tex *>(BKE_libblock_find_name(bmain, ID_TE, image.id.name + 2));
  if (tex == nullptr) {
    tex = BKE_texture_add(bmain, image.id.name + 2);
    if (tex == nullptr) {
      return;
    }
    /* Like the images, textures belong to whatever uses them. */
    id_us_min(&tex->id);
    tex->type = TEX_IMAGE;
    mark_owner(tex->id, ntree);
  }
  else if (ID_IS_LINKED(tex) || !marked_owned_by(tex->id, ntree).value_or(false)) {
    /* A texture of the user's own that happens to have this name. */
    return;
  }
  if (tex->type != TEX_IMAGE || tex->ima != &image) {
    tex->type = TEX_IMAGE;
    if (tex->ima) {
      id_us_min(&tex->ima->id);
    }
    tex->ima = &image;
    id_us_plus(&image.id);
    DEG_id_tag_update(&tex->id, 0);
    DEG_relations_tag_update(bmain);
  }
}

static int2 texture_graph_size(const bNodeTree &ntree)
{
  int2 size(ntree.texture_graph_size[0], ntree.texture_graph_size[1]);
  if (size.x <= 0) {
    size.x = TEXTURE_GRAPH_DEFAULT_SIZE;
  }
  if (size.y <= 0) {
    size.y = TEXTURE_GRAPH_DEFAULT_SIZE;
  }
  return size;
}

/**
 * Whether an output holds colors (stored in scene linear) or plain data like roughness, height
 * or normals (stored as Non-Color so shaders read the values unchanged).
 */
static bool output_holds_color(const char *name, const ResultType type)
{
  if (!ELEM(type, ResultType::Color, ResultType::Float4)) {
    return false;
  }
  char lower[MAX_NAME];
  STRNCPY(lower, name);
  BLI_str_tolower_ascii(lower, sizeof(lower));
  for (const char *data_word : {"normal", "roughness", "metal", "height", "occlusion", "mask"}) {
    if (strstr(lower, data_word)) {
      return false;
    }
  }
  return true;
}

/** Read one pixel of any result type as RGBA, grayscale values fill RGB. */
template<typename T> static float4 to_rgba(const T &value)
{
  if constexpr (std::is_same_v<T, float>) {
    return float4(value, value, value, 1.0f);
  }
  else if constexpr (std::is_same_v<T, float2>) {
    return float4(value.x, value.y, 0.0f, 1.0f);
  }
  else if constexpr (std::is_same_v<T, float3>) {
    return float4(value, 1.0f);
  }
  else if constexpr (std::is_same_v<T, float4>) {
    return value;
  }
  else if constexpr (std::is_same_v<T, Color>) {
    return float4(value);
  }
  else if constexpr (std::is_same_v<T, int32_t> || std::is_same_v<T, bool>) {
    const float f = float(value);
    return float4(f, f, f, 1.0f);
  }
  return float4(0.0f, 0.0f, 0.0f, 1.0f);
}

template<typename T>
static void copy_result_typed(const Result &result, const int2 size, float *r_pixels)
{
  if (result.is_single_value()) {
    const float4 value = to_rgba(result.get_single_value<T>());
    threading::parallel_for(IndexRange(int64_t(size.x) * size.y), 4096, [&](IndexRange range) {
      for (const int64_t i : range) {
        copy_v4_v4(r_pixels + i * 4, value);
      }
    });
    return;
  }
  threading::parallel_for(IndexRange(size.y), 16, [&](IndexRange rows) {
    for (const int y : rows) {
      for (int x = 0; x < size.x; x++) {
        const float4 value = to_rgba(result.load_pixel<T>(int2(x, y)));
        copy_v4_v4(r_pixels + (int64_t(y) * size.x + x) * 4, value);
      }
    }
  });
}

static void copy_result_to_rgba(const Result &result, const int2 size, float *r_pixels)
{
  switch (result.type()) {
    case ResultType::Float:
      copy_result_typed<float>(result, size, r_pixels);
      break;
    case ResultType::Float2:
      copy_result_typed<float2>(result, size, r_pixels);
      break;
    case ResultType::Float3:
      copy_result_typed<float3>(result, size, r_pixels);
      break;
    case ResultType::Float4:
      copy_result_typed<float4>(result, size, r_pixels);
      break;
    case ResultType::Color:
      copy_result_typed<Color>(result, size, r_pixels);
      break;
    case ResultType::Int:
      copy_result_typed<int32_t>(result, size, r_pixels);
      break;
    case ResultType::Bool:
      copy_result_typed<bool>(result, size, r_pixels);
      break;
    default: {
      const float4 black(0.0f, 0.0f, 0.0f, 1.0f);
      for (int64_t i = 0; i < int64_t(size.x) * size.y; i++) {
        copy_v4_v4(r_pixels + i * 4, black);
      }
      break;
    }
  }
}

/**
 * Find or create the generated float image with the given name, sized to the graph. Images the
 * user turned into something else (a file, a movie) are left alone.
 */
static Image *ensure_output_image(Main *bmain,
                                  const bNodeTree &ntree,
                                  const std::string &name,
                                  const int2 size,
                                  const bool is_data)
{
  Image *image = reinterpret_cast<Image *>(
      BKE_libblock_find_name(bmain, ID_IM, name.c_str()));
  if (image == nullptr) {
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    image = BKE_image_add_generated(
        bmain, size.x, size.y, name.c_str(), 128, true, IMA_GENTYPE_BLANK, black, false, is_data,
        false);
    if (image) {
      /* Images are owned by the shaders that use them, not by the graph. */
      id_us_min(&image->id);
      mark_image_owner(*image, ntree);
    }
    return image;
  }
  if (image->source != IMA_SRC_GENERATED) {
    return nullptr;
  }
  mark_image_owner(*image, ntree);
  /* The generated size and format live on the first tile. */
  ImageTile *tile = BKE_image_get_tile(image, 0);
  if (tile &&
      (tile->gen_x != size.x || tile->gen_y != size.y || !(tile->gen_flag & IMA_GEN_FLOAT)))
  {
    tile->gen_x = size.x;
    tile->gen_y = size.y;
    tile->gen_flag |= IMA_GEN_FLOAT;
    BKE_image_signal(bmain, image, nullptr, IMA_SIGNAL_FREE);
  }
  return image;
}

/**
 * An image a run of a graph made, waiting to be written into its image data-block. Runs in the
 * background can't touch the data-blocks, so they hand these over to the main thread.
 */
struct PendingImage {
  std::string name;
  bool is_data = false;
  /** Outputs of the graph also get a texture, the viewer does not. */
  bool is_output = false;
  /** RGBA at the graph's full size. */
  Array<float> pixels;
};

/** Stretch RGBA pixels to another size, blending the nearest ones and wrapping at the edges. */
static Array<float> resample_rgba(const Span<float> pixels, const int2 from, const int2 to)
{
  Array<float> result(int64_t(to.x) * to.y * 4);
  const float2 step = float2(from) / float2(to);
  threading::parallel_for(IndexRange(to.y), 16, [&](const IndexRange rows) {
    for (const int y : rows) {
      const float fy = (float(y) + 0.5f) * step.y - 0.5f;
      const int y0 = int(std::floor(fy));
      const float ty = fy - float(y0);
      const int ya = ((y0 % from.y) + from.y) % from.y;
      const int yb = (ya + 1) % from.y;
      for (int x = 0; x < to.x; x++) {
        const float fx = (float(x) + 0.5f) * step.x - 0.5f;
        const int x0 = int(std::floor(fx));
        const float tx = fx - float(x0);
        const int xa = ((x0 % from.x) + from.x) % from.x;
        const int xb = (xa + 1) % from.x;
        const auto load = [&](const int px, const int py) {
          return float4(&pixels[(int64_t(py) * from.x + px) * 4]);
        };
        const float4 bottom = math::interpolate(load(xa, ya), load(xb, ya), tx);
        const float4 top = math::interpolate(load(xa, yb), load(xb, yb), tx);
        copy_v4_v4(&result[(int64_t(y) * to.x + x) * 4], math::interpolate(bottom, top, ty));
      }
    }
  });
  return result;
}

static Image *write_pixels_to_image(Main *bmain,
                                    const bNodeTree &ntree,
                                    const std::string &name,
                                    const Span<float> pixels,
                                    const int2 size,
                                    const bool is_data)
{
  Image *image = ensure_output_image(bmain, ntree, name, size, is_data);
  if (image == nullptr) {
    return nullptr;
  }

  BLI_thread_lock(LOCK_DRAW_IMAGE);
  void *lock;
  ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
  if (ibuf && ibuf->x == size.x && ibuf->y == size.y &&
      pixels.size() == int64_t(size.x) * size.y * 4)
  {
    if (ibuf->float_buffer.data == nullptr) {
      IMB_alloc_float_pixels(ibuf, 4, false);
    }
    if (ibuf->float_buffer.data && ibuf->channels == 4) {
      memcpy(ibuf->float_buffer.data, pixels.data(), pixels.size_in_bytes());
      IMB_free_byte_pixels(ibuf);
      ibuf->userflags |= IB_DISPLAY_BUFFER_INVALID;
    }
  }
  BKE_image_release_ibuf(image, ibuf, lock);
  BLI_thread_unlock(LOCK_DRAW_IMAGE);

  BKE_image_partial_update_mark_full_update(image);
  /* Let modifiers, textures and shaders that read the image see the new pixels. */
  DEG_id_tag_update(&image->id, 0);
  return image;
}

static void write_pending_images(Main *bmain,
                                 const bNodeTree &ntree,
                                 const Span<PendingImage> images,
                                 const int2 size)
{
  for (const PendingImage &pending : images) {
    Image *image = write_pixels_to_image(
        bmain, ntree, pending.name, pending.pixels, size, pending.is_data);
    if (image && pending.is_output) {
      ensure_output_texture(bmain, ntree, *image);
    }
  }
}

/* -------------------------------------------------------------------- */
/** \name Signature
 * \{ */

struct Signature {
  uint64_t h = 1469598103934665603ull;

  void bytes(const void *data, const size_t size)
  {
    const uint8_t *p = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < size; i++) {
      h = (h ^ p[i]) * 1099511628211ull;
    }
  }
  template<typename T> void value(const T &v)
  {
    this->bytes(&v, sizeof(T));
  }
  void string(const char *s)
  {
    if (s) {
      this->bytes(s, strlen(s));
    }
    this->value(uint8_t(0));
  }
  /** The bytes of a block made with the guarded allocator, like socket values and node data. */
  void block(const void *data)
  {
    if (data) {
      this->bytes(data, MEM_allocN_len(data));
    }
  }

  /** A socket's own value, without parts that change when nothing was edited. */
  void socket_value(const bNodeSocket &socket)
  {
    if (socket.default_value == nullptr) {
      return;
    }
    if (socket.type == SOCK_MENU) {
      this->value(static_cast<const bNodeSocketValueMenu *>(socket.default_value)->value);
      return;
    }
    this->block(socket.default_value);
  }

  /** The settings of a node, not its inputs. */
  void node_settings(const bNode &node, const int depth)
  {
    this->string(node.idname);
    this->value(int(node.flag & NODE_MUTED));
    this->value(node.custom1);
    this->value(node.custom2);
    this->value(node.custom3);
    this->value(node.custom4);
    this->block(node.storage);
    this->id(node.id, depth);
  }

  /** An ID used by a node. Node groups go by what is in them, since a background run works on a
   * copy of the graph and its groups; other IDs are shared, so they go by their address. */
  void id(const ID *id, const int depth)
  {
    if (id && GS(id->name) == ID_NT) {
      if (depth < 16) {
        this->tree(*reinterpret_cast<const bNodeTree *>(id), depth + 1);
      }
      return;
    }
    this->value(id);
  }

  /** Everything in a tree that changes what it makes. Nodes and sockets go by their identifiers,
   * not their addresses, so a copy of a tree gets the same signature. */
  void tree(const bNodeTree &ntree, const int depth)
  {
    this->value(ntree.texture_graph_flag);
    this->value(ntree.active_viewer_key);
    const int2 size = texture_graph_size(ntree);
    this->value(size);
    for (const bNodeTreeInterfaceSocket *socket : ntree.interface_inputs()) {
      this->string(socket->identifier);
      this->block(socket->socket_data);
    }
    for (const bNodeTreeInterfaceSocket *socket : ntree.interface_outputs()) {
      this->string(socket->identifier);
      this->string(socket->name);
    }
    for (const bNode *node : ntree.all_nodes()) {
      if (node->is_frame()) {
        continue;
      }
      this->value(node->identifier);
      this->string(node->idname);
      this->value(int(node->flag & (NODE_MUTED | NODE_PREVIEW | NODE_DO_OUTPUT)));
      this->value(node->custom1);
      this->value(node->custom2);
      this->value(node->custom3);
      this->value(node->custom4);
      this->block(node->storage);
      this->id(node->id, depth);
      for (const bNodeSocket &socket : node->inputs) {
        this->string(socket.identifier);
        this->value(int(socket.flag & SOCK_UNAVAIL));
        this->socket_value(socket);
      }
      for (const bNodeSocket &socket : node->outputs) {
        this->string(socket.identifier);
        this->value(int(socket.flag & SOCK_UNAVAIL));
      }
    }
    for (const bNodeLink &link : ntree.links) {
      this->value(link.fromnode ? link.fromnode->identifier : -1);
      this->string(link.fromsock ? link.fromsock->identifier : "");
      this->value(link.tonode ? link.tonode->identifier : -1);
      this->string(link.tosock ? link.tosock->identifier : "");
      this->value(int(link.flag & (NODE_LINK_MUTED | NODE_LINK_VALID)));
    }
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Kept Results
 *
 * What each node made is kept after a graph is computed. A node is known by a key made from its
 * settings and the keys of the nodes feeding it, so when nothing before a node changed, the same
 * key comes back and its results are used again instead of running it.
 * \{ */

struct KeptResults {
  Map<std::string, Result> outputs;
  int64_t bytes = 0;

  void free()
  {
    for (Result &result : outputs.values()) {
      result.free();
    }
    outputs.clear();
  }
};

struct GraphCache {
  Map<uint64_t, KeptResults> entries;
  Set<uint64_t> used;
  int64_t bytes = 0;

  void clear()
  {
    for (KeptResults &kept : entries.values()) {
      kept.free();
    }
    entries.clear();
    used.clear();
    bytes = 0;
  }

  /** Let go of what the last computation did not use. */
  void drop_unused()
  {
    Vector<uint64_t> unused;
    for (const uint64_t key : entries.keys()) {
      if (!used.contains(key)) {
        unused.append(key);
      }
    }
    for (const uint64_t key : unused) {
      KeptResults &kept = entries.lookup(key);
      bytes -= kept.bytes;
      kept.free();
      entries.remove(key);
    }
    used.clear();
  }
};

/** Most memory kept for one graph. */
static constexpr int64_t max_kept_bytes = int64_t(4) << 30;

/** Kept results of each graph, by session UID. Drafts are kept apart, so a draft run does not
 * throw away what the full size run kept. */
static Map<uint32_t, GraphCache> &graph_caches()
{
  static Map<uint32_t, GraphCache> caches;
  return caches;
}
static Map<uint32_t, GraphCache> &draft_caches()
{
  static Map<uint32_t, GraphCache> caches;
  return caches;
}

/** Held while a graph is computed, by the background job or a direct run, and while the kept
 * results are changed. */
static std::mutex &cache_mutex()
{
  static std::mutex mutex;
  return mutex;
}

/** Only touched on the main thread. */
struct GraphStats {
  /** How long each node took the last time it ran at full size. */
  Map<bNodeInstanceKey, timeit::Nanoseconds> node_times;
  /** Seconds the last full size run took, negative when unknown. */
  double full_seconds = -1.0;
};
static Map<uint32_t, GraphStats> &graph_stats()
{
  static Map<uint32_t, GraphStats> stats;
  return stats;
}

static int64_t result_bytes(const Result &result)
{
  if (result.is_single_value()) {
    return 64;
  }
  const int2 size = result.domain().data_size;
  return int64_t(size.x) * size.y * 16;
}

/** \} */

class TextureGraphContext : public Context {
 private:
  const Scene &scene_;
  const bNodeTree &ntree_;
  /** Name of the original graph, for the image names. */
  std::string graph_name_;
  /** The graph's size, and the size this run works at (smaller for a draft). */
  int2 full_size_;
  int2 size_;
  float draft_scale_ = 1.0f;
  const bool *stop_ = nullptr;
  Profiler *profiler_ = nullptr;
  Vector<PendingImage> images_;
  /* Progress shown in the status bar: this run fills the part from progress_start_ to
   * progress_end_. Counted in steps done out of the nodes in the graph. */
  float *progress_ = nullptr;
  bool *do_update_ = nullptr;
  float progress_start_ = 0.0f;
  float progress_end_ = 1.0f;
  int total_steps_ = 1;
  mutable int done_steps_ = 0;
  /* Results kept from earlier runs of this graph, and the key of each node in this run. */
  GraphCache *cache_ = nullptr;
  uint64_t salt_ = 0;
  Map<const bNode *, uint64_t> keys_;

  uint64_t node_key(const bNode &node)
  {
    if (const uint64_t *key = keys_.lookup_ptr(&node)) {
      return *key;
    }
    Signature signature;
    signature.value(salt_);
    signature.node_settings(node, 0);
    for (const bNodeSocket *input : node.input_sockets()) {
      signature.string(input->identifier);
      const bool available = is_socket_available(input);
      signature.value(available);
      if (!available) {
        continue;
      }
      const bNodeSocket *from = get_output_linked_to_input(*input);
      if (from) {
        signature.value(this->node_key(from->owner_node()));
        signature.string(from->identifier);
      }
      else {
        signature.socket_value(*input);
      }
    }
    keys_.add(&node, signature.h);
    return signature.h;
  }

  /** Only nodes of the graph itself are kept, not those inside node groups. */
  bool keeps(const bNode &node) const
  {
    if (cache_ == nullptr || &node.owner_tree() != &ntree_) {
      return false;
    }
    /* Nodes without outputs write images, they always run. */
    for (const bNodeSocket *output : node.output_sockets()) {
      if (is_socket_available(output)) {
        return true;
      }
    }
    return false;
  }

 public:
  void use_cache(GraphCache &cache)
  {
    cache_ = &cache;
    Signature signature;
    signature.value(size_);
    for (const bNodeTreeInterfaceSocket *socket : ntree_.interface_inputs()) {
      signature.string(socket->identifier);
      signature.block(socket->socket_data);
    }
    salt_ = signature.h;
  }

  bool load_node_results(const bNode &node, NodeOperation &operation) override
  {
    if (!this->keeps(node)) {
      return false;
    }
    const uint64_t key = this->node_key(node);
    KeptResults *kept = cache_->entries.lookup_ptr(key);
    if (kept == nullptr) {
      return false;
    }
    for (const bNodeSocket *output : node.output_sockets()) {
      if (!is_socket_available(output)) {
        continue;
      }
      Result &result = operation.get_result(output->identifier);
      if (!result.should_compute()) {
        continue;
      }
      const Result *old = kept->outputs.lookup_ptr(output->identifier);
      if (old == nullptr || old->type() != result.type() || result.is_allocated()) {
        return false;
      }
    }
    for (const bNodeSocket *output : node.output_sockets()) {
      if (!is_socket_available(output)) {
        continue;
      }
      Result &result = operation.get_result(output->identifier);
      if (result.should_compute()) {
        result.share_data(kept->outputs.lookup(output->identifier));
      }
    }
    cache_->used.add(key);
    return true;
  }

  void store_node_results(const bNode &node, NodeOperation &operation) override
  {
    if (!this->keeps(node)) {
      return;
    }
    const uint64_t key = this->node_key(node);
    if (cache_->entries.contains(key)) {
      cache_->used.add(key);
      return;
    }
    KeptResults kept;
    for (const bNodeSocket *output : node.output_sockets()) {
      if (!is_socket_available(output)) {
        continue;
      }
      Result &result = operation.get_result(output->identifier);
      if (!result.is_allocated()) {
        continue;
      }
      Result copy(*this, result.type(), result.precision());
      copy.share_data(result);
      kept.bytes += result_bytes(result);
      kept.outputs.add(output->identifier, copy);
    }
    if (kept.outputs.is_empty() || cache_->bytes + kept.bytes > max_kept_bytes) {
      kept.free();
      return;
    }
    cache_->bytes += kept.bytes;
    cache_->entries.add(key, std::move(kept));
    cache_->used.add(key);
  }

 public:
  TextureGraphContext(StaticCacheManager &cache_manager,
                      const Scene &scene,
                      const bNodeTree &ntree,
                      std::string graph_name,
                      const float draft_scale,
                      const bool *stop,
                      Profiler *profiler)
      : Context(cache_manager),
        scene_(scene),
        ntree_(ntree),
        graph_name_(std::move(graph_name)),
        full_size_(texture_graph_size(ntree)),
        draft_scale_(draft_scale),
        stop_(stop),
        profiler_(profiler)
  {
    size_ = draft_scale_ < 1.0f ?
                math::max(int2(math::round(float2(full_size_) * draft_scale_)), int2(1)) :
                full_size_;
  }

  float draft_scale() const override
  {
    return draft_scale_;
  }

  /** A run in the background also stops on Esc. */
  bool is_canceled() const override
  {
    return stop_ && (*stop_ || G.is_break);
  }

  void show_progress(float *progress, bool *do_update, const float start, const float end)
  {
    progress_ = progress;
    do_update_ = do_update;
    progress_start_ = start;
    progress_end_ = end;
    total_steps_ = math::max(count_nodes(ntree_, 0), 1);
  }

  /** Nodes that run as a step, inside node groups too. */
  static int count_nodes(const bNodeTree &ntree, const int depth)
  {
    int count = 0;
    for (const bNode *node : ntree.all_nodes()) {
      if (node->is_frame() || node->is_reroute() || node->is_group_input() ||
          node->is_group_output())
      {
        continue;
      }
      if (node->is_group() && node->id && depth < 16) {
        count += count_nodes(*reinterpret_cast<const bNodeTree *>(node->id), depth + 1);
        continue;
      }
      count++;
    }
    return count;
  }

  void evaluate_operation_post() const override
  {
    if (progress_ == nullptr) {
      return;
    }
    done_steps_++;
    /* Nodes computed together count once, so it may not reach the end before the run does. */
    const float part = math::min(float(done_steps_) / float(total_steps_), 0.99f);
    *progress_ = progress_start_ + (progress_end_ - progress_start_) * part;
    if (do_update_) {
      *do_update_ = true;
    }
  }

  Profiler *profiler() const override
  {
    return profiler_;
  }

  Vector<PendingImage> &images()
  {
    return images_;
  }

  /** Copy a result into a pending image at the graph's full size. */
  void keep_image(const char *output_name,
                  const Result &result,
                  const bool is_data,
                  const bool is_output)
  {
    PendingImage pending;
    pending.name = graph_name_ + " " + output_name;
    pending.is_data = is_data;
    pending.is_output = is_output;
    Array<float> pixels(int64_t(size_.x) * size_.y * 4);
    copy_result_to_rgba(result, size_, pixels.data());
    pending.pixels = size_ == full_size_ ? std::move(pixels) :
                                           resample_rgba(pixels, size_, full_size_);
    images_.append(std::move(pending));
  }

  const Scene &get_scene() const override
  {
    return scene_;
  }

  Domain get_compositing_domain() const override
  {
    return Domain(size_);
  }

  bool use_gpu() const override
  {
    return false;
  }

  /* Every texture made by a graph should tile, so reads past an edge come from the other side. */
  bool wrap_edges() const override
  {
    return true;
  }

  ResultPrecision get_precision() const override
  {
    return ResultPrecision::Full;
  }

  /** Realize the result on the graph's domain if needed, then hand it to \a write. */
  template<typename Fn> void write_realized(Result &result, const Fn &write)
  {
    const InputDescriptor input_descriptor = {result.type(),
                                              InputRealizationMode::OperationDomain};
    SimpleOperation *realization = RealizeOnDomainOperation::construct_if_needed(
        *this, result, input_descriptor, this->get_compositing_domain());
    if (realization) {
      Result realize_input = this->create_result(result.type(), result.precision());
      realize_input.wrap_external(result);
      realization->map_input_to_result(&realize_input);
      realization->evaluate();
      Result &realized = realization->get_result();
      write(realized);
      realized.release();
      delete realization;
      return;
    }
    write(result);
  }

  void write_viewer(Result &viewer_result) override
  {
    const bool is_data = viewer_result.meta_data.is_non_color_data;
    this->write_realized(viewer_result, [&](const Result &result) {
      this->keep_image("Viewer", result, is_data, false);
    });
  }

  /** Make a single value result from the default value of a graph input. */
  void set_input_default(const bNodeTreeInterfaceSocket &socket, Result &result)
  {
    const bke::bNodeSocketType *typeinfo = socket.socket_typeinfo();
    const eNodeSocketDatatype type = typeinfo ? typeinfo->type : SOCK_CUSTOM;
    switch (type) {
      case SOCK_FLOAT:
        result = this->create_result(ResultType::Float);
        result.allocate_single_value();
        result.set_single_value(
            static_cast<const bNodeSocketValueFloat *>(socket.socket_data)->value);
        return;
      case SOCK_INT:
        result = this->create_result(ResultType::Int);
        result.allocate_single_value();
        result.set_single_value(
            int32_t(static_cast<const bNodeSocketValueInt *>(socket.socket_data)->value));
        return;
      case SOCK_BOOLEAN:
        result = this->create_result(ResultType::Bool);
        result.allocate_single_value();
        result.set_single_value(
            bool(static_cast<const bNodeSocketValueBoolean *>(socket.socket_data)->value));
        return;
      case SOCK_VECTOR:
        result = this->create_result(ResultType::Float3);
        result.allocate_single_value();
        result.set_single_value(
            float3(static_cast<const bNodeSocketValueVector *>(socket.socket_data)->value));
        return;
      case SOCK_RGBA:
        result = this->create_result(ResultType::Color);
        result.allocate_single_value();
        result.set_single_value(
            Color(float4(static_cast<const bNodeSocketValueRGBA *>(socket.socket_data)->value)));
        return;
      default:
        result = this->create_result(ResultType::Color);
        result.allocate_invalid();
        return;
    }
  }

  void evaluate(const bool with_previews)
  {
    NodeGroupOutputTypes needed_outputs = NodeGroupOutputTypes::GroupOutputNode |
                                          NodeGroupOutputTypes::ViewerNode;
    if (with_previews) {
      /* Nodes not linked to an output still show what they make. */
      needed_outputs |= NodeGroupOutputTypes::NodePreviews | NodeGroupOutputTypes::UnlinkedNodes;
    }
    Map<bNodeInstanceKey, bke::bNodePreview> *node_previews = with_previews ?
                                                                  &ntree_.runtime->previews :
                                                                  nullptr;

    NodeGroupOperation operation(*this,
                                 ntree_,
                                 needed_outputs,
                                 node_previews,
                                 ntree_.active_viewer_key,
                                 bke::NODE_INSTANCE_KEY_BASE);

    ntree_.ensure_interface_cache();

    /* Every output of the graph becomes an image. */
    for (const bNodeTreeInterfaceSocket *output_socket : ntree_.interface_outputs()) {
      operation.get_result(output_socket->identifier).set_reference_count(1);
    }

    /* Graph inputs act as parameters of the graph, they take their default values. */
    Vector<std::unique_ptr<Result>> inputs;
    for (const bNodeTreeInterfaceSocket *input_socket : ntree_.interface_inputs()) {
      std::unique_ptr<Result> input = std::make_unique<Result>(
          this->create_result(ResultType::Float));
      this->set_input_default(*input_socket, *input);
      operation.map_input_to_result(input_socket->identifier, input.get());
      inputs.append(std::move(input));
    }

    operation.evaluate();

    for (const bNodeTreeInterfaceSocket *output_socket : ntree_.interface_outputs()) {
      Result &output = operation.get_result(output_socket->identifier);
      if (!output.should_compute()) {
        continue;
      }
      const char *output_name = output_socket->name ? output_socket->name : "";
      const bool is_data = !output_holds_color(output_name, output.type());
      if (!this->is_canceled()) {
        this->write_realized(output, [&](const Result &result) {
          this->keep_image(output_name, result, is_data, true);
        });
      }
      output.release();
    }
  }
};


void ED_texture_graph_clear_cache()
{
  std::lock_guard lock(cache_mutex());
  for (Map<uint32_t, GraphCache> *caches : {&graph_caches(), &draft_caches()}) {
    for (GraphCache &cache : caches->values()) {
      cache.clear();
    }
    caches->clear();
  }
  graph_stats().clear();
}

uint64_t ED_texture_graph_signature(const bNodeTree *ntree)
{
  if (ntree == nullptr) {
    return 0;
  }
  const_cast<bNodeTree *>(ntree)->ensure_interface_cache();
  Signature signature;
  signature.value(ntree->id.session_uid);
  signature.tree(*ntree, 0);
  return signature.h == 0 ? 1 : signature.h;
}

const Map<bNodeInstanceKey, timeit::Nanoseconds> *ED_texture_graph_node_times(
    const bNodeTree *ntree)
{
  if (ntree == nullptr) {
    return nullptr;
  }
  const GraphStats *stats = graph_stats().lookup_ptr(ntree->id.session_uid);
  return stats ? &stats->node_times : nullptr;
}

static bNodeTree *find_graph(Main *bmain, const uint32_t uid)
{
  for (bNodeTree &tree : bmain->nodetrees) {
    if (tree.id.session_uid == uid) {
      return &tree;
    }
  }
  return nullptr;
}

/** Let go of results kept for graphs that are gone. Skipped while a graph is being computed. */
static void drop_gone_graphs(Main *bmain)
{
  std::unique_lock lock(cache_mutex(), std::try_to_lock);
  if (!lock.owns_lock()) {
    return;
  }
  Set<uint32_t> alive;
  for (const bNodeTree &tree : bmain->nodetrees) {
    alive.add(tree.id.session_uid);
  }
  for (Map<uint32_t, GraphCache> *caches : {&graph_caches(), &draft_caches()}) {
    Vector<uint32_t> gone;
    for (const uint32_t uid : caches->keys()) {
      if (!alive.contains(uid)) {
        gone.append(uid);
      }
    }
    for (const uint32_t uid : gone) {
      caches->lookup(uid).clear();
      caches->remove(uid);
    }
  }
}

/**
 * Compute a graph, using and updating the results kept from earlier runs at the same size.
 * Returns false when it was stopped before the end; then it hands over no images.
 */
static bool run_graph(const Scene &scene,
                      const bNodeTree &ntree,
                      const uint32_t uid,
                      const std::string &name,
                      const float draft_scale,
                      const bool with_previews,
                      wmJobWorkerStatus *worker_status,
                      const float progress_start,
                      const float progress_end,
                      Profiler *profiler,
                      Vector<PendingImage> &r_images)
{
  const bool *stop = worker_status ? &worker_status->stop : nullptr;
  std::lock_guard lock(cache_mutex());
  Map<uint32_t, GraphCache> &caches = draft_scale < 1.0f ? draft_caches() : graph_caches();
  GraphCache &cache = caches.lookup_or_add_default(uid);

  const_cast<bNodeTree &>(ntree).ensure_interface_cache();
  StaticCacheManager cache_manager;
  TextureGraphContext context(cache_manager, scene, ntree, name, draft_scale, stop, profiler);
  context.use_cache(cache);
  if (worker_status) {
    context.show_progress(
        &worker_status->progress, &worker_status->do_update, progress_start, progress_end);
  }
  context.evaluate(with_previews);
  const bool finished = !context.is_canceled();
  if (finished) {
    cache.drop_unused();
    r_images = std::move(context.images());
  }
  else {
    /* What did run is kept, the next run starts from there. */
    cache.used.clear();
  }
  cache_manager.reset();
  return finished;
}

void ED_texture_graph_evaluate(Main *bmain,
                               Scene *scene,
                               bNodeTree *ntree,
                               const bool with_previews)
{
  if (bmain == nullptr || scene == nullptr || ntree == nullptr ||
      ntree->type != NTREE_TEXTURE_GRAPH)
  {
    return;
  }

  /* A run in the background would write its images after this one, stop it. */
  if (wmWindowManager *wm = static_cast<wmWindowManager *>(bmain->wm.first)) {
    WM_jobs_kill_type(wm, ntree, WM_JOB_TYPE_TEXTURE_GRAPH);
  }

  /* A graph is often used by nothing but its images, keep it saved in the file anyway. */
  if (!ID_IS_LINKED(ntree)) {
    id_fake_user_set(&ntree->id);
  }
  drop_gone_graphs(bmain);

  const uint32_t uid = ntree->id.session_uid;
  Profiler profiler;
  Vector<PendingImage> images;
  run_graph(*scene,
            *ntree,
            uid,
            ntree->id.name + 2,
            1.0f,
            with_previews,
            nullptr,
            0.0f,
            1.0f,
            &profiler,
            images);
  write_pending_images(bmain, *ntree, images, texture_graph_size(*ntree));
  for (const auto item : profiler.get_nodes_evaluation_times().items()) {
    graph_stats().lookup_or_add_default(uid).node_times.add_overwrite(item.key, item.value);
  }
  remove_stale_images(bmain, *ntree);
}

/* -------------------------------------------------------------------- */
/** \name Background Job
 *
 * Edits in the node editor compute the graph in the background, on a copy of it, so the editor
 * never waits. Each new edit stops the run and starts again. A big graph that was slow last time
 * first runs at a small size, which shows up quickly, then at its full size.
 * \{ */

/** Longest side of the quick first run. */
static constexpr int draft_size = 512;
/** Graphs that took less than this at full size skip the quick first run. */
static constexpr double draft_seconds = 0.15;

static float draft_scale_for(const bNodeTree &ntree)
{
  const int2 size = texture_graph_size(ntree);
  const int longest = math::max(size.x, size.y);
  if (longest <= draft_size) {
    return 1.0f;
  }
  const GraphStats *stats = graph_stats().lookup_ptr(ntree.id.session_uid);
  const double seconds = stats ? stats->full_seconds : -1.0;
  /* Not run yet: only big images are worth a draft. */
  const bool slow = seconds >= 0.0 ? seconds >= draft_seconds : longest >= 2048;
  return slow ? float(draft_size) / float(longest) : 1.0f;
}

struct TextureGraphJob {
  Main *bmain = nullptr;
  Scene *scene = nullptr;
  uint32_t uid = 0;
  std::string name;
  int2 size = int2(0);
  float draft_scale = 1.0f;

  /* Copies of the graph, made when the run starts. The draft has its own copy, so its node
   * previews can be shown while the full size run fills in those of the other. */
  bNodeTree *draft_tree = nullptr;
  bNodeTree *full_tree = nullptr;

  std::mutex mutex;
  Vector<PendingImage> draft_images;
  bool draft_ready = false;
  bool draft_shown = false;
  Vector<PendingImage> full_images;
  bool full_ready = false;
  double full_seconds = 0.0;
  Profiler profiler;
};

static void texture_graph_job_init(void *customdata)
{
  TextureGraphJob &job = *static_cast<TextureGraphJob *>(customdata);
  G.is_break = false;
  bNodeTree *ntree = find_graph(job.bmain, job.uid);
  if (ntree == nullptr) {
    return;
  }
  if (!ID_IS_LINKED(ntree)) {
    id_fake_user_set(&ntree->id);
  }
  drop_gone_graphs(job.bmain);
  ntree->ensure_interface_cache();
  job.name = ntree->id.name + 2;
  job.size = texture_graph_size(*ntree);
  job.draft_scale = draft_scale_for(*ntree);
  job.full_tree = bke::node_tree_localize(ntree, nullptr);
  if (job.draft_scale < 1.0f) {
    job.draft_tree = bke::node_tree_localize(ntree, nullptr);
  }
}

static void texture_graph_job_start(void *customdata, wmJobWorkerStatus *worker_status)
{
  TextureGraphJob &job = *static_cast<TextureGraphJob *>(customdata);
  if (job.full_tree == nullptr) {
    return;
  }
  /* The full size run takes most of the time, the draft fills the first part of the bar. */
  const float draft_part = job.draft_tree ? 0.2f : 0.0f;

  if (job.draft_tree) {
    Vector<PendingImage> images;
    if (!run_graph(*job.scene,
                   *job.draft_tree,
                   job.uid,
                   job.name,
                   job.draft_scale,
                   true,
                   worker_status,
                   0.0f,
                   draft_part,
                   nullptr,
                   images))
    {
      return;
    }
    {
      std::lock_guard lock(job.mutex);
      job.draft_images = std::move(images);
      job.draft_ready = true;
    }
    worker_status->do_update = true;
    worker_status->progress = draft_part;
  }

  const timeit::TimePoint start = timeit::Clock::now();
  Vector<PendingImage> images;
  if (!run_graph(*job.scene,
                 *job.full_tree,
                 job.uid,
                 job.name,
                 1.0f,
                 true,
                 worker_status,
                 draft_part,
                 1.0f,
                 &job.profiler,
                 images))
  {
    return;
  }
  const timeit::Nanoseconds duration = timeit::Clock::now() - start;
  std::lock_guard lock(job.mutex);
  job.full_images = std::move(images);
  job.full_seconds = double(duration.count()) * 1e-9;
  job.full_ready = true;
  worker_status->progress = 1.0f;
}

static void notify_graph_changed()
{
  WM_main_add_notifier(NC_IMAGE | NA_PAINTING, nullptr);
  WM_main_add_notifier(NC_SPACE | ND_SPACE_NODE_VIEW, nullptr);
}

/** Show the quick first run while the full size one goes on. */
static void texture_graph_job_update(void *customdata)
{
  TextureGraphJob &job = *static_cast<TextureGraphJob *>(customdata);
  Vector<PendingImage> images;
  {
    std::lock_guard lock(job.mutex);
    if (!job.draft_ready || job.draft_shown || job.full_ready) {
      return;
    }
    job.draft_shown = true;
    images = std::move(job.draft_images);
  }
  bNodeTree *ntree = find_graph(job.bmain, job.uid);
  if (ntree == nullptr) {
    return;
  }
  write_pending_images(job.bmain, *ntree, images, job.size);
  bke::node_preview_merge_tree(ntree, job.draft_tree, true);
  notify_graph_changed();
}

static void texture_graph_job_complete(void *customdata)
{
  TextureGraphJob &job = *static_cast<TextureGraphJob *>(customdata);
  if (!job.full_ready) {
    return;
  }
  bNodeTree *ntree = find_graph(job.bmain, job.uid);
  if (ntree == nullptr) {
    return;
  }
  write_pending_images(job.bmain, *ntree, job.full_images, job.size);
  job.full_images.clear();
  bke::node_preview_merge_tree(ntree, job.full_tree, true);

  GraphStats &stats = graph_stats().lookup_or_add_default(job.uid);
  stats.full_seconds = job.full_seconds;
  /* Nodes that used kept results did not run, they keep the time of their last run. */
  for (const auto item : job.profiler.get_nodes_evaluation_times().items()) {
    stats.node_times.add_overwrite(item.key, item.value);
  }
  remove_stale_images(job.bmain, *ntree);
  notify_graph_changed();
}

static void texture_graph_job_free(void *customdata)
{
  TextureGraphJob *job = static_cast<TextureGraphJob *>(customdata);
  for (bNodeTree *tree : {job->draft_tree, job->full_tree}) {
    if (tree) {
      BKE_id_free(nullptr, &tree->id);
    }
  }
  MEM_delete(job);
}

void ED_texture_graph_job(const bContext *C, bNodeTree *ntree)
{
  if (ntree == nullptr || ntree->type != NTREE_TEXTURE_GRAPH) {
    return;
  }
  wmWindowManager *wm = CTX_wm_manager(C);
  wmJob *wm_job = WM_jobs_get(wm,
                              CTX_wm_window(C),
                              ntree,
                              "Texture Graph",
                              WM_JOB_PROGRESS,
                              WM_JOB_TYPE_TEXTURE_GRAPH);

  TextureGraphJob *job = MEM_new<TextureGraphJob>(__func__);
  job->bmain = CTX_data_main(C);
  job->scene = CTX_data_scene(C);
  job->uid = ntree->id.session_uid;

  WM_jobs_customdata_set(wm_job, job, texture_graph_job_free);
  WM_jobs_timer(wm_job, 0.05, 0, 0);
  WM_jobs_callbacks_ex(wm_job,
                       texture_graph_job_start,
                       texture_graph_job_init,
                       texture_graph_job_update,
                       nullptr,
                       texture_graph_job_complete,
                       nullptr);
  WM_jobs_start(wm, wm_job);
}

/** \} */

}  // namespace blender
