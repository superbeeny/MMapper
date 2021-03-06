#pragma once
// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2019 The MMapper Authors

#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>
#include <QDebug>
#include <qopengl.h>

#include "../global/Array.h"
#include "../global/Color.h"
#include "../global/utils.h"
#include "FontFormatFlags.h"

struct TexVert final
{
    glm::vec2 tex;
    glm::vec3 vert;

    explicit TexVert(const glm::vec2 &tex, const glm::vec3 &vert)
        : tex{tex}
        , vert{vert}
    {}
};

struct ColoredTexVert final
{
    Color color;
    glm::vec2 tex;
    glm::vec3 vert;

    explicit ColoredTexVert(const Color &color, const glm::vec2 &tex, const glm::vec3 &vert)
        : color{color}
        , tex{tex}
        , vert{vert}
    {}
};

struct ColorVert final
{
    Color color;
    glm::vec3 vert;

    explicit ColorVert(const Color &color, const glm::vec3 &vert)
        : color{color}
        , vert{vert}
    {}
};

// Similar to ColoredTexVert, except it has a base position in world coordinates.
// the font's vertex shader transforms the world position to screen space,
// rounds to integer pixel offset, and then adds the vertex position in screen space.
//
// Rendering with the font shader requires passing uniforms for the world space
// model-view-projection matrix and the output viewport.
struct FontVert3d final
{
    glm::vec3 base; // world space
    Color color;
    glm::vec2 tex;
    glm::vec2 vert; // screen space

    explicit FontVert3d(const glm::vec3 &base,
                        const Color &color,
                        const glm::vec2 &tex,
                        const glm::vec2 &vert)
        : base(base)
        , color(color)
        , tex(tex)
        , vert(vert)
    {}
};

enum class DrawModeEnum { INVALID = 0, POINTS = 1, LINES = 2, TRIANGLES = 3, QUADS = 4 };

struct LineParams final
{
    float width = 1.f;
    LineParams() = default;

    explicit LineParams(const float width)
        : width(width)
    {}
};

#define XFOREACH_DEPTHFUNC(X) \
    X(NEVER) \
    X(LESS) \
    X(EQUAL) \
    X(LEQUAL) \
    X(GREATER) \
    X(NOTEQUAL) \
    X(GEQUAL) \
    X(ALWAYS)

#define DECL(X) X = GL_##X,
enum class DepthFunctionEnum { XFOREACH_DEPTHFUNC(DECL) DEFAULT = LESS };
#undef DECL

enum class BlendModeEnum {
    /* glDisable(GL_BLEND); */
    NONE,
    /* This is the MMapper2 default setting, but not OpenGL default setting
     * glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); */
    TRANSPARENCY,
    /* This mode allows you to multiply by the painted color, in the range [0,1].
     * glEnable(GL_BLEND); glBlendFuncSeparate(GL_ZERO, GL_SRC_COLOR, GL_ZERO, GL_ONE); */
    MODULATE,
};

enum class CullingEnum {

    // Culling is disabled: glDisable(GL_CULL_FACE)
    DISABLED,

    // GL_BACK: back faces are culled (the usual default if GL_CULL_FACE is enabled)
    BACK,

    // GL_FRONT: front faces are culled
    FRONT,

    // GL_FRONT_AND_BACK: both front and back faces are culled
    // (you probably don't ever want this)
    FRONT_AND_BACK

};

class MMTexture;
using SharedMMTexture = std::shared_ptr<MMTexture>;

struct GLRenderState final
{
    // glEnable(GL_BLEND)
    BlendModeEnum blend = BlendModeEnum::NONE;

    CullingEnum culling = CullingEnum::DISABLED;

    // glEnable(GL_DEPTH_TEST) + glDepthFunc()
    using OptDepth = std::optional<DepthFunctionEnum>;
    OptDepth depth;

    // glLineWidth() + { glEnable(LINE_STIPPLE) + glLineStipple() }
    LineParams lineParams;

    using Textures = MMapper::Array<SharedMMTexture, 2>;
    struct Uniforms final
    {
        Color color;
        // glEnable(TEXTURE_2D), or glEnable(TEXTURE_3D)
        Textures textures;
        std::optional<float> pointSize;
    };

