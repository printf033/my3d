#pragma once

#include "logger.hpp"
#include "node.hpp"
#include "vertex.hpp"
#include "converter.hpp"
#include <unordered_map>
#include <shared_mutex>
#include <fstream>
#include <vector>
#include <atomic>
#include <memory>
#include <string>
#include <cassert>
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <stb/stb_image.h>
#include <filament/Engine.h>
#include <filament/Renderer.h>
#include <filament/TextureSampler.h>
#include <filament/Material.h>
#include <filament/View.h>
#include <filament/Scene.h>
#include <utils/Entity.h>
#include <utils/EntityManager.h>
#include <filament/TransformManager.h>
#include <filament/RenderableManager.h>
#include <filament/VertexBuffer.h>
#include <filament/IndexBuffer.h>
#include <filament/Texture.h>
#include <filament/IndirectLight.h>
#include <filament/LightManager.h>
#include <filament/Skybox.h>
#include <ktxreader/Ktx1Reader.h>
#include <ktxreader/Ktx2Reader.h>
#include <gltfio/ResourceLoader.h>
#include <math/mat3.h>
#include <math/vec3.h>
#include <math/quat.h>

class Engine
{
    filament::Engine *engineGPU_ = nullptr;
    filament::Renderer *rendererGPU_ = nullptr;
    filament::VertexBuffer *verticesGPU_ = nullptr;
    filament::IndexBuffer *indicesGPU_ = nullptr;
    std::unordered_map<std::string, filament::Texture *> texturesGPU_;
    std::unordered_map<std::string, filament::Material *> shadersGPU_;
    std::unordered_map<std::string, filament::View *> viewsGPU_;
    std::unordered_map<std::string, filament::Camera *> camerasGPU_;
    std::unordered_map<std::string, utils::Entity> entitiesGPU_;
    std::unordered_map<std::string, filament::Skybox *> skyboxesGPU_;
    std::unordered_map<std::string, filament::IndirectLight *> imageBasedLightsGPU_;
    std::unordered_map<std::string, filament::Scene *> scenesGPU_;

