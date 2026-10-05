#include "asset/importer.hpp"

#include "asset/image_ops.hpp"

#include <assimp/Importer.hpp>
#include <assimp/GltfMaterial.h>
#include <assimp/color4.h>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <stb/stb_image.h>

#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace my3d::asset
{
namespace
{

// ---------------------------------------------------------------- 类型转换

// assimp 的 aiMatrix4x4 是行主序（a1,b1,c1,d1 是第一行），引擎 Mat4 是列主序
// m[col * 4 + row]，所以这里是转置。
//
// 刻意用命名字段逐项展开，不用 aiMatrix4x4::operator[]：后者返回 aiVector3D
// （只有 x/y/z 三个分量），且步长语义是历史遗留的错位访问，col == 3 时根本取不到
// 第 4 列。也不用 (&a1)[col] 这类指针算术：这里不假设结构体成员的存放顺序，
// 16 个字段一眼可验，将来 assimp 改布局也不会静默出错。
//
// 关于转置：assimp 的文档说 aiMatrix4x4 是 row-major，很容易顺势推出「搬到列向量
// 约定必须转置」。实测结论是【不要转置】—— 逐元素直搬才与 math::compose 的语义一致。
//
// 判据（对 mia 的 98 个节点全跑过）：用 aiDecomposeMatrix 把 aiMatrix4x4 拆成
// T/R/S，再用 math::compose(t, quat, s) 重建，然后与两种搬法逐元素比最大差：
//     |compose - 直搬| = 0.000000   （81/81 个非单位阵节点）
//     |compose - 转置| = 2.000000   （90° 旋转被翻了号）
// 也就是说 assimp 的 a1..a4 就是矩阵的第 0 行，m[r][c] 就是「第 r 行第 c 列」，
// 与 my3d 的列向量约定（v' = M * v）直接对得上。
//
// 历史：旧 src/converter.hpp 和更早的 toMat4 都做了转置，把 assimp 的每一行
// 塞进一列。平移分量落在对称位置看不出来，旋转与缩放则是反的；静态模型因为整条
// 节点链一起错，看上去仍是「正常」的模型，一上蒙皮（boneWorld * inverseBind 的
// 复合顺序被转置打乱）就整具甩到屏幕外。动画通道里那次 transpose 是同一个错误
// 前提的第二个落点，一并去掉了。
math::Mat4 toMat4(const aiMatrix4x4 &src) noexcept
{
    math::Mat4 out;
    // 行 0（assimp 名称 a1..a4 就是第 0 行的四个分量）
    out.m[0] = src.a1;  // at(0,0)
    out.m[4] = src.a2;  // at(0,1)
    out.m[8] = src.a3;  // at(0,2)
    out.m[12] = src.a4; // at(0,3)
    // 行 1
    out.m[1] = src.b1;
    out.m[5] = src.b2;
    out.m[9] = src.b3;
    out.m[13] = src.b4;
    // 行 2
    out.m[2] = src.c1;
    out.m[6] = src.c2;
    out.m[10] = src.c3;
    out.m[14] = src.c4;
    // 行 3
    out.m[3] = src.d1;
    out.m[7] = src.d2;
    out.m[11] = src.d3;
    out.m[15] = src.d4;
    return out;
}

inline math::Vec3 toVec3(const aiVector3D &v) noexcept
{
    return {v.x, v.y, v.z};
}

inline math::Vec4 toVec4(const aiColor4D &c) noexcept
{
    return {c.r, c.g, c.b, c.a};
}

// 退化向量（零长度）会让 normalize 产出零向量，法线/切线不能是零向量。
inline math::Vec3 safeNormalize(const math::Vec3 &v, const math::Vec3 &fallback) noexcept
{
    const float len2 = math::lengthSq(v);
    if (!(len2 > 1e-20f))
        return fallback;
    return v * (1.0f / std::sqrt(len2));
}

// ---------------------------------------------------------------- 导入器

class SceneImporter
{
public:
    SceneImporter(const ImportOptions &options, const aiScene *scene, std::string baseDir)
        : m_options(&options), m_scene(scene), m_baseDir(std::move(baseDir))
    {
    }

    // 顶层流程：材质 → 网格 → 节点树。
    void run(const std::string &path)
    {
        m_out.sourcePath = path;

        m_out.materials.reserve(m_scene->mNumMaterials);
        for (unsigned i = 0; i < m_scene->mNumMaterials; ++i)
            m_out.materials.push_back(buildMaterial(m_scene->mMaterials[i], i));

        // meshes 与 assimp 的数组保持同序，节点里的索引才能直接引用。
        m_out.meshes.reserve(m_scene->mNumMeshes);
        for (unsigned i = 0; i < m_scene->mNumMeshes; ++i)
        {
            const aiMesh *mesh = m_scene->mMeshes[i];
            m_out.meshes.push_back(mesh != nullptr ? buildMesh(mesh)
                                                   : MeshData{});
        }

        m_out.root = buildNode(m_scene->mRootNode, kInvalidIndex);

        // 骨骼与动画都要「节点名 → 索引」，而这张表要整棵树建完才完整。
        // 放在这里做，而不是在 buildMesh 里边建边查。
        const std::unordered_map<std::string, uint32_t> nodeIndex = indexNodesByName();
        resolveSkinning(nodeIndex);
        buildAnimations(nodeIndex);

        const AssetNode &root = m_out.nodes[m_out.root];
        m_out.bounds = root.localBounds.transformed(root.localTransform);

        m_out.images = std::move(m_imagePool);
    }

    AssetScene takeScene() { return std::move(m_out); }
    std::vector<std::string> takeWarnings() { return std::move(m_warnings); }

private:
    void warn(std::string msg) { m_warnings.push_back(std::move(msg)); }

    // ------------------------------------------------------------ 贴图

    ImagePtr adopt(const std::shared_ptr<ImageData> &img, const std::string &key)
    {
        ImagePtr shared = downscaleToFit(img, m_options->maxTextureSize);
        m_imagePool.push_back(shared);
        m_textureCache.emplace(key, shared);
        return shared;
    }

    // glTF 内嵌贴图。mHeight == 0 表示 pcData 是压缩字节流（png/jpg...），
    // 交给 stb 解码；否则 pcData 是已解码的 aiTexel 数组。
    ImagePtr loadEmbedded(const aiTexture *tex, const std::string &key, ColorSpace cs)
    {
        int width = 0;
        int height = 0;
        stbi_uc *decoded = nullptr;
        bool fromMalloc = false;

        if (tex->mHeight == 0)
        {
            decoded = stbi_load_from_memory(reinterpret_cast<const stbi_uc *>(tex->pcData),
                                            static_cast<int>(tex->mWidth), &width, &height,
                                            nullptr, 4);
        }
        else if (tex->pcData != nullptr)
        {
            width = static_cast<int>(tex->mWidth);
            height = static_cast<int>(tex->mHeight);
            decoded = static_cast<stbi_uc *>(std::malloc(static_cast<size_t>(width) * height * 4));
            fromMalloc = true;
            if (decoded != nullptr)
            {
                for (int i = 0; i < width * height; ++i)
                {
                    decoded[4 * i + 0] = tex->pcData[i].r;
                    decoded[4 * i + 1] = tex->pcData[i].g;
                    decoded[4 * i + 2] = tex->pcData[i].b;
                    decoded[4 * i + 3] = tex->pcData[i].a;
                }
            }
        }

        if (decoded == nullptr)
        {
            warn("embedded texture " + key + " could not be decoded");
            return nullptr;
        }

        auto img = std::make_shared<ImageData>();
        img->name = key;
        img->width = static_cast<uint32_t>(width);
        img->height = static_cast<uint32_t>(height);
        img->colorSpace = cs;
        img->pixels.assign(decoded, decoded + static_cast<size_t>(width) * height * 4);

        if (fromMalloc)
            std::free(decoded);
        else
            stbi_image_free(decoded);

        if (!img->valid())
        {
            warn("embedded texture " + key + " has degenerate dimensions");
            return nullptr;
        }
        return adopt(img, key);
    }

    ImagePtr loadExternal(const std::string &filePath, ColorSpace cs)
    {
        int width = 0;
        int height = 0;
        stbi_uc *decoded = stbi_load(filePath.c_str(), &width, &height, nullptr, 4);
        if (decoded == nullptr)
        {
            warn("texture missing or undecodable: " + filePath);
            return nullptr;
        }

        auto img = std::make_shared<ImageData>();
        img->name = filePath;
        img->width = static_cast<uint32_t>(width);
        img->height = static_cast<uint32_t>(height);
        img->colorSpace = cs;
        img->pixels.assign(decoded, decoded + static_cast<size_t>(width) * height * 4);
        stbi_image_free(decoded);

        return adopt(img, filePath);
    }

    // 解析 assimp 的贴图路径：'*N' 是内嵌索引，其余按文件名到 baseDir 下找。
    ImagePtr resolve(const std::string &pathStr, ColorSpace cs)
    {
        if (pathStr.empty())
            return nullptr;

        if (pathStr[0] == '*')
        {
            const int index = std::atoi(pathStr.c_str() + 1);
            if (m_scene == nullptr || index < 0 ||
                static_cast<unsigned>(index) >= m_scene->mNumTextures)
            {
                // 旧代码这里 assert 之后直接索引，release 构建下就是越界读。
                warn("embedded texture " + pathStr + " out of range (have " +
                     std::to_string(m_scene != nullptr ? m_scene->mNumTextures : 0u) + ")");
                return nullptr;
            }
            const std::string key = "embedded:" + pathStr;
            if (const auto it = m_textureCache.find(key); it != m_textureCache.end())
                return it->second;
            return loadEmbedded(m_scene->mTextures[index], key, cs);
        }

        // 只取文件名：模型里的相对路径不可信，不据此穿越目录。
        const size_t slash = pathStr.find_last_of("/\\");
        const std::string fileName =
            (slash == std::string::npos) ? pathStr : pathStr.substr(slash + 1);
        const std::string fullPath = m_baseDir + fileName;

        if (const auto it = m_textureCache.find(fullPath); it != m_textureCache.end())
            return it->second;
        return loadExternal(fullPath, cs);
    }

    // ------------------------------------------------------------ 材质

    MaterialDesc buildMaterial(const aiMaterial *mat, uint32_t index)
    {
        MaterialDesc desc;
        desc.name = "material_" + std::to_string(index);

        if (mat != nullptr)
        {
            aiString name;
            if (mat->Get(AI_MATKEY_NAME, name) == AI_SUCCESS && name.length > 0)
                desc.name = name.C_Str();

            desc.model = m_options->shadingModel;
            int shading = 0;
            if (mat->Get(AI_MATKEY_SHADING_MODEL, shading) == AI_SUCCESS &&
                shading == aiShadingMode_NoShading)
                desc.model = ShadingModel::Unlit;

            // 旧代码把 baseColor / roughness / metallic 读进未初始化的栈变量，
            // Get 失败时留下的是垃圾值。这里每个字段都先落到确定默认值。
            desc.baseColorFactor = math::Vec4{1.0f, 1.0f, 1.0f, 1.0f};
            aiColor4D baseColor{1.0f, 1.0f, 1.0f, 1.0f};
            if (mat->Get(AI_MATKEY_COLOR_DIFFUSE, baseColor) == AI_SUCCESS)
                desc.baseColorFactor = toVec4(baseColor);

            desc.emissiveFactor = math::Vec4{0.0f, 0.0f, 0.0f, 1.0f};
            aiColor4D emissive{0.0f, 0.0f, 0.0f, 1.0f};
            if (mat->Get(AI_MATKEY_COLOR_EMISSIVE, emissive) == AI_SUCCESS)
                desc.emissiveFactor = toVec4(emissive);

            // 与旧代码一致：发光强度放在 emissiveFactor.a 里传递。
            float emissiveIntensity = 1.0f;
            if (mat->Get(AI_MATKEY_EMISSIVE_INTENSITY, emissiveIntensity) == AI_SUCCESS)
                desc.emissiveFactor.w = emissiveIntensity;
            desc.emissiveFactor.w *= m_options->emissiveScale;

            desc.roughnessFactor = 1.0f;
            mat->Get(AI_MATKEY_ROUGHNESS_FACTOR, desc.roughnessFactor);
            desc.metallicFactor = 0.0f;
            mat->Get(AI_MATKEY_METALLIC_FACTOR, desc.metallicFactor);

            int twoSided = 0;
            if (mat->Get(AI_MATKEY_TWOSIDED, twoSided) == AI_SUCCESS)
                desc.doubleSided = twoSided != 0;

            aiString alphaMode;
            if (mat->Get(AI_MATKEY_GLTF_ALPHAMODE, alphaMode) == AI_SUCCESS)
            {
                const std::string mode = alphaMode.C_Str();
                desc.alphaMask = (mode == "MASK");
                if (mode == "BLEND")
                    warn("material '" + desc.name +
                         "' uses alpha BLEND; backends may draw it opaque");
            }
            mat->Get(AI_MATKEY_GLTF_ALPHACUTOFF, desc.alphaCutoff);

            // 贴图回退链与旧代码一致。srgb 编码只给颜色贴图，数据贴图保持线性。
            auto firstTexture = [&](std::initializer_list<aiTextureType> types,
                                    ColorSpace cs) -> TextureSlot {
                for (const aiTextureType type : types)
                {
                    if (mat->GetTextureCount(type) == 0)
                        continue;
                    aiString pathStr;
                    if (mat->GetTexture(type, 0, &pathStr) != AI_SUCCESS || pathStr.length == 0)
                        continue;

                    TextureSlot slot;
                    slot.source = pathStr.C_Str();
                    slot.image = resolve(slot.source, cs);
                    if (slot.image != nullptr)
                        return slot;
                }
                return {};
            };

            desc.baseColor = firstTexture({aiTextureType_BASE_COLOR, aiTextureType_DIFFUSE},
                                          ColorSpace::SRGB);
            desc.emissive = firstTexture({aiTextureType_EMISSIVE}, ColorSpace::SRGB);
            desc.normal = firstTexture({aiTextureType_NORMALS, aiTextureType_HEIGHT},
                                       ColorSpace::Linear);
            desc.ambientOcclusion = firstTexture(
                {aiTextureType_AMBIENT_OCCLUSION, aiTextureType_LIGHTMAP}, ColorSpace::Linear);
            desc.roughness = firstTexture(
                {aiTextureType_DIFFUSE_ROUGHNESS, aiTextureType_SHININESS}, ColorSpace::Linear);
            desc.metallic = firstTexture({aiTextureType_METALNESS}, ColorSpace::Linear);
        }

        return desc;
    }

    // ------------------------------------------------------------ 网格

    MeshData buildMesh(const aiMesh *mesh)
    {
        MeshData out;
        out.name = mesh->mName.C_Str();
        out.vertices.resize(mesh->mNumVertices);

        const bool hasNormals = mesh->HasNormals();
        const bool hasTangents = mesh->HasTangentsAndBitangents();
        const bool hasUV = mesh->HasTextureCoords(0);

        // 旧代码在逐顶点循环里打这两条日志，一个模型能刷满屏；这里每个网格一次。
        if (!hasNormals && mesh->mNumVertices > 0)
            warn("mesh '" + out.name + "': no normals, defaulting to +Z");
        if (!hasUV && mesh->mNumVertices > 0)
            warn("mesh '" + out.name + "': no uv0, defaulting to (0,0)");

        for (unsigned i = 0; i < mesh->mNumVertices; ++i)
        {
            Vertex &v = out.vertices[i];
            v.position = toVec3(mesh->mVertices[i]);

            const math::Vec3 n =
                hasNormals ? safeNormalize(toVec3(mesh->mNormals[i]), math::Vec3{0.0f, 0.0f, 1.0f})
                           : math::Vec3{0.0f, 0.0f, 1.0f};

            if (hasTangents)
            {
                const math::Vec3 rawTangent = toVec3(mesh->mTangents[i]);
                const math::Vec3 rawBitangent = toVec3(mesh->mBitangents[i]);
                // Gram-Schmidt：assimp 给的切线未必严格垂直于法线。
                const math::Vec3 t =
                    safeNormalize(rawTangent - n * math::dot(n, rawTangent),
                                  math::Vec3{1.0f, 0.0f, 0.0f});
                const float handedness =
                    math::dot(math::cross(n, t), rawBitangent) < 0.0f ? -1.0f : 1.0f;
                v.tangent = math::Vec4{t.x, t.y, t.z, handedness};
            }
            else
            {
                // 与旧代码同一套兜底：挑一个与法线不平行的参考轴构造正交基。
                const math::Vec3 up = std::abs(n.z) < 0.999f ? math::Vec3{0.0f, 0.0f, 1.0f}
                                                             : math::Vec3{1.0f, 0.0f, 0.0f};
                const math::Vec3 t =
                    safeNormalize(math::cross(up, n), math::Vec3{1.0f, 0.0f, 0.0f});
                v.tangent = math::Vec4{t.x, t.y, t.z, 1.0f};
            }

            v.normal = n;
            v.uv0 = hasUV ? math::Vec2{mesh->mTextureCoords[0][i].x, mesh->mTextureCoords[0][i].y}
                          : math::Vec2{0.0f, 0.0f};

            // 直接由顶点求包围盒，不依赖 aiProcess_GenBoundingBoxes。
            out.bounds.expand(v.position);
        }

        out.indices.reserve(static_cast<size_t>(mesh->mNumFaces) * 3);
        for (unsigned f = 0; f < mesh->mNumFaces; ++f)
        {
            const aiFace &face = mesh->mFaces[f];
            if (face.mNumIndices != 3)
            {
                // aiProcess_Triangulate 之后不该出现；出现就跳过而不是越界读。
                if (face.mNumIndices != 0)
                    warn("mesh '" + out.name + "': skipped non-triangle face");
                continue;
            }
            for (unsigned k = 0; k < 3; ++k)
                out.indices.push_back(face.mIndices[k]);
        }

        if (mesh->HasBones())
            buildSkinning(mesh, out);

        return out;
    }

    // 骨骼权重与逆绑定矩阵：assimp 按骨骼组织（一个 aiBone 带上它影响的所有
    // 顶点），引擎要的是按顶点组织（每顶点 4 个槽位）。这一次转置只在这里做。
    //
    // 槽位顺序 = 网格里 aiBone 的出现顺序；jointNames / inverseBind 按同一个
    // 循环 push，三个数组下标严格对齐 —— MeshData::skinned() 会校验这一点。
    void buildSkinning(const aiMesh *mesh, MeshData &out)
    {
        out.skinAttributes.assign(mesh->mNumVertices, SkinAttributes{});
        out.jointNames.reserve(mesh->mNumBones);
        out.inverseBind.reserve(mesh->mNumBones);

        // 每个顶点已写下的槽位数。不能用「权重是否为 0」判断空槽：权重 0 是
        // 合法数据，靠值判断会把它当成未写而覆盖掉。
        std::vector<uint8_t> used(mesh->mNumVertices, 0);
        uint64_t dropped = 0;

        for (unsigned b = 0; b < mesh->mNumBones; ++b)
        {
            const aiBone *bone = mesh->mBones[b];

            // 三个数组必须同长同序，所以占位骨骼也要 push，不能跳过。
            if (bone == nullptr || b > 0xFFFFu)
            {
                out.jointNames.emplace_back();
                out.inverseBind.push_back(math::Mat4::identity());
                if (bone != nullptr)
                    dropped += bone->mNumWeights;   // 槽位号超出 uint16，权重全丢
                continue;
            }

            out.jointNames.push_back(bone->mName.C_Str());
            out.inverseBind.push_back(toMat4(bone->mOffsetMatrix));

            const uint16_t slot = static_cast<uint16_t>(b);

            for (unsigned w = 0; w < bone->mNumWeights; ++w)
            {
                const aiVertexWeight &vw = bone->mWeights[w];
                if (vw.mVertexId >= mesh->mNumVertices)
                {
                    // aiProcess_FindInvalidData 不管骨骼权重，越界只能自己挡。
                    ++dropped;
                    continue;
                }

                uint8_t &count = used[vw.mVertexId];
                if (count >= 4)
                {
                    // aiProcess_LimitBoneWeights 已经裁到 4；还能走到这里就说明
                    // 那个后处理没生效。静默计数丢弃，不越界写。
                    ++dropped;
                    continue;
                }

                SkinAttributes &sa = out.skinAttributes[vw.mVertexId];
                sa.joints[count] = slot;
                sa.weights[count] = vw.mWeight;
                ++count;
            }
        }

        if (dropped > 0)
            warn("mesh '" + out.name + "': dropped " + std::to_string(dropped) +
                 " bone influence(s) beyond the 4-per-vertex limit");

        // bindVertices 必须在顶点全部写完之后拷：它是蒙皮的唯一输入，此后不再改。
        out.bindVertices = out.vertices;
    }

    // ------------------------------------------------------------ 节点名 / 骨骼 / 动画

    // 节点名 → 索引。重名保留第一个（emplace 不覆盖），与 AnimationDriver 的
    // 解析口径一致 —— 两边口径不同会让「导入时报没对上」和「运行时驱动了另一
    // 根骨骼」同时成立。
    std::unordered_map<std::string, uint32_t> indexNodesByName() const
    {
        std::unordered_map<std::string, uint32_t> index;
        index.reserve(m_out.nodes.size() * 2);
        for (uint32_t i = 0; i < m_out.nodes.size(); ++i)
        {
            const std::string &name = m_out.nodes[i].name;
            if (!name.empty())
                index.emplace(name, i);
        }
        return index;
    }

    // 把骨骼名解析成 AssetNode 索引。这一步必须等节点树建完：aiMesh::mBones
    // 给的是名字，而名字只有遍历完 mRootNode 才能确定它落在哪个下标上。
    void resolveSkinning(const std::unordered_map<std::string, uint32_t> &nodeIndex)
    {
        for (MeshData &mesh : m_out.meshes)
        {
            if (mesh.jointNames.empty())
                continue;

            mesh.jointNodes.assign(mesh.jointNames.size(), kInvalidIndex);
            size_t missing = 0;

            for (size_t b = 0; b < mesh.jointNames.size(); ++b)
            {
                const auto it = nodeIndex.find(mesh.jointNames[b]);
                if (it == nodeIndex.end())
                {
                    ++missing;
                    continue;
                }
                mesh.jointNodes[b] = it->second;
            }

            if (missing > 0)
                warn("mesh '" + mesh.name + "': " + std::to_string(missing) + " of " +
                     std::to_string(mesh.jointNames.size()) +
                     " bone(s) not found in the node table; their weights are ignored");
        }
    }

    void buildAnimations(const std::unordered_map<std::string, uint32_t> &nodeIndex)
    {
        m_out.animations.reserve(m_scene->mNumAnimations);

        for (unsigned a = 0; a < m_scene->mNumAnimations; ++a)
        {
            const aiAnimation *src = m_scene->mAnimations[a];
            if (src == nullptr)
                continue;

            AnimationClip clip;
            clip.name = src->mName.length > 0 ? src->mName.C_Str()
                                              : ("anim" + std::to_string(a));

            // mTicksPerSecond == 0 在 assimp 里表示「文件没写」，不是「无穷快」。
            // 按 1.0 处理（时间单位即秒），并留一条 warning。直接除会得到 inf
            // 时长，之后每一次取模、每一次区间定位都是 NaN。
            double toSeconds = 1.0;
            if (src->mTicksPerSecond > 0.0)
                toSeconds = 1.0 / src->mTicksPerSecond;
            else if (src->mTicksPerSecond == 0.0 && src->mDuration > 0.0)
                warn("animation '" + clip.name +
                     "': tick rate is 0 (unspecified); treating key times as seconds");

            clip.durationSeconds = static_cast<float>(src->mDuration * toSeconds);
            clip.channels.reserve(src->mNumChannels);

            size_t unresolved = 0;

            for (unsigned c = 0; c < src->mNumChannels; ++c)
            {
                const aiNodeAnim *srcChannel = src->mChannels[c];
                if (srcChannel == nullptr)
                    continue;

                AnimationChannel channel;
                channel.nodeName = srcChannel->mNodeName.C_Str();

                const auto it = nodeIndex.find(channel.nodeName);
                if (it == nodeIndex.end())
                    ++unresolved;   // 通道照留，索引保持 kInvalidIndex
                else
                    channel.node = it->second;

                channel.positions.reserve(srcChannel->mNumPositionKeys);
                for (unsigned k = 0; k < srcChannel->mNumPositionKeys; ++k)
                {
                    const aiVectorKey &key = srcChannel->mPositionKeys[k];
                    channel.positions.push_back(
                        Vec3Key{static_cast<float>(key.mTime * toSeconds), toVec3(key.mValue)});
                }

                channel.rotations.reserve(srcChannel->mNumRotationKeys);
                for (unsigned k = 0; k < srcChannel->mNumRotationKeys; ++k)
                {
                    const aiQuatKey &key = srcChannel->mRotationKeys[k];
                    // 归一化：slerp 假设两个输入都是单位四元数，文件里的值未必是
                    // （量化、手改都会破坏）。零四元数退化成单位旋转，不产出 NaN。
                    const math::Quat q{static_cast<float>(key.mValue.x),
                                       static_cast<float>(key.mValue.y),
                                       static_cast<float>(key.mValue.z),
                                       static_cast<float>(key.mValue.w)};
                    channel.rotations.push_back(
                        QuatKey{static_cast<float>(key.mTime * toSeconds), math::normalize(q)});
                }

                channel.scales.reserve(srcChannel->mNumScalingKeys);
                for (unsigned k = 0; k < srcChannel->mNumScalingKeys; ++k)
                {
                    const aiVectorKey &key = srcChannel->mScalingKeys[k];
                    channel.scales.push_back(
                        Vec3Key{static_cast<float>(key.mTime * toSeconds), toVec3(key.mValue)});
                }

                clip.channels.push_back(std::move(channel));
            }

            if (unresolved > 0)
                warn("animation '" + clip.name + "': " + std::to_string(unresolved) +
                     " channel(s) target node names that are not in the node table");

            m_out.animations.push_back(std::move(clip));
        }
    }

    // ------------------------------------------------------------ 节点

    uint32_t buildNode(const aiNode *node, uint32_t parent)
    {
        const uint32_t index = static_cast<uint32_t>(m_out.nodes.size());
        m_out.nodes.push_back(AssetNode{});

        {
            AssetNode &n = m_out.nodes[index];
            n.name = node->mName.C_Str();
            n.parent = parent;
            n.localTransform = toMat4(node->mTransformation);

            n.primitives.reserve(node->mNumMeshes);
            for (unsigned i = 0; i < node->mNumMeshes; ++i)
            {
                const uint32_t meshIndex = node->mMeshes[i];
                if (meshIndex >= m_out.meshes.size())
                {
                    warn("node '" + n.name + "': mesh index " + std::to_string(meshIndex) +
                         " out of range");
                    continue;
                }
                AssetPrimitive prim;
                prim.mesh = meshIndex;
                const aiMesh *mesh = m_scene->mMeshes[meshIndex];
                prim.material = (mesh != nullptr && mesh->mMaterialIndex < m_out.materials.size())
                                    ? mesh->mMaterialIndex
                                    : kInvalidIndex;
                n.primitives.push_back(prim);
            }
        }

        std::vector<uint32_t> children;
        children.reserve(node->mNumChildren);
        for (unsigned i = 0; i < node->mNumChildren; ++i)
            children.push_back(buildNode(node->mChildren[i], index));

        // 递归 push_back 已经让上面那个引用失效，这里重新取。
        AssetNode &n = m_out.nodes[index];
        n.children = std::move(children);

        for (const AssetPrimitive &prim : n.primitives)
            if (prim.mesh < m_out.meshes.size())
                n.meshBounds.expand(m_out.meshes[prim.mesh].bounds);

        // localBounds 自底向上合并：本节点网格 ∪ 子树盒（变换到本节点空间）。
        n.localBounds = n.meshBounds;
        for (const uint32_t child : n.children)
        {
            const AssetNode &c = m_out.nodes[child];
            n.localBounds.expand(c.localBounds.transformed(c.localTransform));
        }

        return index;
    }

    const ImportOptions *m_options = nullptr;
    const aiScene *m_scene = nullptr;
    std::string m_baseDir;

    AssetScene m_out;
    std::vector<std::string> m_warnings;
    std::unordered_map<std::string, ImagePtr> m_textureCache;
    std::vector<ImagePtr> m_imagePool;
};

// 外部贴图目录：显式指定优先，否则用 "<模型目录>/textures/"（与旧代码一致）。
std::string makeBaseDir(const std::string &path, const ImportOptions &options)
{
    std::string dir;
    if (!options.textureDirectory.empty())
    {
        dir = options.textureDirectory;
        if (dir.back() != '/' && dir.back() != '\\')
            dir += '/';
        return dir;
    }

    const size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos)
        return "textures/";
    return path.substr(0, slash + 1) + "textures/";
}

} // namespace