    Uniforms uniforms;

    GLRenderState withBlend(const BlendModeEnum new_blend) const
    {
        GLRenderState copy = *this;
        copy.blend = new_blend;
        return copy;
    }

    GLRenderState withColor(const Color &new_color) const
    {
        GLRenderState copy = *this;
        copy.uniforms.color = new_color;
        return copy;
    }

    GLRenderState withCulling(const CullingEnum new_culling) const
    {
        GLRenderState copy = *this;
        copy.culling = new_culling;
        return copy;
    }

    GLRenderState withDepthFunction(const DepthFunctionEnum new_depth) const
    {
        GLRenderState copy = *this;
        copy.depth = new_depth;
        return copy;
    }

    GLRenderState withDepthFunction(std::nullopt_t) const
    {
        GLRenderState copy = *this;
        copy.depth.reset();
        return copy;
    }

    GLRenderState withLineParams(const LineParams &new_lineParams) const
    {
        GLRenderState copy = *this;
        copy.lineParams = new_lineParams;
        return copy;
    }

    GLRenderState withPointSize(const GLfloat size) const
    {
        GLRenderState copy = *this;
        copy.uniforms.pointSize = size;
        return copy;
    }

    GLRenderState withTexture0(const SharedMMTexture &new_texture) const
    {
        GLRenderState copy = *this;
        copy.uniforms.textures = Textures{new_texture, nullptr};
        return copy;
    }
};

struct NODISCARD IRenderable
{
public:
    IRenderable() = default;
    virtual ~IRenderable();

public:
    DELETE_CTORS_AND_ASSIGN_OPS(IRenderable);

public:
    // Clears the contents of the mesh, but does not give up its GL resources.
    virtual void clear() = 0;
    // Clears the mesh and destroys the GL resources.
    virtual void reset() = 0;
    virtual bool isEmpty() const = 0;

public:
    virtual void render(const GLRenderState &renderState) = 0;
};

struct NODISCARD TexturedRenderable final : IRenderable
{
private:
    SharedMMTexture m_texture;
    std::unique_ptr<IRenderable> m_mesh;

public:
    explicit TexturedRenderable(const SharedMMTexture &tex, std::unique_ptr<IRenderable> mesh);
    ~TexturedRenderable() override;

public:
    DELETE_CTORS_AND_ASSIGN_OPS(TexturedRenderable);

public:
    void clear() override;
    void reset() override;
    bool isEmpty() const override;

public:
    void render(const GLRenderState &renderState) override;

public:
    SharedMMTexture replaceTexture(const SharedMMTexture &tex)
    {
        return std::exchange(m_texture, tex);
    }
};

enum class BufferUsageEnum { STATIC_DRAW, DYNAMIC_DRAW };

class NODISCARD UniqueMesh final
{
private:
    std::unique_ptr<IRenderable> m_mesh;

public:
    UniqueMesh() = default;
    explicit UniqueMesh(std::unique_ptr<IRenderable> mesh)
        : m_mesh{std::move(mesh)}
    {
        deref(m_mesh);
    }
    ~UniqueMesh() = default;
    DEFAULT_MOVES_DELETE_COPIES(UniqueMesh);

    void render(const GLRenderState &rs) { deref(m_mesh).render(rs); }
};

struct NODISCARD UniqueMeshVector final
{
private:
    std::vector<UniqueMesh> m_meshes;

public:
    UniqueMeshVector() = default;
    explicit UniqueMeshVector(std::vector<UniqueMesh> &&meshes)
        : m_meshes{std::move(meshes)}
    {}

    void render(const GLRenderState &rs)
    {
        for (auto &mesh : m_meshes) {
            mesh.render(rs);
        }
    }
};

struct Viewport
{
    glm::ivec2 offset;
    glm::ivec2 size;
};

static constexpr const size_t VERTS_PER_LINE = 2;
static constexpr const size_t VERTS_PER_TRI = 3;
static constexpr const size_t VERTS_PER_QUAD = 4;