    std::vector<Vertex> verticesCPU_;
    std::vector<uint32_t> indicesCPU_;
    std::unordered_map<std::string, Node> modelsCPU_;

public:
    Engine() noexcept { engineGPU_ = filament::Engine::create(filament::Engine::Backend::DEFAULT); }
    ~Engine() noexcept = default;
    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;
    Engine(Engine &&) noexcept = delete;
    Engine &operator=(Engine &&) noexcept = delete;
    void loadModel(const std::string &file, const std::string &filamat)
    {
        if (modelsCPU_.contains(file))
            return;
        Assimp::Importer importer;
        auto scene = importer.ReadFile(file,
                                       aiProcess_Triangulate |
                                           aiProcess_CalcTangentSpace |
                                           aiProcess_GenSmoothNormals |
                                           aiProcess_GenBoundingBoxes |
                                           aiProcess_JoinIdenticalVertices |
                                           aiProcess_ImproveCacheLocality |
                                           aiProcess_FindInvalidData);
        if (scene && scene->mRootNode)
        {
            Node tmp = importNode(scene->mRootNode, scene->mMeshes, scene->mMaterials, scene->mTextures, file, filamat);
            modelsCPU_.emplace(file, std::move(tmp));
            if (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE)
                LOG_ERROR(importer.GetErrorString());
        }
        LOG_INFO("load model {}", file);
    }
    inline void asyncVerticesIndices2GPU() noexcept
    {
        verticesGPU_ = filament::VertexBuffer::Builder()
                           .vertexCount(verticesCPU_.size())
                           .bufferCount(1)
                           .attribute(filament::VertexAttribute::POSITION, 0, filament::VertexBuffer::AttributeType::FLOAT3, 0, sizeof(Vertex))
                           .attribute(filament::VertexAttribute::TANGENTS, 0, filament::VertexBuffer::AttributeType::FLOAT4, offsetof(Vertex, tbn), sizeof(Vertex))
                           .attribute(filament::VertexAttribute::UV0, 0, filament::VertexBuffer::AttributeType::FLOAT2, offsetof(Vertex, uv), sizeof(Vertex))
                           .build(*engineGPU_);
        verticesGPU_->setBufferAt(*engineGPU_, 0,
                                  filament::VertexBuffer::BufferDescriptor(
                                      verticesCPU_.data(),
                                      verticesCPU_.size() * sizeof(Vertex)));
        indicesGPU_ = filament::IndexBuffer::Builder()
                          .indexCount(indicesCPU_.size())
                          .bufferType(filament::IndexBuffer::IndexType::UINT)
                          .build(*engineGPU_);
        indicesGPU_->setBuffer(*engineGPU_,
                               filament::IndexBuffer::BufferDescriptor(
                                   indicesCPU_.data(),
                                   indicesCPU_.size() * sizeof(uint32_t)));
    }
    inline filament::Engine *getEngine() noexcept
    {
        return engineGPU_;
    }
    inline filament::Renderer *getRenderer() noexcept
    {
        if (rendererGPU_)
            return rendererGPU_;
        return rendererGPU_ = engineGPU_->createRenderer();
    }
    inline filament::View *getView(const std::string &key)
    {
        if (viewsGPU_.contains(key))
            return viewsGPU_[key];
        viewsGPU_.emplace(key, engineGPU_->createView());
        return viewsGPU_[key];
    }
    inline filament::Camera *getCamera(const std::string &key)
    {
        if (camerasGPU_.contains(key))
            return camerasGPU_[key];
        camerasGPU_.emplace(key, engineGPU_->createCamera(getEntity(key)));
        return camerasGPU_[key];
    }
    inline filament::Scene *getScene(const std::string &key)
    {
        if (scenesGPU_.contains(key))
            return scenesGPU_[key];
        scenesGPU_.emplace(key, engineGPU_->createScene());
        return scenesGPU_[key];
    }
    inline utils::Entity getEntity(const std::string &key)
    {
        if (entitiesGPU_.contains(key))
            return entitiesGPU_[key];
        entitiesGPU_.emplace(key, utils::EntityManager::get().create());
        return entitiesGPU_[key];
    }
    void addIBL2Scene(const std::string &sceneKey, const std::string &ktx, float intensity = 30000.0f)
    {
        auto scene = getScene(sceneKey);
        if (!texturesGPU_.contains(ktx))
        {
            filament::Texture *texture = nullptr;
            if (ktx.back() != '2')
            {
                std::vector<uint8_t> buffer;
                readFile(ktx, buffer);
                auto bytes = buffer.data();
                auto nbytes = buffer.size();
                struct KtxPack
                {
                    std::vector<uint8_t> buffer;
                    ktxreader::Ktx1Bundle bundle;
                } *pack = new KtxPack(std::move(buffer), ktxreader::Ktx1Bundle(bytes, nbytes));
                texture = ktxreader::Ktx1Reader::createTexture(engineGPU_, pack->bundle, false, [](void *user) -> void
                                                               { delete reinterpret_cast<KtxPack *>(user); }, pack);
            }
            else
            {
                std::vector<uint8_t> buffer;
                readFile(ktx, buffer);
                ktxreader::Ktx2Reader reader(*engineGPU_);
                reader.requestFormat(filament::Texture::InternalFormat::RGB16F);
                reader.requestFormat(filament::Texture::InternalFormat::RGB8);
                texture = reader.load(buffer.data(), buffer.size(), ktxreader::Ktx2Reader::TransferFunction::LINEAR);
            }
            if (!texture)
            {
                LOG_ERROR("load {}", ktx);
                return;
            }
            LOG_DEBUG("load ibl {}: {}x{}, {} levels", ktx, texture->getWidth(0), texture->getHeight(0), texture->getLevels());
            texturesGPU_.emplace(ktx, texture);
        }
        auto texture = texturesGPU_[ktx];
        if (!imageBasedLightsGPU_.contains(ktx))
        {
            filament::math::float3 sh[9] = {};
            readSphericalHarmonics(ktx.substr(0, ktx.find_last_of("/")) + "/sh.txt", sh);
            auto ibl = filament::IndirectLight::Builder()
                           .reflections(texture)
                           .irradiance(3, sh)
                           .intensity(intensity)
                           .build(*engineGPU_);
            imageBasedLightsGPU_.emplace(ktx, ibl);
        }
        scene->setIndirectLight(imageBasedLightsGPU_[ktx]);
        LOG_INFO("add ibl {} to scene {}", ktx, sceneKey);
    }
    void addSkybox2Scene(const std::string &sceneKey, const std::string &ktx)
    {
        auto scene = getScene(sceneKey);
        if (!texturesGPU_.contains(ktx))
        {
            filament::Texture *texture = nullptr;
            if (ktx.back() != '2')
            {
                std::vector<uint8_t> buffer;
                readFile(ktx, buffer);
                auto bytes = buffer.data();
                auto nbytes = buffer.size();
                struct KtxPack
                {
                    std::vector<uint8_t> buffer;
                    ktxreader::Ktx1Bundle bundle;
                } *pack = new KtxPack(std::move(buffer), ktxreader::Ktx1Bundle(bytes, nbytes));
                texture = ktxreader::Ktx1Reader::createTexture(engineGPU_, pack->bundle, true, [](void *user) -> void
                                                               { delete reinterpret_cast<KtxPack *>(user); }, pack);
            }
            else
            {
                std::vector<uint8_t> buffer;
                readFile(ktx, buffer);
                ktxreader::Ktx2Reader reader(*engineGPU_);
                reader.requestFormat(filament::Texture::InternalFormat::RGB16F);
                reader.requestFormat(filament::Texture::InternalFormat::RGB8);
                texture = reader.load(buffer.data(), buffer.size(), ktxreader::Ktx2Reader::TransferFunction::sRGB);
            }
            if (!texture)
            {
                LOG_ERROR("load {}", ktx);
                return;
            }
            LOG_DEBUG("load skybox {}: {}x{}, {} levels", ktx, texture->getWidth(0), texture->getHeight(0), texture->getLevels());
            texturesGPU_.emplace(ktx, texture);
        }
        auto texture = texturesGPU_[ktx];
        if (!skyboxesGPU_.contains(ktx))
        {
            auto skybox = filament::Skybox::Builder()
                              .environment(texture)
                              .showSun(false)
                              .build(*engineGPU_);
            skyboxesGPU_.emplace(ktx, skybox);
        }
        scene->setSkybox(skyboxesGPU_[ktx]);
        LOG_INFO("add skybox {} to scene {}", ktx, sceneKey);
    }
    inline void addModel2Scene(const std::string &sceneKey, const std::string &modelKey, const std::string &loadedFile) noexcept
    {
        syncNode2GPU(getScene(sceneKey), getEntity(modelKey), modelsCPU_[loadedFile]);
        LOG_INFO("add model {} to scene {}", modelKey, sceneKey);
    }
    inline void syncNode2GPU(filament::Scene *scene, utils::Entity dst, Node &node) noexcept
    {
        scene->addEntity(dst);
        syncTransform2GPU(dst, node.transform);
        if (!node.meshes.empty())
        {
            filament::RenderableManager::Builder builder(node.meshes.size());
            auto sphere = node.bound.getBoundingSphere();
            builder.boundingBox(filament::Box().set(sphere.xyz - sphere.w, sphere.xyz + sphere.w));
            size_t idx = 0;
            for (auto &[_, primitive] : node.meshes)
            {
                builder.geometry(idx,
                                 filament::RenderableManager::PrimitiveType::TRIANGLES,
                                 verticesGPU_,
                                 indicesGPU_,
                                 primitive.indexOffset,
                                 primitive.indexCount)
                    .material(idx,
                              primitive.material);
                ++idx;
            }
            builder.build(*engineGPU_, dst);
        }
        for (auto &[key, child] : node.children)
        {
            auto entity = getEntity(key);
            syncParent2GPU(entity, dst);
            syncNode2GPU(scene, entity, child);
        }
    }
    inline void syncTransform2GPU(utils::Entity dst, filament::math::mat4f &transform) noexcept
    {
        auto &manager = engineGPU_->getTransformManager();
        manager.setTransform(manager.getInstance(dst), transform);
    }
    inline void syncParent2GPU(utils::Entity dst, utils::Entity parent) noexcept
    {
        auto &manager = engineGPU_->getTransformManager();
        manager.setParent(manager.getInstance(dst), manager.getInstance(parent));
    }

private:
    Node importNode(aiNode *node, aiMesh *meshes[], aiMaterial *materials[], aiTexture *textures[], const std::string &file, const std::string &filamat)
    {
        assert(node);
        assert(meshes);
        assert(materials);
        Node tmp;
        tmp.transform = Converter::assimp2filament(node->mTransformation);
        tmp.meshes.reserve(node->mNumMeshes);
        filament::math::float3 nodeMin;
        filament::math::float3 nodeMax;
        LOG_TRACE("node key: {}, mesh num: {}, children num: {}", node->mName.C_Str(), node->mNumMeshes, node->mNumChildren);
        for (unsigned int i = 0; i < node->mNumMeshes; ++i)
        {
            aiMesh *mesh = meshes[node->mMeshes[i]];
            std::string key = mesh->mName.C_Str();
            tmp.meshes.emplace(key, importMesh(mesh, materials[mesh->mMaterialIndex], textures, file, filamat));
            filament::math::float3 meshMin = tmp.meshes[key].bound.getMin();
            filament::math::float3 meshMax = tmp.meshes[key].bound.getMax();
            if (i != 0)
            {
                nodeMin.x = std::min(nodeMin.x, meshMin.x);
                nodeMin.y = std::min(nodeMin.y, meshMin.y);
                nodeMin.z = std::min(nodeMin.z, meshMin.z);
                nodeMax.x = std::max(nodeMax.x, meshMax.x);
                nodeMax.y = std::max(nodeMax.y, meshMax.y);
                nodeMax.z = std::max(nodeMax.z, meshMax.z);
            }
            else
            {
                nodeMin = meshMin;
                nodeMax = meshMax;
            }
        }
        tmp.children.reserve(node->mNumChildren);
        for (unsigned int i = 0; i < node->mNumChildren; ++i)
        {
            aiNode *child = node->mChildren[i];
            std::string key = child->mName.C_Str();
            tmp.children.emplace(key, importNode(child, meshes, materials, textures, file, filamat));
            filament::math::float3 childMin = tmp.children[key].bound.getMin();
            filament::math::float3 childMax = tmp.children[key].bound.getMax();
            if (i != 0)
            {
                nodeMin.x = std::min(nodeMin.x, childMin.x);
                nodeMin.y = std::min(nodeMin.y, childMin.y);
                nodeMin.z = std::min(nodeMin.z, childMin.z);
                nodeMax.x = std::max(nodeMax.x, childMax.x);
                nodeMax.y = std::max(nodeMax.y, childMax.y);
                nodeMax.z = std::max(nodeMax.z, childMax.z);
            }
            else
            {
                nodeMin = childMin;
                nodeMax = childMax;
            }
        }
        tmp.bound.set(nodeMin, nodeMax);
        return tmp;
    }
    Mesh importMesh(aiMesh *mesh, aiMaterial *material, aiTexture *textures[], const std::string &file, const std::string &filamat)
    {
        assert(mesh);
        assert(material);
        Mesh tmp;
        uint32_t base = static_cast<uint32_t>(verticesCPU_.size());
        tmp.indexOffset = static_cast<uint32_t>(indicesCPU_.size());
        tmp.indexCount = mesh->mNumFaces * 3U;
        // vertices
        verticesCPU_.resize(verticesCPU_.size() + mesh->mNumVertices);
        for (unsigned int i = 0; i < mesh->mNumVertices; ++i)
        {
            auto &vertex = verticesCPU_[base + i];
            // position
            vertex.xyz = {mesh->mVertices[i].x, mesh->mVertices[i].y, mesh->mVertices[i].z};
            // normal
            if (mesh->HasNormals())
            {
                filament::math::float3 n = {mesh->mNormals[i].x, mesh->mNormals[i].y, mesh->mNormals[i].z};
                if (mesh->HasTangentsAndBitangents())
                {
                    filament::math::float3 t = {mesh->mTangents[i].x, mesh->mTangents[i].y, mesh->mTangents[i].z};
                    filament::math::float3 b = {mesh->mBitangents[i].x, mesh->mBitangents[i].y, mesh->mBitangents[i].z};
                    vertex.tbn = filament::math::mat3f::packTangentFrame({t, b, n});
                }
                else
                {
                    filament::math::float3 up = (std::abs(n.z) < 0.999f)
                                                    ? filament::math::float3{0.0f, 0.0f, 1.0f}
                                                    : filament::math::float3{1.0f, 0.0f, 0.0f};
                    filament::math::float3 t = normalize(cross(up, n));
                    filament::math::float3 b = cross(n, t);
                    vertex.tbn = filament::math::mat3f::packTangentFrame({t, b, n});
                }
            }
            else
            {
                vertex.tbn = {0.0f, 0.0f, 0.0f, 1.0f};
                LOG_WARN("vertex {} {} {} has no normal");
            }
            // texture coordinate
            if (mesh->HasTextureCoords(0))
            {
                vertex.uv = {mesh->mTextureCoords[0][i].x, mesh->mTextureCoords[0][i].y};
            }
            else
            {
                vertex.uv = {0.0f, 0.0f};
                LOG_WARN("vertex {} {} {} has no texture coordinate");
            }
        }
        // indices
        indicesCPU_.reserve(indicesCPU_.size() + mesh->mNumFaces * 3);
        for (unsigned int i = 0; i < mesh->mNumFaces; ++i)
        {
            for (unsigned int j = 0; j < 3U; ++j)
            {
                uint32_t index = static_cast<uint32_t>(mesh->mFaces[i].mIndices[j]);
                indicesCPU_.push_back(base + index);
            }
        }
        // box
        tmp.bound.set(Converter::assimp2filament(mesh->mAABB.mMin), Converter::assimp2filament(mesh->mAABB.mMax));
        // material
        tmp.material = loadShader(filamat)->createInstance();
        filament::TextureSampler sampler(filament::TextureSampler::MinFilter::LINEAR_MIPMAP_LINEAR,
                                         filament::TextureSampler::MagFilter::LINEAR,
                                         filament::TextureSampler::WrapMode::CLAMP_TO_EDGE);
        size_t lastSlash = file.find_last_of("/\\");
        std::string baseDir = (lastSlash == std::string::npos) ? "textures/" : file.substr(0, lastSlash + 1) + "textures/";
        auto resolveAndLoad = [&](aiTextureType type, const char *paramName) -> bool
        {
            aiString path;
            filament::Texture *texture = nullptr;
            if (material->GetTexture(type, 0, &path) == AI_SUCCESS)
            {
                std::string pathStr(path.C_Str());
                if (pathStr.empty())
                    return false;
                if (pathStr[0] == '*')
                {
                    assert(textures);
                    int index = std::stoi(pathStr.substr(1));
                    aiTexture *embedded = textures[index];
                    std::string key = baseDir + pathStr;
                    texture = loadEmbeddedMaterialAsyncTextures2GPU(embedded, key, ((type == aiTextureType_DIFFUSE || type == aiTextureType_BASE_COLOR) ? filament::Texture::InternalFormat::SRGB8_A8 : filament::Texture::InternalFormat::RGBA8));
                    LOG_DEBUG("load {}", key);
                }
                else
                {
                    size_t p = pathStr.find_last_of("/\\");
                    std::string fileName = (p == std::string::npos) ? pathStr : pathStr.substr(p + 1);
                    std::string fullPath = baseDir + fileName;
                    texture = loadLocalMaterialAsyncTextures2GPU(fullPath, ((type == aiTextureType_DIFFUSE || type == aiTextureType_BASE_COLOR) ? filament::Texture::InternalFormat::SRGB8_A8 : filament::Texture::InternalFormat::RGBA8));
                    LOG_DEBUG("load {}", fullPath);
                }
                if (!texture)
                {
                    LOG_ERROR("load {}", pathStr);
                    return false;
                }
            }
            tmp.material->setParameter(paramName, texture, sampler);
            return true;
        };
        // base color
        aiColor4D baseColorFactor;
        material->Get(AI_MATKEY_COLOR_DIFFUSE, baseColorFactor);
        tmp.material->setParameter("baseColorFactor", Converter::assimp2filament(baseColorFactor));
        LOG_INFO("baseColorFactor {} {} {} {}", baseColorFactor.r, baseColorFactor.g, baseColorFactor.b, baseColorFactor.a);
        if (material->GetTextureCount(aiTextureType_BASE_COLOR) > 0)
            resolveAndLoad(aiTextureType_BASE_COLOR, "baseColor");
        else
            resolveAndLoad(aiTextureType_DIFFUSE, "baseColor");
        // emissive
        if (material->GetTextureCount(aiTextureType_EMISSIVE) > 0)
        {
            aiColor4D emissiveFactor;
            material->Get(AI_MATKEY_COLOR_EMISSIVE, emissiveFactor);
            material->Get(AI_MATKEY_EMISSIVE_INTENSITY, emissiveFactor.a);
            tmp.material->setParameter("emissiveFactor", Converter::assimp2filament(emissiveFactor));
            LOG_INFO("emissiveFactor {} {} {} {}", emissiveFactor.r, emissiveFactor.g, emissiveFactor.b, emissiveFactor.a);
            resolveAndLoad(aiTextureType_EMISSIVE, "emissive");
        }
        // normal
        if (material->GetTextureCount(aiTextureType_NORMALS) > 0)
            resolveAndLoad(aiTextureType_NORMALS, "normal");
        else if (material->GetTextureCount(aiTextureType_HEIGHT) > 0)
            resolveAndLoad(aiTextureType_HEIGHT, "normal");
        // ambient occlusion
        if (material->GetTextureCount(aiTextureType_AMBIENT_OCCLUSION) > 0)
            resolveAndLoad(aiTextureType_AMBIENT_OCCLUSION, "ambientOcclusion");
        else if (material->GetTextureCount(aiTextureType_LIGHTMAP) > 0)
            resolveAndLoad(aiTextureType_LIGHTMAP, "ambientOcclusion");
        // roughness
        if (material->GetTextureCount(aiTextureType_DIFFUSE_ROUGHNESS) > 0)
        {
            float roughnessFactor;
            material->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughnessFactor);
            tmp.material->setParameter("roughnessFactor", roughnessFactor);
            LOG_INFO("roughnessFactor {}", roughnessFactor);
            resolveAndLoad(aiTextureType_DIFFUSE_ROUGHNESS, "roughness");
        }
        else if (material->GetTextureCount(aiTextureType_SHININESS) > 0)
        {
            float roughnessFactor;
            material->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughnessFactor);
            tmp.material->setParameter("roughnessFactor", roughnessFactor);
            LOG_INFO("roughnessFactor {}", roughnessFactor);
            resolveAndLoad(aiTextureType_SHININESS, "roughness");
        }
        // metallic
        if (material->GetTextureCount(aiTextureType_METALNESS) > 0)
        {
            float metallicFactor;
            material->Get(AI_MATKEY_METALLIC_FACTOR, metallicFactor);
            tmp.material->setParameter("metallicFactor", metallicFactor);
            LOG_INFO("metallicFactor {}", metallicFactor);
            resolveAndLoad(aiTextureType_METALNESS, "metallic");
        }
        return tmp;
    }
    filament::Material *loadShader(const std::string &filamat)
    {
        if (shadersGPU_.contains(filamat))
            return shadersGPU_[filamat];
        std::vector<uint8_t> buffer;
        if (!readFile(filamat, buffer))
            return nullptr;
        filament::Material *ptr = filament::Material::Builder()
                                      .package(buffer.data(), buffer.size())
                                      .build(*engineGPU_);
        shadersGPU_.emplace(filamat, ptr);
        return ptr;
    }
    filament::Texture *loadEmbeddedMaterialAsyncTextures2GPU(const aiTexture *texture,
                                                             const std::string &key,
                                                             filament::Texture::InternalFormat format)
    {
        assert(texture);
        if (texturesGPU_.contains(key))
            return texturesGPU_[key];
        int width = 0;
        int height = 0;
        stbi_uc *image = nullptr;
        if (texture->mHeight == 0)
        {
            image = stbi_load_from_memory(
                reinterpret_cast<const stbi_uc *>(texture->pcData),
                texture->mWidth,
                &width,
                &height,
                nullptr,
                4);
            if (!image)
                return nullptr;
        }
        else
        {
            width = texture->mWidth;
            height = texture->mHeight;
            image = reinterpret_cast<stbi_uc *>(
                malloc(size_t(width * height * 4)));
            aiTexel *src = texture->pcData;
            stbi_uc *dst = image;
            for (int i = 0; i < width * height; ++i)
            {
                dst[4 * i + 0] = src[i].r;
                dst[4 * i + 1] = src[i].g;
                dst[4 * i + 2] = src[i].b;
                dst[4 * i + 3] = src[i].a;
            }
        }
        uint32_t levels = static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1;
        filament::Texture *ptr = filament::Texture::Builder()
                                     .width(uint32_t(width))
                                     .height(uint32_t(height))
                                     .levels(levels)
                                     .format(format)
                                     .sampler(filament::Texture::Sampler::SAMPLER_2D)
                                     .build(*engineGPU_);
        ptr->setImage(*engineGPU_, 0,
                      filament::Texture::PixelBufferDescriptor(
                          image,
                          size_t(width * height * 4),
                          filament::Texture::Format::RGBA,
                          filament::Texture::Type::UBYTE,
                          [](void *mem, size_t, void *)
                          { free(mem); }));
        // ptr->generateMipmaps(*engineGPU_);///////////////////////
        texturesGPU_.emplace(key, ptr);
        return ptr;
    }
    filament::Texture *loadLocalMaterialAsyncTextures2GPU(const std::string &file,
                                                          filament::Texture::InternalFormat format)
    {
        if (texturesGPU_.contains(file))
            return texturesGPU_[file];
        int width = 0;
        int height = 0;
        stbi_uc *image = stbi_load(
            file.c_str(),
            &width,
            &height,
            nullptr,
            4);
        if (!image)
            return nullptr;
        uint32_t levels = static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1;
        filament::Texture *ptr = filament::Texture::Builder()
                                     .width(uint32_t(width))
                                     .height(uint32_t(height))
                                     .levels(levels)
                                     .format(format)
                                     .sampler(filament::Texture::Sampler::SAMPLER_2D)
                                     .build(*engineGPU_);
        ptr->setImage(*engineGPU_, 0,
                      filament::Texture::PixelBufferDescriptor(
                          image,
                          size_t(width * height * 4),
                          filament::Texture::Format::RGBA,
                          filament::Texture::Type::UBYTE,
                          [](void *mem, size_t, void *)
                          { stbi_image_free(mem); }));
        // ptr->generateMipmaps(*engineGPU_);///////////////////////
        texturesGPU_.emplace(file, ptr);
        return ptr;
    }
    bool readFile(const std::string &path, std::vector<uint8_t> &buffer)
    {
        std::ifstream ifs(path, std::ios::binary | std::ios::ate);
        if (!ifs)
        {
            LOG_ERROR("cannot open {}", path);
            return false;
        }
        std::streamsize size = ifs.tellg();
        ifs.seekg(0, std::ios::beg);
        buffer.resize(static_cast<size_t>(size));
        if (!ifs.read(reinterpret_cast<char *>(buffer.data()), size))
        {
            LOG_ERROR("cannot read {}", path);
            return false;
        }
        return true;
    }
    bool readSphericalHarmonics(const std::string &path, filament::math::float3 sh[9])
    {
        std::ifstream ifs(path);
        if (!ifs)
        {
            LOG_ERROR("cannot open spherical harmonics file {}", path);
            for (int i = 0; i < 9; i++)
                sh[i] = filament::math::float3(1.0f);
            return false;
        }
        std::string line;
        int index = 0;
        while (std::getline(ifs, line) && index < 9)
        {
            if (line.empty() || line[0] == '/' || line[0] == '#')
                continue;
            size_t start = line.find('(');
            size_t end = line.find(')');
            if (start != std::string::npos && end != std::string::npos)
            {
                std::string values = line.substr(start + 1, end - start - 1);
                float r, g, b;
                if (sscanf(values.c_str(), "%f, %f, %f", &r, &g, &b) == 3)
                {
                    sh[index] = filament::math::float3(r, g, b);
                    ++index;
                    LOG_TRACE(values);
                }
            }
        }
        if (index < 9)
        {
            LOG_ERROR("incomplete spherical harmonics data, got {} bands", index);
            return false;
        }
        return true;
    }
};