ImportResult importScene(const std::string &path, const ImportOptions &options)
{
    ImportResult result;

    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(path,
                                             aiProcess_Triangulate |
                                                 aiProcess_CalcTangentSpace |
                                                 aiProcess_GenSmoothNormals |
                                                 aiProcess_JoinIdenticalVertices |
                                                 aiProcess_ImproveCacheLocality |
                                                 aiProcess_LimitBoneWeights |
                                                 aiProcess_FindInvalidData);

    if (scene == nullptr)
    {
        result.error = std::string("assimp: ") + importer.GetErrorString();
        return result;
    }
    if (scene->mRootNode == nullptr)
    {
        result.error = "assimp: file has no root node";
        return result;
    }

    SceneImporter worker(options, scene, makeBaseDir(path, options));
    worker.run(path);

    result.scene = worker.takeScene();
    result.warnings = worker.takeWarnings();

    result.stats.nodeCount = static_cast<uint32_t>(result.scene.nodes.size());
    result.stats.meshCount = static_cast<uint32_t>(result.scene.meshes.size());
    result.stats.materialCount = static_cast<uint32_t>(result.scene.materials.size());
    result.stats.textureCount = static_cast<uint32_t>(result.scene.images.size());
    for (const MeshData &mesh : result.scene.meshes)
    {
        result.stats.vertexCount += mesh.vertexCount();
        result.stats.triangleCount += mesh.triangleCount();
        if (mesh.skinned())
            ++result.stats.skinnedMeshCount;
    }
    result.stats.animationCount = static_cast<uint32_t>(result.scene.animations.size());

    result.ok = true;
    return result;
}

} // namespace my3d::asset
